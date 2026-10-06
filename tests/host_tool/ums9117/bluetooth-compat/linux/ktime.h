/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef BLUETOOTH_HOST_KTIME_H
#define BLUETOOTH_HOST_KTIME_H

#include <linux/types.h>

#define NSEC_PER_SEC 1000000000ULL
u64 ktime_get_ns(void);

#endif
