/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_TEST_SDBOOT_UNALIGNED_H
#define FPLINUX_TEST_SDBOOT_UNALIGNED_H

#include <endian.h>
#include <stdint.h>
#include <string.h>

static inline uint32_t get_unaligned_le32(const void *address)
{
	uint32_t value;

	memcpy(&value, address, sizeof(value));
	return le32toh(value);
}

#endif
