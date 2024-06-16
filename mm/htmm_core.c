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
    int hotness_factor = memcg ? get_accesses_from_idx(memcg->active_threshold + 1) : 0;
    /* third tail page */
    page[3].hot_utils = 0;
    page[3].total_accesses = hotness_factor;
    page[3].idx = 0;
    SetPageHtmm(&page[3]);

    if (hotness_factor < 0)
	hotness_factor = 0;
    pginfo.total_accesses = hotness_factor;
    pginfo.nr_accesses = hotness_factor;

    /* fourth~ tail pages */
    for (i = 0; i < HPAGE_PMD_NR; i++) {
		idx = 4 + i / 4;
		offset = i % 4;
	
		page[idx].compound_pginfo[offset] = pginfo;
		SetPageHtmm(&page[idx]);
    }

    if (!memcg)
	return;

    if (htmm_skip_cooling)
	page[3].cooling_clock = memcg->cooling_clock + 1;
    else
	page[3].cooling_clock = memcg->cooling_clock;

    ClearPageActive(page);
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

    newpage[3].hot_utils = page[3].hot_utils;   
    newpage[3].total_accesses = page[3].total_accesses;
    newpage[3].cooling_clock = page[3].cooling_clock;
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

void check_transhuge_cooling(void *arg, struct page *page, bool locked)
{
    struct mem_cgroup *memcg = arg ? (struct mem_cgroup *)arg : page_memcg(page);
    struct page *meta_page;
    pginfo_t *pginfo;
    int i, idx, offset;
    unsigned int memcg_cclock;

    if (!memcg || !memcg->htmm_enabled)
	return;

    meta_page = get_meta_page(page);

    spin_lock(&memcg->access_lock);
    /* check cooling */
    memcg_cclock = READ_ONCE(memcg->cooling_clock);
    if (memcg_cclock > meta_page->cooling_clock) {
	    unsigned int diff = memcg_cclock - meta_page->cooling_clock;
	    unsigned long prev_idx, cur_idx;
	    unsigned int refs = 0;
	    unsigned int bp_hot_thres = min(memcg->active_threshold,
					 memcg->bp_active_threshold);

	    /* perform cooling */
	    meta_page->hot_utils = 0;
	    for (i = 0; i < HPAGE_PMD_NR; i++) { // subpages
		int j;

		idx = 4 + i / 4;
		offset = i % 4;
		pginfo =&(page[idx].compound_pginfo[offset]);
		prev_idx = get_idx(pginfo->total_accesses);
		if (prev_idx >= bp_hot_thres) {
		    meta_page->hot_utils++;
		    refs += pginfo->total_accesses;
		}

		/* get the sum of the square of H_ij*/
		// skewness += (pginfo->total_accesses * pginfo->total_accesses);
		if (prev_idx >= (memcg->bp_active_threshold))
		    pginfo->may_hot = true;
		else
		    pginfo->may_hot = false;

		/* halves access counts of subpages */
		for (j = 0; j < diff; j++)
		    pginfo->total_accesses >>= 1;

		/* updates estimated base page histogram */
		cur_idx = get_idx(pginfo->total_accesses);
		memcg->ebp_hotness_hg[cur_idx]++;
	    }

	    /* halves access count for a huge page */
	    for (i = 0; i < diff; i++)		
		meta_page->total_accesses >>= 1;

	    cur_idx = meta_page->total_accesses;
	    cur_idx = get_idx(cur_idx);
	    memcg->hotness_hg[cur_idx] += HPAGE_PMD_NR;
	    meta_page->idx = cur_idx;

	    /* updates skewness */
	    // if (meta_page->hot_utils == 0)
		// skewness = 0;
	    // else if (meta_page->idx >= 13) // very hot pages 
		// skewness = 0;
	    // else {
		// skewness /= 11; /* scale down */
		// skewness = skewness / (meta_page->hot_utils);
		// skewness = skewness / (meta_page->hot_utils);
		// skewness = get_skew_idx(skewness);
	    // }
	    // meta_page->skewness_idx = skewness;
	    // memcg->access_map[skewness] += 1;

	    if (meta_page->hot_utils) {
		refs /= HPAGE_PMD_NR; /* actual access counts */
		memcg->sum_util += refs; /* total accesses to huge pages */
		memcg->num_util += 1; /* the number of huge pages */
	    }

	    meta_page->cooling_clock = memcg_cclock;
    } else
	meta_page->cooling_clock = memcg_cclock;

    spin_unlock(&memcg->access_lock);
}

