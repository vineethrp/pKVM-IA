/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright 2026 Google LLC
 */

#ifndef _PKVM_INTEL_IOMMU_H_
#define _PKVM_INTEL_IOMMU_H_

#ifdef CONFIG_PKVM_INTEL
#include <asm/kvm_pkvm.h>
#endif

#ifdef __PKVM_HYP__
#include <asm/pkvm_spinlock.h>
#endif

#define PKVM_MAX_IOMMUS	16
#define PKVM_MAX_SATC_DEVS	16

/* Page-table levels represented by the IOMMU SAGAW capability. */
#define PKVM_IOMMU_PGT_4LEVEL	BIT(2)
#define PKVM_IOMMU_PGT_5LEVEL	BIT(3)

struct pkvm_iommu_info {
	u64 reg_phys;
	u64 reg_size;
	u64 cap;
	u64 ecap;
	u16 segment;
	bool scalable_mode;
	int seq_id;
	int agaw;
	int msagaw;
};

struct pkvm_iommu_device_id {
	u16 segment;
	u16 bdf;
};

struct qi_desc;
struct intel_iommu;

#ifndef __PKVM_HYP__
#include <asm/kvm_host.h>
#endif

#ifdef CONFIG_PKVM_INTEL
PKVM_DECLARE(int, pkvm_prepare_iommus,
	     (const struct pkvm_iommu_info *infos, unsigned int nr_iommus,
	      const struct pkvm_iommu_device_id *satc_devs,
	      unsigned int nr_satc_devs));

#ifndef __PKVM_HYP__
u64 pkvm_readq(struct intel_iommu *iommu, unsigned long offset);
u32 pkvm_readl(struct intel_iommu *iommu, unsigned long offset);
void pkvm_writeq(struct intel_iommu *iommu, unsigned long offset, u64 val);
void pkvm_writel(struct intel_iommu *iommu, unsigned long offset, u32 val);

int __init pkvm_scan_satc_devs(struct pkvm_iommu_device_id *satc_devs,
			       unsigned int *nr_satc_devs,
			       unsigned int max_satc_devs);

int __init pkvm_host_prepare_iommu(void);
int __init pkvm_host_init_iommu(void);

int pkvm_qi_submit_sync(struct intel_iommu *iommu, struct qi_desc *desc,
			unsigned int count, unsigned long options);
#else
extern unsigned int iommu_pglvl_mask;
extern unsigned int iommu_pgsz_mask;

static inline bool iommu_supports_2m_page(void)
{
	return iommu_pgsz_mask & BIT(PG_LEVEL_2M);
}

static inline bool iommu_supports_1g_page(void)
{
	return iommu_pgsz_mask & BIT(PG_LEVEL_1G);
}

static inline bool iommu_supports_5levels(void)
{
	return iommu_pglvl_mask & PKVM_IOMMU_PGT_5LEVEL;
}

static inline bool iommu_supports_4levels(void)
{
	return iommu_pglvl_mask & PKVM_IOMMU_PGT_4LEVEL;
}

struct intel_iommu *iommu_from_phys(u64 phys);

static inline bool is_iommu_mmio(u64 phys)
{
	return !!iommu_from_phys(phys);
}

bool overlaps_iommu_mmio(u64 phys, u64 size);
bool is_dev_in_satc(u16 segment, u16 bdf);

int pkvm_intel_iommu_init(void);
int pkvm_iommu_mmio_read(u64 phys, int len, u64 *val);
int pkvm_iommu_mmio_write(u64 phys, int len, u64 val);
int pkvm_iommu_qi_submit(u64 phys, u64 desc_gpa, u32 count, u32 options);
#endif /* !__PKVM_HYP__ */
#else /* !CONFIG_PKVM_INTEL */
static inline int pkvm_qi_submit_sync(struct intel_iommu *iommu,
				      struct qi_desc *desc, unsigned int count,
				      unsigned long options)
{
	return -EOPNOTSUPP;
}
#endif /* CONFIG_PKVM_INTEL */

#endif /* _PKVM_INTEL_IOMMU_H_ */
