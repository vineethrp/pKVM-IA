// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2026 Google LLC
 */
#include <asm/kvm_pkvm.h>

#include "pkvm/debug.h"
#include "pkvm/mmu.h"
#include "iommu.h"

static struct intel_iommu iommus[PKVM_MAX_IOMMUS];
static unsigned int nr_iommus;
unsigned int iommu_pgsz_mask;
unsigned int iommu_pglvl_mask;

static bool ranges_overlap(u64 start_a, u64 size_a, u64 start_b, u64 size_b)
{
	return start_a < start_b + size_b && start_b < start_a + size_a;
}

int __init pkvm_prepare_iommus(const struct pkvm_iommu_info *infos,
			       unsigned int count)
{
	unsigned int pgsz_mask = BIT(PG_LEVEL_4K) |
				 BIT(PG_LEVEL_2M) |
				 BIT(PG_LEVEL_1G);
	unsigned int pglvl_mask = PKVM_IOMMU_PGT_4LEVEL |
				  PKVM_IOMMU_PGT_5LEVEL;
	unsigned int i, j;

	if (!infos || !count || count > ARRAY_SIZE(iommus) || nr_iommus)
		return -EINVAL;

	for (i = 0; i < count; i++) {
		const struct pkvm_iommu_info *info = &infos[i];
		unsigned int unit_pgsz_mask = BIT(PG_LEVEL_4K);
		unsigned int unit_pglvl_mask;

		if (!info->reg_phys || !PAGE_ALIGNED(info->reg_phys) ||
		    !info->reg_size || !PAGE_ALIGNED(info->reg_size) ||
		    info->reg_phys + info->reg_size < info->reg_phys)
			return -EINVAL;

		unit_pglvl_mask = cap_sagaw(info->cap) &
				  (PKVM_IOMMU_PGT_4LEVEL |
				   PKVM_IOMMU_PGT_5LEVEL);
		if (!unit_pglvl_mask)
			return -EOPNOTSUPP;

		if (cap_super_page_val(info->cap) & BIT(0))
			unit_pgsz_mask |= BIT(PG_LEVEL_2M);
		if (cap_super_page_val(info->cap) & BIT(1))
			unit_pgsz_mask |= BIT(PG_LEVEL_1G);

		pglvl_mask &= unit_pglvl_mask;
		pgsz_mask &= unit_pgsz_mask;

		for (j = 0; j < i; j++) {
			if (info->seq_id == infos[j].seq_id ||
			    ranges_overlap(info->reg_phys, info->reg_size,
					   infos[j].reg_phys,
					   infos[j].reg_size))
				return -EINVAL;
		}
	}

	if (!pglvl_mask)
		return -EOPNOTSUPP;

	for (i = 0; i < count; i++) {
		struct intel_iommu *iommu = &iommus[i];
		const struct pkvm_iommu_info *info = &infos[i];

		iommu->reg_phys = info->reg_phys;
		iommu->reg_size = info->reg_size;
		iommu->cap = info->cap;
		iommu->ecap = info->ecap;
		iommu->segment = info->segment;
		iommu->seq_id = info->seq_id;
		iommu->agaw = info->agaw;
		iommu->msagaw = info->msagaw;
	}

	nr_iommus = count;
	iommu_pglvl_mask = pglvl_mask;
	iommu_pgsz_mask = pgsz_mask;
	return 0;
}

int pkvm_intel_iommu_init(void)
{
	unsigned int i;

	for (i = 0; i < nr_iommus; i++) {
		struct intel_iommu *iommu = &iommus[i];
		int ret;

		iommu->reg = __pkvm_va(iommu->reg_phys);
		ret = pkvm_hyp_mmu_map((unsigned long)iommu->reg,
				       iommu->reg_phys, iommu->reg_size,
				       (u64)pgprot_val(PAGE_KERNEL_IO_NOCACHE));
		if (ret) {
			pkvm_err("iommu%d: failed to map MMIO: %d\n",
				 iommu->seq_id, ret);
			return ret;
		}

		pkvm_spin_lock_init(&iommu->lock);
		iommu->vgsts = readl(iommu->reg + DMAR_GSTS_REG);
	}

	return 0;
}
