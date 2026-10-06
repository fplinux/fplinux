/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LCM_HOST_SPINLOCK_H
#define LCM_HOST_SPINLOCK_H
/* Layout placeholder only: the transport never locks; no locking is tested. */
typedef struct {
	unsigned int unused;
} spinlock_t;
#endif
