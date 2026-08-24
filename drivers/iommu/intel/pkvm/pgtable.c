// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2026 Google LLC */

#include <linux/percpu.h>

#include "pkvm/pkvm.h"
#include "pkvm/mem_protect.h"
#include "pkvm/memory.h"
#include "pkvm/mmu.h"

#include "../iommu.h"

#define IOMMU_PTE_ADDR_MASK	GENMASK_ULL(51, VTD_PAGE_SHIFT)
#define IOMMU_PTE_ENTRIES	(VTD_PAGE_SIZE / sizeof(u64))

/*
 * Bit 63 is ignored by hardware in both first- and second-level
 * translations. Keep it set on protected leaf PTEs so a later unmap can
 * retain the mapping metadata after clearing the hardware-present bits.
 */
#define IOMMU_PTE_MAPPED	BIT_ULL(63)

static DEFINE_PER_CPU(struct dmar_domain *, __current_iommu_domain);
#define current_iommu_domain (*this_cpu_ptr(&__current_iommu_domain))

static void *iommu_pgtable_zalloc_page(struct pkvm_memcache *mc)
{
	struct pkvm_page *page_meta;
	void *page;

	if (WARN_ON_ONCE(!current_iommu_domain ||
			 mc != &current_iommu_domain->mc))
		return NULL;

	page = pop_pkvm_memcache_page(mc, pkvm_phys_to_virt);
	if (!page)
		return NULL;

	memset(page, 0, VTD_PAGE_SIZE);
	if (current_iommu_domain->needs_cpu_flush)
		clflush_cache_range(page, VTD_PAGE_SIZE);
	page_meta = pkvm_virt_to_page(page);
	pkvm_set_page_refcounted(page_meta);

	return page;
}

static void iommu_pgtable_get_page(void *vaddr)
{
	pkvm_page_ref_inc(pkvm_virt_to_page(vaddr));
}

static void iommu_pgtable_put_page(void *vaddr)
{
	struct pkvm_page *page = pkvm_virt_to_page(vaddr);

	if (pkvm_page_ref_dec_and_test(page))
		push_pkvm_memcache_page(&current_iommu_domain->mc, vaddr,
					pkvm_virt_to_phys);
}

static int iommu_pgtable_page_count(void *vaddr)
{
	return pkvm_page_count(vaddr);
}

static const struct pkvm_pgtable_mm_ops iommu_pgtable_mm_ops = {
	.zalloc_page = iommu_pgtable_zalloc_page,
	.get_page = iommu_pgtable_get_page,
	.put_page = iommu_pgtable_put_page,
	.page_count = iommu_pgtable_page_count,
};

static bool iommu_fl_pte_present(void *ptep)
{
	return !!(READ_ONCE(*(u64 *)ptep) & DMA_FL_PTE_PRESENT);
}

static bool iommu_sl_pte_present(void *ptep)
{
	return !!(READ_ONCE(*(u64 *)ptep) &
		  (DMA_PTE_READ | DMA_PTE_WRITE));
}

static bool iommu_pte_annotated(void *ptep)
{
	return !!(READ_ONCE(*(u64 *)ptep) & IOMMU_PTE_MAPPED);
}

static bool iommu_pte_huge(void *ptep)
{
	return !!(READ_ONCE(*(u64 *)ptep) & DMA_PTE_LARGE_PAGE);
}

static void iommu_pte_mkhuge(void *ptep)
{
	*(u64 *)ptep |= DMA_PTE_LARGE_PAGE;
}

static unsigned long iommu_pte_to_phys(void *ptep)
{
	return READ_ONCE(*(u64 *)ptep) & IOMMU_PTE_ADDR_MASK;
}

static u64 iommu_pte_to_prot(void *ptep)
{
	return READ_ONCE(*(u64 *)ptep) &
		~(IOMMU_PTE_ADDR_MASK | DMA_PTE_LARGE_PAGE);
}

