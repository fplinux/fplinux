/* SPDX-License-Identifier: GPL-2.0-only */
/* Byte-addressed memory models the mailbox component's shared SIPC window. */
#include <string.h>
#include <linux/types.h>

#define __iomem

static inline u32 readl(const void *address)
{
	return *(const u32 *)address;
}

static inline void writel(u32 value, void *address)
{
	*(u32 *)address = value;
}

static inline void memset_io(void *destination, int value, size_t bytes)
{
	memset(destination, value, bytes);
}

static inline void memcpy_toio(void *destination, const void *source,
			       size_t bytes)
{
	memcpy(destination, source, bytes);
}

static inline void memcpy_fromio(void *destination, const void *source,
				 size_t bytes)
{
	memcpy(destination, source, bytes);
}

#define mb() __sync_synchronize()
