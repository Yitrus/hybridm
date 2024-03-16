/*
 * -- kmigrated 
 */
#include <linux/kthread.h>
#include <linux/list.h>
#include <linux/memcontrol.h>
#include <linux/mempolicy.h>
#include <linux/mmzone.h>
#include <linux/mm_inline.h>
#include <linux/migrate.h>
#include <linux/swap.h>
#include <linux/rmap.h>
#include <linux/delay.h>
#include <linux/node.h>
#include <linux/htmm.h>
#include <linux/wait.h>
#include <linux/sched.h>

#include "internal.h"

#define MIN_WATERMARK_LOWER_LIMIT   128 * 100 // 50MB
#define MIN_WATERMARK_UPPER_LIMIT   2560 * 100 // 1000MB
#define MAX_WATERMARK_LOWER_LIMIT   256 * 100 // 100MB
#define MAX_WATERMARK_UPPER_LIMIT   3840 * 100 // 1500MB

#ifdef ARCH_HAS_PREFETCHW
#define prefetchw_prev_lru_page(_page, _base, _field)                   \
	do {                                                            \
		if ((_page)->lru.prev != _base) {                       \
			struct page *prev;				\
			prev = lru_to_page(&(_page->lru));		\
			prefetchw(&prev->_field);			\
		}                                                       \
	} while (0)
#else
#define prefetchw_prev_lru_page(_page, _base, _field) do { } while (0)
#endif

unsigned int nr_action;

void add_memcg_to_kmigraterd(struct mem_cgroup *memcg, int nid)
{
    struct mem_cgroup_per_node *mz, *pn = memcg->nodeinfo[nid];
    pg_data_t *pgdat = NODE_DATA(nid);

    if (!pgdat)
	return;
    
    if (pn->memcg != memcg)
	printk("memcg mismatch!\n");

    spin_lock(&pgdat->kmigraterd_lock);
    list_for_each_entry(mz, &pgdat->kmigraterd_head, kmigraterd_list) {
	if (mz == pn)
	    goto add_unlock;
    }
    list_add_tail(&pn->kmigraterd_list, &pgdat->kmigraterd_head);
add_unlock:
    spin_unlock(&pgdat->kmigraterd_lock);
}

void del_memcg_from_kmigraterd(struct mem_cgroup *memcg, int nid)
{
    struct mem_cgroup_per_node *mz, *pn = memcg->nodeinfo[nid];
    pg_data_t *pgdat = NODE_DATA(nid);
    
    if (!pgdat)
	return;

    spin_lock(&pgdat->kmigraterd_lock);
    list_for_each_entry(mz, &pgdat->kmigraterd_head, kmigraterd_list) {
	if (mz == pn) {
	    list_del(&pn->kmigraterd_list);
	    break;
	}
    }
    spin_unlock(&pgdat->kmigraterd_lock);
}

unsigned long get_memcg_demotion_watermark(unsigned long max_nr_pages)
{
    max_nr_pages = max_nr_pages * 2 / 100; // 2%
    if (max_nr_pages < MIN_WATERMARK_LOWER_LIMIT)
	return MIN_WATERMARK_LOWER_LIMIT;
    else if (max_nr_pages > MIN_WATERMARK_UPPER_LIMIT)
	return MIN_WATERMARK_UPPER_LIMIT;
    else
	return max_nr_pages;
}

unsigned long get_memcg_promotion_watermark(unsigned long max_nr_pages)
{
    max_nr_pages = max_nr_pages * 3 / 100; // 3%
    if (max_nr_pages < MAX_WATERMARK_LOWER_LIMIT)
	return MIN_WATERMARK_LOWER_LIMIT;
    else if (max_nr_pages > MAX_WATERMARK_UPPER_LIMIT)
	return MIN_WATERMARK_UPPER_LIMIT;
    else
	return max_nr_pages;
}

