/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_LAUNCHER_LAUNCH_H
#define FPLINUX_LAUNCHER_LAUNCH_H

#include <stddef.h>
#include <stdbool.h>

struct fplinux_launcher_backdrop {
	void *pixels;
	size_t bytes;
	int width;
	int height;
};

/* The mapping survives closing the borrowed descriptor. */
bool fplinux_launcher_backdrop_map(struct fplinux_launcher_backdrop *backdrop,
				   int fd, int width, int height);
void fplinux_launcher_backdrop_clear(struct fplinux_launcher_backdrop *backdrop);

/* Copies RGB565 rows into an owned, packed, close-on-exec anonymous file. */
int fplinux_launcher_launch_fd(const void *pixels, size_t stride, int width,
			       int height);

#endif
