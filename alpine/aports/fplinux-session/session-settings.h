/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_SESSION_SETTINGS_H
#define FPLINUX_SESSION_SETTINGS_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#include "fplinux-language.h"
#include "fplinux-storage.h"
#include "fplinux-ui-session.h"

enum fplinux_session_setting {
	FPLINUX_SESSION_SETTING_BRIGHTNESS,
	FPLINUX_SESSION_SETTING_VOLUME,
	FPLINUX_SESSION_SETTING_ALARM_VOLUME,
	FPLINUX_SESSION_SETTING_AUDIO_OUTPUT,
	FPLINUX_SESSION_SETTING_SCREEN_OFF,
	FPLINUX_SESSION_SETTING_AUTO_LOCK,
	FPLINUX_SESSION_SETTING_LOCALE,
	FPLINUX_SESSION_SETTING_INPUT_LOCALE,
	FPLINUX_SESSION_SETTING_TIME,
	FPLINUX_SESSION_SETTING_THEME,
	FPLINUX_SESSION_SETTING_MIDI_SOUNDFONT,
	FPLINUX_SESSION_SETTING_UNKNOWN,
};

#define FPLINUX_SESSION_SOUNDFONT_DEFAULT "default"

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
	bool has_volume;
	unsigned int volume;
	bool has_alarm_volume;
	unsigned int alarm_volume;
	char audio_output[16];
	unsigned int screen_off_seconds;
	int auto_lock_seconds;
	char locale[FPLINUX_UI_SESSION_LOCALE_BYTES];
	char input_locale[FPLINUX_UI_SESSION_LOCALE_BYTES];
	char lang[FPLINUX_LANGUAGE_TEXT_BYTES];
	enum fplinux_ui_session_theme theme;
	char midi_soundfont[FPLINUX_UI_SESSION_TEXT_BYTES];
};

/*
 * languages is the directory of language files; NULL means none is installed.
 * A locale is a code of two or three letters a-z whose file there says
 * system=yes. The default locale is en, else ru, else the built-in English.
 * Input uses the selected locale if its file says input=yes, else English.
 */
void fplinux_session_settings_defaults(
	struct fplinux_session_settings *settings, const char *languages);
enum fplinux_session_setting fplinux_session_setting_find(const char *name);
/*
 * Stores one persistent value after exact validation. The time setting is
 * not persistent and is rejected here.
 */
bool fplinux_session_settings_parse(struct fplinux_session_settings *settings,
				    const char *languages,
				    enum fplinux_session_setting setting,
				    const char *value);
bool fplinux_session_settings_apply(struct fplinux_session_settings *settings,
				    const char *languages,
				    const struct fplinux_ui_setting *request);
union fplinux_ui_setting_value
fplinux_session_settings_value(const struct fplinux_session_settings *settings,
			       enum fplinux_ui_setting_kind kind);
/* Formats a persistent value as the file spells it. */
bool fplinux_session_settings_format(
	const struct fplinux_session_settings *settings,
	enum fplinux_session_setting setting, char *value, size_t capacity);
/* Finds a readable installed SF2 candidate for a validated soundfont ID. */
bool fplinux_session_soundfont_path(const char *directory, const char *id,
				    char *path, size_t capacity);
/*
 * Loads key=value lines over the defaults. A missing file leaves the defaults;
 * an unknown key or invalid value is ignored. Returns false only when an
 * existing file cannot be read.
 */
bool fplinux_session_settings_load(struct fplinux_session_settings *settings,
				   const char *languages, const char *path);
/* Replaces path atomically; reports publication separately from durability. */
struct fplinux_storage_result
fplinux_session_settings_save(const struct fplinux_session_settings *settings,
			      const char *path);
/*
 * Maps a local calendar reading to an instant within the RTC range.
 */
bool fplinux_session_time_setpoint(const struct fplinux_ui_setting_time *value,
				   time_t *seconds);

#endif