static u64 iommu_fl_calc_pte_perm(bool read, bool write, bool exec)
{
	u64 prot = DMA_FL_PTE_US | DMA_FL_PTE_ACCESS;

	if (read || write)
		prot |= DMA_FL_PTE_PRESENT;
	if (write)
		prot |= DMA_PTE_WRITE | DMA_FL_PTE_DIRTY;

	return prot;
}

static u64 iommu_sl_calc_pte_perm(bool read, bool write, bool exec)
{
	u64 prot = 0;

	if (read)
		prot |= DMA_PTE_READ;
	if (write)
		prot |= DMA_PTE_WRITE;

	return prot;
}

static u64 iommu_calc_pte_memtype(bool mmio)
{
	return 0;
}

static int iommu_vaddr_to_index(unsigned long vaddr, int level)
{
	return (vaddr >> (VTD_PAGE_SHIFT +
			  (level - 1) * LEVEL_STRIDE)) & LEVEL_MASK;
}

static unsigned long iommu_level_to_size(int level)
{
	return 1UL << (VTD_PAGE_SHIFT + (level - 1) * LEVEL_STRIDE);
}

static u64 iommu_level_to_mask(int level)
{
	return ~(iommu_level_to_size(level) - 1);
}

static bool iommu_fl_pte_is_leaf(void *ptep, int level)
{
	return level == PG_LEVEL_4K || !iommu_fl_pte_present(ptep) ||
	       iommu_pte_huge(ptep);
}

static bool iommu_sl_pte_is_leaf(void *ptep, int level)
{
	return level == PG_LEVEL_4K || !iommu_sl_pte_present(ptep) ||
	       iommu_pte_huge(ptep);
}

static int iommu_pte_size(int level)
{
	return sizeof(u64);
}

static int iommu_pte_count(int level)
{
	return IOMMU_PTE_ENTRIES;
}

static void iommu_pte_set(void *ptep, u64 val)
{
	WRITE_ONCE(*(u64 *)ptep, val);
	if (current_iommu_domain->needs_cpu_flush)
		clflush_cache_range(ptep, sizeof(u64));
}

static u64 iommu_pte_get(void *ptep)
{
	return READ_ONCE(*(u64 *)ptep);
}

static void iommu_pgtable_flush_tlb(struct pkvm_pgtable *pgt,
				    unsigned long vaddr,
				    unsigned long size)
{
	struct dmar_domain *domain = container_of(pgt, struct dmar_domain, pgt);

	cache_tag_flush_range(domain, vaddr, vaddr + size - 1, 0);
}

static const struct pkvm_pgtable_ops iommu_fl_pgtable_ops = {
	.pte_present = iommu_fl_pte_present,
	.pte_annotated = iommu_pte_annotated,
	.pte_huge = iommu_pte_huge,
	.pte_mkhuge = iommu_pte_mkhuge,
	.pte_to_phys = iommu_pte_to_phys,
	.pte_to_prot = iommu_pte_to_prot,
	.calc_pte_perm = iommu_fl_calc_pte_perm,
	.calc_pte_memtype = iommu_calc_pte_memtype,
	.vaddr_to_index = iommu_vaddr_to_index,
	.level_to_size = iommu_level_to_size,
	.level_to_mask = iommu_level_to_mask,
	.pte_is_leaf = iommu_fl_pte_is_leaf,
	.pte_size = iommu_pte_size,
	.pte_count = iommu_pte_count,
	.pte_set = iommu_pte_set,
	.pte_get = iommu_pte_get,
	.flush_tlb = iommu_pgtable_flush_tlb,
};

static const struct pkvm_pgtable_ops iommu_sl_pgtable_ops = {
	.pte_present = iommu_sl_pte_present,
	.pte_annotated = iommu_pte_annotated,
	.pte_huge = iommu_pte_huge,
	.pte_mkhuge = iommu_pte_mkhuge,
	.pte_to_phys = iommu_pte_to_phys,
	.pte_to_prot = iommu_pte_to_prot,
	.calc_pte_perm = iommu_sl_calc_pte_perm,
	.calc_pte_memtype = iommu_calc_pte_memtype,
	.vaddr_to_index = iommu_vaddr_to_index,
	.level_to_size = iommu_level_to_size,
	.level_to_mask = iommu_level_to_mask,
	.pte_is_leaf = iommu_sl_pte_is_leaf,
	.pte_size = iommu_pte_size,
	.pte_count = iommu_pte_count,
	.pte_set = iommu_pte_set,
	.pte_get = iommu_pte_get,
	.flush_tlb = iommu_pgtable_flush_tlb,
};

