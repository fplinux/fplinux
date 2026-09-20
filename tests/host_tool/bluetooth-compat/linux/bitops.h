/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef BLUETOOTH_HOST_BITOPS_H
#define BLUETOOTH_HOST_BITOPS_H

#include <assert.h>
#include <linux/types.h>

#define BIT(bit) (1UL << (bit))

static inline unsigned long __ffs(unsigned long value)
{
	assert(value);
	return __builtin_ctzl(value);
}

#endif
