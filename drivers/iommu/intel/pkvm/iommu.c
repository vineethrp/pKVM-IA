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
static struct pkvm_iommu_device_id satc_devs[PKVM_MAX_SATC_DEVS];
static unsigned int nr_satc_devs;
unsigned int iommu_pgsz_mask;
unsigned int iommu_pglvl_mask;

/* x86 MSI address fields used by DMAR fault and performance interrupts. */
#define X86_MSI_ADDR_BASE		0xfee00000U
#define X86_MSI_ADDR_MASK		0xfff00ff3U
#define X86_MSI_ADDR_DEST_ID_MASK	GENMASK_U32(19, 12)
#define X86_MSI_ADDR_DEST_ID_SHIFT	12
#define X86_MSI_ADDR_DEST_MODE_LOGICAL	BIT(2)
#define X86_MSI_UADDR_DEST_ID_MASK	GENMASK_U32(31, 8)

/* GCMD bits that enable or disable IOMMU features. */
#define DMAR_GSTS_EN_BITS	(DMA_GCMD_TE | DMA_GCMD_QIE | \
				 DMA_GCMD_IRE | DMA_GCMD_CFI)
/* One-shot GCMD bits that have no effect when cleared. */
#define DMAR_GCMD_ONESHOT	DMA_GCMD_SRTP
/* GCMD bits that may pass directly through to hardware. */
#define DMAR_GCMD_DIRECT	(DMA_GCMD_IRE | DMA_GCMD_CFI)
/* GCMD bits currently understood by pKVM. */
#define DMAR_GCMD_SUPPORTED	(DMAR_GSTS_EN_BITS | DMAR_GCMD_ONESHOT)

bool is_dev_in_satc(u16 segment, u16 bdf)
{
	unsigned int i;

	for (i = 0; i < nr_satc_devs; i++) {
		if (segment == satc_devs[i].segment &&
		    bdf == satc_devs[i].bdf)
			return true;
	}

	return false;
}

struct intel_iommu *iommu_from_phys(u64 phys)
{
	unsigned int i;

	for (i = 0; i < nr_iommus; i++) {
		struct intel_iommu *iommu = &iommus[i];

		if (phys >= iommu->reg_phys &&
		    phys < iommu->reg_phys + iommu->reg_size)
			return iommu;
	}

	return NULL;
}

bool overlaps_iommu_mmio(u64 phys, u64 size)
{
	u64 end;
	unsigned int i;

	if (!size || size > U64_MAX - phys)
		return true;

	end = phys + size;

	for (i = 0; i < nr_iommus; i++) {
		struct intel_iommu *iommu = &iommus[i];

		if (phys < iommu->reg_phys + iommu->reg_size &&
		    end > iommu->reg_phys)
			return true;
	}

	return false;
}

static int iommu_direct_mmio_read(struct intel_iommu *iommu, u64 phys,
				  int len, u64 *val)
{
	u64 offset = phys - iommu->reg_phys;
	void __iomem *reg = iommu->reg + offset;

	switch (len) {
	case 4:
		*val = readl(reg);
		break;
	case 8:
		*val = readq(reg);
		break;
	default:
		pkvm_err("%s: unsupported length %d\n", __func__, len);
		return -EINVAL;
	}

	return 0;
}

static int iommu_direct_mmio_write(struct intel_iommu *iommu, u64 phys,
				   int len, u64 val)
{
	u64 offset = phys - iommu->reg_phys;
	void __iomem *reg = iommu->reg + offset;

	switch (len) {
	case 4:
		writel((u32)val, reg);
		break;
	case 8:
		writeq(val, reg);
		break;
	default:
		pkvm_err("%s: unsupported length %d\n", __func__, len);
		return -EINVAL;
	}

	return 0;
}

static u32 pkvm_dmar_readl(struct intel_iommu *iommu, unsigned long offset)
{
	return readl(iommu->reg + offset);
}

