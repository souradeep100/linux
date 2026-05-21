/* SPDX-License-Identifier: GPL-2.0 */

/*
 * Linux-specific definitions for managing interactions with Microsoft's
 * Hyper-V hypervisor. The definitions in this file are specific to
 * the ARM64 architecture.  See include/asm-generic/mshyperv.h for
 * definitions are that architecture independent.
 *
 * Definitions that are derived from Hyper-V code or headers should not go in
 * this file, but should instead go in the relevant files in include/hyperv.
 *
 * Copyright (C) 2021, Microsoft, Inc.
 *
 * Author : Michael Kelley <mikelley@microsoft.com>
 */

#ifndef _ASM_MSHYPERV_H
#define _ASM_MSHYPERV_H

#include <linux/types.h>
#include <linux/arm-smccc.h>
#include <hyperv/hvhdk.h>

/*
 * Declare calls to get and set Hyper-V VP register values on ARM64, which
 * requires a hypercall.
 */

void hv_set_vpreg(u32 reg, u64 value);
u64 hv_get_vpreg(u32 reg);
void hv_get_vpreg_128(u32 reg, struct hv_get_vp_registers_output *result);

static inline void hv_set_msr(unsigned int reg, u64 value)
{
	hv_set_vpreg(reg, value);
}

static inline u64 hv_get_msr(unsigned int reg)
{
	return hv_get_vpreg(reg);
}

/*
 * Nested is not supported on arm64
 */
static inline void hv_set_non_nested_msr(unsigned int reg, u64 value)
{
	hv_set_msr(reg, value);
}

static inline u64 hv_get_non_nested_msr(unsigned int reg)
{
	return hv_get_msr(reg);
}
#ifdef CONFIG_PCI_MSI
u64 hv_build_devid_type_pci(struct pci_dev *pdev);
int hv_map_msi_interrupt(struct irq_data *data,
			 struct hv_interrupt_entry *out_entry);
struct irq_domain *hv_create_pci_msi_domain(void);
#endif

/* SMCCC hypercall parameters */
#define HV_SMCCC_FUNC_NUMBER	1
#define HV_FUNC_ID	ARM_SMCCC_CALL_VAL(			\
				ARM_SMCCC_STD_CALL,		\
				ARM_SMCCC_SMC_64,		\
				ARM_SMCCC_OWNER_VENDOR_HYP,	\
				HV_SMCCC_FUNC_NUMBER)

#include <asm-generic/mshyperv.h>

#endif