unsigned long get_nr_lru_pages_node(struct mem_cgroup *memcg, pg_data_t *pgdat)
{
    struct lruvec *lruvec;
    unsigned long nr_pages = 0;
    enum lru_list lru;

    lruvec = mem_cgroup_lruvec(memcg, pgdat);

    for_each_lru(lru)
	nr_pages += lruvec_lru_size(lruvec, lru, MAX_NR_ZONES);
   
    return nr_pages;
}

static unsigned long need_lowertier_promotion(pg_data_t *pgdat, struct mem_cgroup *memcg)
{// edit by 100, 因为之前已经确定了能迁移多少页面到上级，原本的逻辑是上层剩多少迁移多少
// 现在虽然我们执行action是希望迁移的，不过按理说应该也是充分利用，DRAM别空着啊。
    struct lruvec *lruvec;
    unsigned long lruvec_size;

    lruvec = mem_cgroup_lruvec(memcg, pgdat);
    lruvec_size = lruvec_lru_size(lruvec, LRU_ACTIVE_ANON, MAX_NR_ZONES);
    
    if (htmm_mode == HTMM_NO_MIG)
	return 0;

    return lruvec_size;
}


static bool need_toptier_demotion(pg_data_t *pgdat, struct mem_cgroup *memcg, unsigned long *nr_exceeded)
{// edit by 100, 这时的降级，判断会不会超过最大可降级的情况
    unsigned long nr_lru_pages;
    // unsigned long fasttier_max_watermark, fasttier_min_watermark;
    int target_nid = htmm_cxl_mode ? 1 : next_demotion_node(pgdat->node_id);
    pg_data_t *target_pgdat;
  
    if (target_nid == NUMA_NO_NODE)
		return false;

    target_pgdat = NODE_DATA(target_nid);

    nr_lru_pages = get_nr_lru_pages_node(memcg, pgdat);

	if (nr_lru_pages < *nr_exceeded)
	    *nr_exceeded = nr_lru_pages - 1U * 128 * 100;

	return true;

}

static unsigned long node_free_pages(pg_data_t *pgdat)
{
    int z;
    long free_pages;
    long total = 0;

    for (z = pgdat->nr_zones - 1; z >= 0; z--) {
	struct zone *zone = pgdat->node_zones + z;
	long nr_high_wmark_pages;

	if (!populated_zone(zone))
	    continue;

	free_pages = zone_page_state(zone, NR_FREE_PAGES);
	free_pages -= zone->nr_reserved_highatomic;
	free_pages -= zone->lowmem_reserve[ZONE_MOVABLE];

	nr_high_wmark_pages = high_wmark_pages(zone);
	if (free_pages >= nr_high_wmark_pages)
	    total += (free_pages - nr_high_wmark_pages);
    }
    return (unsigned long)total;
}

static bool promotion_available(int target_nid, struct mem_cgroup *memcg,
	unsigned long long *nr_to_promote)
{
    pg_data_t *pgdat;
    unsigned long max_nr_pages, cur_nr_pages;
    unsigned long nr_isolated;
    unsigned long fasttier_max_watermark;

    if (target_nid == NUMA_NO_NODE)
	return false;
    
    pgdat = NODE_DATA(target_nid);

    cur_nr_pages = get_nr_lru_pages_node(memcg, pgdat);
    max_nr_pages = memcg->nodeinfo[target_nid]->max_nr_base_pages;
    nr_isolated = node_page_state(pgdat, NR_ISOLATED_ANON) +
		  node_page_state(pgdat, NR_ISOLATED_FILE);
    
    fasttier_max_watermark = get_memcg_promotion_watermark(max_nr_pages);

    if (max_nr_pages == ULONG_MAX) {
	*nr_to_promote = node_free_pages(pgdat);
	return true;
    }
    else if (cur_nr_pages + nr_isolated < max_nr_pages - fasttier_max_watermark) {
	*nr_to_promote = max_nr_pages - fasttier_max_watermark - cur_nr_pages - nr_isolated;
	return true;
    }
    return false;
}

// static bool need_lru_adjusting(struct mem_cgroup_per_node *pn)
// {
//     return READ_ONCE(pn->need_adjusting);
// }

