/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_DISPLAY_TEST_PATTERN_H
#define FPLINUX_DISPLAY_TEST_PATTERN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum fplinux_display_test_pattern {
	FPLINUX_DISPLAY_TEST_GEOMETRY,
	FPLINUX_DISPLAY_TEST_GRAY,
	FPLINUX_DISPLAY_TEST_IDS,
	FPLINUX_DISPLAY_TEST_MOTION,
};

/* RGB565 pixels occupy width * 2 bytes; row padding is left untouched. */
bool fplinux_display_test_draw(uint8_t *pixels, size_t size, size_t stride,
			       uint32_t width, uint32_t height,
			       enum fplinux_display_test_pattern pattern,
			       uint32_t frame_id);

#endif
