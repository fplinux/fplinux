/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_FM_HOST_UNALIGNED_H
#define FPLINUX_FM_HOST_UNALIGNED_H

#include <linux/types.h>

static inline u16 get_unaligned_le16(const u8 *bytes)
{
	return bytes[0] | (u16)bytes[1] << 8;
}

static inline u32 get_unaligned_le32(const u8 *bytes)
{
	return bytes[0] | (u32)bytes[1] << 8 | (u32)bytes[2] << 16 |
	       (u32)bytes[3] << 24;
}

static inline void put_unaligned_le16(u16 value, u8 *bytes)
{
	bytes[0] = value;
	bytes[1] = value >> 8;
}

static inline void put_unaligned_le32(u32 value, u8 *bytes)
{
	bytes[0] = value;
	bytes[1] = value >> 8;
	bytes[2] = value >> 16;
	bytes[3] = value >> 24;
}

#endif
