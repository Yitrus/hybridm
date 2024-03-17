/*
 * memory access sampling for hugepage-aware tiered memory management.
 */
#include <linux/kthread.h>
#include <linux/memcontrol.h>
#include <linux/mempolicy.h>
#include <linux/sched.h>
//有这个头文件应该就能知道代表算力的宏定义
#include <linux/perf_event.h>
#include <linux/delay.h>
#include <linux/sched/cputime.h>

#include "../kernel/events/internal.h"

#include <linux/htmm.h>

struct task_struct *access_sampling = NULL;
struct perf_event ***mem_event;

static __u64 get_pebs_event(enum events e)
{//edit by 100, 带宽的收集包括两者的读取量，还有写入量。
    switch (e) { //但是这些定义和我看到的不太一样啊
	case DRAMREAD:
	    return DRAM_LLC_LOAD_MISS;
	case MEMWRITE:
	    return ALL_STORES;
	case CPU_CYCLES:
		return PERF_COUNT_HW_CPU_CYCLES;
	case INSTRUCTIONS:
		return PERF_COUNT_HW_INSTRUCTIONS;
	default:
	    return N_HTMMEVENTS;
    }
}

static int __perf_event_open(__u64 config, __u64 config1, __u64 cpu,
	__u64 type, __u32 pid)
{ //再看看采样模板，不希望记录上下文数据
    struct perf_event_attr attr; // 函数需要的结构体，告诉这个文件描述符该怎么创建，因为采样不同的事件最后传回的perf_event结构体也不一样。
    struct file *file; // 已打开的文件在内核中用file结构体表示，文件描述符表中的指针指向file结构体。
    int event_fd, __pid; // 我要接收的文件句柄

    memset(&attr, 0, sizeof(struct perf_event_attr));

	if(config != PERF_COUNT_HW_CPU_CYCLES && config != PERF_COUNT_HW_INSTRUCTIONS){
		// 要检测的类型有硬件和自定义类，因为原本的自定义类型表现的很好所以保留
		attr.type = PERF_TYPE_RAW; 
	}else{
		attr.type = PERF_TYPE_HARDWARE; 
	}
    attr.size = sizeof(struct perf_event_attr);
    attr.config = config; //要监测的采样事件
    attr.config1 = config1;

	attr.sample_period = 100007;
	// 采样事件间隔，这里将其设计为一个固定的值，其实就是把htmm_inst_sample_period的值拿出来了而已

    attr.sample_type = PERF_SAMPLE_IP; //要采样的信息是什么，IP就是对应的值，TID先保留看需不需要过滤线程， | PERF_SAMPLE_TID
    attr.disabled = 0;
    attr.exclude_kernel = 1;
    attr.exclude_hv = 1;
    attr.exclude_callchain_kernel = 1;
    attr.exclude_callchain_user = 1;
    attr.precise_ip = 1;
    attr.enable_on_exec = 1;

    if (pid == 0)
		__pid = -1;
    else
		__pid = pid;
	
    event_fd = htmm__perf_event_open(&attr, __pid, cpu, -1, 0);
    if (event_fd <= 0) {
		printk("[error htmm__perf_event_open failure] event_fd: %d\n", event_fd);
		return -1;
    }

    file = fget(event_fd);
    if (!file) {
		printk("invalid file\n");
		return -1;
    }
    mem_event[cpu][type] = fget(event_fd)->private_data;
    return 0;
}

static int pebs_init(pid_t pid, int node)
{
    int cpu, event;

    mem_event = kzalloc(sizeof(struct perf_event **) * CPUS_PER_SOCKET, GFP_KERNEL);
    for (cpu = 0; cpu < CPUS_PER_SOCKET; cpu++) {
		mem_event[cpu] = kzalloc(sizeof(struct perf_event *) * N_HTMMEVENTS, GFP_KERNEL);
    }
    
    for (cpu = 0; cpu < CPUS_PER_SOCKET; cpu++) {
	for (event = 0; event < N_HTMMEVENTS; event++) { 
	/*要先确定有多少个cpu核心，还要确定哪些事件是根据cpu核心来确定的
	在设计的Q-Table表里，应该都是根据CPU核心来确定的
	ubuntu20那台是旧服务器12个核心一个socket，而新电脑ubuntu18那台是28个核心
	都能跑的原因应该是运行脚本里写了限制task数目来着。*/
	    if (get_pebs_event(event) == N_HTMMEVENTS) {
			mem_event[cpu][event] = NULL;
			continue;
	    }

	    if (__perf_event_open(get_pebs_event(event), 0, cpu, event, pid))
			return -1;
	    if (htmm__perf_event_init(mem_event[cpu][event], BUFFER_SIZE))
			return -1;
	}
    }

    return 0;
}

static void pebs_disable(void)
{
    int cpu, event;


    for (cpu = 0; cpu < CPUS_PER_SOCKET; cpu++) {
	for (event = 0; event < N_HTMMEVENTS; event++) {
	    if (mem_event[cpu][event])
			perf_event_disable(mem_event[cpu][event]);
	}
    }
}