static __always_inline void update_lru_sizes(struct lruvec *lruvec,
	enum lru_list lru, unsigned long *nr_zone_taken)
{
    int zid;

    for (zid = 0; zid < MAX_NR_ZONES; zid++) {
	if (!nr_zone_taken[zid])
	    continue;

	update_lru_size(lruvec, lru, zid, -nr_zone_taken[zid]);
    }
}

static unsigned long isolate_lru_pages(unsigned long nr_to_scan,
	struct lruvec *lruvec, enum lru_list lru, struct list_head *dst,
	isolate_mode_t mode)
{
    struct list_head *src = &lruvec->lists[lru];
    unsigned long nr_zone_taken[MAX_NR_ZONES] = { 0 };
    unsigned long scan = 0, nr_taken = 0;
    LIST_HEAD(busy_list);

    while (scan < nr_to_scan && !list_empty(src)) {
	struct page *page;
	unsigned long nr_pages;

	page = lru_to_page(src);
	prefetchw_prev_lru_page(page, src, flags);
	VM_WARN_ON(!PageLRU(page));

	nr_pages = compound_nr(page);
	scan += nr_pages;

	if (!__isolate_lru_page_prepare(page, 0)) {
	    list_move(&page->lru, src);
	    continue;
	}
	if (unlikely(!get_page_unless_zero(page))) {
	    list_move(&page->lru, src);
	    continue;
	}
	if (!TestClearPageLRU(page)) {
	    put_page(page);
	    list_move(&page->lru, src);
	    continue;
	}

	nr_taken += nr_pages;
	nr_zone_taken[page_zonenum(page)] += nr_pages;
	list_move(&page->lru, dst);
    }

    update_lru_sizes(lruvec, lru, nr_zone_taken);
    return nr_taken;
}


static struct page *alloc_migrate_page(struct page *page, unsigned long node)
{
    int nid = (int) node;
    int zidx;
    struct page *newpage = NULL;
    gfp_t mask = (GFP_HIGHUSER_MOVABLE |
		  __GFP_THISNODE | __GFP_NOMEMALLOC |
		  __GFP_NORETRY | __GFP_NOWARN) &
		  ~__GFP_RECLAIM;

    if (PageHuge(page))
	return NULL;

    zidx = zone_idx(page_zone(page));
    if (is_highmem_idx(zidx) || zidx == ZONE_MOVABLE)
	mask |= __GFP_HIGHMEM;

    if (thp_migration_supported() && PageTransHuge(page)) {
	mask |= GFP_TRANSHUGE_LIGHT;
	newpage = __alloc_pages_node(nid, mask, HPAGE_PMD_ORDER);

	if (!newpage)
	    return NULL;

	prep_transhuge_page(newpage);
	__prep_transhuge_page_for_htmm(NULL, newpage);
    } else
	newpage = __alloc_pages_node(nid, mask, 0);

    return newpage;
}

static unsigned long migrate_page_list(struct list_head *migrate_list,
	pg_data_t *pgdat, bool promotion)
{
    int target_nid;
    unsigned int nr_succeeded = 0;

    if (promotion)
	target_nid = htmm_cxl_mode ? 0 : next_promotion_node(pgdat->node_id);
    else
	target_nid = htmm_cxl_mode ? 1 : next_demotion_node(pgdat->node_id);

    if (list_empty(migrate_list))
	return 0;

    if (target_nid == NUMA_NO_NODE)
	return 0;

    migrate_pages(migrate_list, alloc_migrate_page, NULL,
	    target_nid, MIGRATE_ASYNC, MR_NUMA_MISPLACED, &nr_succeeded);

    if (promotion)
	count_vm_events(HTMM_NR_PROMOTED, nr_succeeded);
    else
	count_vm_events(HTMM_NR_DEMOTED, nr_succeeded);

    return nr_succeeded;
}

