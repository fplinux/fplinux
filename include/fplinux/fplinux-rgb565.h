/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef FPLINUX_RGB565_H
#define FPLINUX_RGB565_H

#include <stddef.h>
#include <stdint.h>

enum fplinux_rgb565_scale {
	FPLINUX_RGB565_FIT,
	FPLINUX_RGB565_ONE_TO_ONE,
	FPLINUX_RGB565_INTEGER,
};

/* Clockwise rotation of the source image. */
enum fplinux_rgb565_rotation {
	FPLINUX_RGB565_ROTATE_0,
	FPLINUX_RGB565_ROTATE_90,
	FPLINUX_RGB565_ROTATE_270,
};

struct fplinux_rgb565_viewport {
	int source_width, source_height;
	int destination_width, destination_height;
	int x, y, width, height;
	enum fplinux_rgb565_rotation rotation;
};

/*
 * Initialize a centered image in a destination of 1..4096 pixels per axis.
 * Source dimensions have the same limits. FIT preserves aspect ratio with
 * dimensions rounded down (at least one pixel). INTEGER uses the largest
 * whole enlargement that fits, or the smallest reciprocal whole reduction
 * that fits, with dimensions rounded down. ONE_TO_ONE may be clipped.
 * Returns 0 on success, -1 on invalid input; failure leaves *viewport intact.
 * Treat the resulting fields as read-only until the next initialization.
 */
int fplinux_rgb565_viewport_init(struct fplinux_rgb565_viewport *viewport,
				 int source_width, int source_height,
				 int destination_width, int destination_height,
				 enum fplinux_rgb565_scale scale,
				 enum fplinux_rgb565_rotation rotation);

/*
 * Copy source into the visible viewport; leave margins and row padding alone.
 * RGB565 is represented by native uint16_t values (red in bits 15..11).
 * Strides are in pixels. Buffers must be distinct and contain all rows and
 * pixels described by the viewport and strides; no memory is allocated.
 * Enlargement uses top-left nearest sampling; reduction averages each colour
 * channel over the covered area, rounded to nearest channel value.
 * Returns 0 or -1; invalid arguments leave the destination untouched.
 */
int fplinux_rgb565_transform(const struct fplinux_rgb565_viewport *viewport,
			     const uint16_t *source, size_t source_stride,
			     uint16_t *destination, size_t destination_stride);

/*
 * Map a destination pixel to source coordinates using top-left sampling.
 * Returns 1 inside the visible image, 0 outside, -1 on invalid arguments.
 * Coordinates are written only on a return value of 1.
 */
int fplinux_rgb565_map_point(const struct fplinux_rgb565_viewport *viewport,
			     int x, int y, int *source_x, int *source_y);

#endif
