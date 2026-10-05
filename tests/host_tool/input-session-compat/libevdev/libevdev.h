/* SPDX-License-Identifier: GPL-2.0-only */
/* Stub the library's event boundary; no kernel state synchronization is run. */
#ifndef FPLINUX_TEST_LIBEVDEV_H
#define FPLINUX_TEST_LIBEVDEV_H

#include <linux/input.h>

struct libevdev;
enum libevdev_read_flag {
	LIBEVDEV_READ_FLAG_SYNC = 1,
	LIBEVDEV_READ_FLAG_NORMAL = 2,
};
enum libevdev_read_status {
	LIBEVDEV_READ_STATUS_SUCCESS = 0,
	LIBEVDEV_READ_STATUS_SYNC = 1,
};
int libevdev_new_from_fd(int fd, struct libevdev **device);
void libevdev_free(struct libevdev *device);
int libevdev_set_clock_id(struct libevdev *device, int clock_id);
const char *libevdev_get_name(const struct libevdev *device);
const char *libevdev_get_phys(const struct libevdev *device);
int libevdev_has_event_code(const struct libevdev *device, unsigned int type,
			    unsigned int code);
int libevdev_next_event(struct libevdev *device, unsigned int flags,
			struct input_event *event);
int libevdev_get_event_value(const struct libevdev *device, unsigned int type,
			     unsigned int code);

#endif