void check_base_cooling(pginfo_t *pginfo, struct page *page, bool locked)
{
    struct mem_cgroup *memcg = page_memcg(page);
    unsigned long prev_accessed, cur_idx;
    unsigned int memcg_cclock;

    if (!memcg || !memcg->htmm_enabled)
	return;

    spin_lock(&memcg->access_lock);
    memcg_cclock = READ_ONCE(memcg->cooling_clock);
    if (memcg_cclock > pginfo->cooling_clock) {
	unsigned int diff = memcg_cclock - pginfo->cooling_clock;    
	int j;
	    
	prev_accessed = pginfo->total_accesses;
	cur_idx = get_idx(prev_accessed);
	if (cur_idx >= (memcg->bp_active_threshold))
	    pginfo->may_hot = true;
	else
	    pginfo->may_hot = false;

	/* halves access count */
	for (j = 0; j < diff; j++)
	    pginfo->total_accesses >>= 1;
	//if (pginfo->total_accesses == 0)
	  //  pginfo->total_accesses = 1;

	cur_idx = get_idx(pginfo->total_accesses);
	memcg->hotness_hg[cur_idx]++;
	memcg->ebp_hotness_hg[cur_idx]++;

	pginfo->cooling_clock = memcg_cclock;
    } else
	pginfo->cooling_clock = memcg_cclock;
    spin_unlock(&memcg->access_lock);
}

int set_page_coolstatus(struct page *page, pte_t *pte, struct mm_struct *mm)
{
    struct mem_cgroup *memcg = get_mem_cgroup_from_mm(mm);
    struct page *pte_page;
    pginfo_t *pginfo;
    int hotness_factor;

    if (!memcg || !memcg->htmm_enabled)
	return 0;

    pte_page = virt_to_page((unsigned long)pte);
    if (!PageHtmm(pte_page))
	return 0;

    pginfo = get_pginfo_from_pte(pte);
    if (!pginfo)
	return 0;
    
    hotness_factor = get_accesses_from_idx(memcg->active_threshold + 1);
    
    pginfo->total_accesses = hotness_factor;
    pginfo->nr_accesses = hotness_factor;
    if (htmm_skip_cooling)
	pginfo->cooling_clock = READ_ONCE(memcg->cooling_clock) + 1;
    else
	pginfo->cooling_clock = READ_ONCE(memcg->cooling_clock);
    pginfo->may_hot = false;

    return 0;
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

void check_failed_list(struct mem_cgroup_per_node *pn,
	struct list_head *tmp, struct list_head *failed_list)
{
    struct mem_cgroup *memcg = pn->memcg;
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

        spin_lock(&memcg->access_lock);
	    memcg->hotness_hg[idx] += HPAGE_PMD_NR;
	    spin_unlock(&memcg->access_lock);
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

//计算最高位数，得到属于哪个直方图bin
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

// 申请的虚拟页，要中断时才能和物理页联系起来，想给cgroup的直方图加标记
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
    spin_lock(&memcg->access_lock);
    if (memcg->hotness_hg[idx] > 0)
	memcg->hotness_hg[idx]--;
    if (memcg->ebp_hotness_hg[idx] > 0)
	memcg->ebp_hotness_hg[idx]--;
    spin_unlock(&memcg->access_lock);
}

void uncharge_htmm_page(struct page *page, struct mem_cgroup *memcg)
{
    unsigned int nr_pages = thp_nr_pages(page);
    unsigned int idx;
    int i;

    if (!memcg || !memcg->htmm_enabled)
	return;
    
    page = compound_head(page);
    if (nr_pages != 1) { // hugepage
	    struct page *meta = get_meta_page(page);

	    idx = meta->idx;
        spin_lock(&memcg->access_lock);
	    if (memcg->hotness_hg[idx] >= nr_pages)
	        memcg->hotness_hg[idx] -= nr_pages;
	    else
	        memcg->hotness_hg[idx] = 0;
	
	    for (i = 0; i < HPAGE_PMD_NR; i++) {
	        int base_idx = 4 + i / 4;
	        int offset = i % 4;
	        pginfo_t *pginfo;

	        pginfo = &(page[base_idx].compound_pginfo[offset]);
	        idx = get_idx(pginfo->total_accesses);
	        if (memcg->ebp_hotness_hg[idx] > 0)
		        memcg->ebp_hotness_hg[idx]--;
	    }
	    spin_unlock(&memcg->access_lock);
    }
}

static bool need_cooling(struct mem_cgroup *memcg)
{
    struct mem_cgroup_per_node *pn;
    int nid;

    for_each_node_state(nid, N_MEMORY) {
	pn = memcg->nodeinfo[nid];
	if (!pn)
	    continue;
    
	if (READ_ONCE(pn->need_cooling))
	    return true;
    }
    return false;
}

static void set_lru_cooling(struct mm_struct *mm)
{
    struct mem_cgroup *memcg = get_mem_cgroup_from_mm(mm);
    struct mem_cgroup_per_node *pn;
    int nid;

    if (!memcg || !memcg->htmm_enabled)
	return;
    
    for_each_node_state(nid, N_MEMORY) {
	pn = memcg->nodeinfo[nid];
	if (!pn)
	    continue;
    
	WRITE_ONCE(pn->need_cooling, true);
    }
}

void set_lru_adjusting(struct mem_cgroup *memcg, bool inc_thres)
{
    struct mem_cgroup_per_node *pn;
    int nid;

    for_each_node_state(nid, N_MEMORY) {
    
	pn = memcg->nodeinfo[nid];
	if (!pn)
	    continue;

	WRITE_ONCE(pn->need_adjusting, true);
	if (inc_thres)
	    WRITE_ONCE(pn->need_adjusting_all, true);
    }
}

// 这个是什么时候用起来的，在update—basic和huge page时
void move_page_to_active_lru(struct page *page)
{
    struct lruvec *lruvec;
    LIST_HEAD(l_active);

    lruvec = mem_cgroup_page_lruvec(page);
    
    spin_lock_irq(&lruvec->lru_lock);
    if (PageActive(page)){
        goto lru_unlock;
    }

    if (!__isolate_lru_page_prepare(page, 0)){
        // printk("isolate_lru_page_prepare");
        goto lru_unlock;
    }

    if (unlikely(!get_page_unless_zero(page))){
        // printk("get_page_unless_zero");
        goto lru_unlock;
    }
	    
    if (!TestClearPageLRU(page)) {
        // printk("TestClearPageLRU");
	    put_page(page);
	    goto lru_unlock;
    }
    
    list_move(&page->lru, &l_active);
    update_lru_size(lruvec, page_lru(page), page_zonenum(page),
		    -thp_nr_pages(page));
    SetPageActive(page);

    if (!list_empty(&l_active)){
        unsigned int nr;
        nr = move_pages_to_lru(lruvec, &l_active);
        // printk("ok move pages to lru and the nr is %d", nr);
    }
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

// 就是这个函数，需要和固定阈值来个对比吧~哎呀，肯定是没有放之四海而皆准的标准啦~
static void update_base_page(struct vm_area_struct *vma,
	struct page *page, pginfo_t *pginfo)
{
    struct mem_cgroup *memcg = get_mem_cgroup_from_mm(vma->vm_mm);
    unsigned long prev_accessed, prev_idx, cur_idx;
    bool hot;

     /* check cooling status and perform cooling if the page needs to be cooled */
    check_base_cooling(pginfo, page, false);
    prev_accessed = pginfo->total_accesses; //以前被访问的次数？
    pginfo->nr_accesses++; //这次被访问的次数累计
    pginfo->total_accesses += HPAGE_PMD_NR;
    
    prev_idx = get_idx(prev_accessed); //所在bins的位置
    cur_idx = get_idx(pginfo->nr_accesses); //我可以用这个来提升在lru两部分的移动吧

    spin_lock(&memcg->access_lock);

    if (prev_idx != cur_idx) {
	if (memcg->hotness_hg[prev_idx] > 0)
	    memcg->hotness_hg[prev_idx]--;
	memcg->hotness_hg[cur_idx]++;

	if (memcg->ebp_hotness_hg[prev_idx] > 0)
	    memcg->ebp_hotness_hg[prev_idx]--;
	memcg->ebp_hotness_hg[cur_idx]++;
    }

    if (pginfo->may_hot == true)
	memcg->max_dram_sampled++;
    if (cur_idx >= (memcg->bp_active_threshold))
	pginfo->may_hot = true;
    else
	pginfo->may_hot = false;

    spin_unlock(&memcg->access_lock);

    //这里再多一层判断，关于小页面选动态阈值还是默认阈值为热
    if(memcg->active_threshold > 2){
        //动态阈值是小页面被访问4次以上，pginfo->nr_accesses >= 4
        //小页面元数据*512，12; 不乘以元数据后，采样本身就表示频率比较高，试试门槛为被采样到2次(2次效果不行，感觉被采样到就不容易了，试试都迁移走)
        if(cur_idx >= memcg->active_threshold){
                hot = true;
        }else{
                hot = false;
        }
    }else {
        if(cur_idx >= 2){
                hot = true;
        }else{
                hot = false;
        }
    }  
    
    if (hot) // 如果hot真，将page移动到活跃列表（已经在活跃会怎样？移动到活跃头？）
	    move_page_to_active_lru(page);
    else if (PageActive(page)) //如果hot假，并且page是活跃的，将page移动到非活跃列表
	    move_page_to_inactive_lru(page);
}

// 前后这两个针对base和huge的页面是这次要改要做调整的主要函数
static void update_huge_page(struct vm_area_struct *vma, pmd_t *pmd,
	struct page *page, unsigned long address)
{
    struct mem_cgroup *memcg = get_mem_cgroup_from_mm(vma->vm_mm);
    struct page *meta_page;
    pginfo_t *pginfo;
    unsigned long prev_idx, cur_idx;
    bool hot, pg_split = false;
    unsigned long pginfo_prev;

    meta_page = get_meta_page(page); //大页的访问信息记录主要就靠这个结构体
    pginfo = get_compound_pginfo(page, address);

    /* check cooling status */
    check_transhuge_cooling((void *)memcg, page, false);

    //这一段是针对这个小页面在赋值（好吧有可能其他原因被拆分）
    pginfo_prev = pginfo->total_accesses;
    pginfo->nr_accesses++;
    pginfo->total_accesses += HPAGE_PMD_NR;
    
    meta_page->total_accesses++;

#ifndef DEFERRED_SPLIT_ISOLATED
    if (check_split_huge_page(memcg, meta_page, false)) {
	    pg_split = move_page_to_deferred_split_queue(memcg, page);
    }
#endif

    /*subpage 更新子页面的标识 删了因为不做拆分嘛 */


    /* hugepage */
    prev_idx = meta_page->idx;
    cur_idx = meta_page->total_accesses;
    cur_idx = get_idx(cur_idx);
    if (prev_idx != cur_idx) {
	spin_lock(&memcg->access_lock);
	if (memcg->hotness_hg[prev_idx] >= HPAGE_PMD_NR)
	    memcg->hotness_hg[prev_idx] -= HPAGE_PMD_NR;
	else
	    memcg->hotness_hg[prev_idx] = 0;

	memcg->hotness_hg[cur_idx] += HPAGE_PMD_NR;
	spin_unlock(&memcg->access_lock);
    }
    meta_page->idx = cur_idx;

    if (pg_split)
	return;

    // 选大的作为阈值
    if(memcg->active_threshold > 4){
        if(cur_idx >= memcg->active_threshold){
            hot = true;
        }else{
            hot = false;
        }
    }else{
        if(cur_idx >= 4){ // meta_page->total_accesses >= 16
            hot = true;
        }else{
            hot = false;
        }
    }
    
    if (hot)
	move_page_to_active_lru(page);
    else if (PageActive(page))
	move_page_to_inactive_lru(page);
}

static int __update_pte_pginfo(struct vm_area_struct *vma, pmd_t *pmd,
				unsigned long address)
{
    pte_t *pte, ptent;
    spinlock_t *ptl;
    pginfo_t *pginfo;
    struct page *page, *pte_page;
    int ret = 0;
    unsigned long tmp_addr;

    pte = pte_offset_map_lock(vma->vm_mm, pmd, address, &ptl);
    ptent = *pte;
    if (!pte_present(ptent))
	goto pte_unlock;

    page = vm_normal_page(vma, address, ptent);
    if (!page || PageKsm(page))
	goto pte_unlock;

    if (page != compound_head(page))
	goto pte_unlock;

    pte_page = virt_to_page((unsigned long)pte);
    if (!PageHtmm(pte_page))
	goto pte_unlock;

    pginfo = get_pginfo_from_pte(pte);
    if (!pginfo)
	goto pte_unlock;

    tmp_addr = page_to_phys(page);
    if(tmp_addr <= DRAM_ADDR_END){
        atomic_inc(&hit_dram);
    }else if(tmp_addr>=PM_ADDR_START && tmp_addr<=PM_ADDR_END){
        atomic_inc(&hit_pm);
    }else{
        atomic_inc(&hit_other);
    }

    update_base_page(vma, page, pginfo);
    pte_unmap_unlock(pte, ptl);
    if (htmm_cxl_mode) {
	if (page_to_nid(page) == 0)
	    return 1;
	else
	    return 2;
    }
    else {
	if (node_is_toptier(page_to_nid(page)))
	    return 1;
	else
	    return 2;
    }

pte_unlock:
    pte_unmap_unlock(pte, ptl);
    return ret;
}

static int __update_pmd_pginfo(struct vm_area_struct *vma, pud_t *pud,
				unsigned long address)
{
    pmd_t *pmd, pmdval;
    bool ret = 0;
    unsigned long tmp_addr;
    pmd = pmd_offset(pud, address);
    if (!pmd || pmd_none(*pmd)){
        // printk("__update_pmd_pginfo !pmd || pmd_none(*pmd)");
        return ret;
    }
	
    if (is_swap_pmd(*pmd)){
        // printk("__update_pmd_pginfo is_swap_pmd(*pmd)");
        return ret;
    }
	
    if (!pmd_trans_huge(*pmd) && !pmd_devmap(*pmd) && unlikely(pmd_bad(*pmd))) {
        // printk("__update_pmd_pginfo !pmd_trans_huge(*pmd) && !pmd_devmap(*pmd) && unlikely(pmd_bad(*pmd))");
	    pmd_clear_bad(pmd);
	    return ret;
    }

    // printk("begin update get address");
    pmdval = *pmd;
    if (pmd_trans_huge(pmdval) || pmd_devmap(pmdval)) {
        struct page *page;

        if (is_huge_zero_pmd(pmdval)){
            // printk("__update_pmd_pginfo is_huge_zero_pmd(pmdval)");
            return ret;
        }
        
        page = pmd_page(pmdval);
        if (!page){
            // printk("__update_pmd_pginfo !page");
            goto pmd_unlock;
        }
        
        if (!PageCompound(page)) {
            // printk("__update_pmd_pginfo !PageCompound(page)");
            goto pmd_unlock;
	    }

        tmp_addr = page_to_phys(page);
        // hit_total += 1;
        if(tmp_addr <= DRAM_ADDR_END){
            atomic_inc(&hit_dram);
        }else if(tmp_addr>=PM_ADDR_START && tmp_addr<=PM_ADDR_END){
            atomic_inc(&hit_pm);
        }else{
            atomic_inc(&hit_other);
        }

        update_huge_page(vma, pmd, page, address);
        if (htmm_cxl_mode) {
            if (page_to_nid(page) == 0)
            return 1;
            else
            return 2;
        }
        else {
            if (node_is_toptier(page_to_nid(page)))
            return 1;
            else
            return 2;
        }
    pmd_unlock:
        return 0;
    }

    /* base page */
    return __update_pte_pginfo(vma, pmd, address);
}

static int __update_pginfo(struct vm_area_struct *vma, unsigned long address)
{
    pgd_t *pgd;
    p4d_t *p4d;
    pud_t *pud;

    pgd = pgd_offset(vma->vm_mm, address);
    if (pgd_none_or_clear_bad(pgd)){
        // printk("__update_pginfo pgd_none_or_clear_bad");
        return 0;
    }
    
    p4d = p4d_offset(pgd, address);
    if (p4d_none_or_clear_bad(p4d)){
        // printk("__update_pginfo p4d_none_or_clear_bad");
        return 0;
    }
    
    pud = pud_offset(p4d, address);
    if (pud_none_or_clear_bad(pud)){
        // printk("__update_pginfo pud_none_or_clear_bad");
        return 0;
    }
    
    return __update_pmd_pginfo(vma, pud, address);
}

/* protected by memcg->access_lock 这个是统计直方图用的，但是active和inactive的阈值没定，如果设置一个的话，可能会用到*/
static void reset_memcg_stat(struct mem_cgroup *memcg)
{
    int i;

    for (i = 0; i < 16; i++) {
		memcg->hotness_hg[i] = 0;
		memcg->ebp_hotness_hg[i] = 0;
    }

    for (i = 0; i < 21; i++)
		memcg->access_map[i] = 0;

    memcg->sum_util = 0;
    memcg->num_util = 0;
}

static bool __cooling(struct mm_struct *mm,
	struct mem_cgroup *memcg)
{
    int nid;

    /* check whether the previous cooling is done or not. */
    for_each_node_state(nid, N_MEMORY) {
	struct mem_cgroup_per_node *pn = memcg->nodeinfo[nid];
	if (pn && READ_ONCE(pn->need_cooling)) {
	    spin_lock(&memcg->access_lock);
	    memcg->cooling_clock++;
	    spin_unlock(&memcg->access_lock);
	    return false;
	}
    }

    spin_lock(&memcg->access_lock);

    reset_memcg_stat(memcg); 
    memcg->cooling_clock++;
    memcg->bp_active_threshold--;
    memcg->cooled = true;
    smp_mb();
    spin_unlock(&memcg->access_lock);
    set_lru_cooling(mm);
    return true;
}

static void __adjust_active_threshold(struct mm_struct *mm,
	struct mem_cgroup *memcg)
{
    unsigned long nr_active = 0;
    unsigned long max_nr_pages = memcg->max_nr_dram_pages -
	    get_memcg_promotion_watermark(memcg->max_nr_dram_pages);
    bool need_warm = false;
    int idx_hot, idx_bp;

    //if (need_cooling(memcg))
//	return;

    spin_lock(&memcg->access_lock);

    for (idx_hot = 15; idx_hot >= 0; idx_hot--) {
		unsigned long nr_pages = memcg->hotness_hg[idx_hot];
		if (nr_active + nr_pages > max_nr_pages)
			break;
		nr_active += nr_pages;
    }
	
    if (idx_hot != 15)
		idx_hot++;

	//yxy添加修改
	// idx_hot += 2;
	// if(idx_hot > 15){
		// idx_hot = 15;
	// }

    if (nr_active < (max_nr_pages * 75 / 100))
		need_warm = true;

    /* for the estimated base page histogram */
    nr_active = 0;
    for (idx_bp = 15; idx_bp >= 0; idx_bp--) {
		unsigned long nr_pages = memcg->ebp_hotness_hg[idx_bp];
		if (nr_active + nr_pages > max_nr_pages)
			break;
		nr_active += nr_pages;
    }
    if (idx_bp != 15)
	idx_bp++;
	
	//yxy添加修改
	// idx_bp += 2;
	// if(idx_bp > 15){
		// idx_bp = 15;
	// }

    spin_unlock(&memcg->access_lock);

    // minimum hot threshold
    if (idx_hot < htmm_thres_hot)
		idx_hot = htmm_thres_hot;
    if (idx_bp < htmm_thres_hot)
		idx_bp = htmm_thres_hot;

    /* some pages may not be reflected in the histogram when cooling happens */
    if (memcg->cooled) {
	/* when cooling happens, thres will be current - 1 */
	if (idx_hot < memcg->active_threshold)
	    if (memcg->active_threshold > 1)
		memcg->active_threshold--;
	if (idx_bp < memcg->bp_active_threshold)
	    memcg->bp_active_threshold = idx_bp;
	
	memcg->cooled = false;
	set_lru_adjusting(memcg, true);
    }else { /* normal case */
	if (idx_hot > memcg->active_threshold) {
	    //printk("thres: %d -> %d\n", memcg->active_threshold, idx_hot);
	    memcg->active_threshold = idx_hot;
	    set_lru_adjusting(memcg, true);
	}
	/* estimated base page histogram */
	memcg->bp_active_threshold = idx_bp;
    }

    /* set warm threshold */
    if (!htmm_nowarm) { // warm enabled
	if (need_warm)
	    memcg->warm_threshold = memcg->active_threshold - 1;
	else
	    memcg->warm_threshold = memcg->active_threshold;
    } else { // disable warm
	memcg->warm_threshold = memcg->active_threshold;
    }
}

static bool need_memcg_cooling (struct mem_cgroup *memcg)
{
    unsigned long usage = page_counter_read(&memcg->memory);
    if (memcg->nr_alloc + htmm_thres_cooling_alloc <= usage) {
	memcg->nr_alloc = usage;
	return true;	
    }
    return false;
}

void update_pginfo(pid_t pid, unsigned long address, enum events e)
{
    struct pid *pid_struct = find_get_pid(pid);
    struct task_struct *p = pid_struct ? pid_task(pid_struct, PIDTYPE_PID) : NULL;
    struct mm_struct *mm = p ? p->mm : NULL;
    struct vm_area_struct *vma; 
    struct mem_cgroup *memcg;
    int ret;
    static unsigned long last_thres_adaptation;
    last_thres_adaptation= jiffies;

    if (htmm_mode == HTMM_NO_MIG){
        // printk("update pginfo htmm no mig");
        goto put_task;
    }

    if (!mm) {
        // printk("update pginfo ! mm");
		goto put_task;
    }

    if (!mmap_read_trylock(mm)){
        // printk("update pginfo mmap_read_trylock");
        goto put_task;
    }

    vma = find_vma(mm, address);
    if (unlikely(!vma)){
        // printk("update pginfo unlikely(!vma)");
        goto mmap_unlock;
    }
		
    if (!vma->vm_mm || !vma_migratable(vma) ||
	(vma->vm_file && (vma->vm_flags & (VM_READ | VM_WRITE)) == (VM_READ))){
        // printk("update pginfo !vma_migratable");
        goto mmap_unlock;
    }
		
    memcg = get_mem_cgroup_from_mm(mm);
    if (!memcg || !memcg->htmm_enabled){
        // printk("update pginfo memcg->htmm_enabled");
        goto mmap_unlock;
    }
    
    /* increase sample counts only for valid records */
    ret = __update_pginfo(vma, address);
    if (ret == 1) { /* memory accesses to DRAM */
		memcg->nr_sampled++;
		memcg->nr_dram_sampled++;
		memcg->nr_max_sampled++;
    }
    else if (ret == 2) {
		memcg->nr_sampled++;
		memcg->nr_max_sampled++;
    } else
		goto mmap_unlock;
    
    /* cooling and split decision */
    if (memcg->nr_sampled % htmm_cooling_period == 0 ||
	    need_memcg_cooling(memcg)) {
	/* cooling -- updates thresholds and sets need_cooling flags */
	if (__cooling(mm, memcg)) {
	    // unsigned long temp_rhr = memcg->prev_dram_sampled;
	    /* updates actual access stat */
	    memcg->prev_dram_sampled >>= 1;
	    memcg->prev_dram_sampled += memcg->nr_dram_sampled;
	    memcg->nr_dram_sampled = 0;
	    /* updates estimated access stat */
	    memcg->prev_max_dram_sampled >>= 1;
	    memcg->prev_max_dram_sampled += memcg->max_dram_sampled;
	    memcg->max_dram_sampled = 0;

	    printk("total_accesses: %lu max_dram_hits: %lu cur_hits: %lu \n",
		    memcg->nr_max_sampled, memcg->prev_max_dram_sampled, memcg->prev_dram_sampled);
	    memcg->nr_max_sampled >>= 1;
	}
    }
    /* threshold adaptation */
    else if (memcg->nr_sampled % htmm_adaptation_period == 0) {
	__adjust_active_threshold(mm, memcg);
    }

mmap_unlock:
    mmap_read_unlock(mm);
put_task:
    put_pid(pid_struct);
}
