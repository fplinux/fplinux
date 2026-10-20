/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_SC9820E_LINUX_HANDOFF_H
#define FPLINUX_SC9820E_LINUX_HANDOFF_H

/*
 * A32 entry: r0 = kernel physical entry, r1 = DTB physical address,
 * r2 = final stack physical address. The caller has masked IRQ/FIQ in HYP,
 * completed device reset and kernel relocation, and retained an identity map
 * and aligned RAM stack.
 * All three physical regions remain accessible after translation is disabled.
 * The function enters Linux in HYP and does not return.
 */
#ifndef __ASSEMBLER__
#include <stdint.h>

__attribute__((noreturn)) void sc9820e_enter_linux(uint32_t kernel_phys,
						   uint32_t dtb_phys,
						   uint32_t stack_phys);
#endif

#endif
