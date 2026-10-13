/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_MARLIN2_SETUP_HOST_WORKQUEUE_H
#define FPLINUX_MARLIN2_SETUP_HOST_WORKQUEUE_H

#include <stdbool.h>

/* Setup runs through the HCI callback; these cases enqueue no transmit work. */
struct work_struct {
	void (*function)(struct work_struct *work);
};
#define INIT_WORK(item, callback) ((item)->function = (callback))
static inline bool schedule_work(struct work_struct *work)
{
	work->function(work);
	return true;
}
static inline bool cancel_work_sync(struct work_struct *work)
{
	(void)work;
	return false;
}
static inline bool flush_work(struct work_struct *work)
{
	(void)work;
	return false;
}

#endif