static unsigned long shrink_page_list(struct list_head *page_list,
	pg_data_t* pgdat, struct mem_cgroup *memcg, bool shrink_active,
	unsigned long nr_to_reclaim)
{
    LIST_HEAD(demote_pages);
    LIST_HEAD(ret_pages);
    unsigned long nr_reclaimed = 0;
    unsigned long nr_demotion_cand = 0;

    cond_resched();

    while (!list_empty(page_list)) {
	struct page *page;
	
	page = lru_to_page(page_list);
	list_del(&page->lru);

	if (!trylock_page(page))
	    goto keep;
	if (!shrink_active && PageAnon(page) && PageActive(page))
	    goto keep_locked;
	if (unlikely(!page_evictable(page)))
	    goto keep_locked;
	if (PageWriteback(page))
	    goto keep_locked;
	if (PageTransHuge(page) && !thp_migration_supported())
	    goto keep_locked;
	if (!PageAnon(page) && nr_demotion_cand > nr_to_reclaim + HTMM_MIN_FREE_PAGES)
	    goto keep_locked;
// edit by 100, 不需要任何直方图的判断了
	// if (htmm_nowarm == 0 && PageAnon(page)) {
	    // if (PageTransHuge(page)) {
		// struct page *meta = get_meta_page(page);

		// if (meta->idx >= memcg->warm_threshold)
		    // goto keep_locked;
	    // } else {
		// unsigned int idx = get_pginfo_idx(page);

		// if (idx >= memcg->warm_threshold)
		    // goto keep_locked;
	    // }
	// }

	unlock_page(page);
	list_add(&page->lru, &demote_pages);
	nr_demotion_cand += compound_nr(page);
	continue;

keep_locked:
	unlock_page(page);
keep:
	list_add(&page->lru, &ret_pages);
    }

    nr_reclaimed = migrate_page_list(&demote_pages, pgdat, false);
    if (!list_empty(&demote_pages))
		list_splice(&demote_pages, page_list);

    list_splice(&ret_pages, page_list);
    return nr_reclaimed;
}

static unsigned long promote_page_list(struct list_head *page_list,
	pg_data_t *pgdat)
{
    LIST_HEAD(promote_pages);
    LIST_HEAD(ret_pages);
    unsigned long nr_promoted = 0;

    cond_resched();

    while (!list_empty(page_list)) {
	struct page *page;

	page = lru_to_page(page_list);
	list_del(&page->lru);
	
	if (!trylock_page(page))
	    goto __keep;
	if (!PageActive(page) && htmm_mode != HTMM_NO_MIG)
	    goto __keep_locked;
	if (unlikely(!page_evictable(page)))
	    goto __keep_locked;
	if (PageWriteback(page))
	    goto __keep_locked;
	if (PageTransHuge(page) && !thp_migration_supported())
	    goto __keep_locked;

	list_add(&page->lru, &promote_pages);
	unlock_page(page);
	continue;
__keep_locked:
	unlock_page(page);
__keep:
	list_add(&page->lru, &ret_pages);
    }

    nr_promoted = migrate_page_list(&promote_pages, pgdat, true);
    if (!list_empty(&promote_pages))
	list_splice(&promote_pages, page_list);

    list_splice(&ret_pages, page_list);
    return nr_promoted;
}

static unsigned long demote_inactive_list(unsigned long nr_to_scan,
	unsigned long nr_to_reclaim, struct lruvec *lruvec,
	enum lru_list lru, bool shrink_active)
{
    LIST_HEAD(page_list);
    pg_data_t *pgdat = lruvec_pgdat(lruvec);
    unsigned long nr_reclaimed = 0, nr_taken;
    int file = is_file_lru(lru);

    lru_add_drain();

    spin_lock_irq(&lruvec->lru_lock);
    nr_taken = isolate_lru_pages(nr_to_scan, lruvec, lru, &page_list, 0);
    __mod_node_page_state(pgdat, NR_ISOLATED_ANON + file, nr_taken);
    spin_unlock_irq(&lruvec->lru_lock);

    if (nr_taken == 0) {
	return 0;
    }

