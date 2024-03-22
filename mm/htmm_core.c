#include <linux/mm.h>
#include <linux/kernel.h>
#include <linux/huge_mm.h>
#include <linux/mm_inline.h>
#include <linux/pid.h>
#include <linux/htmm.h>
#include <linux/mempolicy.h>
#include <linux/migrate.h>
#include <linux/swap.h>
#include <linux/sched/task.h>
#include <linux/xarray.h>
#include <linux/math.h>
#include <linux/random.h>
#include <trace/events/htmm.h>

#include "internal.h"
#include <asm/pgtable.h>

void htmm_mm_init(struct mm_struct *mm)
{
    struct mem_cgroup *memcg = get_mem_cgroup_from_mm(mm);

    if (!memcg || !memcg->htmm_enabled) {
	mm->htmm_enabled = false;
	return;
    }
    mm->htmm_enabled = true;
}

void htmm_mm_exit(struct mm_struct *mm)
{
    struct mem_cgroup *memcg = get_mem_cgroup_from_mm(mm);
    if (!memcg)
	return;
    /* do nothing */
}

/* Hugepage uses tail pages to store access information.
 * See struct page declaration in linux/mm_types.h */
void __prep_transhuge_page_for_htmm(struct mm_struct *mm, struct page *page)
{
    int i, idx, offset;
    struct mem_cgroup *memcg = mm ? get_mem_cgroup_from_mm(mm) : NULL;
    pginfo_t pginfo = { 0, 0, 0, false, };
    /* third tail page */
    page[3].idx = 0;
    SetPageHtmm(&page[3]);

    /* fourth~ tail pages */
    for (i = 0; i < HPAGE_PMD_NR; i++) {
		idx = 4 + i / 4;
		offset = i % 4;
	
		page[idx].compound_pginfo[offset] = pginfo;
		SetPageHtmm(&page[idx]);
    }

    if (!memcg)
	return;

	//是不是你，搞得没有active list了？！
    //ClearPageActive(page);
}

void prep_transhuge_page_for_htmm(struct vm_area_struct *vma,
				  struct page *page)
{
    prep_transhuge_page(page);

    if (vma->vm_mm->htmm_enabled)
	    __prep_transhuge_page_for_htmm(vma->vm_mm, page);
    else
	    return;
}

void clear_transhuge_pginfo(struct page *page)
{
    INIT_LIST_HEAD(&page->lru);
    set_page_private(page, 0);
}

void copy_transhuge_pginfo(struct page *page,
			   struct page *newpage)
{
    int i, idx, offset;
    pginfo_t zero_pginfo = { 0 };

    VM_BUG_ON_PAGE(!PageCompound(page), page);
    VM_BUG_ON_PAGE(!PageCompound(newpage), newpage);

    page = compound_head(page);
    newpage = compound_head(newpage);

    if (!PageHtmm(&page[3]))
	return;

    newpage[3].total_accesses = page[3].total_accesses;
    newpage[3].idx = page[3].idx;

    SetPageHtmm(&newpage[3]);

    for (i = 0; i < HPAGE_PMD_NR; i++) {
	idx = 4 + i / 4;
	offset = i % 4;

	newpage[idx].compound_pginfo[offset].nr_accesses =
			page[idx].compound_pginfo[offset].nr_accesses;
	newpage[idx].compound_pginfo[offset].total_accesses =
			page[idx].compound_pginfo[offset].total_accesses;
	
	page[idx].compound_pginfo[offset] = zero_pginfo;
	page[idx].mapping = TAIL_MAPPING;
	SetPageHtmm(&newpage[idx]);
    }
}

pginfo_t *get_compound_pginfo(struct page *page, unsigned long address)
{
    int idx, offset;
    VM_BUG_ON_PAGE(!PageCompound(page), page);
    
    idx = 4 + ((address & ~HPAGE_PMD_MASK) >> PAGE_SHIFT) / 4;
    offset = ((address & ~HPAGE_PMD_MASK) >> PAGE_SHIFT) % 4;

    return &(page[idx].compound_pginfo[offset]);
}


