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

	// attr.sample_period = 19997;
	if (config == ALL_STORES)
		attr.sample_period = htmm_inst_sample_period;
    else
		attr.sample_period = get_sample_period(0);

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

/* 这里才是每个cpu一个去采样，采样线程在收集数据 */
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

		printk("the pid we sample: %d", pid);

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

static void pebs_update_period(uint64_t value, uint64_t inst_value)
{
    int cpu, event;

    for (cpu = 0; cpu < CPUS_PER_SOCKET; cpu++) {
	for (event = 0; event < N_HTMMEVENTS; event++) {
	    int ret;
	    if (!mem_event[cpu][event])
		continue;

	    switch (event) {
		case DRAMREAD:
		case NVMREAD:
		case CXLREAD:
		    ret = perf_event_period(mem_event[cpu][event], value);
		    break;
		case MEMWRITE:
		    ret = perf_event_period(mem_event[cpu][event], inst_value);
		    break;
		default:
		    ret = 0;
		    break;
	    }

	    if (ret == -EINVAL)
		printk("failed to update sample period");
	}
    }
}

//原子变量
atomic_t hit_dram = ATOMIC_INIT(0);
atomic_t hit_pm = ATOMIC_INIT(0);
atomic_t hit_other = ATOMIC_INIT(0);

