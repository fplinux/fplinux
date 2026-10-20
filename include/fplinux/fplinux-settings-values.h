/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_SETTINGS_VALUES_H
#define FPLINUX_SETTINGS_VALUES_H

/* Level 0 switches the backlight off and is not a user level. */
#define FPLINUX_SETTINGS_BRIGHTNESS_MIN 1U
#define FPLINUX_SETTINGS_BRIGHTNESS_MAX 10U
#define FPLINUX_SETTINGS_YEAR_MIN 1980
#define FPLINUX_SETTINGS_YEAR_MAX 2099

#define FPLINUX_SETTINGS_SCREEN_OFF_COUNT 5U
#define FPLINUX_SETTINGS_AUTO_LOCK_COUNT 6U

/* Canonical SETTING values, in the order offered by Settings. */
extern const char *const
	fplinux_settings_screen_off_values[FPLINUX_SETTINGS_SCREEN_OFF_COUNT];
/* The first auto-lock choice is "off"; the remaining choices are seconds. */
extern const char *const
	fplinux_settings_auto_lock_values[FPLINUX_SETTINGS_AUTO_LOCK_COUNT];

#endif
