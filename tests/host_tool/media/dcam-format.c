// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <errno.h>
#include <linux/media-bus-format.h>
#include <stdio.h>
#include <stdlib.h>

#include "ums9117-dcam-format.h"

struct dcam_case {
	u32 width;
	u32 height;
	u32 code;
	unsigned int data_width;
	unsigned int support;
	struct ums9117_dcam_format expected;
};

static void check_selected_formats(unsigned int expected_support)
{
	static const struct dcam_case cases[] = {
		{ 240,
		  320,
		  MEDIA_BUS_FMT_YUYV8_2X8,
		  1,
		  1,
		  { 76800, 0x014000f0, 0, 0x013f01df, 0x601 } },
		{ 640,
		  476,
		  MEDIA_BUS_FMT_YUYV8_2X8,
		  2,
		  2,
		  { 304640, 0x01dc0280, 2, 0x01db0501, 0x409 } },
		{ 800,
		  600,
		  MEDIA_BUS_FMT_YUYV8_2X8,
		  0,
		  4,
		  { 480000, 0x02580320, 0, 0x0257063f, 0x008 } },
		{ 1600,
		  1200,
		  MEDIA_BUS_FMT_YUYV8_2X8,
		  0,
		  4,
		  { 1920000, 0x04b00640, 0, 0x04af0c7f, 0x008 } },
	};
	struct ums9117_dcam_format actual;
	size_t i;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		const struct dcam_case *test = &cases[i];
		int ret;

		ret = ums9117_dcam_format_configure(test->width, test->height,
						    test->code,
						    test->data_width, &actual);
		if (!(expected_support & test->support)) {
			assert(ret == -EINVAL);
			continue;
		}
		assert(ret == 0);
		assert(actual.y_bytes == test->expected.y_bytes);
		assert(actual.source_size == test->expected.source_size);
		assert(actual.capture_start == test->expected.capture_start);
		assert(actual.capture_end == test->expected.capture_end);
		assert(actual.capture_control ==
		       test->expected.capture_control);
	}
}

static void check_unsupported_formats(void)
{
	static const struct dcam_case cases[] = {
		{ 640, 480, MEDIA_BUS_FMT_YUYV8_2X8, 2, 0, {} },
		{ 640, 476, MEDIA_BUS_FMT_YVYU8_2X8, 2, 0, {} },
		{ 640, 476, MEDIA_BUS_FMT_YVYU8_2X8, 1, 0, {} },
		{ 240, 320, MEDIA_BUS_FMT_YVYU8_2X8, 1, 0, {} },
		{ 240, 320, MEDIA_BUS_FMT_YUYV8_2X8, 2, 0, {} },
		{ 640, 476, MEDIA_BUS_FMT_YVYU8_2X8, 0, 0, {} },
		{ 640, 476, MEDIA_BUS_FMT_YVYU8_2X8, 4, 0, {} },
		{ 0, 0, MEDIA_BUS_FMT_YVYU8_2X8, 2, 0, {} },
	};
	struct ums9117_dcam_format actual;
	size_t i;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		const struct dcam_case *test = &cases[i];

		assert(ums9117_dcam_format_configure(
			       test->width, test->height, test->code,
			       test->data_width, &actual) == -EINVAL);
	}
}

int main(int argc, char *argv[])
{
	assert(argc == 2);
	check_selected_formats(strtoul(argv[1], NULL, 0));
	check_unsupported_formats();
	puts("DCAM format checks passed");
	return 0;
}
