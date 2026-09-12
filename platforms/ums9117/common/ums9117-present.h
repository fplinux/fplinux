/* SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note */
#ifndef FPLINUX_UMS9117_PRESENT_H
#define FPLINUX_UMS9117_PRESENT_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define UMS9117_PRESENT_RGB565 0x50424752U
#define UMS9117_PRESENT_NV16 0x3631564eU

/*
 * One tightly packed native-size frame, copied from userspace. NV16 contains
 * the full Y plane followed by interleaved Cb,Cr at the same row count.
 * RGB565 is little-endian. Both layouts occupy width * height * 2 bytes.
 * Successful return confirms LCDC DONE, not optical presentation latency.
 */
struct ums9117_present {
	__aligned_u64 pixels;
	__u32 format;
	__u32 bytes;
	__aligned_u64 sequence;
	__aligned_u64 transfer_ns;
};

#define UMS9117_FBIO_PRESENT _IOWR('F', 0x80, struct ums9117_present)

#endif
