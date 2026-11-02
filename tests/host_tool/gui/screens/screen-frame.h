/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_SCREEN_FRAME_H
#define FPLINUX_SCREEN_FRAME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Save the visible pixels as packed little-endian RGB565 rows. */
bool screen_frame_write(const char *path, const uint16_t *pixels,
			unsigned int width, unsigned int height,
			size_t stride_bytes);
bool screen_frame_read(const char *path, uint16_t *pixels, unsigned int width,
		       unsigned int height);

#endif
