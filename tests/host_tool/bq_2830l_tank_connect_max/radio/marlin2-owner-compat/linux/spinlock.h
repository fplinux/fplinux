/* SPDX-License-Identifier: GPL-2.0-only */
#include "../kernel-boundary.h"

/* The fixture uses pthread mutexes, not kernel IRQ masking. */
typedef struct mutex spinlock_t;
#define spin_lock_init(lock) mutex_init(lock)
#define spin_lock_irqsave(lock, flags) ((flags) = 0, mutex_lock(lock))
#define spin_unlock_irqrestore(lock, flags) \
	do {                                \
		(void)(flags);              \
		mutex_unlock(lock);         \
	} while (0)
