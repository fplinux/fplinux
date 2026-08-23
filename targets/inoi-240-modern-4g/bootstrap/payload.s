	@ SPDX-License-Identifier: GPL-2.0-only
	.syntax unified

	.section .fplinux_payload, "a", %progbits
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
	.incbin "../out/inoi240.dtb"
linux_dtb_end:

	.balign 64
	.global fplinux_session_start
	.global fplinux_session_end
fplinux_session_start:
	.space 512, 0
fplinux_session_end:
