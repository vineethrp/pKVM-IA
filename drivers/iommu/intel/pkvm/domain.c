// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2026 Google LLC */

#include <linux/hashtable.h>

#include "pkvm/debug.h"
#include "pkvm/pkvm.h"
#include "pkvm/vmx/ept.h"

#include "../iommu.h"

static DEFINE_HASHTABLE(iommu_domain_hash, 8);
static DECLARE_BITMAP(iommu_domain_bitmap, PKVM_MAX_IOMMU_DOMAINS);
static struct dmar_domain iommu_domains[PKVM_MAX_IOMMU_DOMAINS];
static DEFINE_PKVM_SPINLOCK(iommu_domain_lock);
static struct dmar_domain passthrough_domain;

static unsigned int domain_pgsize_mask(struct intel_iommu *iommu,
				       bool use_first_level)
{
	unsigned int mask = BIT(PG_LEVEL_4K) | BIT(PG_LEVEL_2M);

	if (use_first_level) {
		if (cap_fl1gp_support(iommu->cap))
			mask |= BIT(PG_LEVEL_1G);
	} else {
		unsigned int superpage = cap_super_page_val(iommu->cap);

		if (!(superpage & BIT(0)))
			mask &= ~BIT(PG_LEVEL_2M);
		if (superpage & BIT(1))
			mask |= BIT(PG_LEVEL_1G);
	}

	return mask;
}

static int domain_iova_bits(struct intel_iommu *iommu,
			    bool use_first_level, u8 agaw, u8 *iova_bits)
{
	unsigned int width;

	if (agaw != 2 && agaw != 3)
		return -EINVAL;

	if (use_first_level) {
		if (!sm_supported(iommu) || !ecap_flts(iommu->ecap) ||
		    (agaw == 3 && !cap_fl5lp_support(iommu->cap)))
			return -EINVAL;
	} else if (sm_supported(iommu) && !ecap_slts(iommu->ecap)) {
		return -EINVAL;
	} else if (!(cap_sagaw(iommu->cap) & BIT(agaw))) {
		return -EINVAL;
	}

	width = min_t(unsigned int, 30 + agaw * LEVEL_STRIDE,
		      cap_mgaw(iommu->cap));
	if (use_first_level)
		width--;
	if (width < VTD_PAGE_SHIFT)
		return -EINVAL;

	*iova_bits = width;
	return 0;
}

static bool iommu_paging_structure_coherency(struct intel_iommu *iommu)
{
	return sm_supported(iommu) ?
			ecap_smpwc(iommu->ecap) : ecap_coherent(iommu->ecap);
}

static bool domain_compatible(struct dmar_domain *domain,
			      struct intel_iommu *iommu)
{
	u8 iova_bits;

	if (domain == &passthrough_domain)
		return (!sm_supported(iommu) || ecap_slts(iommu->ecap)) &&
		       (cap_sagaw(iommu->cap) & BIT(domain->agaw));

	if (domain_iova_bits(iommu, domain->use_first_level, domain->agaw,
			     &iova_bits))
		return false;
	if (domain->iova_bits > iova_bits)
		return false;
	if (domain->pgt.cap.allowed_pgsz &
	    ~domain_pgsize_mask(iommu, domain->use_first_level))
		return false;
	if (!iommu_paging_structure_coherency(iommu) &&
	    !domain->needs_cpu_flush)
		return false;

	return true;
}

int pkvm_iommu_domain_init(void)
{
	int level = pkvm_host_ept_level();

	if (level < 2 || level > 6)
		return -EINVAL;

	passthrough_domain.root_pa = pkvm_host_ept_root();
	passthrough_domain.agaw = level - 2;
	pkvm_spin_lock_init(&passthrough_domain.lock);
	pkvm_spin_lock_init(&passthrough_domain.cache_lock);
	INIT_LIST_HEAD(&passthrough_domain.cache_tags);
	passthrough_domain.qi_batch = &passthrough_domain._qi_batch;

	return 0;
}

void pkvm_iommu_pt_flush(unsigned long paddr, unsigned long size)
{
	if (passthrough_domain.qi_batch)
		cache_tag_flush_range(&passthrough_domain, paddr,
				      paddr + size - 1, 0);
}

