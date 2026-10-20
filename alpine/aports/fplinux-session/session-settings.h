/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_SESSION_SETTINGS_H
#define FPLINUX_SESSION_SETTINGS_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#include "fplinux-language.h"
#include "fplinux-ui-session.h"

enum fplinux_session_setting {
	FPLINUX_SESSION_SETTING_BRIGHTNESS,
	FPLINUX_SESSION_SETTING_SCREEN_OFF,
	FPLINUX_SESSION_SETTING_AUTO_LOCK,
	FPLINUX_SESSION_SETTING_LOCALE,
	FPLINUX_SESSION_SETTING_INPUT_LOCALE,
	FPLINUX_SESSION_SETTING_TIME,
	FPLINUX_SESSION_SETTING_THEME,
	FPLINUX_SESSION_SETTING_UNKNOWN,
};

/*
 * Persistent session settings. has_brightness is false until the user chose
 * a level; the brightness service keeps its own level until then. locale is
 * the system language code and lang the LANG its language file sets for
 * applications. input_locale independently selects the input language.
 * theme is the system theme, "light" (the default) or "dark".
 */
struct fplinux_session_settings {
	bool has_brightness;
	unsigned int brightness;
	unsigned int screen_off_seconds;
	int auto_lock_seconds;
	char locale[FPLINUX_UI_SESSION_LOCALE_BYTES];
	char input_locale[FPLINUX_UI_SESSION_LOCALE_BYTES];
	char lang[FPLINUX_LANGUAGE_TEXT_BYTES];
	enum fplinux_ui_session_theme theme;
};

/*
 * languages is the directory of language files; NULL means none is installed.
 * A locale is a code of two or three letters a-z whose file there says
 * system=yes. The default locale is ru, else en, else the built-in English.
 * Input uses the selected locale if its file says input=yes, else English.
 */
void fplinux_session_settings_defaults(
	struct fplinux_session_settings *settings, const char *languages);
enum fplinux_session_setting fplinux_session_setting_find(const char *name);
/*
 * Stores one persistent value after exact validation. The time setting is
 * not persistent and is rejected here; use fplinux_session_time_parse().
 */
bool fplinux_session_settings_parse(struct fplinux_session_settings *settings,
				    const char *languages,
				    enum fplinux_session_setting setting,
				    const char *value);
/* Formats a persistent value as the protocol and the file spell it. */
bool fplinux_session_settings_format(
	const struct fplinux_session_settings *settings,
	enum fplinux_session_setting setting, char *value, size_t capacity);
/*
 * Loads key=value lines over the defaults. A missing file leaves the defaults;
 * an unknown key or invalid value is ignored. Returns false only when an
 * existing file cannot be read.
 */
bool fplinux_session_settings_load(struct fplinux_session_settings *settings,
				   const char *languages, const char *path);
/* Replaces path atomically with a synchronized root-readable 0644 file. */
bool fplinux_session_settings_save(
	const struct fplinux_session_settings *settings, const char *path);
/*
 * Parses local "YYYY-MM-DD HH:MM" (seconds 0) or "YYYY-MM-DD HH:MM:SS" within
 * the RTC range 1980...2099.
 */
bool fplinux_session_time_parse(const char *value, time_t *seconds);
bool fplinux_session_time_format(time_t seconds, char *value, size_t capacity);

#endif
