// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2026 Google LLC
 */

#define pr_fmt(fmt) "DMAR: pKVM: " fmt

#include <linux/kernel.h>
#include "iommu.h"

u64 pkvm_readq(struct intel_iommu *iommu, unsigned long offset)
{
	union pkvm_hc_data data = {};
	int ret;

	ret = pkvm_hypercall_out(iommu_mmio_read, &data,
				 iommu->reg_phys + offset, sizeof(u64));
	if (ret)
		pr_err("IOMMU MMIO read failed at %pa+%#lx: %d\n",
		       &iommu->reg_phys, offset, ret);

	return data.iommu_mmio_read.val;
}

u32 pkvm_readl(struct intel_iommu *iommu, unsigned long offset)
{
	union pkvm_hc_data data = {};
	int ret;

	ret = pkvm_hypercall_out(iommu_mmio_read, &data,
				 iommu->reg_phys + offset, sizeof(u32));
	if (ret)
		pr_err("IOMMU MMIO read failed at %pa+%#lx: %d\n",
		       &iommu->reg_phys, offset, ret);

	return data.iommu_mmio_read.val;
}

void pkvm_writeq(struct intel_iommu *iommu, unsigned long offset, u64 val)
{
	int ret;

	ret = pkvm_hypercall(iommu_mmio_write, iommu->reg_phys + offset,
			     sizeof(u64), val);
	if (ret)
		pr_err("IOMMU MMIO write failed at %pa+%#lx: %d\n",
		       &iommu->reg_phys, offset, ret);
}

void pkvm_writel(struct intel_iommu *iommu, unsigned long offset, u32 val)
{
	int ret;

	ret = pkvm_hypercall(iommu_mmio_write, iommu->reg_phys + offset,
			     sizeof(u32), val);
	if (ret)
		pr_err("IOMMU MMIO write failed at %pa+%#lx: %d\n",
		       &iommu->reg_phys, offset, ret);
}

int __init pkvm_host_prepare_iommu(void)
{
	struct pkvm_iommu_info infos[PKVM_MAX_IOMMUS];
	struct dmar_drhd_unit *drhd;
	struct intel_iommu *iommu;
	unsigned int nr_iommus = 0;
	int ret;

	down_write(&dmar_global_lock);
	ret = dmar_table_init();
	if (ret) {
		pr_err("Failed to initialize DMAR table: %d\n", ret);
		goto out;
	}

	ret = -ENODEV;
	for_each_iommu(iommu, drhd) {
		struct pkvm_iommu_info *info;

		if (nr_iommus == ARRAY_SIZE(infos)) {
			pr_warn("More than %zu IOMMUs are not supported\n",
				ARRAY_SIZE(infos));
			ret = -E2BIG;
			goto out;
		}

		if (drhd->ignored) {
			pr_warn("iommu%d is ignored\n", iommu->seq_id);
			ret = -EOPNOTSUPP;
			goto out;
		}

		if (readl(iommu->reg + DMAR_GSTS_REG) & DMA_GSTS_TES) {
			pr_warn("iommu%d has translation enabled\n",
				iommu->seq_id);
			ret = -EBUSY;
			goto out;
		}

		if (cap_rwbf(iommu->cap)) {
			pr_warn("iommu%d requires write-buffer flushing\n",
				iommu->seq_id);
			ret = -EOPNOTSUPP;
			goto out;
		}

		if (!ecap_qis(iommu->ecap)) {
			pr_warn("iommu%d lacks queued invalidation\n",
				iommu->seq_id);
			ret = -EOPNOTSUPP;
			goto out;
		}

		info = &infos[nr_iommus++];
		info->reg_phys = iommu->reg_phys;
		info->reg_size = iommu->reg_size;
		info->cap = iommu->cap;
		info->ecap = iommu->ecap;
		info->segment = iommu->segment;
		info->scalable_mode = sm_supported(iommu);
		info->seq_id = iommu->seq_id;
		info->agaw = iommu->agaw;
		info->msagaw = iommu->msagaw;
	}

	if (!nr_iommus)
		goto out;

	ret = pkvm_sym(pkvm_prepare_iommus)(infos, nr_iommus);
out:
	up_write(&dmar_global_lock);
	return ret;
}

int __init pkvm_host_init_iommu(void)
{
	int ret;

	ret = intel_iommu_init();
	if (ret)
		pr_err("IOMMU initialization failed: %d\n", ret);
	else
		pr_info("%s IOMMU initialized\n",
			pkvm_enabled() ? "Protected" : "Host");

	return ret;
}

int pkvm_qi_submit_sync(struct intel_iommu *iommu, struct qi_desc *desc,
			unsigned int count, unsigned long options)
{
	struct qi_desc *desc_ptr;
	int ret;

	desc_ptr = kcalloc(count, sizeof(struct qi_desc), GFP_ATOMIC);
	if (!desc_ptr)
		return -ENOMEM;

	memcpy(desc_ptr, desc, count * sizeof(struct qi_desc));
	ret = pkvm_hypercall(iommu_qi_submit, iommu->reg_phys,
			     virt_to_phys(desc_ptr), count, options);
	kfree(desc_ptr);
	return ret;
}