unsigned long long nr_bw = 0, nr_cyc = 0, nr_ins = 0;
static int ksamplingd(void *data)
{
	//edit by 100, 计算
    unsigned long long nr_throttled = 0, nr_lost = 0, nr_unknown = 0;
	unsigned long sleep_timeout;
	sleep_timeout = usecs_to_jiffies(2000);

    const struct cpumask *cpumask = cpumask_of_node(0);
    if (!cpumask_empty(cpumask))
		do_set_cpus_allowed(access_sampling, cpumask);

    while (!kthread_should_stop()) {
		int cpu, event, cond = false;
    
		if (htmm_mode == HTMM_NO_MIG) {
			msleep_interruptible(10000);
			continue;
		}
	
		for (cpu = 0; cpu < CPUS_PER_SOCKET; cpu++) {
			for (event = 0; event < N_HTMMEVENTS; event++) {
				//处理某个cpu的某个事件的采样缓冲区数据
				do {
					struct perf_buffer *rb;
					struct perf_event_mmap_page *up;
					struct perf_event_header *ph;
					struct htmm_event *he;
					unsigned long pg_index, offset;
					int page_shift;
					__u64 head;
	
					if (!mem_event[cpu][event]) {
						//continue;这个是个二维指针，指向的就是之前采样采的fd->private,一个ring buff
						break;
					}

					__sync_synchronize(); // 进行内存屏障操作，确保之前的内存操作完成。

					rb = mem_event[cpu][event]->rb;
					if (!rb) {
						printk("event->rb is NULL\n");
						return -1;
					}
					/* perf_buffer is ring buffer */
					up = READ_ONCE(rb->user_page);
					head = READ_ONCE(up->data_head);
					if (head == up->data_tail) { // 检查环形缓冲区是否有新数据（即头尾指针不相等）
						// if (cpu < 16) //为啥是16啊？
						// 	nr_skip++;
						//continue;
						break;
					}

					/* 计算环形缓冲区中未处理的数据量（head - data_tail），
					并根据这个差值与预设的最大和最小采样率比较，设置条件变量 cond*/
					head -= up->data_tail;
					if (head > (BUFFER_SIZE * ksampled_max_sample_ratio / 100)) {
						cond = true;
					} else if (head < (BUFFER_SIZE * ksampled_min_sample_ratio / 100)) {
						cond = false;
					}

					/* read barrier 确保之前的读操作被正确执行和完成，以防止编译器或处理器重排序。*/
					smp_rmb();

					/*根据环形缓冲区的页面序号（page_order）和偏移量，
					计算出当前需要处理的采样数据的具体内存位置。*/
					page_shift = PAGE_SHIFT + page_order(rb);
					/* get address of a tail sample */
					offset = READ_ONCE(up->data_tail);
					pg_index = (offset >> page_shift) & (rb->nr_pages - 1);
					offset &= (1 << page_shift) - 1;

					/*处理采样数据：根据采样数据头（ph->type）的类型，执行不同的操作。
					这可能包括更新页面信息、统计不同类型事件的发生次数
					（如DRAM读取、NVM读取或写入操作）、记录节流或丢失的样本等。*/
					ph = (void*)(rb->data_pages[pg_index] + offset);
					switch (ph->type) {
						// 性能采样数据。应该一般都是这个状态，我看模板也是从这里拿的
						case PERF_RECORD_SAMPLE:
							he = (struct htmm_event *)ph;

							if (event == DRAMREAD || event == MEMWRITE) {
								nr_bw += he->ip;
							}
							else if (event == CPU_CYCLES) {
								nr_cyc += he->ip;;
							}
							else if(event == INSTRUCTIONS){
								nr_ins += he->ip;
							}
								
							break;
						// 节流（减少数据采集频率）
						case PERF_RECORD_THROTTLE:
						// 解除节流（增加数据采集频率）的记录，统计这些事件的发生次数。
						case PERF_RECORD_UNTHROTTLE:
							nr_throttled++;
							break;
						//处理丢失的样本事件
						case PERF_RECORD_LOST_SAMPLES:
							nr_lost ++;
							break;
						default:
						//处理未知类型事件
							nr_unknown++;
							break;
					}
					update_stats(nr_bw, nr_cyc, nr_ins);
					nr_bw = 0;
					nr_cyc = 0;
					nr_ins = 0;
					/* read, write barrier 确保所有先前的写操作完成，然后更新环形缓冲区的 data_tail，以指向下一个待处理的采样数据的位置。*/
					smp_mb();
					WRITE_ONCE(up->data_tail, up->data_tail + ph->size);
				} while (cond);
			}
		}	
		/* if ksampled_soft_cpu_quota is zero, disable dynamic pebs feature */
		if (!ksampled_soft_cpu_quota)
			continue;

		/* sleep 这确实是while中线程需要循环做的事情，
		然后每次执行后需要休眠如果原本的定义不行可以采用msleep_interruptible(2000);*/
		schedule_timeout_interruptible(sleep_timeout);
    } 

    return 0;
}

static int ksamplingd_run(void)
{
    int err = 0;
    
    if (!access_sampling) {
		access_sampling = kthread_run(ksamplingd, NULL, "ksamplingd");
		if (IS_ERR(access_sampling)) {
	    	err = PTR_ERR(access_sampling);
	    	access_sampling = NULL;
		}
    }
    return err;
}

int ksamplingd_init(pid_t pid, int node)
{ //希望在调试迁移线程时不受采样线程影响
    // int ret;

    // if (access_sampling)
	// 	return 0;

    // ret = pebs_init(pid, node); //采样线程从这里就在报错了，应该就是core按照server定义的，多了
    // if (ret) {
	// 	printk("htmm__perf_event_init failure... ERROR:%d", ret);
	// 	return 0;
    // }

    // return ksamplingd_run();
	return 0;
}

void ksamplingd_exit(void)
{
    // if (access_sampling) {
	// 	kthread_stop(access_sampling);
	// 	access_sampling = NULL;
    // }
    // pebs_disable();
}
