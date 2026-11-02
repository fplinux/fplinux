/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef FPLINUX_UI_VIEW_SETTINGS_H
#define FPLINUX_UI_VIEW_SETTINGS_H

#include <stdbool.h>
#include <stddef.h>

#include "fplinux-rgb565.h"

struct fplinux_ui_prefs;
struct fplinux_ui_screen;

struct fplinux_ui_view_settings {
	enum fplinux_rgb565_scale scale;
	enum fplinux_rgb565_rotation rotation;
};

struct fplinux_ui_view_fields {
	size_t scale;
	size_t rotation;
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
bool fplinux_ui_view_settings_reset(struct fplinux_ui_prefs *prefs);

/* On append failure, any field added by this call is removed. */
bool fplinux_ui_view_settings_form_append(
	struct fplinux_ui_screen *form,
	const struct fplinux_ui_view_settings *settings,
	struct fplinux_ui_view_fields *fields);
bool fplinux_ui_view_settings_form_read(
	const struct fplinux_ui_screen *form,
	const struct fplinux_ui_view_fields *fields,
	struct fplinux_ui_view_settings *settings);

/* identity_key is the runtime's lowercase SHA-256 program identity. */
struct fplinux_ui_prefs *
fplinux_ui_program_prefs_open(const char *app_id, const char *identity_key);

#endif
