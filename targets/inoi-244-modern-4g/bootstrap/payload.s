	@ SPDX-License-Identifier: GPL-2.0-only
	.syntax unified

	.section .rodata.inoi244_payload, "a", %progbits
	.balign 64
	.global linux_zimage_start
	.global linux_zimage_end
linux_zimage_start:
	.incbin "../out/zImage"
linux_zimage_end:

	.balign 64
	.global linux_dtb_start
	.global linux_dtb_end
linux_dtb_start:
	.incbin "../out/inoi244.dtb"
linux_dtb_end:
