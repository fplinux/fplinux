/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef QIN_SIPC_TEST_IO_H
#define QIN_SIPC_TEST_IO_H

#include <endian.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#define __iomem
#define cpu_to_le32(value) htole32(value)

static inline void writel(uint32_t value, void *address)
{
	uint32_t little = htole32(value);

	memcpy(address, &little, sizeof(little));
}

static inline uint32_t readl(const void *address)
{
	uint32_t little;

	memcpy(&little, address, sizeof(little));
	return le32toh(little);
}

#define memset_io memset
#define memcpy_toio memcpy
#define memcpy_fromio memcpy
#define wmb() atomic_thread_fence(memory_order_release)
#define rmb() atomic_thread_fence(memory_order_acquire)
#define mb() atomic_thread_fence(memory_order_seq_cst)

#endif
