	@ SPDX-License-Identifier: GPL-2.0-only
	.arch armv7-a
	.syntax unified

	/*
	 * Linux ARM boot protocol:
	 *   r0 = 0
	 *   r1 = legacy machine id (unused with DT)
	 *   r2 = physical DTB address
	 *   SVC mode, IRQ/FIQ masked, MMU and D-cache off
	 */
	.section .text.ta1618_linux_handoff, "ax", %progbits
	.p2align 2
	.arm
	.type ta1618_linux_handoff, %function
	.global ta1618_linux_handoff
ta1618_linux_handoff:
	mov	r4, r0			@ zImage physical address
	mov	r5, r1			@ DTB physical address

	cpsid	if
	dsb	sy
	isb

	/* Disable MMU, alignment checking, D-cache and I-cache. */
	mrc	p15, 0, r3, c1, c0, 0
	bic	r3, r3, #0x00000007
	bic	r3, r3, #0x00001000
	mcr	p15, 0, r3, c1, c0, 0
	isb

	/* Invalidate unified TLB and branch predictor after leaving fpdoom MMU. */
	mov	r3, #0
	mcr	p15, 0, r3, c8, c7, 0
	mcr	p15, 0, r3, c7, c5, 6
	dsb	sy
	isb

	/* Enter SVC with IRQ and FIQ masked; do not touch the stack afterwards. */
	msr	cpsr_c, #0xd3
	mov	r0, #0
	mvn	r1, #0
	mov	r2, r5
	bx	r4

	.size ta1618_linux_handoff, . - ta1618_linux_handoff
