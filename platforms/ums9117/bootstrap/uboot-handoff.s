	@ SPDX-License-Identifier: GPL-2.0-only
	.arch armv7-a
	.syntax unified

	.section .text.ums9117_uboot_handoff, "ax", %progbits
	.p2align 2
	.arm
	.type ums9117_uboot_handoff, %function
	.global ums9117_uboot_handoff
ums9117_uboot_handoff:
	mov	r4, r0			@ U-Boot entry physical address
	mov	r5, r1			@ stage0 handoff physical address

	cpsid	if
	dsb	sy
	isb

	/* Leave the identity-mapped fpdoom environment with all caches off. */
	mrc	p15, 0, r3, c1, c0, 0
	bic	r3, r3, #0x00000007
	bic	r3, r3, #0x00001000
	mcr	p15, 0, r3, c1, c0, 0
	isb

	mov	r3, #0
	mcr	p15, 0, r3, c8, c7, 0
	mcr	p15, 0, r3, c7, c5, 6
	dsb	sy
	isb

	msr	cpsr_c, #0xd3
	mov	r0, r5
	mov	r1, #0
	mov	r2, #0
	mov	r3, #0
	bx	r4

	.size ums9117_uboot_handoff, . - ums9117_uboot_handoff
