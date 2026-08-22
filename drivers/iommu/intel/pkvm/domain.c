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

static bool domain_compatible(struct dmar_domain *domain,
			      struct intel_iommu *iommu)
{
	if (sm_supported(iommu) && !ecap_slts(iommu->ecap))
		return false;

	return cap_sagaw(iommu->cap) & BIT(domain->agaw);
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
pkvm_alloc_iommu_domain(phys_addr_t root, u8 agaw, bool use_first_level)
{
	struct dmar_domain *domain;
	unsigned long index;

	if (!root || !PAGE_ALIGNED(root) || agaw > 3)
		return ERR_PTR(-EINVAL);

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
	domain->use_first_level = use_first_level;
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
