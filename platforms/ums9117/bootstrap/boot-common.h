/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_UMS9117_BOOT_COMMON_H
#define FPLINUX_UMS9117_BOOT_COMMON_H

#include <stdint.h>

struct ums9117_boot_canvas {
	uint16_t *pixels;
	uint32_t width;
	uint32_t height;
	uint32_t stride;
};

int ums9117_boot_canvas_valid(const struct ums9117_boot_canvas *canvas);
void ums9117_boot_canvas_fill_rect(void *context, uint32_t x, uint32_t y,
				   uint32_t width, uint32_t height,
				   uint16_t colour);
void ums9117_boot_canvas_present(void *context);
uint32_t ums9117_bootstrap_ram_bytes(void);

#endif
