/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026, FPLinux contributors. */
#include <arch.h>
#include <asm_macros.S>

	.globl plat_my_core_pos
	.globl platform_mem_init

func plat_my_core_pos
	mrs x0, mpidr_el1
	and x0, x0, #MPIDR_CPU_MASK
	ret
endfunc plat_my_core_pos

func platform_mem_init
	ret
endfunc platform_mem_init
