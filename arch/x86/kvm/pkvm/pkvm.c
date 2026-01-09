// SPDX-License-Identifier: GPL-2.0
#include <linux/types.h>
#include "pkvm.h"

/*
 * Needed by kvm_spurious_fault(), which is a generic fault handler for
 * hardware virtualization instructions. The pKVM hypervisor doesn't have
 * knowledge about host reboot or shutdown, so faults must remain fatal in
 * the hypervisor image and virt_rebooting is always false.
 */
__visible bool virt_rebooting;

struct pkvm_hyp *pkvm_hyp;
DEFINE_PER_CPU(struct pkvm_pcpu *, phys_cpu);
DEFINE_PER_CPU(struct kvm_vcpu *, host_vcpu);
