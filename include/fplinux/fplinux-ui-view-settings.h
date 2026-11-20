/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef FPLINUX_UI_VIEW_SETTINGS_H
#define FPLINUX_UI_VIEW_SETTINGS_H

#include <stdbool.h>
#include <stddef.h>

#include "fplinux-rgb565.h"

struct fplinux_ui_prefs;
struct fplinux_ui;
struct fplinux_ui_screen;

struct fplinux_ui_view_settings {
	enum fplinux_rgb565_scale scale;
	enum fplinux_rgb565_rotation rotation;
};

enum fplinux_ui_view_setting {
	FPLINUX_UI_VIEW_SCALE,
	FPLINUX_UI_VIEW_ROTATION,
};

struct fplinux_ui_view_settings fplinux_ui_view_settings_defaults(void);
/* Missing fields inherit defaults. Invalid stored settings return false. */
bool fplinux_ui_view_settings_load(
	struct fplinux_ui_view_settings *settings,
	const struct fplinux_ui_prefs *prefs,
	const struct fplinux_ui_view_settings *defaults);
/* Store/reset change prefs memory only; caller saves and checks the result. */
bool fplinux_ui_view_settings_store(
	const struct fplinux_ui_view_settings *settings,
	struct fplinux_ui_prefs *prefs);
/* Store one override, preserving the other field's explicit value or absence. */
bool fplinux_ui_view_settings_store_one(struct fplinux_ui_prefs *prefs,
					enum fplinux_ui_view_setting setting,
					size_t selected);
bool fplinux_ui_view_settings_reset(struct fplinux_ui_prefs *prefs);

/* The first two rows are indexed by fplinux_ui_view_setting. */
struct fplinux_ui_screen *fplinux_ui_view_settings_list_new(
	struct fplinux_ui *ui, const char *title,
	const struct fplinux_ui_view_settings *settings);
struct fplinux_ui_screen *fplinux_ui_view_settings_choice_new(
	struct fplinux_ui *ui, enum fplinux_ui_view_setting setting,
	const struct fplinux_ui_view_settings *settings);

/* identity_key is the runtime's lowercase SHA-256 program identity. */
struct fplinux_ui_prefs *
fplinux_ui_program_prefs_open(const char *app_id, const char *identity_key);

#endif
