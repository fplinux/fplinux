	@ SPDX-License-Identifier: GPL-2.0-only
	/*
	 * Copyright (C) 2001 Deep Blue Solutions Ltd.
	 * Copyright (C) 2005 ARM Ltd.
	 */
	.arch armv8-a
	.syntax unified
	.arm

	/* Both operations preserve r7/r8/r11 and never write to the stack. */
	.macro walk_dcache operation
	dmb	sy
	mrc	p15, 1, r0, c0, c0, 1
	mov	r3, r0, lsr #23
	ands	r3, r3, #7 << 1
	beq	.Lcache_finished\@
	mov	r10, #0
.Lcache_level\@:
	add	r2, r10, r10, lsr #1
	mov	r1, r0, lsr r2
	and	r1, r1, #7
	cmp	r1, #2
	blt	.Lcache_next_level\@
	mcr	p15, 2, r10, c0, c0, 0
	isb
	mrc	p15, 1, r1, c0, c0, 0
	and	r2, r1, #7
	add	r2, r2, #4
	movw	r4, #0x3ff
	ands	r4, r4, r1, lsr #3
	clz	r5, r4
	movw	r6, #0x7fff
	and	r1, r6, r1, lsr #13
	mov	r6, #1
	movne	r4, r4, lsl r5
	movne	r6, r6, lsl r5
.Lcache_way\@:
	mov	r9, r1
.Lcache_set\@:
	mov	r5, r9, lsl r2
	orr	r5, r5, r4
	orr	r5, r5, r10
	mcr	p15, 0, r5, c7, \operation, 2
	subs	r9, r9, #1
	bge	.Lcache_set\@
	subs	r4, r4, r6
	bcs	.Lcache_way\@
.Lcache_next_level\@:
	add	r10, r10, #2
	cmp	r3, r10
	bgt	.Lcache_level\@
.Lcache_finished\@:
	mov	r10, #0
	mcr	p15, 2, r10, c0, c0, 0
	dsb	sy
	isb
	bx	lr
	.endm

	.section .text.sc9820e_enter_linux, "ax", %progbits
	.p2align 2
	.global sc9820e_enter_linux
	.type sc9820e_enter_linux, %function
sc9820e_enter_linux:
	mov	r7, r0
	mov	r8, r1
	mov	r11, r2

	/* Keep the remaining HSCTLR fields while disabling instruction fetch caching. */
	mrc	p15, 4, r3, c1, c0, 0
	bic	r3, r3, #0x1000
	mcr	p15, 4, r3, c1, c0, 0
	isb
	mov	r3, #0
	mcr	p15, 0, r3, c7, c5, 0
	mcr	p15, 0, r3, c7, c5, 6
	dsb	sy
	isb

	/* Kernel relocation and device reset stack writes must precede this clean. */
	bl	.Lclean_invalidate_dcache
	mrc	p15, 4, r3, c1, c0, 0
	bic	r3, r3, #5
	mcr	p15, 4, r3, c1, c0, 0
	isb
	bl	.Linvalidate_dcache

	/* HYP and PL1 must both satisfy the Linux translation/cache preconditions. */
	mrc	p15, 4, r3, c1, c0, 0
	tst	r3, #5
	bne	.Linvalid_state
	tst	r3, #0x1000
	bne	.Linvalid_state
	mrc	p15, 0, r3, c1, c0, 0
	tst	r3, #5
	bne	.Linvalid_state
	tst	r3, #0x1000
	bne	.Linvalid_state

	mov	r3, #0
	mcr	p15, 4, r3, c1, c1, 0
	isb
	mov	sp, r11
	dsb	sy
	isb
	mov	r0, #0
	mvn	r1, #0
	mov	r2, r8
	bx	r7
.Linvalid_state:
	b	.Linvalid_state
	.size sc9820e_enter_linux, . - sc9820e_enter_linux

.Lclean_invalidate_dcache:
	walk_dcache c14
.Linvalidate_dcache:
	walk_dcache c6

	.section .note.GNU-stack, "", %progbits
