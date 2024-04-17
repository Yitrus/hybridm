#include <uapi/linux/perf_event.h>
#include <linux/mmzone.h>
#include <asm/pgtable.h>


#define DEFERRED_SPLIT_ISOLATED 1

#define BUFFER_SIZE	32 /* 128: 1MB */
//edit by 100, ubuntu20那台是旧服务器12个核心一个socket，而新电脑ubuntu18那台是28个核心
#define CPUS_PER_SOCKET 24
#define MAX_MIGRATION_RATE_IN_MBPS  2048 /* 2048MB per sec */

/* 定义判断地址的常量 16进制*/
#define DRAM_ADDR_END 0x207fffffff
#define PM_ADDR_START 0x2080000000
#define PM_ADDR_END 0x11c7fffffff

/* pebs events */
#define DRAM_LLC_LOAD_MISS  0x1d3
#define REMOTE_DRAM_LLC_LOAD_MISS   0x2d3
#define NVM_LLC_LOAD_MISS   0x80d1
#define ALL_STORES	    0x82d0
#define ALL_LOADS	    0x81d0
#define STLB_MISS_STORES    0x12d0
#define STLB_MISS_LOADS	    0x11d0

#define LLC_MISS_DIF1 0x41
#define LLC_MISS_DIF2 0x0a
#define LLC_MISS_DIF3 0x0b

/* tmm option */
#define HTMM_NO_MIG	    0x0	/* unused */
#define	HTMM_BASELINE	    0x1 /* unused */
#define HTMM_HUGEPAGE_OPT   0x2 /* only used */
#define HTMM_HUGEPAGE_OPT_V2	0x3 /* unused */

/**/
#define DRAM_ACCESS_LATENCY 80
#define NVM_ACCESS_LATENCY  270
#define CXL_ACCESS_LATENCY  170
#define DELTA_CYCLES	(NVM_ACCESS_LATENCY - DRAM_ACCESS_LATENCY)

#define pcount 30
/* only prime numbers */
static const unsigned int pebs_period_list[pcount] = {
    199,    // 200 - min
    293,    // 300
    401,    // 400
    499,    // 500
    599,    // 600
    701,    // 700
    797,    // 800
    907,    // 900
    997,    // 1000
    1201,   // 1200
    1399,   // 1400
    1601,   // 1600
    1801,   // 1800
    1999,   // 2000
    2503,   // 2500
    3001,   // 3000
    3499,   // 3500
    4001,   // 4000
    4507,   // 4507
    4999,   // 5000
    6007,   // 6000
    7001,   // 7000
    7993,   // 8000
    9001,   // 9000
    10007,  // 10000
    12007,  // 12000
    13999,  // 14000
    16001,  // 16000
    17989,  // 18000
    19997,  // 20000 - max
};

#define pinstcount 5
/* this is for store instructions */
static const unsigned int pebs_inst_period_list[pinstcount] ={
    100003, // 0.1M
    300007, // 0.3M
    600011, // 0.6M
    1000003,// 1.0M
    1500003,// 1.5M
};

struct htmm_event {
    struct perf_event_header header;
    __u64 ip;
    __u32 pid, tid;
    __u64 addr;
};

enum events {
    // DRAMREAD = 0,
    // NVMREAD = 1,
    MEMWRITE = 0,
    // TLB_MISS_LOADS = 3,
    // TLB_MISS_STORES = 4,
    LLC_MISS_PERF = 1,
    LLC_MISS = 2,
    // CXLREAD = 5, // emulated by remote DRAM node
    N_HTMMEVENTS
};

/* htmm_core.c */
extern void htmm_mm_init(struct mm_struct *mm);
extern void htmm_mm_exit(struct mm_struct *mm);
extern void __prep_transhuge_page_for_htmm(struct mm_struct *mm, struct page *page);
extern void prep_transhuge_page_for_htmm(struct vm_area_struct *vma,
					 struct page *page);
extern void clear_transhuge_pginfo(struct page *page);
extern void copy_transhuge_pginfo(struct page *page,
				  struct page *newpage);
extern pginfo_t *get_compound_pginfo(struct page *page, unsigned long address);

extern void set_lru_adjusting(struct mem_cgroup *memcg, bool inc_thres);

extern void update_pginfo(pid_t pid, unsigned long address, enum events e);

extern void move_page_to_active_lru(struct page *page);
extern void move_page_to_inactive_lru(struct page *page);

extern struct page *get_meta_page(struct page *page);
extern unsigned int get_accesses_from_idx(unsigned int idx);
extern unsigned int get_idx(unsigned long num);
extern void uncharge_htmm_pte(pte_t *pte, struct mem_cgroup *memcg);
extern void uncharge_htmm_page(struct page *page, struct mem_cgroup *memcg);
extern void charge_htmm_page(struct page *page, struct mem_cgroup *memcg);

extern int ksamplingd_init(pid_t pid, int node);
extern void ksamplingd_exit(void);
extern unsigned int hit_ratio;
extern unsigned long hit_dram;
extern unsigned long hit_pm;
extern unsigned long hit_other;
// extern unsigned long long hit_total;
extern unsigned long next_hit_dram;
extern unsigned long next_hit_pm;

/* htmm_migrater.c */
#define HTMM_MIN_FREE_PAGES 256 * 10 // 10MB
extern unsigned long get_nr_lru_pages_node(struct mem_cgroup *memcg, pg_data_t *pgdat);
extern void add_memcg_to_kmigraterd(struct mem_cgroup *memcg, int nid);
extern void del_memcg_from_kmigraterd(struct mem_cgroup *memcg, int nid);
extern unsigned long get_memcg_demotion_watermark(unsigned long max_nr_pages);
extern unsigned long get_memcg_promotion_watermark(unsigned long max_nr_pages);
extern void kmigraterd_wakeup(int nid);
extern int kmigraterd_init(void);
extern void kmigraterd_stop(void);
extern unsigned int nr_action;

/*qtable.c*/
// extern void get_best_action(unsigned int *nr_action);

