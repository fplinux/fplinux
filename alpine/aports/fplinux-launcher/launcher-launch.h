/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_LAUNCHER_LAUNCH_H
#define FPLINUX_LAUNCHER_LAUNCH_H

#include <stddef.h>

/* Copies RGB565 rows into an owned, packed, close-on-exec anonymous file. */
int fplinux_launcher_launch_fd(const void *pixels, size_t stride, int width,
			       int height);

#endif
