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

/* GCMD bits that enable or disable IOMMU features. */
#define DMAR_GSTS_EN_BITS	(DMA_GCMD_TE | DMA_GCMD_QIE | \
				 DMA_GCMD_IRE | DMA_GCMD_CFI)
/* One-shot GCMD bits that have no effect when cleared. */
#define DMAR_GCMD_ONESHOT	(DMA_GCMD_SRTP | DMA_GCMD_SIRTP)
/* GCMD bits that may pass directly through to hardware. */
#define DMAR_GCMD_DIRECT	(DMA_GCMD_TE | DMA_GCMD_IRE | DMA_GCMD_CFI | \
				 DMAR_GCMD_ONESHOT)
/* GCMD bits currently understood by pKVM. */
#define DMAR_GCMD_SUPPORTED	(DMAR_GSTS_EN_BITS | DMAR_GCMD_ONESHOT)

static struct intel_iommu *iommu_from_phys(u64 phys)
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
	u32 gcmd = iommu->vgsts & DMAR_GSTS_EN_BITS;
	u32 status;

	if ((bit & DMAR_GCMD_ONESHOT) && !set)
		return -EINVAL;

	if (set)
		gcmd |= bit;
	else
		gcmd &= ~bit;

	writel(gcmd, iommu->reg + DMAR_GCMD_REG);
	IOMMU_WAIT_OP(iommu, DMAR_GSTS_REG, pkvm_dmar_readl,
		      (!!(status & bit) == set), status);
	iommu->vgsts = (iommu->vgsts & DMAR_GCMD_ONESHOT) | gcmd;

	return 0;
}

static int initialize_qi(struct intel_iommu *iommu)
{
	struct q_inval *qi = iommu->qi;
	u64 val = __pkvm_pa(qi->desc);

	/*
	 * TODO: Write-protect the QI descriptor page once the hypervisor takes
	 * over all QI operations.
	 */

	iommu->flush.flush_context = qi_flush_context;
	iommu->flush.flush_iotlb = qi_flush_iotlb;

	pkvm_spin_lock_init(&qi->q_lock);
	qi->free_head = 0;
	qi->free_tail = 0;
	qi->free_cnt = QI_LENGTH;

	/*
	 * Set DW=1 and QS=1 in IQA_REG when Scalable Mode capability
	 * is present.
	 */
	if (ecap_smts(iommu->ecap))
		val |= BIT_ULL(11) | BIT_ULL(0);

	/* Write zero to the tail register and program the queue address. */
	writel(0, iommu->reg + DMAR_IQT_REG);
	writeq(val, iommu->reg + DMAR_IQA_REG);

	return handle_gcmd_direct(iommu, DMA_GCMD_QIE, true);
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

		/*
		 * The host dynamically allocates iommu->qi, but pKVM embeds the
		 * structure. Point qi at the embedded instance both to reuse the
		 * host representation and to record that QI has been initialized.
		 */
		iommu->qi = &iommu->_qi;
		iommu->qi->desc =
			pkvm_host_gpa_to_virt(iommu->viqa & VTD_PAGE_MASK);
		ret = initialize_qi(iommu);
	} else {
		if (!iommu->qi)
			ret = handle_gcmd_direct(iommu, DMA_GCMD_QIE, false);
		else
			iommu->vgsts &= ~DMA_GSTS_QIES;
	}

	pkvm_dbg("iommu%d: Queued invalidation %s\n", iommu->seq_id,
		 enable ? "enabled" : "disabled");
	return ret;
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
	default:
		/* Registers not emulated by pKVM pass through to hardware. */
		ret = iommu_direct_mmio_write(iommu, phys, len, val);
	}

	pkvm_spin_unlock(&iommu->lock);
	return ret;
}

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
