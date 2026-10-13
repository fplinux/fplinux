/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef BLUETOOTH_HOST_COMPLETION_H
#define BLUETOOTH_HOST_COMPLETION_H

#include <stdbool.h>

/* Waits pump the fake workqueue; they do not model kernel concurrency. */
struct completion {
	bool done;
};

static inline void init_completion(struct completion *completion)
{
	completion->done = false;
}

static inline void reinit_completion(struct completion *completion)
{
	completion->done = false;
}

static inline void complete(struct completion *completion)
{
	completion->done = true;
}

#define msecs_to_jiffies(milliseconds) (milliseconds)
unsigned long wait_for_completion_timeout(struct completion *completion,
					  unsigned long timeout);

#endif