static int handle_gcmd_direct(struct intel_iommu *iommu, u32 bit, bool set)
{
	u32 gsts = readl(iommu->reg + DMAR_GSTS_REG);
	u32 gcmd = gsts & DMAR_GSTS_EN_BITS;
	u32 status;

	BUG_ON(gsts != iommu->vgsts);

	if ((bit & DMAR_GCMD_ONESHOT) && !set)
		return -EINVAL;

	if (set) {
		if (gcmd & bit)
			return 0;
		gcmd |= bit;
	} else {
		if (!(gcmd & bit))
			return 0;
		gcmd &= ~bit;
	}

	writel(gcmd, iommu->reg + DMAR_GCMD_REG);
	if (set) {
		IOMMU_WAIT_OP(iommu, DMAR_GSTS_REG, pkvm_dmar_readl,
			      (status & bit), status);
		iommu->vgsts |= bit;
	} else {
		IOMMU_WAIT_OP(iommu, DMAR_GSTS_REG, pkvm_dmar_readl,
			      !(status & bit), status);
		iommu->vgsts &= ~bit;
	}

	return 0;
}

static int initialize_qi(struct intel_iommu *iommu)
{
	void *desc = pkvm_host_gpa_to_virt(iommu->viqa & VTD_PAGE_MASK);
	u64 desc_sz = ecap_smts(iommu->ecap) ? SZ_8K : SZ_4K;
	struct q_inval *qi = &iommu->_qi;
	u64 desc_pa = __pkvm_pa(desc);
	u64 val = desc_pa;
	int ret;

	ret = pkvm_host_donate_hyp_share_ro(desc_pa, desc_sz, true);
	if (ret) {
		pkvm_err("iommu%d: failed to write protect QI desc!\n",
			 iommu->seq_id);
		return ret;
	}

	iommu->flush.flush_context = qi_flush_context;
	iommu->flush.flush_iotlb = qi_flush_iotlb;

	pkvm_spin_lock_init(&qi->q_lock);
	qi->free_head = 0;
	qi->free_tail = 0;
	qi->free_cnt = QI_LENGTH;
	qi->desc = desc;

	/*
	 * Set DW=1 and QS=1 in IQA_REG when Scalable Mode capability
	 * is present.
	 */
	if (ecap_smts(iommu->ecap))
		val |= BIT_ULL(11) | BIT_ULL(0);

	/* Write zero to the tail register and program the queue address. */
	writel(0, iommu->reg + DMAR_IQT_REG);
	writeq(val, iommu->reg + DMAR_IQA_REG);

	ret = handle_gcmd_direct(iommu, DMA_GCMD_QIE, true);
	if (ret) {
		pkvm_hyp_donate_host(desc_pa, desc_sz, false);
		return ret;
	}

	/*
	 * Host IOMMU driver dynamically allocates iommu->qi, but pKVM has it
	 * embedded. For easy re-use of host code, the embedded field is named
	 * as iommu->_qi, and the pointer iommu->qi points to iommu->_qi.
	 * Also, it serves as a flag to denote whether qi is
	 * enabled(similar to how host driver does)
	 */
	iommu->qi = qi;

	return 0;
}

static int handle_gcmd_qie(struct intel_iommu *iommu, bool enable)
{
	int ret = 0;

	if (enable) {
		if (iommu->qi || iommu->vgsts & DMA_GSTS_QIES) {
			pkvm_err("iommu%d: QI already enabled\n", iommu->seq_id);
			return -EBUSY;
		} else if (!iommu->viqa) {
			pkvm_err("iommu%d: QIE before setting IQA\n",
				 iommu->seq_id);
			return -EINVAL;
		}

		ret = initialize_qi(iommu);
	} else {
		if (!iommu->qi) {
			ret = handle_gcmd_direct(iommu, DMA_GCMD_QIE, false);
		} else {
			pkvm_warn("iommu%d: disabling QI is not allowed\n",
				  iommu->seq_id);
			return -EPERM;
		}
	}

	pkvm_dbg("iommu%d: Queued invalidation %s\n", iommu->seq_id,
		 enable ? "enabled" : "disabled");
	return ret;
}

