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
#include <linux/atomic.h>
#include <linux/list.h>
#include "pkvm/pgtable.h"
#endif

#define PKVM_MAX_IOMMUS	16
#define PKVM_MAX_SATC_DEVS	16
#define PKVM_MAX_IOMMU_DOMAINS	128
#define PKVM_MAX_IOMMU_DEVICES	256

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
struct dmar_domain;
struct device_domain_info;
struct pkvm_device;

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

int pkvm_iec_flush(struct intel_iommu *iommu, bool global, int index,
		   int mask);
int pkvm_alloc_domain(struct intel_iommu *iommu, void *root, u8 agaw,
		      bool use_first_level);
int pkvm_free_domain(void *root);
int pkvm_domain_map(void *root, int nid, unsigned long iova,
		    phys_addr_t phys, size_t size, unsigned int prot,
		    gfp_t gfp);
int pkvm_domain_unmap(void *root, unsigned long iova, size_t size);
int pkvm_domain_sync(void *root, unsigned long iova, size_t size);
int pkvm_context_mapping(struct intel_iommu *iommu,
			 struct device_domain_info *info, u8 bus, u8 devfn,
			 u64 root_gpa, u16 did);
int pkvm_context_clear(struct intel_iommu *iommu, u8 bus, u8 devfn);
int pkvm_pasid_table_setup(struct intel_iommu *iommu,
			   struct device_domain_info *info,
			   u8 bus, u8 devfn);
int pkvm_pasid_setup_fl(struct device_domain_info *info,
			phys_addr_t fsptptr, u32 pasid, u16 did,
			int flags);
int pkvm_pasid_setup_sl(struct device_domain_info *info,
			phys_addr_t root, u32 pasid, u16 did);
int pkvm_pasid_teardown(struct device_domain_info *info, u32 pasid);
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

int pkvm_iommu_domain_init(void);
void pkvm_iommu_pgtable_init(struct dmar_domain *domain,
			     unsigned int allowed_pgsz);
int pkvm_iommu_pgtable_destroy(struct dmar_domain *domain);
int pkvm_iommu_pgtable_map(struct dmar_domain *domain, unsigned long iova,
			   phys_addr_t phys, size_t size, u64 prot,
			   struct pkvm_memcache *host_mc);
int pkvm_iommu_pgtable_unmap(struct dmar_domain *domain, unsigned long iova,
			     size_t size);
struct dmar_domain *
pkvm_alloc_iommu_domain(struct intel_iommu *iommu, phys_addr_t root, u8 agaw,
			bool use_first_level);
struct dmar_domain *pkvm_get_iommu_domain_by_root(phys_addr_t root);
struct dmar_domain *
pkvm_get_iommu_domain(phys_addr_t root, u16 did,
		      struct intel_iommu *iommu);
struct dmar_domain *
pkvm_find_iommu_domain(phys_addr_t root, u16 did,
		       struct intel_iommu *iommu);
void pkvm_put_iommu_domain(struct dmar_domain *domain);
int pkvm_free_iommu_domain(phys_addr_t root,
			   struct pkvm_memcache *teardown_mc);
struct pkvm_device *
pkvm_alloc_iommu_device(const struct device_domain_info *info);
struct pkvm_device *
pkvm_get_iommu_device(struct intel_iommu *iommu, u32 segment,
		      u8 bus, u8 devfn);
void pkvm_remove_iommu_device(struct pkvm_device *device);
struct cache_tag *pkvm_alloc_cache_tag(void);
void pkvm_free_cache_tag(struct cache_tag *tag);

int pkvm_intel_iommu_init(void);
void pkvm_iommu_pt_flush(unsigned long paddr, unsigned long size);
int pkvm_iommu_mmio_read(u64 phys, int len, u64 *val);
int pkvm_iommu_mmio_write(u64 phys, int len, u64 val);
int pkvm_iommu_iec_flush(u64 phys, int index, int mask, bool global);
int pkvm_iommu_alloc_domain(u64 iommu_phys, u64 root_gpa, u8 agaw,
			    bool use_first_level);
int pkvm_iommu_free_domain(u64 root_gpa, struct pkvm_memcache *mc);
int pkvm_iommu_domain_map(struct iommu_domain_map_data *in,
			  struct iommu_domain_map_data *out);
int pkvm_iommu_domain_unmap(u64 root_gpa, unsigned long iova, size_t size);
int pkvm_iommu_domain_sync(u64 root_gpa, unsigned long iova, size_t size);
int pkvm_iommu_clear_ce(struct clear_ce_data *data);
int pkvm_iommu_set_lm_ce(struct set_lm_ce_data *in,
			 struct set_lm_ce_data *out);
int pkvm_iommu_set_sm_ce(struct set_sm_ce_data *in,
			 struct set_sm_ce_data *out);
int pkvm_iommu_pasid_setup_fl(struct pasid_setup_fl_data *in,
			      struct pasid_setup_fl_data *out);
int pkvm_iommu_pasid_setup_sl(struct pasid_setup_sl_data *in,
			      struct pasid_setup_sl_data *out);
int pkvm_iommu_pasid_teardown(struct pasid_teardown_data *data);
#endif /* !__PKVM_HYP__ */
#else /* !CONFIG_PKVM_INTEL */
static inline int pkvm_iec_flush(struct intel_iommu *iommu, bool global,
				 int index, int mask)
{
	return -EOPNOTSUPP;
}

static inline int pkvm_alloc_domain(struct intel_iommu *iommu, void *root,
				    u8 agaw, bool use_first_level)
{
	return -EOPNOTSUPP;
}

static inline int pkvm_free_domain(void *root)
{
	return -EOPNOTSUPP;
}

static inline int pkvm_domain_map(void *root, int nid, unsigned long iova,
				  phys_addr_t phys, size_t size,
				  unsigned int prot, gfp_t gfp)
{
	return -EOPNOTSUPP;
}

static inline int pkvm_domain_unmap(void *root, unsigned long iova,
				    size_t size)
{
	return -EOPNOTSUPP;
}

static inline int pkvm_domain_sync(void *root, unsigned long iova,
				   size_t size)
{
	return -EOPNOTSUPP;
}

static inline int pkvm_context_mapping(struct intel_iommu *iommu,
				       struct device_domain_info *info,
				       u8 bus, u8 devfn, u64 root_gpa,
				       u16 did)
{
	return -EOPNOTSUPP;
}

static inline int pkvm_context_clear(struct intel_iommu *iommu,
				     u8 bus, u8 devfn)
{
	return -EOPNOTSUPP;
}

static inline int pkvm_pasid_table_setup(struct intel_iommu *iommu,
					 struct device_domain_info *info,
					 u8 bus, u8 devfn)
{
	return -EOPNOTSUPP;
}

static inline int pkvm_pasid_setup_fl(struct device_domain_info *info,
				      phys_addr_t fsptptr, u32 pasid,
				      u16 did, int flags)
{
	return -EOPNOTSUPP;
}

static inline int pkvm_pasid_setup_sl(struct device_domain_info *info,
				      phys_addr_t root, u32 pasid,
				      u16 did)
{
	return -EOPNOTSUPP;
}

static inline int pkvm_pasid_teardown(struct device_domain_info *info,
				      u32 pasid)
{
	return -EOPNOTSUPP;
}
#endif /* CONFIG_PKVM_INTEL */

#endif /* _PKVM_INTEL_IOMMU_H_ */
