// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright © 2026 Google.
 *
 */
#include <asm/kvm_pkvm.h>
#include <linux/pci.h>
#include "pkvm/mmu.h"
#include "pkvm/memory.h"
#include "pkvm/pkvm.h"
#include "pkvm/debug.h"
#include "../iommu.h"

int pkvm_iommu_qi_submit(u64 phys, u64 desc_gpa, u32 count, u32 options)
{
	struct intel_iommu *iommu = iommu_from_phys(phys);

	if (!iommu || !iommu->qi)
		return -EINVAL;

	/*
	 * This hypercall is temporary so don't bother to write protect
	 * desc_gpa (host to hyp donation). It will be removed in future
	 * patches being replaced by a dedicated hypercall specifically
	 * for submitting QI_IEC_TYPE.
	 */
	return qi_submit_sync(iommu, pkvm_host_gpa_to_virt(desc_gpa),
			      count, options);
}

int pkvm_iommu_clear_ce(struct clear_ce_data *data)
{
	struct intel_iommu *iommu = iommu_from_phys(data->phys);
	struct device_domain_info info = {};

	if (!iommu)
		return -EINVAL;

	info.segment = data->segment;
	info.bus = data->bus;
	info.devfn = data->devfn;
	info.iommu = iommu;

	return domain_context_clear_one(&info, data->bus, data->devfn);
}

static int accept_page_donation(struct intel_iommu *iommu,
				u64 *donation_page_gpa)
{
	phys_addr_t donation_page;
	int ret = 0;

	pkvm_spin_lock(&iommu->lock);
	if (!*donation_page_gpa || iommu->donation_page)
		goto out_unlock;

	donation_page = pkvm_host_gpa_to_phys(*donation_page_gpa);
	ret = pkvm_host_donate_hyp_share_ro(donation_page, VTD_PAGE_SIZE,
					    true);
	if (ret) {
		pkvm_err("iommu%d: failed to accept donated page: %d\n",
			 iommu->seq_id, ret);
		goto out_unlock;
	}

	iommu->donation_page = __pkvm_va(donation_page);
	*donation_page_gpa = 0;

out_unlock:
	pkvm_spin_unlock(&iommu->lock);
	return ret;
}

static int iommu_set_lm_ce(struct set_lm_ce_data *data)
{
	struct intel_iommu *iommu = iommu_from_phys(data->phys);
	struct device_domain_info info = {};
	struct dmar_domain domain = {};
	int ret;

	if (!iommu || !iommu->root_entry)
		return -EINVAL;

	info.segment = data->segment;
	info.bus = data->bus;
	info.devfn = data->devfn;
	info.ats_qdep = data->ats_qdep;
	info.ats_supported = data->ats_supported;
	info.ats_enabled = data->ats_enabled;
	info.iommu = iommu;

	ret = accept_page_donation(iommu, &data->donation_page_gpa);
	if (ret)
		return ret;

	domain.root_pa = pkvm_host_gpa_to_phys(data->root_gpa);
	domain.agaw = data->agaw;
	domain.use_first_level = false;

	ret = domain_context_mapping_one(&domain, &info, data->did);
	if (ret == -EEXIST)
		ret = 0;

	return ret;
}

int pkvm_iommu_set_lm_ce(struct set_lm_ce_data *in,
			 struct set_lm_ce_data *out)
{
	int ret = iommu_set_lm_ce(in);

	*out = *in;
	return ret;
}
