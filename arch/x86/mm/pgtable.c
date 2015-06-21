#include <linux/mm.h>
#include <linux/gfp.h>
#include <asm/pgalloc.h>
#include <asm/pgtable.h>
#include <asm/tlb.h>
#include <asm/fixmap.h>

#define PGALLOC_GFP GFP_KERNEL | __GFP_NOTRACK | __GFP_REPEAT | __GFP_ZERO

#ifdef CONFIG_HIGHPTE
#define PGALLOC_USER_GFP __GFP_HIGHMEM
#else
#define PGALLOC_USER_GFP 0
#endif

gfp_t __userpte_alloc_gfp = PGALLOC_GFP | PGALLOC_USER_GFP;

pte_t *pte_alloc_one_kernel(struct mm_struct *mm, unsigned long address)
{
	return (pte_t *)__get_free_page(PGALLOC_GFP);
}

pgtable_t pte_alloc_one(struct mm_struct *mm, unsigned long address)
{
	struct page *pte;

	pte = alloc_pages(__userpte_alloc_gfp, 0);
	if (!pte)
		return NULL;
	if (!pgtable_page_ctor(pte)) {
		__free_page(pte);
		return NULL;
	}
	return pte;
}

static int __init setup_userpte(char *arg)
{
	if (!arg)
		return -EINVAL;

	/*
	 * "userpte=nohigh" disables allocation of user pagetables in
	 * high memory.
	 */
	if (strcmp(arg, "nohigh") == 0)
		__userpte_alloc_gfp &= ~__GFP_HIGHMEM;
	else
		return -EINVAL;
	return 0;
}
early_param("userpte", setup_userpte);

void ___pte_free_tlb(struct mmu_gather *tlb, struct page *pte)
{
	pgtable_page_dtor(pte);
	paravirt_release_pte(page_to_pfn(pte));
	tlb_remove_page(tlb, pte);
}

#if PAGETABLE_LEVELS > 2
void ___pmd_free_tlb(struct mmu_gather *tlb, pmd_t *pmd)
{
	struct page *page = virt_to_page(pmd);
	paravirt_release_pmd(__pa(pmd) >> PAGE_SHIFT);
	/*
	 * NOTE! For PAE, any changes to the top page-directory-pointer-table
	 * entries need a full cr3 reload to flush.
	 */
#ifdef CONFIG_X86_PAE
	tlb->need_flush_all = 1;
#endif
	pgtable_pmd_page_dtor(page);
	tlb_remove_page(tlb, page);
}

#if PAGETABLE_LEVELS > 3
void ___pud_free_tlb(struct mmu_gather *tlb, pud_t *pud)
{
	paravirt_release_pud(__pa(pud) >> PAGE_SHIFT);
	tlb_remove_page(tlb, virt_to_page(pud));
}
#endif	/* PAGETABLE_LEVELS > 3 */
#endif	/* PAGETABLE_LEVELS > 2 */

static inline void pgd_list_add(pgd_t *pgd)
{
	struct page *page = virt_to_page(pgd);

	list_add(&page->lru, &pgd_list);
}

static inline void pgd_list_del(pgd_t *pgd)
{
	struct page *page = virt_to_page(pgd);

	list_del(&page->lru);
}

#if defined(CONFIG_X86_64) && defined(CONFIG_PAX_MEMORY_UDEREF)
pgdval_t clone_pgd_mask __read_only = ~_PAGE_PRESENT;

void __shadow_user_pgds(pgd_t *dst, const pgd_t *src)
{
	unsigned int count = USER_PGD_PTRS;

	if (!pax_user_shadow_base)
		return;

	while (count--)
		*dst++ = __pgd((pgd_val(*src++) | (_PAGE_NX & __supported_pte_mask)) & ~_PAGE_USER);
}
#endif