static int ksamplingd(void *data)
{
	unsigned long sleep_timeout;
	/* used for calculating average cpu usage of ksampled */
    struct task_struct *t = current; // 通过 current 宏获取到的 task_struct 结构体指针代表了当前正在执行的进程
    /* a unit of cputime: permil (1/1000) */
    u64 total_runtime, exec_runtime, cputime = 0;
    unsigned long total_cputime, elapsed_cputime, cur;
    /* used for periodic checks*/
    unsigned long cpucap_period = msecs_to_jiffies(15000); // 15s
    unsigned long sample_period = 0;
    unsigned long sample_inst_period = 0;
    /* report cpu/period stat */
    unsigned long trace_cputime, trace_period = msecs_to_jiffies(1500); // 3s
    unsigned long trace_runtime;

	const struct cpumask *cpumask;

	 /* orig impl: see read_sum_exec_runtime() */
    trace_runtime = total_runtime = exec_runtime = t->se.sum_exec_runtime;

    trace_cputime = total_cputime = elapsed_cputime = jiffies;
    sleep_timeout = usecs_to_jiffies(2000); //毫秒和秒是1000

	 /* Currently uses a single CPU node(0) 就是说这个采样指针只允许cpu0的去使用, 进程access_sampling将只能在cpumask中指定的CPU上运行 */
    cpumask = cpumask_of_node(0);
    if (!cpumask_empty(cpumask))
		do_set_cpus_allowed(access_sampling, cpumask);

	LIST_HEAD(fast_list);
	pg_data_t *pgdat = NODE_DATA(0);
	struct mem_cgroup_per_node *pn = next_memcg_cand(pgdat); 
	struct mem_cgroup *memcg = pn->memcg;
	struct lruvec *lruvec = mem_cgroup_lruvec(memcg, pgdat);
	struct list_head *src = &lruvec->lists[LRU_ACTIVE_ANON];

    while (!kthread_should_stop()) {
		int cpu, event, cond = false;
    
		if (htmm_mode == HTMM_NO_MIG) {
			printk("HTMM_NO_MIG waiting for sample");
			msleep_interruptible(10000);
			continue;
		}
	
		// trace_printk("--------for double--------");
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

							update_pginfo(he->pid, he->addr, event, &fast_list);
							//TODO：这里打印确定是不是函数内部返回的就不是同一个参数。
							// printk("next_hit_dram %d", atomic_read(&next_hit_dram));
							// printk("next_hit_pm %d", atomic_read(&next_hit_pm));
			    			
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
							printk("what means default");
			    			break;
		    		}
		    		/* read, write barrier */
		    		smp_mb();
		    		WRITE_ONCE(up->data_tail, up->data_tail + ph->size);
				} while (cond);	
	    	}
		}	
		
		/* if ksampled_soft_cpu_quota is zero, disable dynamic pebs feature 这里应该是cpu消耗是0就不休眠*/

		// if (!ksampled_soft_cpu_quota)
	    // continue;

		/* check elasped time */
		cur = jiffies; // 是Linux内核中用于表示时间的基本单位
		if ((cur - elapsed_cputime) >= cpucap_period) { // 计算了两次采样之间的时间差，如果这个时间差超过了cpucap_perio 15s
	    	u64 cur_runtime = t->se.sum_exec_runtime;
	    	exec_runtime = cur_runtime - exec_runtime; //ns 得到上一轮采样的执行时间
	    	elapsed_cputime = jiffies_to_usecs(cur - elapsed_cputime); //us 两次采样时间差
	    	if (!cputime) { //如果是第一次计算CPU的使用率
				// 将两次采样之间的执行时间除以两次采样之间的经过时间，计算出 CPU 使用率的初步值。这个值表示在两次采样之间，CPU 的平均使用率。
				u64 cur_cputime = div64_u64(exec_runtime, elapsed_cputime); 
				// EMA with the scale factor (0.2) 使用指数移动平均值（EMA）算法，以一个固定的比例（这里是 0.2）将当前的 CPU 使用率和之前的 CPU 使用率进行加权平均。这样可以平滑地计算 CPU 使用率，并减少抖动。
				cputime = ((cur_cputime << 3) + (cputime << 1)) / 10;
	    	} else
				cputime = div64_u64(exec_runtime, elapsed_cputime);

	    	/* to prevent frequent updates, allow for a slight variation of +/- 0.5% */
	    	if (cputime > (ksampled_soft_cpu_quota + 5) && sample_period != pcount) {
				// 它检查 CPU 使用率是否超过了预设的软 CPU 配额（ksampled_soft_cpu_quota）加上 5，即是否超过了软配额的 0.5%。如果是，并且当前的采样周期不等于 pcount，则需要增加采样周期
				/* only increase by 1 */
				unsigned long tmp1 = sample_period, tmp2 = sample_inst_period;
				increase_sample_period(&sample_period, &sample_inst_period);
				if (tmp1 != sample_period || tmp2 != sample_inst_period)
					pebs_update_period(get_sample_period(sample_period),get_sample_inst_period(sample_inst_period));
			} else if (cputime < (ksampled_soft_cpu_quota - 5) && sample_period) {
				unsigned long tmp1 = sample_period, tmp2 = sample_inst_period;
				decrease_sample_period(&sample_period, &sample_inst_period);
				if (tmp1 != sample_period || tmp2 != sample_inst_period)
						pebs_update_period(get_sample_period(sample_period),get_sample_inst_period(sample_inst_period));
			}
			/* does it need to prevent ping-pong behavior? */
			elapsed_cputime = cur;
			exec_runtime = cur_runtime;
		}

		fast_promote(src, &fast_list, pgdat);

		/* sleep 这确实是while中线程需要循环做的事情，
		然后每次执行后需要休眠如果原本的定义不行可以采用msleep_interruptible(2000);*/
		schedule_timeout_interruptible(sleep_timeout);
    }


    total_runtime = (t->se.sum_exec_runtime) - total_runtime; // ns
    total_cputime = jiffies_to_usecs(jiffies - total_cputime); // us

    printk("total runtime: %llu ns, total cputime: %lu us, cpu usage: %llu\n", total_runtime, total_cputime, (total_runtime) / total_cputime);

    return 0;
}

static int ksamplingd_run(void)
{
    int err = 0;
    
    if (!access_sampling) {
		access_sampling = kthread_run(ksamplingd, NULL, "ksamplingd"); //这指针就是一个cpu一个了
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

    ret = pebs_init(pid, node); //这里每个cpu已经在采样了
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
