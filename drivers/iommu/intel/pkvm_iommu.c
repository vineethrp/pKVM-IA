// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2026 Google LLC
 */

#define pr_fmt(fmt) "DMAR: pKVM: " fmt

#include <linux/kernel.h>
#include "iommu.h"
#include "pasid.h"
#include "../iommu-pages.h"

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
	struct pkvm_iommu_device_id satc_devs[PKVM_MAX_SATC_DEVS];
	struct dmar_drhd_unit *drhd;
	struct intel_iommu *iommu;
	unsigned int nr_satc_devs;
	unsigned int nr_iommus = 0;
	int ret;

	down_write(&dmar_global_lock);
	ret = dmar_table_init();
	if (ret) {
		pr_err("Failed to initialize DMAR table: %d\n", ret);
		goto out;
	}

	ret = dmar_dev_scope_init();
	if (ret) {
		pr_err("Failed to initialize DMAR device scopes: %d\n", ret);
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

		if (!ecap_ir_support(iommu->ecap)) {
			pr_warn("iommu%d lacks interrupt remapping\n",
				iommu->seq_id);
			ret = -EOPNOTSUPP;
			goto out;
		}

		if (!ecap_eim_support(iommu->ecap)) {
			pr_warn("iommu%d lacks extended interrupt mode\n",
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
		info->superpage_enabled = intel_iommu_superpage_enabled();
		info->seq_id = iommu->seq_id;
		info->agaw = iommu->agaw;
		info->msagaw = iommu->msagaw;
	}

	if (!nr_iommus)
		goto out;

	ret = pkvm_scan_satc_devs(satc_devs, &nr_satc_devs,
				  ARRAY_SIZE(satc_devs));
	if (ret)
		goto out;

	ret = pkvm_sym(pkvm_prepare_iommus)(infos, nr_iommus,
					    satc_devs, nr_satc_devs);
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

int pkvm_iec_flush(struct intel_iommu *iommu, bool global, int index, int mask)
{
	return pkvm_hypercall(iommu_iec_flush, iommu->reg_phys, index, mask,
			      global);
}

int pkvm_alloc_domain(struct intel_iommu *iommu, void *root, u8 agaw,
		      bool use_first_level)
{
	return pkvm_hypercall(iommu_alloc_domain, iommu->reg_phys,
			      virt_to_phys(root), agaw, use_first_level);
}

static void *host_va(phys_addr_t phys)
{
	return __va(phys);
}

static void free_domain_memcache(struct pkvm_memcache *mc)
{
	while (mc->count)
		iommu_free_pages(pop_pkvm_memcache_page(mc, host_va));
}

int pkvm_free_domain(void *root)
{
	union pkvm_hc_data out;
	int ret;

	ret = pkvm_hypercall_out(iommu_free_domain, &out,
				 virt_to_phys(root));
	if (!ret)
		free_domain_memcache(&out.iommu_free_domain.memcache);

	return ret;
}

struct domain_mc_alloc_arg {
	int nid;
	gfp_t gfp;
};

static void *alloc_domain_memcache_page(void *arg)
{
	struct domain_mc_alloc_arg *alloc = arg;

	return iommu_alloc_pages_node_sz(alloc->nid, alloc->gfp, PAGE_SIZE);
}

static phys_addr_t host_pa(void *addr)
{
	return virt_to_phys(addr);
}

int pkvm_domain_map(void *root, int nid, unsigned long iova,
		    phys_addr_t phys, size_t size, unsigned int prot,
		    gfp_t gfp)
{
	union pkvm_hc_data data = {};
	struct iommu_domain_map_data *map = &data.iommu_domain_map.in;
	struct domain_mc_alloc_arg alloc = {
		.nid = nid,
		.gfp = gfp,
	};
	unsigned long required_pages;
	int ret;

	map->root_gpa = virt_to_phys(root);
	map->iova = iova;
	map->phys = phys;
	map->size = size;
	map->prot = prot;
	map->mc.flags = PKVM_MC_DONATE_SHARE_RO;

	ret = pkvm_hypercall_inout(iommu_domain_map, &data, &data);
	if (ret != -ENOMEM)
		goto out;

	required_pages = __pkvm_pgtable_max_pages(size >> PAGE_SHIFT);
	ret = topup_pkvm_memcache(&map->mc, required_pages,
				  alloc_domain_memcache_page, host_pa, &alloc);
	if (!ret)
		ret = pkvm_hypercall_inout(iommu_domain_map, &data, &data);
out:
	free_domain_memcache(&map->mc);
	return ret;
}

int pkvm_domain_unmap(void *root, unsigned long iova, size_t size, bool sync)
{
	return pkvm_hypercall(iommu_domain_unmap, virt_to_phys(root), iova,
			      size, sync);
}

int pkvm_domain_sync(void *root, unsigned long iova, size_t size)
{
	return pkvm_hypercall(iommu_domain_sync, virt_to_phys(root), iova,
			      size);
}

int pkvm_context_mapping(struct intel_iommu *iommu,
			 struct device_domain_info *info, u8 bus, u8 devfn,
			 u64 root_gpa, u16 did)
{
	union pkvm_hc_data d = {};
	struct set_lm_ce_data *data = &d.iommu_set_lm_ce.in;
	int ret;

	data->phys = iommu->reg_phys;
	data->root_gpa = root_gpa;
	data->segment = iommu->segment;
	data->did = did;
	data->bus = bus;
	data->devfn = devfn;
	data->ats_qdep = info ? info->ats_qdep : 0;
	data->ats_supported = info ? info->ats_supported : 0;
	data->ats_enabled = info ? info->ats_enabled : 0;

	spin_lock(&iommu->lock);
	ret = pkvm_hypercall_inout(iommu_set_lm_ce, &d, &d);
	if (ret == -ENOMEM) {
		void *page;

		page = iommu_alloc_pages_node_sz(iommu->node, GFP_ATOMIC, SZ_4K);
		if (!page) {
			ret = -ENOMEM;
			goto out_unlock;
		}

		data->donation_page_gpa = virt_to_phys(page);
		ret = pkvm_hypercall_inout(iommu_set_lm_ce, &d, &d);
		if (data->donation_page_gpa)
			iommu_free_pages(phys_to_virt(data->donation_page_gpa));
	}

out_unlock:
	spin_unlock(&iommu->lock);
	return ret;
}

int pkvm_context_clear(struct intel_iommu *iommu, u8 bus, u8 devfn)
{
	union pkvm_hc_data d = {};
	struct clear_ce_data *data = &d.iommu_clear_ce.data;

	data->phys = iommu->reg_phys;
	data->segment = iommu->segment;
	data->bus = bus;
	data->devfn = devfn;

	return pkvm_hypercall_in(iommu_clear_ce, &d);
}

int pkvm_pasid_table_setup(struct intel_iommu *iommu,
			   struct device_domain_info *info,
			   u8 bus, u8 devfn)
{
	union pkvm_hc_data d = {};
	struct set_sm_ce_data *data = &d.iommu_set_sm_ce.in;
	int ret;

	data->phys = iommu->reg_phys;
	data->pasid_table_gpa = virt_to_phys(info->pasid_table->table);
	data->max_pasid = info->pasid_table->max_pasid;
	data->segment = iommu->segment;
	data->bus = bus;
	data->devfn = devfn;
	data->ats_qdep = info->ats_qdep;
	data->ats_enabled = info->ats_enabled;
	data->ats_supported = info->ats_supported;
	data->pasid_supported = info->pasid_supported;
	data->pasid_enabled = info->pasid_enabled;
	data->pri_supported = info->pri_supported;
	data->pri_enabled = info->pri_enabled;

	spin_lock(&iommu->lock);
	ret = pkvm_hypercall_inout(iommu_set_sm_ce, &d, &d);
	if (ret == -ENOMEM) {
		void *page;

		page = iommu_alloc_pages_node_sz(iommu->node, GFP_ATOMIC, SZ_4K);
		if (!page) {
			ret = -ENOMEM;
			goto out_unlock;
		}

		data->donation_page_gpa = virt_to_phys(page);
		ret = pkvm_hypercall_inout(iommu_set_sm_ce, &d, &d);
		if (data->donation_page_gpa)
			iommu_free_pages(phys_to_virt(data->donation_page_gpa));
	}

out_unlock:
	spin_unlock(&iommu->lock);
	return ret;
}

int pkvm_pasid_setup_fl(struct device_domain_info *info,
			phys_addr_t fsptptr, u32 pasid, u16 did,
			int flags)
{
	union pkvm_hc_data d = {};
	struct pasid_setup_fl_data *data = &d.iommu_pasid_setup_fl.in;
	struct intel_iommu *iommu = info->iommu;
	int ret;

	data->phys = iommu->reg_phys;
	data->fsptptr_gpa = fsptptr;
	data->pasid = pasid;
	data->flags = flags;
	data->segment = info->segment;
	data->did = did;
	data->bus = info->bus;
	data->devfn = info->devfn;

	spin_lock(&iommu->lock);
	ret = pkvm_hypercall_inout(iommu_pasid_setup_fl, &d, &d);
	if (ret == -ENOMEM) {
		void *page;

		page = iommu_alloc_pages_node_sz(iommu->node, GFP_ATOMIC, SZ_4K);
		if (!page)
			goto out_unlock;

		data->donation_page_gpa = virt_to_phys(page);
		ret = pkvm_hypercall_inout(iommu_pasid_setup_fl, &d, &d);
		if (data->donation_page_gpa)
			iommu_free_pages(phys_to_virt(data->donation_page_gpa));
	}

out_unlock:
	spin_unlock(&iommu->lock);
	return ret;
}

int pkvm_pasid_setup_sl(struct device_domain_info *info,
			phys_addr_t root, u32 pasid, u16 did)
{
	union pkvm_hc_data d = {};
	struct pasid_setup_sl_data *data = &d.iommu_pasid_setup_sl.in;
	struct intel_iommu *iommu = info->iommu;
	int ret;

	data->phys = iommu->reg_phys;
	data->root_gpa = root;
	data->pasid = pasid;
	data->segment = info->segment;
	data->did = did;
	data->bus = info->bus;
	data->devfn = info->devfn;

	spin_lock(&iommu->lock);
	ret = pkvm_hypercall_inout(iommu_pasid_setup_sl, &d, &d);
	if (ret == -ENOMEM) {
		void *page;

		page = iommu_alloc_pages_node_sz(iommu->node, GFP_ATOMIC, SZ_4K);
		if (!page)
			goto out_unlock;

		data->donation_page_gpa = virt_to_phys(page);
		ret = pkvm_hypercall_inout(iommu_pasid_setup_sl, &d, &d);
		if (data->donation_page_gpa)
			iommu_free_pages(phys_to_virt(data->donation_page_gpa));
	}

out_unlock:
	spin_unlock(&iommu->lock);
	return ret;
}

int pkvm_pasid_teardown(struct device_domain_info *info, u32 pasid)
{
	union pkvm_hc_data d = {};
	struct pasid_teardown_data *data = &d.iommu_pasid_teardown.data;
	struct intel_iommu *iommu = info->iommu;

	data->phys = iommu->reg_phys;
	data->pasid = pasid;
	data->segment = info->segment;
	data->bus = info->bus;
	data->devfn = info->devfn;

	return pkvm_hypercall_in(iommu_pasid_teardown, &d);
}

int pkvm_modify_irte(struct intel_iommu *iommu, int index,
		     const struct irte *modified)
{
	union pkvm_hc_data d = {};
	struct modify_irte_data *data = &d.iommu_modify_irte.data;
	int ret;

	data->phys = iommu->reg_phys;
	data->index = index;
	data->irte_lo = modified->low;
	data->irte_hi = modified->high;

	ret = pkvm_hypercall_in(iommu_modify_irte, &d);
	if (ret)
		pr_err("iommu%d: failed to modify IRTE%d: %d\n",
		       iommu->seq_id, index, ret);

	return ret;
}

int pkvm_write_iommu_msi(struct intel_iommu *iommu, u32 offset, u32 data,
			 u32 addr, u32 uaddr)
{
	int ret;

	ret = pkvm_hypercall(iommu_msi_write, iommu->reg_phys, offset, data,
			     addr, uaddr);
	if (ret)
		pr_err("iommu%d: DMAR MSI write failed: %d\n",
		       iommu->seq_id, ret);

	return ret;
}