#ifdef CONFIG_PAX_PER_CPU_PGD
void __clone_user_pgds(pgd_t *dst, const pgd_t *src)
{
	unsigned int count = USER_PGD_PTRS;

	while (count--) {
		pgd_t pgd;

#ifdef CONFIG_X86_64
		pgd = __pgd(pgd_val(*src++) | _PAGE_USER);
#else
		pgd = *src++;
#endif

#if defined(CONFIG_X86_64) && defined(CONFIG_PAX_MEMORY_UDEREF)
		pgd = __pgd(pgd_val(pgd) & clone_pgd_mask);
#endif

		*dst++ = pgd;
	}

}
#endif

#ifdef CONFIG_X86_64
#define pxd_t				pud_t
#define pyd_t				pgd_t
#define paravirt_release_pxd(pfn)	paravirt_release_pud(pfn)
#define pgtable_pxd_page_ctor(page)	true
#define pgtable_pxd_page_dtor(page)	do {} while (0)
#define pxd_free(mm, pud)		pud_free((mm), (pud))
#define pyd_populate(mm, pgd, pud)	pgd_populate((mm), (pgd), (pud))
#define pyd_offset(mm, address)		pgd_offset((mm), (address))
#define PYD_SIZE			PGDIR_SIZE
#define mm_inc_nr_pxds(mm)		do {} while (0)
#define mm_dec_nr_pxds(mm)		do {} while (0)
#else
#define pxd_t				pmd_t
#define pyd_t				pud_t
#define paravirt_release_pxd(pfn)	paravirt_release_pmd(pfn)
#define pgtable_pxd_page_ctor(page)	pgtable_pmd_page_ctor(page)
#define pgtable_pxd_page_dtor(page)	pgtable_pmd_page_dtor(page)
#define pxd_free(mm, pud)		pmd_free((mm), (pud))
#define pyd_populate(mm, pgd, pud)	pud_populate((mm), (pgd), (pud))
#define pyd_offset(mm, address)		pud_offset((mm), (address))
#define PYD_SIZE			PUD_SIZE
#define mm_inc_nr_pxds(mm)		mm_inc_nr_pmds(mm)
#define mm_dec_nr_pxds(mm)		mm_dec_nr_pmds(mm)
#endif

#ifdef CONFIG_PAX_PER_CPU_PGD
static inline void pgd_ctor(struct mm_struct *mm, pgd_t *pgd) {}
static inline void pgd_dtor(pgd_t *pgd) {}
#else
static void pgd_set_mm(pgd_t *pgd, struct mm_struct *mm)
{
	BUILD_BUG_ON(sizeof(virt_to_page(pgd)->index) < sizeof(mm));
	virt_to_page(pgd)->index = (pgoff_t)mm;
}

struct mm_struct *pgd_page_get_mm(struct page *page)
{
	return (struct mm_struct *)page->index;
}

static void pgd_ctor(struct mm_struct *mm, pgd_t *pgd)
{
	/* If the pgd points to a shared pagetable level (either the
	   ptes in non-PAE, or shared PMD in PAE), then just copy the
	   references from swapper_pg_dir. */
	if (PAGETABLE_LEVELS == 2 ||
	    (PAGETABLE_LEVELS == 3 && SHARED_KERNEL_PMD) ||
	    PAGETABLE_LEVELS == 4) {
		clone_pgd_range(pgd + KERNEL_PGD_BOUNDARY,
				swapper_pg_dir + KERNEL_PGD_BOUNDARY,
				KERNEL_PGD_PTRS);
	}

	/* list required to sync kernel mapping updates */
	if (!SHARED_KERNEL_PMD) {
		pgd_set_mm(pgd, mm);
		pgd_list_add(pgd);
	}
}

static void pgd_dtor(pgd_t *pgd)
{
	if (SHARED_KERNEL_PMD)
		return;

	spin_lock(&pgd_lock);
	pgd_list_del(pgd);
	spin_unlock(&pgd_lock);
}
#endif

