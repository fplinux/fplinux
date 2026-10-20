/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef UMS9117_FIXED_FORMAT_H
#define UMS9117_FIXED_FORMAT_H

#include <linux/errno.h>
#include <linux/media-bus-format.h>
#include <media/v4l2-subdev.h>

static inline int
ums9117_fixed_format_init_state(struct v4l2_subdev_state *state, u32 width,
				u32 height)
{
	struct v4l2_mbus_framefmt *format =
		v4l2_subdev_state_get_format(state, 0);

	format->width = width;
	format->height = height;
	format->code = MEDIA_BUS_FMT_YUYV8_2X8;
	format->field = V4L2_FIELD_NONE;
	format->colorspace = V4L2_COLORSPACE_SRGB;
	format->ycbcr_enc = V4L2_YCBCR_ENC_601;
	format->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	format->xfer_func = V4L2_XFER_FUNC_SRGB;
	return 0;
}

static inline int
ums9117_fixed_format_enum_mbus_code(struct v4l2_subdev *sd,
				    struct v4l2_subdev_state *state,
				    struct v4l2_subdev_mbus_code_enum *code)
{
	(void)sd;
	(void)state;
	if (code->pad || code->index)
		return -EINVAL;
	code->code = MEDIA_BUS_FMT_YUYV8_2X8;
	return 0;
}

static inline int
ums9117_fixed_format_enum_frame_size(struct v4l2_subdev_frame_size_enum *size,
				     u32 width, u32 height)
{
	if (size->pad || size->index || size->code != MEDIA_BUS_FMT_YUYV8_2X8)
		return -EINVAL;
	size->min_width = width;
	size->max_width = width;
	size->min_height = height;
	size->max_height = height;
	return 0;
}

static inline int
ums9117_fixed_format_set_fmt(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *state,
			     struct v4l2_subdev_format *format)
{
	(void)sd;
	if (format->pad)
		return -EINVAL;
	format->format = *v4l2_subdev_state_get_format(state, 0);
	return 0;
}

#endif
