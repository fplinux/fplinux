/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_SESSION_LAUNCH_SURFACE_H
#define FPLINUX_SESSION_LAUNCH_SURFACE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fplinux-ui-session.h"

#define FPLINUX_LAUNCH_FRAME_MAX_BYTES \
	(FPLINUX_UI_SESSION_FRAME_MAX_PIXELS * 2U)
#define FPLINUX_LAUNCH_SPINNER_MAX_SIZE 32
#define FPLINUX_LAUNCH_SPINNER_PIXELS \
	(FPLINUX_LAUNCH_SPINNER_MAX_SIZE * FPLINUX_LAUNCH_SPINNER_MAX_SIZE)
#define FPLINUX_LAUNCH_SPINNER_PHASES 8U
#define FPLINUX_LAUNCH_SPINNER_MS 80

/* One tightly packed RGB565 image, owned by the shell. */
struct fplinux_launch_frame {
	uint16_t *pixels;
	size_t bytes;
	int width;
	int height;
};

struct fplinux_launch_frame_rect {
	int x;
	int y;
	int width;
	int height;
};

void fplinux_launch_frame_clear(struct fplinux_launch_frame *frame);
bool fplinux_launch_frame_fill(struct fplinux_launch_frame *frame, int width,
			       int height, uint32_t rgb);
/* Renderer pixels are top-down premultiplied ABGR8888, clipped after placement. */
void fplinux_launch_frame_blend(struct fplinux_launch_frame *frame,
				const uint32_t *pixels, int width, int height,
				int x, int y,
				struct fplinux_launch_frame_rect clip);
/* Returns an owned close-on-exec descriptor; the frame keeps its pixels. */
int fplinux_launch_frame_fd(const struct fplinux_launch_frame *frame);
/* Borrows fd, including its offset. Failure also clears the previous frame. */
bool fplinux_launch_frame_copy(struct fplinux_launch_frame *frame, int fd,
			       int width, int height);
/* Loads a packaged RGB565 image into owned storage and closes its descriptor. */
bool fplinux_launch_frame_load(struct fplinux_launch_frame *frame,
			       const char *path, int width, int height);
/* Tightly packed premultiplied ARGB8888; invalid geometry clears and fails. */
bool fplinux_launch_spinner_draw(uint32_t pixels[FPLINUX_LAUNCH_SPINNER_PIXELS],
				 int size, int line_width, unsigned int phase,
				 uint32_t rgb);

#endif
