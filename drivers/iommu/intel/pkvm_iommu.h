/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright 2026 Google LLC
 */

#ifndef _PKVM_INTEL_IOMMU_H_
#define _PKVM_INTEL_IOMMU_H_

#ifndef __PKVM_HYP__
#include <asm/kvm_host.h>

#ifdef CONFIG_PKVM_INTEL
int __init pkvm_host_prepare_iommu(void);
int __init pkvm_host_init_iommu(void);
#endif
#else /* __PKVM_HYP__ */
int pkvm_intel_iommu_init(void);
#endif /* __PKVM_HYP__ */

#endif /* _PKVM_INTEL_IOMMU_H_ */