struct list_head *get_deferred_list(struct page *page)
{
    struct mem_cgroup *memcg = page_memcg(compound_head(page));
    struct mem_cgroup_per_node *pn = memcg->nodeinfo[page_to_nid(page)];

    if (!memcg || !memcg->htmm_enabled)
	return NULL;
    else
	return &pn->deferred_list; 
}

bool deferred_split_huge_page_for_htmm(struct page *page)
{
    struct deferred_split *ds_queue = get_deferred_split_queue(page);
    unsigned long flags;

    VM_BUG_ON_PAGE(!PageTransHuge(page), page);

    if (PageSwapCache(page))
	return false;

    if (!ds_queue)
	return false;
    
    spin_lock_irqsave(&ds_queue->split_queue_lock, flags);
    if (list_empty(page_deferred_list(page))) {
	count_vm_event(THP_DEFERRED_SPLIT_PAGE);
	list_add_tail(page_deferred_list(page), &ds_queue->split_queue);
	ds_queue->split_queue_len++;

	if (node_is_toptier(page_to_nid(page)))
	    count_vm_event(HTMM_MISSED_DRAMREAD);
	else
	    count_vm_event(HTMM_MISSED_NVMREAD);
    }
    spin_unlock_irqrestore(&ds_queue->split_queue_lock, flags);
    return true;
}

void check_failed_list(struct mem_cgroup_per_node *pn,
	struct list_head *tmp, struct list_head *failed_list)
{
    while (!list_empty(tmp)) {
		struct page *page = lru_to_page(tmp);
		struct page *meta;
		unsigned int idx;

		list_move(&page->lru, failed_list);
		
		if (!PageTransHuge(page))
			VM_WARN_ON(1);

		if (PageLRU(page)) {
			if (!TestClearPageLRU(page)) {
			VM_WARN_ON(1);
			}
		}

		meta = get_meta_page(page);
		idx = meta->idx;
    }
}

struct page *get_meta_page(struct page *page)
{
    page = compound_head(page);
    return &page[3];
}

unsigned int get_accesses_from_idx(unsigned int idx)
{
    unsigned int accesses = 1;
    
    if (idx == 0)
	return 0;

    while (idx--) {
	accesses <<= 1;
    }

    return accesses;
}

unsigned int get_idx(unsigned long num)
{
    unsigned int cnt = 0;
   
    num++;
    while (1) {
	num = num >> 1;
	if (num)
	    cnt++;
	else	
	    return cnt;
	
	if (cnt == 15)
	    break;
    }

    return cnt;
}

int get_skew_idx(unsigned long num)
{
    int cnt = 0;
    unsigned long tmp;
    
    /* 0, 1-3, 4-15, 16-63, 64-255, 256-1023, 1024-2047, 2048-3071, ... */
    tmp = num;
    if (tmp >= 1024) {
	while (tmp > 1024 && cnt < 9) { // <16
	    tmp -= 1024;
	    cnt++;
	}
	cnt += 11;
    }
    else {
	while (tmp) {
	    tmp >>= 1; // >>2
	    cnt++;
	}
    }

    return cnt;
}

/* linux/mm.h */
void free_pginfo_pte(struct page *pte)
{
    if (!PageHtmm(pte))
	return;

    BUG_ON(pte->pginfo == NULL);
    kmem_cache_free(pginfo_cache, pte->pginfo);
    pte->pginfo = NULL;
    ClearPageHtmm(pte);
}

// 申请的虚拟页，要中断时才能和物理页联系起来
void uncharge_htmm_pte(pte_t *pte, struct mem_cgroup *memcg)
{
    struct page *pte_page;
    unsigned int idx;
    pginfo_t *pginfo;

    if (!memcg || !memcg->htmm_enabled)
	return;
    
    pte_page = virt_to_page((unsigned long)pte);
    if (!PageHtmm(pte_page))
	    return;

    pginfo = get_pginfo_from_pte(pte);
    if (!pginfo)
	    return;

    idx = get_idx(pginfo->total_accesses);
}

