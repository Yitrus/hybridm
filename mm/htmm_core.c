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

void check_failed_list(struct mem_cgroup_per_node *pn,
	struct list_head *tmp, struct list_head *failed_list)
{
    while (!list_empty(tmp)) {
		struct page *page = lru_to_page(tmp);
		// struct page *meta;
		// unsigned int idx;

		list_move(&page->lru, failed_list);
		
		if (!PageTransHuge(page))
			VM_WARN_ON(1);

		if (PageLRU(page)) {
			if (!TestClearPageLRU(page)) {
			VM_WARN_ON(1);
			}
		}

		// meta = get_meta_page(page);
		// idx = meta->idx;
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
    pginfo_t *pginfo;

    if (!memcg || !memcg->htmm_enabled)
	return;
    
    pte_page = virt_to_page((unsigned long)pte);
    if (!PageHtmm(pte_page))
	    return;

    pginfo = get_pginfo_from_pte(pte);
    if (!pginfo)
	    return;
}

void uncharge_htmm_page(struct page *page, struct mem_cgroup *memcg)
{
    // unsigned int nr_pages = thp_nr_pages(page);
    // unsigned int idx;

    if (!memcg || !memcg->htmm_enabled)
	return;
    
    page = compound_head(page);
    // if (nr_pages != 1) { // hugepage
	//     struct page *meta = get_meta_page(page);

	    // idx = meta->idx;
    // }
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
        printk("get_page_unless_zero");
        goto lru_unlock;
    }
	    
