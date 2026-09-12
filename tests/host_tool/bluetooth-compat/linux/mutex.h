/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef BLUETOOTH_HOST_MUTEX_H
#define BLUETOOTH_HOST_MUTEX_H

#include <assert.h>
#include <stdbool.h>

/* Single-threaded scheduling fake: detects recursive ownership, not races. */
struct mutex {
	bool held;
};
#define DEFINE_MUTEX(name) struct mutex name
static inline void mutex_lock(struct mutex *lock)
{
	assert(!lock->held);
	lock->held = true;
}
static inline void mutex_unlock(struct mutex *lock)
{
	assert(lock->held);
	lock->held = false;
}

typedef struct mutex spinlock_t;
#define spin_lock_irqsave(lock, flags) ((flags) = 0, mutex_lock(lock))
#define spin_unlock_irqrestore(lock, flags) \
	do {                                \
		(void)(flags);              \
		mutex_unlock(lock);         \
	} while (0)

#endif