/*
 * List of all pgd's needed for non-PAE so it can invalidate entries
 * in both cached and uncached pgd's; not needed for PAE since the
 * kernel pmd is shared. If PAE were not to share the pmd a similar
 * tactic would be needed. This is essentially codepath-based locking
 * against pageattr.c; it is the unique case in which a valid change
 * of kernel pagetables can't be lazily synchronized by vmalloc faults.
 * vmalloc faults work because attached pagetables are never freed.
 * -- nyc
 */

#if defined(CONFIG_X86_32) && defined(CONFIG_X86_PAE)
/*
 * In PAE mode, we need to do a cr3 reload (=tlb flush) when
 * updating the top-level pagetable entries to guarantee the
 * processor notices the update.  Since this is expensive, and
 * all 4 top-level entries are used almost immediately in a
 * new process's life, we just pre-populate them here.
 *
 * Also, if we're in a paravirt environment where the kernel pmd is
 * not shared between pagetables (!SHARED_KERNEL_PMDS), we allocate
 * and initialize the kernel pmds here.
 */
#define PREALLOCATED_PXDS	(SHARED_KERNEL_PMD ? KERNEL_PGD_BOUNDARY : PTRS_PER_PGD)

void pud_populate(struct mm_struct *mm, pud_t *pudp, pmd_t *pmd)
{
	paravirt_alloc_pmd(mm, __pa(pmd) >> PAGE_SHIFT);

	/* Note: almost everything apart from _PAGE_PRESENT is
	   reserved at the pmd (PDPT) level. */
	set_pud(pudp, __pud(__pa(pmd) | _PAGE_PRESENT));

	/*
	 * According to Intel App note "TLBs, Paging-Structure Caches,
	 * and Their Invalidation", April 2007, document 317080-001,
	 * section 8.1: in PAE mode we explicitly have to flush the
	 * TLB via cr3 if the top-level pgd is changed...
	 */
	flush_tlb_mm(mm);
}
#elif defined(CONFIG_X86_64) && defined(CONFIG_PAX_PER_CPU_PGD)
#define PREALLOCATED_PXDS	USER_PGD_PTRS
#else  /* !CONFIG_X86_PAE */

/* No need to prepopulate any pagetable entries in non-PAE modes. */
#define PREALLOCATED_PXDS	0

#endif	/* CONFIG_X86_PAE */

static void free_pxds(struct mm_struct *mm, pxd_t *pxds[])
{
	int i;

	for(i = 0; i < PREALLOCATED_PXDS; i++)
		if (pxds[i]) {
			pgtable_pxd_page_dtor(virt_to_page(pxds[i]));
			free_page((unsigned long)pxds[i]);
			mm_dec_nr_pxds(mm);
		}
}

static int preallocate_pxds(struct mm_struct *mm, pxd_t *pxds[])
{
	int i;
	bool failed = false;

	for(i = 0; i < PREALLOCATED_PXDS; i++) {
		pxd_t *pxd = (pxd_t *)__get_free_page(PGALLOC_GFP);
		if (!pxd)
			failed = true;
		if (pxd && !pgtable_pxd_page_ctor(virt_to_page(pxd))) {
			free_page((unsigned long)pxd);
			pxd = NULL;
			failed = true;
		}
		if (pxd)
			mm_inc_nr_pxds(mm);
		pxds[i] = pxd;
	}

	if (failed) {
		free_pxds(mm, pxds);
		return -ENOMEM;
	}

	return 0;
}

/*
 * Mop up any pmd pages which may still be attached to the pgd.
 * Normally they will be freed by munmap/exit_mmap, but any pmd we
 * preallocate which never got a corresponding vma will need to be
 * freed manually.
 */
static void pgd_mop_up_pxds(struct mm_struct *mm, pgd_t *pgdp)
{
	int i;

	for(i = 0; i < PREALLOCATED_PXDS; i++) {
		pgd_t pgd = pgdp[i];

		if (pgd_val(pgd) != 0) {
			pxd_t *pxd = (pxd_t *)pgd_page_vaddr(pgd);

			set_pgd(pgdp + i, native_make_pgd(0));

			paravirt_release_pxd(pgd_val(pgd) >> PAGE_SHIFT);
			pxd_free(mm, pxd);
			mm_dec_nr_pxds(mm);
		}
	}
}