void pkvm_iommu_pgtable_init(struct dmar_domain *domain,
			     unsigned int allowed_pgsz)
{
	struct pkvm_pgtable_cap cap = {
		.level = agaw_to_level(domain->agaw),
		.allowed_pgsz = allowed_pgsz,
		.flush_tlb_lazy = true,
	};

	if (domain->needs_cpu_flush)
		clflush_cache_range(pkvm_phys_to_virt(domain->root_pa),
				    VTD_PAGE_SIZE);

	if (domain->use_first_level) {
		cap.table_prot = DMA_FL_PTE_PRESENT | DMA_PTE_WRITE |
				 DMA_FL_PTE_US | DMA_FL_PTE_ACCESS;
		domain->pgt.pgt_ops = &iommu_fl_pgtable_ops;
	} else {
		cap.table_prot = DMA_PTE_READ | DMA_PTE_WRITE;
		domain->pgt.pgt_ops = &iommu_sl_pgtable_ops;
	}

	domain->pgt.root_pa = domain->root_pa;
	domain->pgt.cap = cap;
	domain->pgt.mm_ops = &iommu_pgtable_mm_ops;
	pkvm_set_page_refcounted(pkvm_phys_to_page(domain->root_pa));
}

static int iommu_pgtable_unuse_dma(struct pkvm_pgtable_visit_ctx *ctx,
				   unsigned long flags, void *arg)
{
	const struct pkvm_pgtable_ops *pgt_ops = ctx->pgt->pgt_ops;
	unsigned long phys;

	if (!pgt_ops->pte_present(ctx->ptep))
		return 0;

	phys = pgt_ops->pte_to_phys(ctx->ptep);
	pkvm_host_unuse_dma(phys, pgt_ops->level_to_size(ctx->level));

	return 0;
}

int pkvm_iommu_pgtable_destroy(struct dmar_domain *domain)
{
	struct pkvm_pgtable_walker walker = {
		.cb = iommu_pgtable_unuse_dma,
		.arg = NULL,
		.walk_flags = PKVM_PGTABLE_WALK_LEAF,
	};
	int ret;

	if (WARN_ON_ONCE(current_iommu_domain))
		return -EBUSY;

	current_iommu_domain = domain;
	ret = pkvm_pgtable_walk(&domain->pgt, 0,
				pkvm_pgtable_max_size(&domain->pgt),
				&walker);
	if (ret)
		goto out;

	pkvm_pgtable_destroy(&domain->pgt);
out:
	current_iommu_domain = NULL;

	return ret;
}

int pkvm_iommu_pgtable_map(struct dmar_domain *domain, unsigned long iova,
			   phys_addr_t phys, size_t size, u64 prot,
			   struct pkvm_memcache *host_mc)
{
	phys_addr_t phys_end;
	unsigned long iova_end;
	unsigned long mapped_iova;
	unsigned long required_pages;
	int rollback_ret;
	u64 pte_prot;
	int ret;

	if (!size || !PAGE_ALIGNED(iova) || !PAGE_ALIGNED(phys) ||
	    !PAGE_ALIGNED(size) ||
	    check_add_overflow(iova, size, &iova_end) ||
	    iova_end > BIT_ULL(domain->iova_bits) ||
	    check_add_overflow(phys, size, &phys_end) ||
	    phys_end > BIT_ULL(52) ||
	    prot > UINT_MAX ||
	    !(prot & (IOMMU_READ | IOMMU_WRITE)))
		return -EINVAL;

	if (host_mc->flags != PKVM_MC_DONATE_SHARE_RO)
		return -EINVAL;

