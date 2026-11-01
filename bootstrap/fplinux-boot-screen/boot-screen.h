// SPDX-License-Identifier: GPL-2.0-only
#ifndef FPLINUX_BOOT_SCREEN_H
#define FPLINUX_BOOT_SCREEN_H

#include <stddef.h>
#include <stdint.h>

#include "fplinux-status-screen.h"

#define FPLINUX_BOOT_SCREEN_STATUS_BYTES 96U

/* Error detail is copied so callers may pass temporary buffers. */
struct fplinux_boot_screen {
	struct fplinux_status_canvas canvas;
	char detail[FPLINUX_BOOT_SCREEN_STATUS_BYTES];
	uint32_t error_code;
	uint8_t initialized;
	uint8_t failed;
};

int fplinux_boot_screen_init(struct fplinux_boot_screen *screen,
			     const struct fplinux_status_canvas *canvas);
void fplinux_boot_screen_fail(struct fplinux_boot_screen *screen,
			      uint32_t error_code, const char *detail);

#endif
