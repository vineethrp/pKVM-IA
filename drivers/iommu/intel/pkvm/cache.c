// SPDX-License-Identifier: GPL-2.0
/* Copyright(c) 2026 Google. */


#include <linux/hashtable.h>
#include <asm/pkvm_spinlock.h>
#include "../iommu.h"

#define MAX_CACHETAG_NUM 1024
static DECLARE_BITMAP(cache_tag_bitmap, MAX_CACHETAG_NUM);
static struct cache_tag cache_tags[MAX_CACHETAG_NUM];
static pkvm_spinlock_t cache_tag_lock = __PKVM_SPINLOCK_UNLOCKED;

struct cache_tag *pkvm_alloc_cache_tag(void)
{
	struct cache_tag *cache_tag = NULL;
	unsigned long index;

	pkvm_spin_lock(&cache_tag_lock);
	index = find_first_zero_bit(cache_tag_bitmap, MAX_CACHETAG_NUM);
	if (index < MAX_CACHETAG_NUM) {
		__set_bit(index, cache_tag_bitmap);
		cache_tag = &cache_tags[index];
		cache_tag->index = index;
		INIT_LIST_HEAD(&cache_tag->node);
	}
	pkvm_spin_unlock(&cache_tag_lock);

	return cache_tag;
}

void pkvm_free_cache_tag(struct cache_tag *cache_tag)
{
	pkvm_spin_lock(&cache_tag_lock);
	__clear_bit(cache_tag->index, cache_tag_bitmap);
	memset(cache_tag, 0, sizeof(struct cache_tag));
	pkvm_spin_unlock(&cache_tag_lock);
}

int pkvm_cache_assign_domain(struct dmar_domain *domain, u16 did,
			     struct device_domain_info *info, u32 pasid)
{
	struct dev_iommu dev_iommu = { 0 };
	struct device dev = { 0 };
	int ret;

	dev_iommu.priv = (void *)info;
	dev.iommu = &dev_iommu;

	ret = cache_tag_assign(domain, did, &dev, pasid, CACHE_TAG_IOTLB);

	if (!ret && info->ats_supported) {
		ret = cache_tag_assign(domain, did, &dev, pasid, CACHE_TAG_DEVTLB);
		if (ret)
			cache_tag_unassign(domain, did, &dev, pasid, CACHE_TAG_IOTLB);
	}

	return ret;
}

void pkvm_cache_unassign_domain(struct dmar_domain *domain, u16 did,
				struct device_domain_info *info, u32 pasid)
{
	struct dev_iommu dev_iommu = { 0 };
	struct device dev = { 0 };

	dev_iommu.priv = (void *)info;
	dev.iommu = &dev_iommu;

	cache_tag_unassign(domain, did, &dev, pasid, CACHE_TAG_IOTLB);
	if (info->ats_supported)
		cache_tag_unassign(domain, did, &dev, pasid, CACHE_TAG_DEVTLB);
}
