/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright 2026 Google LLC
 */

#ifndef _PKVM_INTEL_IOMMU_H_
#define _PKVM_INTEL_IOMMU_H_

#ifdef CONFIG_PKVM_INTEL
#include <asm/kvm_pkvm.h>
#endif

#define PKVM_MAX_IOMMUS	16

/* Page-table levels represented by the IOMMU SAGAW capability. */
#define PKVM_IOMMU_PGT_4LEVEL	BIT(2)
#define PKVM_IOMMU_PGT_5LEVEL	BIT(3)

struct pkvm_iommu_info {
	u64 reg_phys;
	u64 reg_size;
	u64 cap;
	u64 ecap;
	u16 segment;
	int seq_id;
	int agaw;
	int msagaw;
};

#ifndef __PKVM_HYP__
#include <asm/kvm_host.h>
#endif

#ifdef CONFIG_PKVM_INTEL
PKVM_DECLARE(int, pkvm_prepare_iommus,
	     (const struct pkvm_iommu_info *infos, unsigned int nr_iommus));

#ifndef __PKVM_HYP__
struct intel_iommu;

u64 pkvm_readq(struct intel_iommu *iommu, unsigned long offset);
u32 pkvm_readl(struct intel_iommu *iommu, unsigned long offset);
void pkvm_writeq(struct intel_iommu *iommu, unsigned long offset, u64 val);
void pkvm_writel(struct intel_iommu *iommu, unsigned long offset, u32 val);

int __init pkvm_host_prepare_iommu(void);
int __init pkvm_host_init_iommu(void);
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

int pkvm_intel_iommu_init(void);
int pkvm_iommu_mmio_read(u64 phys, int len, u64 *val);
int pkvm_iommu_mmio_write(u64 phys, int len, u64 val);
#endif
#endif /* CONFIG_PKVM_INTEL */

#endif /* _PKVM_INTEL_IOMMU_H_ */
