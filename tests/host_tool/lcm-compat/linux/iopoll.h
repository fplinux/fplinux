/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LCM_HOST_IOPOLL_H
#define LCM_HOST_IOPOLL_H

#include <linux/io.h>
#include <linux/kernel.h>

u64 lcm_fake_time_us(void);
void lcm_fake_delay_us(unsigned long delay);

/*
 * Kernel boundary fake: poll the supplied MMIO expression and predicate, with
 * virtual delays and a final read at the deadline. No host scheduling or real
 * elapsed-time guarantee is tested. The driver's wait remains unmodified.
 */
#define readl_poll_timeout_atomic(address, value, condition, delay, timeout) \
	({                                                                   \
		u64 deadline = lcm_fake_time_us() + (timeout);               \
		int result = -ETIMEDOUT;                                     \
		for (;;) {                                                   \
			(value) = readl(address);                            \
			if (condition) {                                     \
				result = 0;                                  \
				break;                                       \
			}                                                    \
			if ((timeout) && lcm_fake_time_us() >= deadline)     \
				break;                                       \
			lcm_fake_delay_us(delay);                            \
		}                                                            \
		result;                                                      \
	})

#endif
