/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_SPI_HOST_KTIME_H
#define FPLINUX_SPI_HOST_KTIME_H

#include <linux/types.h>

/* Virtual microseconds; no real host or controller timing is exercised. */
typedef s64 ktime_t;
ktime_t ktime_get(void);

static inline ktime_t ktime_add_us(ktime_t time, u64 delay)
{
	return time + delay;
}

static inline s64 ktime_us_delta(ktime_t later, ktime_t earlier)
{
	return later - earlier;
}

#endif
