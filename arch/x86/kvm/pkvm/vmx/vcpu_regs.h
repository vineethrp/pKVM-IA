/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __PKVM_VMX_VCPU_REGS_H
#define __PKVM_VMX_VCPU_REGS_H

#include <asm/bitsperlong.h>

#define WORD_SIZE (BITS_PER_LONG / 8)

#define VCPU_RAX	(__VCPU_REGS_RAX * WORD_SIZE)
#define VCPU_RCX	(__VCPU_REGS_RCX * WORD_SIZE)
#define VCPU_RDX	(__VCPU_REGS_RDX * WORD_SIZE)
#define VCPU_RBX	(__VCPU_REGS_RBX * WORD_SIZE)
#define VCPU_RBP	(__VCPU_REGS_RBP * WORD_SIZE)
#define VCPU_RSI	(__VCPU_REGS_RSI * WORD_SIZE)
#define VCPU_RDI	(__VCPU_REGS_RDI * WORD_SIZE)

/*
 * asm/kvm_vcpu_regs.h only defines __VCPU_REGS_* for the eight architectural
 * 32-bit GPRs; R8-R15 are spelled out numerically there too (see R64_NUM), and
 * enum kvm_reg hardcodes VCPU_REGS_R8 = 8.  Do the same here.
 */
#define VCPU_R8		(8  * WORD_SIZE)
#define VCPU_R9		(9  * WORD_SIZE)
#define VCPU_R10	(10 * WORD_SIZE)
#define VCPU_R11	(11 * WORD_SIZE)
#define VCPU_R12	(12 * WORD_SIZE)
#define VCPU_R13	(13 * WORD_SIZE)
#define VCPU_R14	(14 * WORD_SIZE)
#define VCPU_R15	(15 * WORD_SIZE)

#endif /* __PKVM_VMX_VCPU_REGS_H */