    nr_reclaimed = shrink_page_list(&page_list, pgdat, lruvec_memcg(lruvec),
	    shrink_active, nr_to_reclaim);

    spin_lock_irq(&lruvec->lru_lock);
    move_pages_to_lru(lruvec, &page_list);
    __mod_node_page_state(pgdat, NR_ISOLATED_ANON + file, -nr_taken);
    spin_unlock_irq(&lruvec->lru_lock);

    mem_cgroup_uncharge_list(&page_list);
    free_unref_page_list(&page_list);

    return nr_reclaimed;
}

static unsigned long promote_active_list(unsigned long nr_to_scan,
	struct lruvec *lruvec, enum lru_list lru)
{
    LIST_HEAD(page_list);
    pg_data_t *pgdat = lruvec_pgdat(lruvec);
    unsigned long nr_taken, nr_promoted;
    
    lru_add_drain();

    spin_lock_irq(&lruvec->lru_lock);
    nr_taken = isolate_lru_pages(nr_to_scan, lruvec, lru, &page_list, 0);
    __mod_node_page_state(pgdat, NR_ISOLATED_ANON, nr_taken);
    spin_unlock_irq(&lruvec->lru_lock);

    if (nr_taken == 0)
	return 0;

    nr_promoted = promote_page_list(&page_list, pgdat);

    spin_lock_irq(&lruvec->lru_lock);
    move_pages_to_lru(lruvec, &page_list);
    __mod_node_page_state(pgdat, NR_ISOLATED_ANON, -nr_taken);
    spin_unlock_irq(&lruvec->lru_lock);

    mem_cgroup_uncharge_list(&page_list);
    free_unref_page_list(&page_list);

    return nr_promoted;
}

static unsigned long demote_lruvec(unsigned long nr_to_reclaim, short priority,
	pg_data_t *pgdat, struct lruvec *lruvec, bool shrink_active)
{
    enum lru_list lru, tmp;
    unsigned long nr_reclaimed = 0;
    long nr_to_scan;

    /* we need to scan file lrus first */
    for_each_evictable_lru(tmp) {
	lru = (tmp + 2) % 4;

	if (!shrink_active && !is_file_lru(lru) && is_active_lru(lru))
	    continue;	
	
	if (is_file_lru(lru)) {
	    nr_to_scan = lruvec_lru_size(lruvec, lru, MAX_NR_ZONES);
	} else {
	    nr_to_scan = lruvec_lru_size(lruvec, lru, MAX_NR_ZONES) >> priority;

	    if (nr_to_scan < nr_to_reclaim)
		nr_to_scan = nr_to_reclaim * 11 / 10; // because warm pages are not demoted
	}

	if (!nr_to_scan)
	    continue;

	while (nr_to_scan > 0) {
	    unsigned long scan = min(nr_to_scan, SWAP_CLUSTER_MAX);
	    nr_reclaimed += demote_inactive_list(scan, scan,
					     lruvec, lru, shrink_active);
	    nr_to_scan -= (long)scan;
	    if (nr_reclaimed >= nr_to_reclaim)
		break;
	}

	if (nr_reclaimed >= nr_to_reclaim)
	    break;
    }

    return nr_reclaimed;
}

static unsigned long promote_lruvec(unsigned long nr_to_promote, short priority,
	pg_data_t *pgdat, struct lruvec *lruvec, enum lru_list lru)
{
    unsigned long nr_promoted = 0, nr;
    
    nr = nr_to_promote >> priority;
    if (nr)
	nr_promoted += promote_active_list(nr, lruvec, lru);

    return nr_promoted;
}

