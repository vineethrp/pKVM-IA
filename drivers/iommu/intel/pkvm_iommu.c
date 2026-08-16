// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2026 Google LLC
 */

#define pr_fmt(fmt) "DMAR: pKVM: " fmt

#include <linux/kernel.h>
#include "iommu.h"

int __init pkvm_host_prepare_iommu(void)
{
	return 0;
}

int __init pkvm_host_init_iommu(void)
{
	int ret;

	ret = intel_iommu_init();
	if (ret)
		pr_err("IOMMU initialization failed: %d\n", ret);
	else
		pr_info("IOMMU initialized\n");

	return ret;
}
