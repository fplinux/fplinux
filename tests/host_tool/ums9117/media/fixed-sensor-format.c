// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdio.h>

#include "ums9117-fixed-format.h"

struct fixed_format_case {
	u32 width;
	u32 height;
};

struct v4l2_mbus_framefmt *
v4l2_subdev_state_get_format(struct v4l2_subdev_state *state, u32 pad)
{
	assert(state);
	assert(pad == 0);
	return &state->format;
}

static void check_format(const struct v4l2_mbus_framefmt *format, u32 width,
			 u32 height)
{
	assert(format->width == width);
	assert(format->height == height);
	assert(format->code == 0x2008);
	assert(format->field == 1);
	assert(format->colorspace == 8);
	assert(format->ycbcr_enc == 1);
	assert(format->quantization == 1);
	assert(format->xfer_func == 2);
}

static void check_code_enumeration(void)
{
	struct v4l2_subdev_mbus_code_enum code = { .code = 0xffff };

	assert(ums9117_fixed_format_enum_mbus_code(NULL, NULL, &code) == 0);
	assert(code.code == 0x2008);
	code.pad = 1;
	code.code = 0xffff;
	assert(ums9117_fixed_format_enum_mbus_code(NULL, NULL, &code) == -22);
	assert(code.code == 0xffff);
	code.pad = 0;
	code.index = 1;
	assert(ums9117_fixed_format_enum_mbus_code(NULL, NULL, &code) == -22);
	assert(code.code == 0xffff);
}

static void check_size_enumeration(u32 width, u32 height)
{
	struct v4l2_subdev_frame_size_enum size = { .code = 0x2008 };

	assert(ums9117_fixed_format_enum_frame_size(&size, width, height) == 0);
	assert(size.min_width == width);
	assert(size.max_width == width);
	assert(size.min_height == height);
	assert(size.max_height == height);
	size.min_width = 17;
	size.max_width = 19;
	size.min_height = 23;
	size.max_height = 29;
	size.pad = 1;
	assert(ums9117_fixed_format_enum_frame_size(&size, width, height) ==
	       -22);
	size.pad = 0;
	size.index = 1;
	assert(ums9117_fixed_format_enum_frame_size(&size, width, height) ==
	       -22);
	size.index = 0;
	size.code = 0x2006;
	assert(ums9117_fixed_format_enum_frame_size(&size, width, height) ==
	       -22);
	assert(size.min_width == 17);
	assert(size.max_width == 19);
	assert(size.min_height == 23);
	assert(size.max_height == 29);
}

static void check_state_and_set_format(u32 width, u32 height)
{
	struct v4l2_subdev_state state = {};
	struct v4l2_subdev_format format = {
		.which = V4L2_SUBDEV_FORMAT_TRY,
		.format = { .width = 1, .height = 2, .code = 0xffff },
	};

	state.format.reserved[0] = 0x789a;
	assert(ums9117_fixed_format_init_state(&state, width, height) == 0);
	check_format(&state.format, width, height);
	assert(state.format.reserved[0] == 0x789a);
	assert(ums9117_fixed_format_set_fmt(NULL, &state, &format) == 0);
	check_format(&format.format, width, height);
	assert(format.format.reserved[0] == 0x789a);
	assert(format.which == V4L2_SUBDEV_FORMAT_TRY);
	check_format(&state.format, width, height);
	assert(state.format.reserved[0] == 0x789a);
	format.which = V4L2_SUBDEV_FORMAT_ACTIVE;
	format.format.width = 1920;
	format.format.height = 1080;
	format.format.code = 0x2006;
	assert(ums9117_fixed_format_set_fmt(NULL, &state, &format) == 0);
	check_format(&format.format, width, height);
	assert(format.which == V4L2_SUBDEV_FORMAT_ACTIVE);
	check_format(&state.format, width, height);
	assert(state.format.reserved[0] == 0x789a);
	format.pad = 1;
	format.format.width = 13;
	assert(ums9117_fixed_format_set_fmt(NULL, NULL, &format) == -22);
	assert(format.format.width == 13);
	check_format(&state.format, width, height);
	assert(state.format.reserved[0] == 0x789a);
}

int main(void)
{
	static const struct fixed_format_case formats[] = { { 640, 480 },
							    { 640, 476 },
							    { 240, 320 } };
	size_t i;

	check_code_enumeration();
	for (i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) {
		check_size_enumeration(formats[i].width, formats[i].height);
		check_state_and_set_format(formats[i].width, formats[i].height);
	}
	puts("Fixed sensor format checks passed");
	return 0;
}
