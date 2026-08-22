// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2026 Google LLC */

#include <linux/bitmap.h>

#include "../iommu.h"

#define PKVM_MAX_IOMMU_CACHE_TAGS	1024

static DECLARE_BITMAP(cache_tag_bitmap, PKVM_MAX_IOMMU_CACHE_TAGS);
static struct cache_tag cache_tags[PKVM_MAX_IOMMU_CACHE_TAGS];
static DEFINE_PKVM_SPINLOCK(cache_tag_lock);

struct cache_tag *pkvm_alloc_cache_tag(void)
{
	struct cache_tag *tag = NULL;
	unsigned long index;

	pkvm_spin_lock(&cache_tag_lock);
	index = find_first_zero_bit(cache_tag_bitmap,
				    PKVM_MAX_IOMMU_CACHE_TAGS);
	if (index < PKVM_MAX_IOMMU_CACHE_TAGS) {
		__set_bit(index, cache_tag_bitmap);
		tag = &cache_tags[index];
		tag->index = index;
		INIT_LIST_HEAD(&tag->node);
	}
	pkvm_spin_unlock(&cache_tag_lock);

	return tag;
}

void pkvm_free_cache_tag(struct cache_tag *tag)
{
	pkvm_spin_lock(&cache_tag_lock);
	__clear_bit(tag->index, cache_tag_bitmap);
	memset(tag, 0, sizeof(*tag));
	pkvm_spin_unlock(&cache_tag_lock);
}
