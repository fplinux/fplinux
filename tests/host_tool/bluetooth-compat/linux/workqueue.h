/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef BLUETOOTH_HOST_WORKQUEUE_H
#define BLUETOOTH_HOST_WORKQUEUE_H

#include <stdbool.h>

struct work_struct {
	void (*function)(struct work_struct *work);
};
struct delayed_work {
	struct work_struct work;
	bool pending;
};
#define INIT_DELAYED_WORK(item, callback) ((item)->work.function = (callback))
bool schedule_delayed_work(struct delayed_work *work, unsigned long delay);
bool cancel_delayed_work_sync(struct delayed_work *work);

#endif