	pkvm_pgtable_lookup_range(&domain->pgt, iova, size, &mapped_iova,
				  NULL, NULL, NULL);
	if (mapped_iova != INVALID_PAGE)
		return -EEXIST;

	required_pages = __pkvm_pgtable_max_pages(size >> PAGE_SHIFT);
	if (host_mc->count > required_pages)
		return -EINVAL;

	if (host_mc->count) {
		ret = pkvm_refill_memcache(&domain->mc,
					   domain->mc.count + host_mc->count,
					   host_mc);
		if (ret)
			return ret;
	}

	if (domain->mc.count < required_pages)
		return -ENOMEM;

	if (WARN_ON_ONCE(current_iommu_domain))
		return -EBUSY;

	pte_prot = IOMMU_PTE_MAPPED;
	pte_prot |= domain->pgt.pgt_ops->calc_pte_perm(prot & IOMMU_READ,
							prot & IOMMU_WRITE,
							false);
	ret = pkvm_host_use_dma(phys, size);
	if (ret)
		return ret;

	current_iommu_domain = domain;
	ret = pkvm_pgtable_map(&domain->pgt, iova, phys, size, pte_prot,
			       &domain->mc);
	if (WARN_ON(ret)) {
		/*
		 * The range was verified empty and enough page-table pages were
		 * reserved for the worst case, so a failure after installing any
		 * mappings indicates a bug. Roll the whole range back defensively.
		 */
		rollback_ret = pkvm_pgtable_unmap(&domain->pgt, iova,
						  INVALID_PAGE, size);
		WARN_ON_ONCE(rollback_ret);
	}
	if (!ret && READ_ONCE(domain->iotlb_sync_map))
		cache_tag_flush_range_np(domain, iova, iova_end - 1);
	current_iommu_domain = NULL;
	if (ret && !rollback_ret)
		pkvm_host_unuse_dma(phys, size);

	return ret;
}

static int validate_unmap_range(struct dmar_domain *domain,
				unsigned long iova, size_t size)
{
	const struct pkvm_pgtable_ops *pgt_ops = domain->pgt.pgt_ops;
	unsigned long end = iova + size;
	unsigned long phys;
	unsigned long leaf_size;
	int level;

	while (iova < end) {
		pkvm_pgtable_lookup(&domain->pgt, iova, &phys, NULL, &level);
		if (!VALID_PAGE(phys))
			return -ENOENT;

		leaf_size = pgt_ops->level_to_size(level);
		if (!IS_ALIGNED(iova, leaf_size) || leaf_size > end - iova)
			return -E2BIG;

		iova += leaf_size;
	}

	return 0;
}

int pkvm_iommu_pgtable_unmap(struct dmar_domain *domain, unsigned long iova,
			     size_t size)
{
	unsigned long mapped_iova, mapped_size, phys;
	unsigned long iova_end;
	int ret;

	if (!size || !PAGE_ALIGNED(iova) || !PAGE_ALIGNED(size) ||
	    check_add_overflow(iova, size, &iova_end) ||
	    iova_end > BIT_ULL(domain->iova_bits))
		return -EINVAL;

	ret = validate_unmap_range(domain, iova, size);
	if (ret)
		return ret;

	if (WARN_ON_ONCE(current_iommu_domain))
		return -EBUSY;

	current_iommu_domain = domain;
	while (iova < iova_end) {
		pkvm_pgtable_lookup_range(&domain->pgt, iova, iova_end - iova,
					  &mapped_iova, &mapped_size,
					  &phys, NULL);
		if (WARN_ON(mapped_iova != iova || !mapped_size ||
			    !VALID_PAGE(phys))) {
			ret = -EFAULT;
			break;
		}

		ret = pkvm_pgtable_unmap(&domain->pgt, iova, phys, mapped_size);
		if (WARN_ON(ret))
			break;

		/* The invalidation completes before the DMA pins are released. */
		pkvm_host_unuse_dma(phys, mapped_size);
		iova += mapped_size;
	}
	current_iommu_domain = NULL;

	return ret;
}
