/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_LAUNCHER_ICONS_H
#define FPLINUX_LAUNCHER_ICONS_H

#include <stddef.h>
#include <stdint.h>

#include <lvgl/lvgl.h>

#define FPLINUX_LAUNCHER_APP_COUNT 64
/* Room for the icon sizes of every profile (launcher-layout.h). */
#define FPLINUX_LAUNCHER_ICON_SIZE_COUNT 8

struct fplinux_launcher_icons {
	/* Sizes of the icon directories, ascending. */
	int sizes[FPLINUX_LAUNCHER_ICON_SIZE_COUNT];
	size_t size_count;
	/* Hashes of problems in the current and previous catalogue load. */
	uint32_t current[2 * FPLINUX_LAUNCHER_APP_COUNT];
	uint32_t previous[2 * FPLINUX_LAUNCHER_APP_COUNT];
	size_t current_count;
	size_t previous_count;
};

void fplinux_launcher_icons_init(struct fplinux_launcher_icons *icons);
void fplinux_launcher_icons_begin(struct fplinux_launcher_icons *icons);
void fplinux_launcher_icons_report(struct fplinux_launcher_icons *icons,
				   const char *app_id, const char *problem);
lv_draw_buf_t *fplinux_launcher_icons_load(struct fplinux_launcher_icons *icons,
					   const char *app_id, const char *name,
					   int size);

#endif