static int set_root_table(struct intel_iommu *iommu)
{
	int ret;

	writeq(iommu->vrta, iommu->reg + DMAR_RTADDR_REG);
	ret = handle_gcmd_direct(iommu, DMA_GCMD_SRTP, true);
	if (ret)
		return ret;

	if (cap_esrtps(iommu->cap))
		return 0;

	iommu->flush.flush_context(iommu, 0, 0, 0, DMA_CCMD_GLOBAL_INVL);
	if (sm_supported(iommu))
		qi_flush_pasid_cache(iommu, 0, QI_PC_GLOBAL, 0);
	iommu->flush.flush_iotlb(iommu, 0, 0, 0, DMA_TLB_GLOBAL_FLUSH);

	return 0;
}

static int handle_gcmd_srtp(struct intel_iommu *iommu)
{
	u32 gsts = readl(iommu->reg + DMAR_GSTS_REG);
	phys_addr_t root_pa;
	int ret;

	BUG_ON(gsts != iommu->vgsts);

	if (!iommu->vrta) {
		pkvm_warn("iommu%d: host RTADDR_REG not set\n",
			  iommu->seq_id);
		return -EINVAL;
	} else if (iommu->vgsts & DMA_GSTS_TES) {
		pkvm_warn("iommu%d: SRTP not allowed after TE\n",
			  iommu->seq_id);
		return -EBUSY;
	} else if (iommu->root_entry) {
		pkvm_warn("iommu%d: SRTP allowed only once\n",
			  iommu->seq_id);
		return -EBUSY;
	} else if (!cap_esrtps(iommu->cap) && !iommu->qi) {
		pkvm_warn("iommu%d: QI required before SRTP\n",
			  iommu->seq_id);
		return -EINVAL;
	}

	root_pa = pkvm_host_gpa_to_phys(iommu->vrta & VTD_PAGE_MASK);
	ret = pkvm_host_donate_hyp_share_ro(root_pa, VTD_PAGE_SIZE, true);
	if (ret) {
		pkvm_err("iommu%d: failed to protect root table: %d\n",
			 iommu->seq_id, ret);
		return ret;
	}

	iommu->root_entry = __pkvm_va(root_pa);
	__iommu_flush_cache(iommu, iommu->root_entry, VTD_PAGE_SIZE);

	ret = set_root_table(iommu);
	if (ret)
		return ret;

	pkvm_dbg("iommu%d: root table set to %#llx\n",
		 iommu->seq_id, iommu->vrta);
	return 0;
}

static int iommu_protect_ir_table(struct intel_iommu *iommu)
{
	phys_addr_t ir_table_pa;
	int ret;

	if (!iommu->virta) {
		pkvm_err("iommu%d: IRTA not set\n", iommu->seq_id);
		return -EINVAL;
	}

	ir_table_pa = pkvm_host_gpa_to_phys(iommu->virta & VTD_PAGE_MASK);
	/*
	 * Interrupt remapping is initialized before the host is
	 * deprivileged. Preserve the trusted entries while making the table
	 * read-only to the host.
	 */
	ret = pkvm_host_donate_hyp_share_ro(ir_table_pa, SZ_1M, false);
	if (ret) {
		pkvm_err("iommu%d: failed to protect IR table: %d\n",
			 iommu->seq_id, ret);
		return ret;
	}

	iommu->ir_table = __pkvm_va(ir_table_pa);
	return 0;
}

static int handle_gcmd_te(struct intel_iommu *iommu, bool enable)
{
	int ret;

	if (enable) {
		if (iommu->vgsts & DMA_GSTS_TES) {
			pkvm_err("iommu%d: TE allowed only once\n",
				 iommu->seq_id);
			return -EBUSY;
		} else if (!(iommu->vgsts & DMA_GSTS_RTPS)) {
			pkvm_err("iommu%d: TE not allowed before SRTP\n",
				 iommu->seq_id);
			return -EINVAL;
		}

		ret = handle_gcmd_direct(iommu, DMA_GCMD_TE, true);
		if (ret)
			return ret;

		pkvm_dbg("iommu%d: translation enabled\n", iommu->seq_id);
	} else {
		pkvm_warn("iommu%d: disabling translation is not allowed\n",
			  iommu->seq_id);
		return -EPERM;
	}

	return 0;
}

