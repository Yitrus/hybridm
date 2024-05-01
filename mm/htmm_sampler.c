/*
 * memory access sampling for hugepage-aware tiered memory management.
 */
#include <linux/kthread.h>
#include <linux/memcontrol.h>
#include <linux/mempolicy.h>
#include <linux/sched.h>
#include <linux/perf_event.h>
#include <linux/delay.h>
#include <linux/sched/cputime.h>

#include "../kernel/events/internal.h"

#include <linux/htmm.h>

struct task_struct *access_sampling = NULL;
struct perf_event ***mem_event;

static bool valid_va(unsigned long addr)
{
    if (!(addr >> (PGDIR_SHIFT + 9)) && addr != 0)
	return true;
    else
	return false;
}

static __u64 get_pebs_event(enum events e)
{ //主要是这些peb事件还不能理解; 因为原来这个效果不错，应该啊保留也没问题
    switch (e) { 
	// case LLC_MISS_PERF:
	// 	return PERF_COUNT_HW_CACHE_MISSES; 
	// case LLC_MISS:
	// 	return LLC_MISS_DIF1;
	
	case DRAMREAD:
	    return DRAM_LLC_LOAD_MISS;
	case NVMREAD:
	    if (!htmm_cxl_mode)
		return NVM_LLC_LOAD_MISS;
	    else
		return N_HTMMEVENTS;
	case MEMWRITE:
	    return ALL_STORES;
	case CXLREAD:
	    if (htmm_cxl_mode)
		return REMOTE_DRAM_LLC_LOAD_MISS;
	    else
		return N_HTMMEVENTS;
	default:
	    return N_HTMMEVENTS;
    }
}

static int __perf_event_open(__u64 config, __u64 config1, __u64 cpu,
	__u64 type, __u32 pid)
{ 
    struct perf_event_attr attr; 
    struct file *file; 
    int event_fd, __pid;

    memset(&attr, 0, sizeof(struct perf_event_attr));

	if(config == PERF_COUNT_HW_CACHE_MISSES){
		attr.type = PERF_TYPE_HARDWARE;
	}else{
		attr.type = PERF_TYPE_RAW;
	}
	
	// attr.type = PERF_TYPE_RAW;

    attr.size = sizeof(struct perf_event_attr);
    attr.config = config; //要监测的采样事件
    attr.config1 = config1;

	attr.sample_period = 19997;

    attr.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_TID | PERF_SAMPLE_ADDR;
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
	    if (get_pebs_event(event) == N_HTMMEVENTS) {
			mem_event[cpu][event] = NULL;
			continue;
	    }
		
	    if (__perf_event_open(get_pebs_event(event), 0, cpu, event, pid)){
			printk("pebs_init __perf_event_open failed %d event", event);
			return -1;
		}
	    if (htmm__perf_event_init(mem_event[cpu][event], BUFFER_SIZE)){
			printk("pebs_init htmm__perf_event_init failed %d event cpu %d", event, cpu);
			return -1;
		}
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
		// printk("-----------pebs disable one-----------");
	}
    }
	printk("-----------pebs disable finish-----------");
}

unsigned int hit_ratio = 0;
// unsigned long long hit_total = 0;
unsigned long hit_dram = 0;
unsigned long hit_pm = 0;
unsigned long hit_other = 0;
unsigned long next_hit_dram = 0;
unsigned long next_hit_pm = 0;
static int ksamplingd(void *data)
{
	unsigned long sleep_timeout;

	sleep_timeout = msecs_to_jiffies(15000); //毫秒和秒是1000
	
    // const struct cpumask *cpumask = cpumask_of_node(0);
    // if (!cpumask_empty(cpumask))
	// 	do_set_cpus_allowed(access_sampling, cpumask);

    while (!kthread_should_stop()) {
		int cpu, event, cond = false;
    
		if (htmm_mode == HTMM_NO_MIG) {
			msleep_interruptible(10000);
			continue;
		}
	
		next_hit_dram = 0;
		next_hit_pm = 0;
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
						printk("event->rb is NULL ");
						return -1;
					}
					/* perf_buffer is ring buffer */
					up = READ_ONCE(rb->user_page);
					head = READ_ONCE(up->data_head);
					if (head == up->data_tail) { // 检查环形缓冲区是否有新数据（即头尾指针不相等）
						// if (cpu < 16) //为啥是16啊？
						// 	nr_skip++;
						// continue;
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
							 if (!valid_va(he->addr)) {
								break;
			    			}

							update_pginfo(he->pid, he->addr, event);
			    			
			    			break;
						case PERF_RECORD_THROTTLE:
							printk("one sample case PERF_RECORD_THROTTLE");
							break;
						case PERF_RECORD_UNTHROTTLE:
							printk("one sample case PERF_RECORD_UNTHROTTLE");
							break;
						case PERF_RECORD_LOST_SAMPLES:
							printk("one sample case PERF_RECORD_SAMPLES");
							break;
						default:
			    			break;
		    		}
		    		/* read, write barrier */
		    		smp_mb();
		    		WRITE_ONCE(up->data_tail, up->data_tail + ph->size);
				} while (cond);
	    	}
		}	

		
		hit_dram = next_hit_dram;
		hit_pm = next_hit_pm;
		if(hit_dram == 0){
			hit_ratio = 0;
		}else if(hit_pm == 0){
			hit_ratio = 100;
		}else{
			hit_ratio = (hit_dram*100 / (hit_dram + hit_pm));
		}
		
		/* if ksampled_soft_cpu_quota is zero, disable dynamic pebs feature */
		// if (!ksampled_soft_cpu_quota)
		// 	continue;

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
		}else{
			printk("------------ksamplingd run normally------------");
		}
    }
    return err;
}

int ksamplingd_init(pid_t pid, int node)
{ 
    int ret;

    if (access_sampling){
		// 初始化时还有，不是原来的return 0，而是把之前那个关掉
		kthread_stop(access_sampling);
		access_sampling = NULL;
		pebs_disable();
		printk("last time ksamplingd exit normally");
	}

    ret = pebs_init(pid, node); //采样线程从这里就在报错了，应该就是core按照server定义的，多了
    if (ret) {
		printk("htmm__perf_event_init failure... ERROR:%d", ret);
		return 0;
    }

	printk("peb init ok then run ksamplingd");
    return ksamplingd_run();
}

void ksamplingd_exit(void)
{
    if (access_sampling) { //初始化时为NULL
		kthread_stop(access_sampling);
		printk("------------ksamplingd exit normally------------");
		access_sampling = NULL;
    }
    pebs_disable();
}
