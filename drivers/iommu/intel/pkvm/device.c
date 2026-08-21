// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2026 Google LLC */

#include <linux/hashtable.h>
#include <linux/pci.h>

#include "../iommu.h"

/*
 * Each requester ID, including a DMA alias, consumes one bounded record.
 * Intersecting alias lists can visit the same RID more than once, but they do
 * not create another record for it.
 *
 * The lock protects the array, allocation bitmap and hash. Callers serialize
 * the lifetime of a device record with the lock of its owning IOMMU.
 */
static DEFINE_HASHTABLE(iommu_device_hash, 8);
static DECLARE_BITMAP(iommu_device_bitmap, PKVM_MAX_IOMMU_DEVICES);
static struct pkvm_device iommu_devices[PKVM_MAX_IOMMU_DEVICES];
static DEFINE_PKVM_SPINLOCK(iommu_device_lock);

static u32 pkvm_device_key(u32 segment, u16 bdf)
{
	return (segment << 16) | bdf;
}

static struct pkvm_device *__pkvm_get_iommu_device(u32 segment, u16 bdf)
{
	struct pkvm_device *device;
	u32 key = pkvm_device_key(segment, bdf);

	hash_for_each_possible(iommu_device_hash, device, hnode, key) {
		if (device->info.segment == segment &&
		    PCI_DEVID(device->info.bus, device->info.devfn) == bdf)
			return device;
	}

	return NULL;
}

static int validate_device_info(const struct device_domain_info *info,
				bool *satc)
{
	struct intel_iommu *iommu = info->iommu;
	u16 bdf = PCI_DEVID(info->bus, info->devfn);

	if (info->segment != iommu->segment ||
	    info->ats_qdep > PCI_ATS_MAX_QDEP ||
	    (info->ats_enabled && !info->ats_supported))
		return -EINVAL;

	*satc = is_dev_in_satc(info->segment, bdf);
	if (!*satc && (info->ats_supported || info->ats_enabled))
		return -EPERM;

	return 0;
}

/* The caller must hold info->iommu->lock. */
struct pkvm_device *
pkvm_alloc_iommu_device(const struct device_domain_info *info)
{
	struct intel_iommu *iommu = info->iommu;
	struct pkvm_device *device;
	unsigned long index;
	u16 bdf = PCI_DEVID(info->bus, info->devfn);
	bool satc;
	int ret;

	ret = validate_device_info(info, &satc);
	if (ret)
		return ERR_PTR(ret);

	pkvm_spin_lock(&iommu_device_lock);
	if (__pkvm_get_iommu_device(info->segment, bdf)) {
		device = ERR_PTR(-EBUSY);
		goto out_unlock;
	}

	index = find_first_zero_bit(iommu_device_bitmap,
				    PKVM_MAX_IOMMU_DEVICES);
	if (index >= PKVM_MAX_IOMMU_DEVICES) {
		device = ERR_PTR(-ENOSPC);
		goto out_unlock;
	}

	__set_bit(index, iommu_device_bitmap);
	device = &iommu_devices[index];
	device->info = *info;
	if (satc && ecap_dit(iommu->ecap))
		device->info.pfsid = bdf;
	device->index = index;
	hash_add(iommu_device_hash, &device->hnode,
		 pkvm_device_key(info->segment, bdf));

out_unlock:
	pkvm_spin_unlock(&iommu_device_lock);
	return device;
}

/* The caller must hold iommu->lock while using the returned device. */
struct pkvm_device *
pkvm_get_iommu_device(struct intel_iommu *iommu, u32 segment,
		      u8 bus, u8 devfn)
{
	struct pkvm_device *device;
	u16 bdf = PCI_DEVID(bus, devfn);

	if (segment != iommu->segment)
		return ERR_PTR(-EINVAL);

	pkvm_spin_lock(&iommu_device_lock);
	device = __pkvm_get_iommu_device(segment, bdf);
	if (!device || device->info.iommu != iommu)
		device = ERR_PTR(-ENOENT);
	pkvm_spin_unlock(&iommu_device_lock);

	return device;
}

/* The caller must hold device->info.iommu->lock. */
void pkvm_remove_iommu_device(struct pkvm_device *device)
{
	u16 bdf = PCI_DEVID(device->info.bus, device->info.devfn);
	u32 segment = device->info.segment;

	pkvm_spin_lock(&iommu_device_lock);
	if (WARN_ON_ONCE(__pkvm_get_iommu_device(segment, bdf) != device))
		goto out_unlock;

	hash_del(&device->hnode);
	__clear_bit(device->index, iommu_device_bitmap);
	memset(device, 0, sizeof(*device));

out_unlock:
	pkvm_spin_unlock(&iommu_device_lock);
}
