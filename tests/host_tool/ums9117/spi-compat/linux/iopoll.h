/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_SPI_HOST_IOPOLL_H
#define FPLINUX_SPI_HOST_IOPOLL_H

#include <linux/delay.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/ktime.h>

/* Fake kernel polling boundary: MMIO and deadlines use virtual time. */
#define readl_poll_timeout_atomic(address, value, condition, delay, timeout) \
	({                                                                   \
		ktime_t deadline = ktime_add_us(ktime_get(), (timeout));     \
		int result = -ETIMEDOUT;                                     \
		for (;;) {                                                   \
			(value) = readl(address);                            \
			if (condition) {                                     \
				result = 0;                                  \
				break;                                       \
			}                                                    \
			if ((timeout) && ktime_get() >= deadline)            \
				break;                                       \
			udelay(delay);                                       \
		}                                                            \
		result;                                                      \
	})

#endif
