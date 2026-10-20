/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_QIN_LINUX_ENTRY_H
#define FPLINUX_QIN_LINUX_ENTRY_H

/*
 * A32 entry: r0 = final stack physical address. Enter in HYP with an aligned
 * RAM stack and an identity map covering the entry and both stacks. The final
 * stack is eight-byte aligned. This function masks IRQ/FIQ, resets USB and
 * returns on the original stack before the caller relocates the kernel.
 */
#ifndef __ASSEMBLER__
#include <stdint.h>

void qin_prepare_kernel(uint32_t stack_phys);
#endif

#endif
