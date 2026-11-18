/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_STATUS_SCREEN_H
#define FPLINUX_STATUS_SCREEN_H

#include <stddef.h>
#include <stdint.h>

#include "fplinux-status-font.h"
#include "fplinux-activity.h"
#include "fplinux-ui-profile.h"

typedef void (*fplinux_status_fill_fn)(void *context, uint32_t x, uint32_t y,
				       uint32_t width, uint32_t height,
				       uint16_t colour);
typedef void (*fplinux_status_present_fn)(void *context);

struct fplinux_status_canvas {
	uint32_t width, height;
	void *context;
	fplinux_status_fill_fn fill;
	fplinux_status_present_fn present;
};

struct fplinux_status_surface {
	uint16_t *pixels;
	uint32_t width, height;
	size_t stride_bytes;
};

struct fplinux_status_mark {
	unsigned int size;
	const uint8_t *runs;
	size_t run_count;
	const uint16_t *palette;
};

struct fplinux_status_resources {
	const struct fplinux_ui_profile_spec *profile;
	const struct fplinux_ui_theme_spec *theme;
	const struct fplinux_status_font *body;
	const struct fplinux_status_font *title;
	const struct fplinux_status_mark *mark;
	const struct fplinux_status_mask *activity;
};

enum fplinux_status_layout {
	FPLINUX_STATUS_BOOT,
	FPLINUX_STATUS_PREPARATION,
};

enum fplinux_status_indicator {
	FPLINUX_STATUS_NONE,
	FPLINUX_STATUS_BUSY,
	FPLINUX_STATUS_PROGRESS,
};

/* Strings and resources are borrowed only for the duration of render. */
struct fplinux_status_view {
	enum fplinux_status_layout layout;
	enum fplinux_status_indicator indicator;
	const char *heading;
	const char *detail;
	const char *progress_percent;
	const char *progress_bytes;
	uint64_t completed, total;
	int error;
};

static inline unsigned int fplinux_status_mark_top(unsigned int height,
						   unsigned int size)
{
	unsigned int centre = height / 5U * 2U + height % 5U * 2U / 5U;
	unsigned int half = size / 2U;

	return centre > half ? centre - half : 0;
}

/* A canvas callback clips no pixels; the renderer clips to its dimensions. */
int fplinux_status_screen_render(
	const struct fplinux_status_canvas *canvas,
	const struct fplinux_status_resources *resources,
	const struct fplinux_status_view *view);
/* Floor of completed / total * scale, clamped without overflowing counts. */
unsigned int fplinux_status_scale(uint64_t completed, uint64_t total,
				  unsigned int scale);

/* Initialise a canvas over a caller-owned RGB565 surface with byte stride. */
int fplinux_status_surface_canvas(struct fplinux_status_canvas *canvas,
				  struct fplinux_status_surface *surface);

/* Raster primitives shared by the status screens and compact shell feedback. */
void fplinux_status_fill(const struct fplinux_status_canvas *canvas, uint32_t x,
			 uint32_t y, uint32_t width, uint32_t height,
			 uint16_t colour);
uint16_t fplinux_status_blend(uint16_t foreground, uint16_t background,
			      unsigned int alpha, unsigned int scale);
unsigned int fplinux_status_text_width(const struct fplinux_status_font *font,
				       const char *text);
void fplinux_status_text_draw(const struct fplinux_status_canvas *canvas,
			      const struct fplinux_status_font *font, int x,
			      int y, const char *text, uint16_t colour,
			      uint16_t background);

#endif
