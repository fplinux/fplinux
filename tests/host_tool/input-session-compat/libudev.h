/* SPDX-License-Identifier: GPL-2.0-only */
/* Fake udev lifetime boundary; no host devices are discovered. */
#ifndef FPLINUX_TEST_LIBUDEV_H
#define FPLINUX_TEST_LIBUDEV_H

struct udev;
struct udev *udev_new(void);
struct udev *udev_unref(struct udev *context);

#endif
