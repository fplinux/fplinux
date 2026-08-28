// SPDX-License-Identifier: GPL-2.0-only
#include <stddef.h>
#include <stdint.h>

#include "generated/fplinux-boot-layout.h"
#include "syscode.h"
#include "ums9117-bootstrap/boot-common.h"
#include "ums9117-common/ums9117-boot-contract.h"

int ums9117_boot_canvas_valid(const struct ums9117_boot_canvas *canvas)
{
	const uint32_t pixels = FPLINUX_BOOT_LAYOUT_FRAMEBUFFER_BYTES / 2U;

	return canvas != NULL && canvas->pixels != NULL && canvas->width != 0 &&
	       canvas->height != 0 && canvas->stride >= canvas->width &&
	       canvas->stride <= pixels &&
	       canvas->height <= pixels / canvas->stride;
}

void ums9117_boot_canvas_fill_rect(void *context, uint32_t x, uint32_t y,
				   uint32_t width, uint32_t height,
				   uint16_t colour)
{
	struct ums9117_boot_canvas *canvas = context;
	uint32_t xx;
	uint32_t yy;

	if (!ums9117_boot_canvas_valid(canvas) || width == 0 || height == 0 ||
	    x >= canvas->width || y >= canvas->height)
		return;
	if (width > canvas->width - x)
		width = canvas->width - x;
	if (height > canvas->height - y)
		height = canvas->height - y;
	for (yy = 0; yy < height; ++yy) {
		uint32_t offset = (y + yy) * canvas->stride + x;

		for (xx = 0; xx < width; ++xx)
			canvas->pixels[offset + xx] = colour;
	}
}

void ums9117_boot_canvas_present(void *context)
{
	if (!ums9117_boot_canvas_valid(context))
		return;
	sys_start_refresh();
	sys_wait_refresh();
}

uint32_t ums9117_bootstrap_ram_bytes(void)
{
	return *(volatile uint32_t
			 *)(uintptr_t)(FPLINUX_BOOT_LAYOUT_RAM_BASE_PHYS +
				       FPLINUX_BOOT_RAM_SIZE_WORD_OFFSET);
}