static int handle_global_cmd(struct intel_iommu *iommu, u32 val)
{
	u32 changed = (iommu->vgsts & DMAR_GSTS_EN_BITS) ^ val;

	if (!changed)
		return 0;

	if (hweight32(changed) > 1) {
		pkvm_warn("iommu%d: multiple GCMD bits changed: %#x\n",
			  iommu->seq_id, val);
		return -EINVAL;
	}

	if (changed & ~DMAR_GCMD_SUPPORTED) {
		pkvm_warn("iommu%d: unsupported GCMD bit: %#x\n",
			  iommu->seq_id, changed);
		return -EOPNOTSUPP;
	}

	pkvm_dbg("iommu%d: GCMD=%#x GSTS=%#x changed=%#x\n",
		 iommu->seq_id, val, iommu->vgsts, changed);

	if (changed & DMA_GCMD_QIE)
		return handle_gcmd_qie(iommu, !!(val & DMA_GCMD_QIE));

	if (changed & DMA_GCMD_SRTP)
		return handle_gcmd_srtp(iommu);

	if (changed & DMA_GCMD_TE)
		return handle_gcmd_te(iommu, !!(val & DMA_GCMD_TE));

	if (changed & ~DMAR_GCMD_DIRECT) {
		pkvm_warn("iommu%d: direct GCMD access denied: %#x (set=%d)\n",
			  iommu->seq_id, changed, !!(val & changed));
		return -EPERM;
	}

	return handle_gcmd_direct(iommu, changed, !!(val & changed));
}

int pkvm_iommu_mmio_read(u64 phys, int len, u64 *val)
{
	struct intel_iommu *iommu = iommu_from_phys(phys);
	u64 offset;
	int ret = 0;

	if (!iommu)
		return -EINVAL;

	pkvm_spin_lock(&iommu->lock);
	offset = phys - iommu->reg_phys;

	switch (offset) {
	case DMAR_GCMD_REG:
		ret = -EINVAL;
		break;
	case DMAR_CAP_REG:
		*val = iommu->cap;
		break;
	case DMAR_ECAP_REG:
		*val = iommu->ecap;
		break;
	case DMAR_IQA_REG:
		*val = iommu->viqa;
		break;
	case DMAR_RTADDR_REG:
		*val = iommu->vrta;
		break;
	case DMAR_IRTA_REG:
		*val = iommu->virta;
		break;
	case DMAR_GSTS_REG:
		*val = iommu->vgsts;
		break;
	default:
		/* Registers not emulated by pKVM pass through to hardware. */
		ret = iommu_direct_mmio_read(iommu, phys, len, val);
	}

	pkvm_spin_unlock(&iommu->lock);
	return ret;
}

