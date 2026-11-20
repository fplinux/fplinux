/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef UMS9117_TEST_SENSOR_SUBDEV_H
#define UMS9117_TEST_SENSOR_SUBDEV_H

#include <time.h>
#include <linux/v4l2-subdev.h>

typedef __u32 u32;

struct v4l2_subdev {
	int unused;
};

struct v4l2_subdev_state {
	struct v4l2_mbus_framefmt format;
};

struct v4l2_mbus_framefmt *
v4l2_subdev_state_get_format(struct v4l2_subdev_state *state, u32 pad);

#endif
