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
#include "pkvm/vmx/ept.h"
#include "pkvm/debug.h"
#include "../iommu.h"
#include "../pasid.h"

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

int pkvm_iommu_alloc_domain(u64 iommu_phys, u64 root_gpa, u8 agaw,
			    bool use_first_level)
{
	phys_addr_t root = pkvm_host_gpa_to_phys(root_gpa);
	struct intel_iommu *iommu = iommu_from_phys(iommu_phys);
	struct dmar_domain *domain;

	if (!iommu)
		return -EINVAL;

	/*
	 * TODO: Protect and refcount the root after page-table updates are
	 * routed through pKVM.
	 */
	domain = pkvm_alloc_iommu_domain(iommu, root, agaw, use_first_level);
	if (IS_ERR(domain))
		return PTR_ERR(domain);

	return 0;
}

int pkvm_iommu_free_domain(u64 root_gpa)
{
	phys_addr_t root = pkvm_host_gpa_to_phys(root_gpa);

	return pkvm_free_iommu_domain(root);
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
	struct dmar_domain *target = &domain;
	phys_addr_t root;
	int level;
	int ret;

	if (!iommu || !iommu->root_entry || sm_supported(iommu))
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

	if (data->did == FLPT_DEFAULT_DID) {
		if (data->root_gpa)
			return -EINVAL;
		root = pkvm_host_ept_root();
		level = pkvm_host_ept_level();
		if (root == INVALID_PAGE || level < 2 || level > 5)
			return -EINVAL;
		domain.agaw = level - 2;
	} else {
		root = pkvm_host_gpa_to_phys(data->root_gpa);
		if (!root || !PAGE_ALIGNED(root) || data->agaw > 3)
			return -EINVAL;
		domain.agaw = data->agaw;
	}

	if (!(cap_sagaw(iommu->cap) & BIT(domain.agaw)))
		return -EINVAL;
	domain.root_pa = root;
	domain.use_first_level = false;
	if (data->did == FLPT_DEFAULT_DID) {
		target = pkvm_get_iommu_domain(0, data->did, iommu);
		if (!target)
			return -EINVAL;
	}

	ret = domain_context_mapping_one(target, &info, data->did);
	if (target != &domain)
		pkvm_put_iommu_domain(target);
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

static int pasid_dir_size(u32 max_pasid, unsigned long *size)
{
	if (max_pasid < PASID_TBL_ENTRIES * 512 ||
	    max_pasid > PASID_MAX || !is_power_of_2(max_pasid))
		return -EINVAL;

	*size = max_pasid >> (PASID_PDE_SHIFT - 3);
	return 0;
}

static int iommu_set_sm_ce(struct set_sm_ce_data *data)
{
	struct intel_iommu *iommu = iommu_from_phys(data->phys);
	struct device_domain_info info = {};
	struct pasid_table table = {};
	phys_addr_t pasid_table_pa;
	unsigned long size;
	int ret;

	if (!iommu || !iommu->root_entry || !sm_supported(iommu))
		return -EINVAL;

	ret = pasid_dir_size(data->max_pasid, &size);
	if (ret)
		return ret;

	pasid_table_pa = pkvm_host_gpa_to_phys(data->pasid_table_gpa);
	if (!pasid_table_pa || !PAGE_ALIGNED(pasid_table_pa))
		return -EINVAL;

	info.segment = data->segment;
	info.bus = data->bus;
	info.devfn = data->devfn;
	info.ats_qdep = data->ats_qdep;
	info.ats_enabled = data->ats_enabled;
	info.ats_supported = data->ats_supported;
	info.pasid_supported = data->pasid_supported;
	info.pasid_enabled = data->pasid_enabled;
	info.pri_supported = data->pri_supported;
	info.pri_enabled = data->pri_enabled;
	info.iommu = iommu;
	table.table = __pkvm_va(pasid_table_pa);
	table.max_pasid = data->max_pasid;
	info.pasid_table = &table;

	ret = accept_page_donation(iommu, &data->donation_page_gpa);
	if (ret)
		return ret;

	ret = pkvm_host_donate_hyp_share_ro(pasid_table_pa, size, true);
	if (ret)
		return ret;

	__iommu_flush_cache(iommu, table.table, size);
	ret = device_pasid_table_setup(&info, data->bus, data->devfn);
	if (ret) {
		pkvm_hyp_donate_host(pasid_table_pa, size, false);
		if (ret == -EEXIST)
			ret = 0;
	}

	return ret;
}

int pkvm_iommu_set_sm_ce(struct set_sm_ce_data *in,
			 struct set_sm_ce_data *out)
{
	int ret = iommu_set_sm_ce(in);

	*out = *in;
	return ret;
}

static int iommu_pasid_setup_fl(struct pasid_setup_fl_data *data)
{
	struct intel_iommu *iommu = iommu_from_phys(data->phys);
	struct device_domain_info info = {};
	struct dmar_domain domain = {};
	phys_addr_t fsptptr;
	int ret;

	if (!iommu || !iommu->root_entry || !sm_supported(iommu))
		return -EINVAL;

	if (data->did == FLPT_DEFAULT_DID)
		return -EPERM;

	if (data->flags & ~(PASID_FLAG_FL5LP | PASID_FLAG_PAGE_SNOOP |
			    PASID_FLAG_PWSNP))
		return -EINVAL;

	fsptptr = pkvm_host_gpa_to_phys(data->fsptptr_gpa);
	if (!fsptptr || !PAGE_ALIGNED(fsptptr))
		return -EINVAL;

	domain.root_pa = fsptptr;
	domain.use_first_level = true;

	info.segment = data->segment;
	info.bus = data->bus;
	info.devfn = data->devfn;
	info.iommu = iommu;

	ret = accept_page_donation(iommu, &data->donation_page_gpa);
	if (ret)
		return ret;

	ret = intel_pasid_setup_first_level(iommu, &info, &domain, fsptptr,
					    data->pasid, data->did,
					    data->flags);
	return ret;
}

int pkvm_iommu_pasid_setup_fl(struct pasid_setup_fl_data *in,
			      struct pasid_setup_fl_data *out)
{
	int ret = iommu_pasid_setup_fl(in);

	*out = *in;
	return ret;
}

static int iommu_pasid_setup_sl(struct pasid_setup_sl_data *data)
{
	struct intel_iommu *iommu = iommu_from_phys(data->phys);
	struct device_domain_info info = {};
	struct dmar_domain domain = {};
	struct dmar_domain *target = &domain;
	phys_addr_t root;
	int level;
	int ret;

	if (!iommu || !iommu->root_entry || !sm_supported(iommu))
		return -EINVAL;

	if (data->did == FLPT_DEFAULT_DID) {
		if (data->root_gpa)
			return -EINVAL;
		root = pkvm_host_ept_root();
		level = pkvm_host_ept_level();
		if (root == INVALID_PAGE || level < 2 || level > 5)
			return -EINVAL;
		domain.agaw = level - 2;
	} else {
		root = pkvm_host_gpa_to_phys(data->root_gpa);
		if (!root || !PAGE_ALIGNED(root) || data->agaw > 3)
			return -EINVAL;
		domain.agaw = data->agaw;
	}

	if (!(cap_sagaw(iommu->cap) & BIT(domain.agaw)))
		return -EINVAL;
	domain.root_pa = root;
	domain.use_first_level = false;
	if (data->did == FLPT_DEFAULT_DID) {
		target = pkvm_get_iommu_domain(0, data->did, iommu);
		if (!target)
			return -EINVAL;
	}

	info.segment = data->segment;
	info.bus = data->bus;
	info.devfn = data->devfn;
	info.iommu = iommu;

	ret = accept_page_donation(iommu, &data->donation_page_gpa);
	if (ret)
		return ret;

	ret = intel_pasid_setup_second_level(iommu, target, &info,
					     data->did, data->pasid);
	if (target != &domain)
		pkvm_put_iommu_domain(target);
	return ret;
}

int pkvm_iommu_pasid_setup_sl(struct pasid_setup_sl_data *in,
			      struct pasid_setup_sl_data *out)
{
	int ret = iommu_pasid_setup_sl(in);

	*out = *in;
	return ret;
}

int pkvm_iommu_pasid_teardown(struct pasid_teardown_data *data)
{
	struct intel_iommu *iommu = iommu_from_phys(data->phys);
	struct pkvm_device *device;
	struct pasid_table *table;
	int ret;

	if (!iommu || !iommu->root_entry || !sm_supported(iommu))
		return -EINVAL;

	pkvm_spin_lock(&iommu->lock);
	device = pkvm_get_iommu_device(iommu, data->segment,
				       data->bus, data->devfn);
	if (IS_ERR(device)) {
		ret = PTR_ERR(device);
		goto out_unlock;
	}

	table = device->info.pasid_table;
	if (!table || data->pasid >= table->max_pasid) {
		ret = -EINVAL;
		goto out_unlock;
	}

	/* Keep the device alive until all PASID invalidations complete. */
	ret = intel_pasid_tear_down_entry(iommu, device, data->pasid, false);

out_unlock:
	pkvm_spin_unlock(&iommu->lock);
	return ret;
}