int pkvm_iommu_mmio_write(u64 phys, int len, u64 val)
{
	struct intel_iommu *iommu = iommu_from_phys(phys);
	u64 offset;
	int ret = 0;

	if (!iommu)
		return -EINVAL;

	pkvm_spin_lock(&iommu->lock);
	offset = phys - iommu->reg_phys;

	switch (offset) {
	case DMAR_CAP_REG:
	case DMAR_ECAP_REG:
	case DMAR_GSTS_REG:
		fallthrough;
	case DMAR_IQH_REG:
		ret = -EINVAL;
		break;
	case DMAR_GCMD_REG:
		ret = handle_global_cmd(iommu, val);
		break;
	case DMAR_IQA_REG:
		if (iommu->viqa) {
			pkvm_err("iommu%d: IQA set more than once\n",
				 iommu->seq_id);
			ret = -EINVAL;
		} else {
			iommu->viqa = val;
		}
		break;
	case DMAR_IQT_REG:
		if (iommu->qi) {
			pkvm_err("iommu%d: write to IQT not allowed!\n",
				 iommu->seq_id);
			ret = -EPERM;
		}
		break;
	case DMAR_RTADDR_REG:
		if (sm_supported(iommu) != !!(val & DMA_RTADDR_SMT)) {
			pkvm_err("iommu%d: RTA scalable-mode mismatch\n",
				 iommu->seq_id);
			ret = -EINVAL;
		} else if (val & ~VTD_PAGE_MASK & ~DMA_RTADDR_SMT) {
			pkvm_err("iommu%d: invalid RTA value %#llx\n",
				 iommu->seq_id, val);
			ret = -EINVAL;
		} else if (iommu->vgsts & DMA_GSTS_TES) {
			pkvm_err("iommu%d: RTA write after translation enabled\n",
				 iommu->seq_id);
			ret = -EBUSY;
		} else {
			iommu->vrta = val;
		}
		break;
	case DMAR_IRTA_REG:
		pkvm_err("iommu%d: IRTA writes are not supported\n",
			 iommu->seq_id);
		ret = -EPERM;
		break;
	case DMAR_FECTL_REG: {
		u32 rsvdp_mask = GENMASK_U32(29, 0);
		u32 rsvdp = readl(iommu->reg + DMAR_FECTL_REG) &
			    rsvdp_mask;

		if ((val & rsvdp_mask) != rsvdp) {
			pkvm_err("iommu%d: FECTL reserved bits mismatch: %#x != %#x\n",
				 iommu->seq_id, rsvdp,
				 (u32)val & rsvdp_mask);
			ret = -EINVAL;
		} else {
			ret = iommu_direct_mmio_write(iommu, phys, len, val);
		}
		break;
	}
	case DMAR_FSTS_REG:
		/*
		 * DMA_FSTS_PRO is deprecated and reserved-zero since VT-d 3.1,
		 * but the host driver still clears it. Keep accepting bit 7.
		 */
		if (val & (GENMASK_U32(31, 16) | GENMASK_U32(3, 2))) {
			pkvm_err("iommu%d: FSTS %#llx has reserved bits set\n",
				 iommu->seq_id, val);
			ret = -EINVAL;
		} else {
			/* FSTS fault bits are cleared by writing one. */
			ret = iommu_direct_mmio_write(iommu, phys, len, val);
		}
		break;
	default:
		/* Registers not emulated by pKVM pass through to hardware. */
		ret = iommu_direct_mmio_write(iommu, phys, len, val);
	}

	pkvm_spin_unlock(&iommu->lock);
	return ret;
}

static u32 iommu_msi_dest_id(u32 addr, u32 uaddr)
{
	return ((addr & X86_MSI_ADDR_DEST_ID_MASK) >>
		X86_MSI_ADDR_DEST_ID_SHIFT) |
	       (uaddr & X86_MSI_UADDR_DEST_ID_MASK);
}

static bool iommu_msi_dest_id_valid(u32 dest_id)
{
	struct pkvm_pcpu *pcpu;
	int i;

	for_each_pkvm_pcpu(i, pcpu) {
		u32 pcpu_dest_id = msi_dest_mode_logical ?
				   pcpu->msi_dest_id : pcpu->apic_id;

		if (dest_id == pcpu_dest_id)
			return true;
	}

	return false;
}

static int iommu_validate_msi_dest_id(struct intel_iommu *iommu, u32 addr,
				      u32 uaddr, const char *name)
{
	u32 dest_id = iommu_msi_dest_id(addr, uaddr);

	if (!iommu_msi_dest_id_valid(dest_id)) {
		pkvm_err("iommu%d: %s MSI destination %#x is not a pKVM CPU\n",
			 iommu->seq_id, name, dest_id);
		return -EINVAL;
	}

	return 0;
}

