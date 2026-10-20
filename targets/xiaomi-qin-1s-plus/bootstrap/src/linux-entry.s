	@ SPDX-License-Identifier: GPL-2.0-only
	/* Copyright (C) 2016-2017 Spreadtrum Communications Inc. */
	.arch armv8-a
	.syntax unified
	.arm

	.equ QIN_AP_AHB_PHYS, 0x20e00000
	.equ QIN_AP_AHB_AHB_EB, 0x0000
	.equ QIN_AP_AHB_AHB_EB_OTG, 1 << 4
	.equ QIN_AP_AHB_AHB_RST, 0x0004
	.equ QIN_AP_AHB_AHB_RST_USB_MASK, (1 << 6) | (1 << 5) | (1 << 4)
	.equ QIN_AP_AHB_OTG_PHY_TEST, 0x3024
	.equ QIN_AP_AHB_OTG_PHY_TEST_VBUS_VALID_MASK, (1 << 24) | (1 << 22)
	.equ QIN_AP_AHB_OTG_PHY_CTRL, 0x3028
	.equ QIN_AP_AHB_OTG_PHY_CTRL_WIDTH_MASK, (1 << 30) | (1 << 29)
	.equ QIN_AP_AHB_OTG_CTRL1, 0x3030
	.equ QIN_AP_AHB_OTG_CTRL1_SAMPLER_SEL, 1 << 20
	.equ QIN_USB_GATE_DELAY_COUNT, 2000000
	.equ QIN_USB_RESET_DELAY_COUNT, 1000000

	.section .text.qin_prepare_kernel, "ax", %progbits
	.p2align 2
	.global qin_prepare_kernel
	.type qin_prepare_kernel, %function
qin_prepare_kernel:
	cpsid	if
	mrs	r3, cpsr
	and	r3, r3, #0x3f
	cmp	r3, #0x1a
	bne	.Linvalid_state
	tst	sp, #7
	bne	.Linvalid_state
	tst	r0, #7
	bne	.Linvalid_state

	push	{r4, lr}
	bl	.Lreset_usb
	pop	{r4, pc}
.Linvalid_state:
	b	.Linvalid_state
	.size qin_prepare_kernel, . - qin_prepare_kernel

.Lreset_usb:
	push	{r4, r5, r6, lr}
	ldr	r4, =QIN_AP_AHB_PHYS
	ldr	r3, [r4, #QIN_AP_AHB_AHB_EB]
	orr	r3, r3, #QIN_AP_AHB_AHB_EB_OTG
	str	r3, [r4, #QIN_AP_AHB_AHB_EB]
	ldr	r0, =QIN_USB_GATE_DELAY_COUNT
	bl	.Ldelay_count
	ldr	r3, [r4, #QIN_AP_AHB_AHB_EB]
	orr	r3, r3, #QIN_AP_AHB_AHB_EB_OTG
	str	r3, [r4, #QIN_AP_AHB_AHB_EB]

	add	r5, r4, #0x3000
	ldr	r3, [r5, #QIN_AP_AHB_OTG_PHY_TEST - 0x3000]
	orr	r3, r3, #QIN_AP_AHB_OTG_PHY_TEST_VBUS_VALID_MASK
	str	r3, [r5, #QIN_AP_AHB_OTG_PHY_TEST - 0x3000]
	ldr	r3, [r5, #QIN_AP_AHB_OTG_PHY_CTRL - 0x3000]
	bic	r3, r3, #QIN_AP_AHB_OTG_PHY_CTRL_WIDTH_MASK
	str	r3, [r5, #QIN_AP_AHB_OTG_PHY_CTRL - 0x3000]
	ldr	r3, [r5, #QIN_AP_AHB_OTG_CTRL1 - 0x3000]
	orr	r3, r3, #QIN_AP_AHB_OTG_CTRL1_SAMPLER_SEL
	str	r3, [r5, #QIN_AP_AHB_OTG_CTRL1 - 0x3000]

	ldr	r3, [r4, #QIN_AP_AHB_AHB_RST]
	orr	r3, r3, #QIN_AP_AHB_AHB_RST_USB_MASK
	str	r3, [r4, #QIN_AP_AHB_AHB_RST]
	ldr	r0, =QIN_USB_RESET_DELAY_COUNT
	bl	.Ldelay_count
	ldr	r3, [r4, #QIN_AP_AHB_AHB_RST]
	bic	r3, r3, #QIN_AP_AHB_AHB_RST_USB_MASK
	str	r3, [r4, #QIN_AP_AHB_AHB_RST]
	ldr	r0, =QIN_USB_RESET_DELAY_COUNT
	bl	.Ldelay_count
	pop	{r4, r5, r6, pc}

	/* The signed RAM counter bounds iterations, not elapsed time. */
.Ldelay_count:
	sub	sp, sp, #8
	mov	r1, #0
.Ldelay_next:
	str	r1, [sp, #4]
	ldr	r1, [sp, #4]
	cmp	r1, r0
	ldrlt	r1, [sp, #4]
	addlt	r1, r1, #1
	blt	.Ldelay_next
	add	sp, sp, #8
	bx	lr
	.ltorg

	.section .note.GNU-stack, "", %progbits