void uncharge_htmm_page(struct page *page, struct mem_cgroup *memcg)
{
    unsigned int nr_pages = thp_nr_pages(page);
    unsigned int idx;

    if (!memcg || !memcg->htmm_enabled)
	return;
    
    page = compound_head(page);
    if (nr_pages != 1) { // hugepage
	    struct page *meta = get_meta_page(page);

	    idx = meta->idx;
    }
}

// void set_lru_adjusting(struct mem_cgroup *memcg, bool inc_thres)
// {
//     struct mem_cgroup_per_node *pn;
//     int nid;

//     for_each_node_state(nid, N_MEMORY) {
    
// 	pn = memcg->nodeinfo[nid];
// 	if (!pn)
// 	    continue;

// 	WRITE_ONCE(pn->need_adjusting, true);
// 	if (inc_thres)
// 	    WRITE_ONCE(pn->need_adjusting_all, true);
//     }
// }

bool move_page_to_deferred_split_queue(struct mem_cgroup *memcg, struct page *page)
{
    struct lruvec *lruvec;
    bool ret = false;

    page = compound_head(page);

    lruvec = mem_cgroup_page_lruvec(page);
    spin_lock_irq(&lruvec->lru_lock);
    
    if (!PageLRU(page))
	goto lru_unlock;

    if (deferred_split_huge_page_for_htmm(compound_head(page))) {
	ret = true;
	goto lru_unlock;
    }
    
lru_unlock:
    spin_unlock_irq(&lruvec->lru_lock);

    return ret;
}

void move_page_to_active_lru(struct page *page)
{
    struct lruvec *lruvec;
    LIST_HEAD(l_active);

    lruvec = mem_cgroup_page_lruvec(page);
    
    spin_lock_irq(&lruvec->lru_lock);
    if (PageActive(page))
	goto lru_unlock;

    if (!__isolate_lru_page_prepare(page, 0))
	goto lru_unlock;

    if (unlikely(!get_page_unless_zero(page)))
	goto lru_unlock;

    if (!TestClearPageLRU(page)) {
	put_page(page);
	goto lru_unlock;
    }
    
    list_move(&page->lru, &l_active);
    update_lru_size(lruvec, page_lru(page), page_zonenum(page),
		    -thp_nr_pages(page));
    SetPageActive(page);

    if (!list_empty(&l_active))
	move_pages_to_lru(lruvec, &l_active);
lru_unlock:
    spin_unlock_irq(&lruvec->lru_lock);

    if (!list_empty(&l_active))
	BUG();
}

void move_page_to_inactive_lru(struct page *page)
{
    struct lruvec *lruvec;
    LIST_HEAD(l_inactive);

    lruvec = mem_cgroup_page_lruvec(page);
    
    spin_lock_irq(&lruvec->lru_lock);
    if (!PageActive(page))
	goto lru_unlock;

    if (!__isolate_lru_page_prepare(page, 0))
	goto lru_unlock;

    if (unlikely(!get_page_unless_zero(page)))
	goto lru_unlock;

    if (!TestClearPageLRU(page)) {
	put_page(page);
	goto lru_unlock;
    }
    
    list_move(&page->lru, &l_inactive);
    update_lru_size(lruvec, page_lru(page), page_zonenum(page),
		    -thp_nr_pages(page));
    ClearPageActive(page);

    if (!list_empty(&l_inactive))
	move_pages_to_lru(lruvec, &l_inactive);
lru_unlock:
    spin_unlock_irq(&lruvec->lru_lock);

    if (!list_empty(&l_inactive))
	BUG();
}

void update_stats(unsigned long long nr_bw, unsigned long long nr_cyc, unsigned long long nr_ins){
	//TODO:怎么得到我想要的cgroup,且重新定义cgroup的结构。不过也可以是内存共享的全局的东西
	//先输出看看长什么样,空闲多少从迁移那边传吧？等等好像都不需要这个哩，咦~

	printk(KERN_INFO "bw %llu cyc %llu ins %llu", nr_bw, nr_cyc, nr_ins);
	
}
