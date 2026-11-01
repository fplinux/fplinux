// SPDX-License-Identifier: GPL-2.0-only
#include "boot-screen.h"
#include "fplinux-boot-mark.h"
#include "generated/fplinux-status-profile.h"

static void copy_text(char *destination, size_t capacity, const char *source)
{
	size_t i = 0;

	if (source)
		while (i + 1U < capacity && source[i]) {
			destination[i] = source[i];
			++i;
		}
	if (capacity)
		destination[i] = '\0';
}

static void error_heading(char *out, uint32_t code)
{
	static const char prefix[] = "Boot error ";
	char digits[10];
	size_t length = 0, i = 0;

	while (prefix[i]) {
		out[i] = prefix[i];
		++i;
	}
	do {
		digits[length++] = (char)('0' + code % 10U);
		code /= 10U;
	} while (code);
	while (length)
		out[i++] = digits[--length];
	out[i] = '\0';
}

static int render(struct fplinux_boot_screen *screen)
{
	const struct fplinux_boot_mark *asset = NULL;
	struct fplinux_status_mark mark = { 0 };
	struct fplinux_status_resources resources = {
		.profile = &fplinux_status_profile,
		.theme = &fplinux_status_themes[1],
		.body = fplinux_status_font_by_id(
			fplinux_status_profile.font_body),
		.title = fplinux_status_font_by_id(
			fplinux_status_profile.font_title),
		.legend = fplinux_status_font_by_id(
			fplinux_status_profile.font_legend),
	};
	struct fplinux_status_view view = {
		.layout = FPLINUX_STATUS_BOOT,
		.indicator = FPLINUX_STATUS_NONE,
		.heading = "Starting FPLinux",
		.detail = "Keep USB connected.",
	};
	char heading[22];

	if (!screen->initialized)
		return -1;
	for (size_t i = 0; i < FPLINUX_BOOT_MARK_COUNT; ++i)
		if (fplinux_boot_marks[i].size ==
		    (unsigned int)fplinux_status_profile.splash_mark) {
			asset = &fplinux_boot_marks[i];
			break;
		}
	if (asset) {
		mark.size = asset->size;
		mark.runs = asset->runs;
		mark.run_count = asset->run_count;
		mark.palette = fplinux_boot_mark_palette;
		resources.mark = &mark;
	}
	if (screen->failed) {
		error_heading(heading, screen->error_code);
		view.heading = heading;
		view.detail = screen->detail;
		view.error = 1;
	}
	return fplinux_status_screen_render(&screen->canvas, &resources, &view);
}

int fplinux_boot_screen_init(struct fplinux_boot_screen *screen,
			     const struct fplinux_status_canvas *canvas)
{
	if (!screen)
		return -1;
	screen->initialized = 0;
	if (!canvas || !canvas->width || !canvas->height || !canvas->fill ||
	    !canvas->present)
		return -1;
	screen->canvas = *canvas;
	screen->detail[0] = '\0';
	screen->error_code = 0;
	screen->failed = 0;
	screen->initialized = 1;
	if (render(screen)) {
		screen->initialized = 0;
		return -1;
	}
	return 0;
}

void fplinux_boot_screen_fail(struct fplinux_boot_screen *screen, uint32_t code,
			      const char *detail)
{
	if (!screen || !screen->initialized)
		return;
	screen->error_code = code;
	copy_text(screen->detail, sizeof(screen->detail), detail);
	screen->failed = 1;
	(void)render(screen);
}