static void pgd_prepopulate_pxd(struct mm_struct *mm, pgd_t *pgd, pxd_t *pxds[])
{
	pyd_t *pyd;
	int i;

	if (PREALLOCATED_PXDS == 0) /* Work around gcc-3.4.x bug */
		return;

#ifdef CONFIG_X86_64
	pyd = pyd_offset(mm, 0L);
#else
	pyd = pyd_offset(pgd, 0L);
#endif

	for (i = 0; i < PREALLOCATED_PXDS; i++, pyd++) {
		pxd_t *pxd = pxds[i];

		if (i >= KERNEL_PGD_BOUNDARY)
			memcpy(pxd, (pxd_t *)pgd_page_vaddr(swapper_pg_dir[i]),
			       sizeof(pxd_t) * PTRS_PER_PMD);

		pyd_populate(mm, pyd, pxd);
	}
}

pgd_t *pgd_alloc(struct mm_struct *mm)
{
	pgd_t *pgd;
	pxd_t *pxds[PREALLOCATED_PXDS];

	pgd = (pgd_t *)__get_free_page(PGALLOC_GFP);

	if (pgd == NULL)
		goto out;

	mm->pgd = pgd;

	if (preallocate_pxds(mm, pxds) != 0)
		goto out_free_pgd;

	if (paravirt_pgd_alloc(mm) != 0)
		goto out_free_pxds;

	/*
	 * Make sure that pre-populating the pmds is atomic with
	 * respect to anything walking the pgd_list, so that they
	 * never see a partially populated pgd.
	 */
	spin_lock(&pgd_lock);

	pgd_ctor(mm, pgd);
	pgd_prepopulate_pxd(mm, pgd, pxds);

	spin_unlock(&pgd_lock);

	return pgd;

out_free_pxds:
	free_pxds(mm, pxds);
out_free_pgd:
	free_page((unsigned long)pgd);
out:
	return NULL;
}

void pgd_free(struct mm_struct *mm, pgd_t *pgd)
{
	pgd_mop_up_pxds(mm, pgd);
	pgd_dtor(pgd);
	paravirt_pgd_free(mm, pgd);
	free_page((unsigned long)pgd);
}

/*
 * Used to set accessed or dirty bits in the page table entries
 * on other architectures. On x86, the accessed and dirty bits
 * are tracked by hardware. However, do_wp_page calls this function
 * to also make the pte writeable at the same time the dirty bit is
 * set. In that case we do actually need to write the PTE.
 */
int ptep_set_access_flags(struct vm_area_struct *vma,
			  unsigned long address, pte_t *ptep,
			  pte_t entry, int dirty)
{
	int changed = !pte_same(*ptep, entry);

	if (changed && dirty) {
		*ptep = entry;
		pte_update_defer(vma->vm_mm, address, ptep);
	}

	return changed;
}

#ifdef CONFIG_TRANSPARENT_HUGEPAGE
int pmdp_set_access_flags(struct vm_area_struct *vma,
			  unsigned long address, pmd_t *pmdp,
			  pmd_t entry, int dirty)
{
	int changed = !pmd_same(*pmdp, entry);

	VM_BUG_ON(address & ~HPAGE_PMD_MASK);

	if (changed && dirty) {
		*pmdp = entry;
		pmd_update_defer(vma->vm_mm, address, pmdp);
		/*
		 * We had a write-protection fault here and changed the pmd
		 * to to more permissive. No need to flush the TLB for that,
		 * #PF is architecturally guaranteed to do that and in the
		 * worst-case we'll generate a spurious fault.
		 */
	}

	return changed;
}
#endif

