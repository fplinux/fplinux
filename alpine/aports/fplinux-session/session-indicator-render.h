/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_SESSION_INDICATOR_RENDER_H
#define FPLINUX_SESSION_INDICATOR_RENDER_H

#include <stddef.h>
#include <stdint.h>

#include "session-indicator.h"

/* The frame owns one immutable RGB565 panel until clear. */
struct fplinux_session_indicator_frame {
	uint16_t *pixels;
	int x, y, width, height;
	size_t stride_bytes;
};

/* Output must be empty. A failed render leaves it empty. */
bool fplinux_session_indicator_render(
	struct fplinux_session_indicator_frame *frame, int screen_width,
	int screen_height, int theme,
	const struct fplinux_session_indicator *indicator);
void fplinux_session_indicator_frame_clear(
	struct fplinux_session_indicator_frame *frame);

#endif