static struct dmar_domain *
__pkvm_get_iommu_domain(phys_addr_t root, bool take_ref)
{
	struct dmar_domain *domain;

	hash_for_each_possible(iommu_domain_hash, domain, hnode, root) {
		if (domain->root_pa != root)
			continue;

		if (take_ref &&
		    WARN_ON_ONCE(!atomic_inc_not_zero(&domain->refcount)))
			return NULL;

		return domain;
	}

	return NULL;
}

struct dmar_domain *
pkvm_get_iommu_domain(phys_addr_t root, u16 did,
		      struct intel_iommu *iommu)
{
	struct dmar_domain *domain;

	if (did == FLPT_DEFAULT_DID) {
		domain = &passthrough_domain;
	} else {
		pkvm_spin_lock(&iommu_domain_lock);
		domain = __pkvm_get_iommu_domain(root, true);
		pkvm_spin_unlock(&iommu_domain_lock);
	}

	if (domain && !domain_compatible(domain, iommu)) {
		pkvm_put_iommu_domain(domain);
		domain = NULL;
	}

	return domain;
}

void pkvm_put_iommu_domain(struct dmar_domain *domain)
{
	/* The static passthrough domain has a permanent lifetime. */
	if (domain == &passthrough_domain)
		return;

	WARN_ON_ONCE(atomic_dec_if_positive(&domain->refcount) <= 0);
}

struct dmar_domain *
pkvm_alloc_iommu_domain(struct intel_iommu *iommu, phys_addr_t root, u8 agaw,
			bool use_first_level)
{
	struct dmar_domain *domain;
	u8 iova_bits;
	unsigned long index;
	int ret;

	if (!iommu || !root || !PAGE_ALIGNED(root))
		return ERR_PTR(-EINVAL);

	ret = domain_iova_bits(iommu, use_first_level, agaw, &iova_bits);
	if (ret)
		return ERR_PTR(ret);

	pkvm_spin_lock(&iommu_domain_lock);
	if (__pkvm_get_iommu_domain(root, false)) {
		domain = ERR_PTR(-EEXIST);
		goto out_unlock;
	}

	index = find_first_zero_bit(iommu_domain_bitmap,
				    PKVM_MAX_IOMMU_DOMAINS);
	if (index >= PKVM_MAX_IOMMU_DOMAINS) {
		domain = ERR_PTR(-ENOMEM);
		goto out_unlock;
	}

	__set_bit(index, iommu_domain_bitmap);
	domain = &iommu_domains[index];
	domain->root_pa = root;
	domain->agaw = agaw;
	domain->iova_bits = iova_bits;
	domain->use_first_level = use_first_level;
	domain->needs_cpu_flush = !iommu_paging_structure_coherency(iommu);
	pkvm_iommu_pgtable_init(domain,
				domain_pgsize_mask(iommu, use_first_level));
	domain->index = index;
	atomic_set(&domain->refcount, 1);
	pkvm_spin_lock_init(&domain->lock);
	pkvm_spin_lock_init(&domain->cache_lock);
	INIT_LIST_HEAD(&domain->cache_tags);
	domain->qi_batch = &domain->_qi_batch;
	hash_add(iommu_domain_hash, &domain->hnode, root);

out_unlock:
	pkvm_spin_unlock(&iommu_domain_lock);
	return domain;
}

int pkvm_free_iommu_domain(phys_addr_t root)
{
	struct dmar_domain *domain;
	int ret = 0;

	pkvm_spin_lock(&iommu_domain_lock);
	domain = __pkvm_get_iommu_domain(root, false);
	if (!domain) {
		ret = -EINVAL;
		goto out_unlock;
	}

	if (atomic_cmpxchg(&domain->refcount, 1, 0) != 1) {
		ret = -EBUSY;
		goto out_unlock;
	}

	hash_del(&domain->hnode);
	__clear_bit(domain->index, iommu_domain_bitmap);
	memset(domain, 0, sizeof(*domain));

out_unlock:
	pkvm_spin_unlock(&iommu_domain_lock);

	return ret;
}
