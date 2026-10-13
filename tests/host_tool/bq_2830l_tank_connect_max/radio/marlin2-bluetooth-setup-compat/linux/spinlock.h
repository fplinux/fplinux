/* SPDX-License-Identifier: GPL-2.0-only */
#include <linux/mutex.h>

#define spin_lock_init(lock) mutex_init(lock)
