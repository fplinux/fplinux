/* SPDX-License-Identifier: GPL-2.0-only */
#include <linux/linkage.h>

	.syntax unified
	.arm

ENTRY(save_boot_params)
	ldr	r12, =fplinux_uboot_handoff_phys
	str	r0, [r12]
	b	save_boot_params_ret
ENDPROC(save_boot_params)