static int iommu_validate_msi_msg(struct intel_iommu *iommu, u32 offset,
				  u32 data, u32 addr, u32 uaddr,
				  const char *name)
{
	if (offset == DMAR_PERFINTRCTL_REG && !ecap_pms(iommu->ecap)) {
		pkvm_err("iommu%d: perf MSI write without PMU support\n",
			 iommu->seq_id);
		return -EINVAL;
	}

	if (data >> 9) {
		pkvm_err("iommu%d: %s MSI data %#x has reserved bits set\n",
			 iommu->seq_id, name, data);
		return -EINVAL;
	}

	if ((addr & X86_MSI_ADDR_MASK) != X86_MSI_ADDR_BASE) {
		pkvm_err("iommu%d: %s MSI address %#x is invalid\n",
			 iommu->seq_id, name, addr);
		return -EINVAL;
	}

	if (!!(addr & X86_MSI_ADDR_DEST_MODE_LOGICAL) !=
	    msi_dest_mode_logical) {
		pkvm_err("iommu%d: %s MSI destination mode is invalid\n",
			 iommu->seq_id, name);
		return -EINVAL;
	}

	if (uaddr & GENMASK_U32(7, 0)) {
		pkvm_err("iommu%d: %s MSI upper address %#x is invalid\n",
			 iommu->seq_id, name, uaddr);
		return -EINVAL;
	}

	return iommu_validate_msi_dest_id(iommu, addr, uaddr, name);
}

int pkvm_iommu_msi_write(u64 phys, u32 offset, u32 data, u32 addr, u32 uaddr)
{
	struct intel_iommu *iommu = iommu_from_phys(phys);
	const char *name;
	int ret;

	if (!iommu)
		return -EINVAL;

	switch (offset) {
	case DMAR_FECTL_REG:
		name = "fault event";
		break;
	case DMAR_PERFINTRCTL_REG:
		name = "performance monitoring";
		break;
	default:
		pkvm_err("iommu%d: unsupported MSI register group %#x\n",
			 iommu->seq_id, offset);
		return -EOPNOTSUPP;
	}

	ret = iommu_validate_msi_msg(iommu, offset, data, addr, uaddr, name);
	if (ret)
		return ret;

	pkvm_spin_lock(&iommu->lock);
	writel(data, iommu->reg + offset + 4);
	writel(addr, iommu->reg + offset + 8);
	writel(uaddr, iommu->reg + offset + 12);
	pkvm_spin_unlock(&iommu->lock);

	return 0;
}

static bool ranges_overlap(u64 start_a, u64 size_a, u64 start_b, u64 size_b)
{
	return start_a < start_b + size_b && start_b < start_a + size_a;
}

int __init pkvm_prepare_iommus(const struct pkvm_iommu_info *infos,
			       unsigned int count,
			       const struct pkvm_iommu_device_id *satc,
			       unsigned int satc_count)
{
	unsigned int pgsz_mask = BIT(PG_LEVEL_4K) |
				 BIT(PG_LEVEL_2M) |
				 BIT(PG_LEVEL_1G);
	unsigned int pglvl_mask = PKVM_IOMMU_PGT_4LEVEL |
				  PKVM_IOMMU_PGT_5LEVEL;
	unsigned int i, j;

	if (!infos || !count || count > ARRAY_SIZE(iommus) || nr_iommus ||
	    satc_count > ARRAY_SIZE(satc_devs) || (satc_count && !satc))
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

	for (i = 0; i < satc_count; i++) {
		for (j = 0; j < count; j++)
			if (satc[i].segment == infos[j].segment)
				break;
		if (j == count)
			return -EINVAL;
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
		iommu->scalable_mode = info->scalable_mode;
		iommu->seq_id = info->seq_id;
		iommu->agaw = info->agaw;
		iommu->msagaw = info->msagaw;
	}

	nr_iommus = count;
	if (satc_count)
		memcpy(satc_devs, satc, sizeof(*satc) * satc_count);
	nr_satc_devs = satc_count;
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
		/*
		 * Interrupt remapping is enabled while x2APIC mode is set up,
		 * before pKVM initialization. Protect the active table before
		 * the host is allowed to modify interrupt entries again.
		 */
		if (!(iommu->vgsts & DMA_GSTS_IRTPS) ||
		    !(iommu->vgsts & DMA_GSTS_IRES)) {
			pkvm_err("iommu%d: interrupt remapping not initialized\n",
				 iommu->seq_id);
			return -EINVAL;
		}

		iommu->virta = readq(iommu->reg + DMAR_IRTA_REG);
		ret = iommu_protect_ir_table(iommu);
		if (ret)
			return ret;
	}

	return pkvm_iommu_domain_init();
}