static unsigned long demote_node(pg_data_t *pgdat, struct mem_cgroup *memcg,
	unsigned long nr_exceeded)
{//edit by 100, 执行降级操作，但是要去掉和直方图、冷处理相关的方案
    struct lruvec *lruvec = mem_cgroup_lruvec(memcg, pgdat);
    short priority = DEF_PRIORITY;
    unsigned long nr_to_reclaim = 0, nr_evictable_pages = 0, nr_reclaimed = 0;
    enum lru_list lru;
    bool shrink_active = false;

    for_each_evictable_lru(lru) {
		if (!is_file_lru(lru) && is_active_lru(lru))
			continue;

		nr_evictable_pages += lruvec_lru_size(lruvec, lru, MAX_NR_ZONES);
    }
    
    nr_to_reclaim = nr_exceeded;	
    
    if (nr_exceeded > nr_evictable_pages)
		shrink_active = true;

    do {
		nr_reclaimed += demote_lruvec(nr_to_reclaim - nr_reclaimed, priority,
					pgdat, lruvec, shrink_active);
		if (nr_reclaimed >= nr_to_reclaim)
			break;
		priority--;
    } while (priority);

    // if (htmm_nowarm == 0) {
	// int target_nid = htmm_cxl_mode ? 1 : next_demotion_node(pgdat->node_id);
	// unsigned long nr_lowertier_active =
	    // target_nid == NUMA_NO_NODE ? 0: need_lowertier_promotion(NODE_DATA(target_nid), memcg);
	
	// nr_lowertier_active = nr_lowertier_active < nr_to_reclaim ?
			// nr_lowertier_active : nr_to_reclaim;
	// if (nr_lowertier_active && nr_reclaimed < nr_lowertier_active)
	    // memcg->warm_threshold = memcg->active_threshold;
    // }

    /* check the condition */
    // do {
		// unsigned long max = memcg->nodeinfo[pgdat->node_id]->max_nr_base_pages;
		// if (get_nr_lru_pages_node(memcg, pgdat) + get_memcg_demotion_watermark(max) < max)
	    // WRITE_ONCE(memcg->nodeinfo[pgdat->node_id]->need_demotion, false);
    // } while (0);
    return nr_reclaimed;
}

static unsigned long promote_node(pg_data_t *pgdat, struct mem_cgroup *memcg)
{
    struct lruvec *lruvec = mem_cgroup_lruvec(memcg, pgdat);
    unsigned long long nr_to_promote, nr_promoted = 0, tmp = 0; //向上迁移是不限量的，有就迁移
    enum lru_list lru = LRU_ACTIVE_ANON;
    short priority = DEF_PRIORITY;
    int target_nid = htmm_cxl_mode ? 0 : next_promotion_node(pgdat->node_id);

    if (!promotion_available(target_nid, memcg, &nr_to_promote))
		return 0;

    nr_to_promote = min(nr_to_promote, lruvec_lru_size(lruvec, lru, MAX_NR_ZONES));
    
    if (nr_to_promote == 0 && htmm_mode == HTMM_NO_MIG) {
		lru = LRU_INACTIVE_ANON;
		nr_to_promote = min(tmp, lruvec_lru_size(lruvec, lru, MAX_NR_ZONES));
    }
    do {
		nr_promoted += promote_lruvec(nr_to_promote, priority, pgdat, lruvec, lru);
		if (nr_promoted >= nr_to_promote)
	    	break;
		priority--;
    } while (priority);
    
    return nr_promoted;
}

static struct mem_cgroup_per_node *next_memcg_cand(pg_data_t *pgdat)
{
    struct mem_cgroup_per_node *pn;

    spin_lock(&pgdat->kmigraterd_lock);
    if (!list_empty(&pgdat->kmigraterd_head)) {
	pn = list_first_entry(&pgdat->kmigraterd_head, typeof(*pn), kmigraterd_list);
	list_move_tail(&pn->kmigraterd_list, &pgdat->kmigraterd_head);
    }
    else
	pn = NULL;
    spin_unlock(&pgdat->kmigraterd_lock);

    return pn;
}

static int kmigraterd_demotion(pg_data_t *pgdat, struct mem_cgroup *memcg, unsigned long nr_demotion)
{
	// edit by 100, 这里做页面 降级 降级 降级 操作
	if (need_toptier_demotion(pgdat, memcg, &nr_demotion)) {
	    demote_node(pgdat, memcg, nr_demotion);
    }
    return 0;
}