    if (!TestClearPageLRU(page)) {
        printk("TestClearPageLRU");
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

static void update_base_page(struct vm_area_struct *vma,
	struct page *page, pginfo_t *pginfo)
{
    bool hot;

    // prev_accessed = pginfo->total_accesses; //以前被访问的次数？
    pginfo->nr_accesses++; //这次被访问的次数累计
    pginfo->total_accesses += HPAGE_PMD_NR;
    
    // prev_idx = get_idx(prev_accessed); //所在bins的位置
    // cur_idx = get_idx(pginfo->nr_accesses); //我可以用这个来提升在lru两部分的移动吧

    if(pginfo->nr_accesses >= 4){ //小页面元数据*512，12; 不乘以元数据后，采样本身就表示频率比较高，试试门槛为被采样到2次(2次效果不行，感觉被采样到就不容易了，试试都迁移走)
        hot = true;
    }else{
        hot = false;
    }
    
    if (hot && !PageActive(page)){
        // pginfo->total_accesses = pginfo->nr_accesses >> 1;
        move_page_to_active_lru(page);
    }



    // else if (PageActive(page)){
    //     if(pginfo->total_accesses <= 4){
    //         move_page_to_inactive_lru(page);
    //     }
        // pginfo->total_accesses -= 1; //在活跃链表里，但最近不热了，受到惩罚
    // }
	    
}
// 前后这两个针对base和huge的页面是这次要改要做调整的主要函数
static void update_huge_page(struct vm_area_struct *vma, pmd_t *pmd,
	struct page *page, unsigned long address)
{
    // struct mem_cgroup *memcg = get_mem_cgroup_from_mm(vma->vm_mm);
    struct page *meta_page;
    pginfo_t *pginfo;
    // unsigned long prev_idx, cur_idx;
    bool hot, pg_split = false;
    unsigned long pginfo_prev;

    meta_page = get_meta_page(page); //大页的访问信息记录主要就靠这个结构体
    pginfo = get_compound_pginfo(page, address);

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

    /*subpage 更新子页面的标识 删了*/

    /* hugepage */
    // prev_idx = meta_page->idx;
    // cur_idx = meta_page->total_accesses;
    // cur_idx = get_idx(cur_idx);
    // meta_page->idx = cur_idx;

    if (pg_split)
	return;


    // hot = cur_idx >= memcg->active_threshold;
    // if(meta_page->total_accesses >= 8 && !ifdram && PageAnon(page)){
    //     add_fast_promote_list(page, fast_list);
    // }
    // else 
    if(meta_page->total_accesses >= 16){ //没有被乘以元数据，但是面广，12可能太大了，先试试8
        hot = true;
    }else{
        hot = false;
    }

    // if (PageActive(page) && !hot) {
	//     move_page_to_inactive_lru(page);
    // } else if (!PageActive(page) && hot) {
	//     move_page_to_active_lru(page);
    // }
    
    if (hot && !PageActive(page)){
        //  meta_page->total_accesses = meta_page->total_accesses >> 1;
        //  meta_page->idx -= 1; 
         move_page_to_active_lru(page);
    }
    // else if (PageActive(page)){
    //     if(meta_page->total_accesses <= 14){
    //         //活跃页面在active里，但是，后面没访问了，就仍走
    //         move_page_to_inactive_lru(page);
    //     }
    // }
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
        printk("__update_pmd_pginfo !pmd || pmd_none(*pmd)");
        return ret;
    }
	
    if (is_swap_pmd(*pmd)){
        // printk("__update_pmd_pginfo is_swap_pmd(*pmd)");
        return ret;
    }
	
    if (!pmd_trans_huge(*pmd) && !pmd_devmap(*pmd) && unlikely(pmd_bad(*pmd))) {
        printk("__update_pmd_pginfo !pmd_trans_huge(*pmd) && !pmd_devmap(*pmd) && unlikely(pmd_bad(*pmd))");
	    pmd_clear_bad(pmd);
	    return ret;
    }

    // printk("begin update get address");
    pmdval = *pmd;
    if (pmd_trans_huge(pmdval) || pmd_devmap(pmdval)) {
        struct page *page;

        if (is_huge_zero_pmd(pmdval)){
            printk("__update_pmd_pginfo is_huge_zero_pmd(pmdval)");
            return ret;
        }
        
        page = pmd_page(pmdval);
        if (!page){
            printk("__update_pmd_pginfo !page");
            goto pmd_unlock;
        }
        
        if (!PageCompound(page)) {
            printk("__update_pmd_pginfo !PageCompound(page)");
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
// 
static int __update_pginfo(struct vm_area_struct *vma, unsigned long address)
{
    pgd_t *pgd;
    p4d_t *p4d;
    pud_t *pud;

    pgd = pgd_offset(vma->vm_mm, address);
    if (pgd_none_or_clear_bad(pgd)){
        printk("__update_pginfo pgd_none_or_clear_bad");
        return 0;
    }
    
    p4d = p4d_offset(pgd, address);
    if (p4d_none_or_clear_bad(p4d)){
        printk("__update_pginfo p4d_none_or_clear_bad");
        return 0;
    }
    
    pud = pud_offset(p4d, address);
    if (pud_none_or_clear_bad(pud)){
        printk("__update_pginfo pud_none_or_clear_bad");
        return 0;
    }
    
    return __update_pmd_pginfo(vma, pud, address);
}

/* protected by memcg->access_lock 这个是统计直方图用的，但是active和inactive的阈值没定，如果设置一个的话，可能会用到*/
// static void reset_memcg_stat(struct mem_cgroup *memcg)
// {
//     int i;

//     for (i = 0; i < 16; i++) {
// 		memcg->hotness_hg[i] = 0;
// 		memcg->ebp_hotness_hg[i] = 0;
//     }

//     for (i = 0; i < 21; i++)
// 		memcg->access_map[i] = 0;

//     memcg->sum_util = 0;
//     memcg->num_util = 0;
// }

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
        printk("update pginfo htmm no mig");
        goto put_task;
    }

    if (!mm) {
        printk("update pginfo ! mm");
		goto put_task;
    }

    if (!mmap_read_trylock(mm)){
        printk("update pginfo mmap_read_trylock");
        goto put_task;
    }

    vma = find_vma(mm, address);
    if (unlikely(!vma)){
        printk("update pginfo unlikely(!vma)");
        goto mmap_unlock;
    }
		
    if (!vma->vm_mm || !vma_migratable(vma) ||
	(vma->vm_file && (vma->vm_flags & (VM_READ | VM_WRITE)) == (VM_READ))){
        // printk("update pginfo !vma_migratable");
        goto mmap_unlock;
    }
		
    memcg = get_mem_cgroup_from_mm(mm);
    if (!memcg || !memcg->htmm_enabled){
        printk("update pginfo memcg->htmm_enabled");
        goto mmap_unlock;
    }
    
    /* increase sample counts only for valid records */
    ret = __update_pginfo(vma, address);

mmap_unlock:
    mmap_read_unlock(mm);
put_task:
    put_pid(pid_struct);
}