int ptep_test_and_clear_young(struct vm_area_struct *vma,
			      unsigned long addr, pte_t *ptep)
{
	int ret = 0;

	if (pte_young(*ptep))
		ret = test_and_clear_bit(_PAGE_BIT_ACCESSED,
					 (unsigned long *) &ptep->pte);

	if (ret)
		pte_update(vma->vm_mm, addr, ptep);

	return ret;
}

#ifdef CONFIG_TRANSPARENT_HUGEPAGE
int pmdp_test_and_clear_young(struct vm_area_struct *vma,
			      unsigned long addr, pmd_t *pmdp)
{
	int ret = 0;

	if (pmd_young(*pmdp))
		ret = test_and_clear_bit(_PAGE_BIT_ACCESSED,
					 (unsigned long *)pmdp);

	if (ret)
		pmd_update(vma->vm_mm, addr, pmdp);

	return ret;
}
#endif

int ptep_clear_flush_young(struct vm_area_struct *vma,
			   unsigned long address, pte_t *ptep)
{
	/*
	 * On x86 CPUs, clearing the accessed bit without a TLB flush
	 * doesn't cause data corruption. [ It could cause incorrect
	 * page aging and the (mistaken) reclaim of hot pages, but the
	 * chance of that should be relatively low. ]
	 *
	 * So as a performance optimization don't flush the TLB when
	 * clearing the accessed bit, it will eventually be flushed by
	 * a context switch or a VM operation anyway. [ In the rare
	 * event of it not getting flushed for a long time the delay
	 * shouldn't really matter because there's no real memory
	 * pressure for swapout to react to. ]
	 */
	return ptep_test_and_clear_young(vma, address, ptep);
}

#ifdef CONFIG_TRANSPARENT_HUGEPAGE
int pmdp_clear_flush_young(struct vm_area_struct *vma,
			   unsigned long address, pmd_t *pmdp)
{
	int young;

	VM_BUG_ON(address & ~HPAGE_PMD_MASK);

	young = pmdp_test_and_clear_young(vma, address, pmdp);
	if (young)
		flush_tlb_range(vma, address, address + HPAGE_PMD_SIZE);

	return young;
}

void pmdp_splitting_flush(struct vm_area_struct *vma,
			  unsigned long address, pmd_t *pmdp)
{
	int set;
	VM_BUG_ON(address & ~HPAGE_PMD_MASK);
	set = !test_and_set_bit(_PAGE_BIT_SPLITTING,
				(unsigned long *)pmdp);
	if (set) {
		pmd_update(vma->vm_mm, address, pmdp);
		/* need tlb flush only to serialize against gup-fast */
		flush_tlb_range(vma, address, address + HPAGE_PMD_SIZE);
	}
}
#endif

/**
 * reserve_top_address - reserves a hole in the top of kernel address space
 * @reserve - size of hole to reserve
 *
 * Can be used to relocate the fixmap area and poke a hole in the top
 * of kernel address space to make room for a hypervisor.
 */
void __init reserve_top_address(unsigned long reserve)
{
#ifdef CONFIG_X86_32
	BUG_ON(fixmaps_set > 0);
	__FIXADDR_TOP = round_down(-reserve, 1 << PMD_SHIFT) - PAGE_SIZE;
	printk(KERN_INFO "Reserving virtual address space above 0x%08lx (rounded to 0x%08lx)\n",
	       -reserve, __FIXADDR_TOP + PAGE_SIZE);
#endif
}

int fixmaps_set;

void __native_set_fixmap(enum fixed_addresses idx, pte_t pte)
{
	unsigned long address = __fix_to_virt(idx);

	if (idx >= __end_of_fixed_addresses) {
		BUG();
		return;
	}
	set_pte_vaddr(address, pte);
	fixmaps_set++;
}

void native_set_fixmap(enum fixed_addresses idx, phys_addr_t phys,
		       pgprot_t flags)
{
	__native_set_fixmap(idx, pfn_pte(phys >> PAGE_SHIFT, flags));
}