static int kmigraterd_promotion(pg_data_t *pgdat, struct mem_cgroup *memcg)
{
   // edit by 100, 这里做页面 升级 升级 升级 操作，promotes hot pages to fast memory node
 
	if (need_lowertier_promotion(pgdat, memcg)) {
	    promote_node(pgdat, memcg);
	}

    return 0;
}

static int kmigraterd(void *p)
{
    pg_data_t *pgdat = (pg_data_t *)p;
    int nid = pgdat->node_id;

// edit by 100,假设已经获得需要操作的数量，现在判断上下应该被迁移的数目
// 这些操作是根据cgroup来做的, 遍历node 0的
	for ( ; ; ) {
	    struct mem_cgroup_per_node *pn;
	    struct mem_cgroup *memcg;
		unsigned long long nr_available;
		long long tmp_demotion;
		unsigned int nr_demotion;
	    LIST_HEAD(split_list);

	    if (kthread_should_stop()){ // 还不清楚这是干啥，先保留下来
	        break;
		}
		pn = next_memcg_cand(pgdat); // 虽然目前不太清楚这个内存控制组是怎么得到的
	    //但是由于内存控制组是全局的，从DRAM节点也能遍历得到也属于PM节点的cgroup
		if (!pn) { // 如果没有内存控制组就睡眠1s
	        msleep_interruptible(1000);
	        continue;
	    }

	    memcg = pn->memcg;
	    if (!memcg || !memcg->htmm_enabled) {
	        spin_lock(&pgdat->kmigraterd_lock);
	        if (!list_empty_careful(&pn->kmigraterd_list))
				list_del(&pn->kmigraterd_list);
	        spin_unlock(&pgdat->kmigraterd_lock);
	        continue;
	    }
	
		get_best_action(&nr_action);
		printk("get the actoin %d", nr_action);
		if(!nr_action){ //如果有行动的话
			promotion_available(nid, memcg, &nr_available);
			tmp_demotion  = (unsigned long long)nr_action-nr_available;
			if(tmp_demotion >0 && tmp_demotion <= INT_MAX){
				nr_demotion = (int)tmp_demotion; //有可能不会降级那么多，相应能升级的就要更少，但不需要知道具体的数，因为迁移上去的页面由当时空多少决定
				printk("get the demotion %d", nr_demotion);//大于0的话就需要降级，降级操作是由DRAM node做的
				kmigraterd_demotion(pgdat, memcg, nr_demotion);
			}
			//升级，升级操作是由PM node做的
			kmigraterd_promotion(NODE_DATA(nid+2), memcg);
		}
		
		// 然后后台线程睡眠
		msleep_interruptible(htmm_promotion_period_in_ms);
    }

    return 0;
}

void kmigraterd_wakeup(int nid)
{
    pg_data_t *pgdat = NODE_DATA(nid);
    wake_up_interruptible(&pgdat->kmigraterd_wait);  
}

static void kmigraterd_run(int nid)
{
    pg_data_t *pgdat = NODE_DATA(nid);
    if (!pgdat || pgdat->kmigraterd)
	return;

    init_waitqueue_head(&pgdat->kmigraterd_wait);

    pgdat->kmigraterd = kthread_run(kmigraterd, pgdat, "kmigraterd%d", nid);
    if (IS_ERR(pgdat->kmigraterd)) {
	pr_err("Fails to start kmigraterd on node %d\n", nid);
	pgdat->kmigraterd = NULL;
    }
}

void kmigraterd_stop(void)
{
    int nid;

    for_each_node_state(nid, N_MEMORY) {
	struct task_struct *km = NODE_DATA(nid)->kmigraterd;

	if (km) {
	    kthread_stop(km);
	    NODE_DATA(nid)->kmigraterd = NULL;
	}
    }
}

int kmigraterd_init(void)
{
    int nid;

    for_each_node_state(nid, N_MEMORY)
	kmigraterd_run(nid);
    return 0;
}
