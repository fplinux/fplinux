/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "session-settings.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "session-power.h"
#include "fplinux-settings-values.h"

#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))

static const char *const setting_names[] = {
	[FPLINUX_SESSION_SETTING_BRIGHTNESS] = "brightness",
	[FPLINUX_SESSION_SETTING_VOLUME] = "volume",
	[FPLINUX_SESSION_SETTING_ALARM_VOLUME] = "alarm_volume",
	[FPLINUX_SESSION_SETTING_AUDIO_OUTPUT] = "audio_output",
	[FPLINUX_SESSION_SETTING_SCREEN_OFF] = "screen_off_seconds",
	[FPLINUX_SESSION_SETTING_AUTO_LOCK] = "auto_lock_seconds",
	[FPLINUX_SESSION_SETTING_LOCALE] = "locale",
	[FPLINUX_SESSION_SETTING_INPUT_LOCALE] = "input_locale",
	[FPLINUX_SESSION_SETTING_TIME] = "time",
	[FPLINUX_SESSION_SETTING_THEME] = "theme",
	[FPLINUX_SESSION_SETTING_MIDI_SOUNDFONT] = "midi_soundfont",
};

static const char *const theme_names[] = {
	[FPLINUX_UI_SESSION_THEME_LIGHT] = "light",
	[FPLINUX_UI_SESSION_THEME_DARK] = "dark",
};

_Static_assert(FPLINUX_UI_SESSION_LOCALE_BYTES >= FPLINUX_LANGUAGE_CODE_BYTES,
	       "every language code fits the protocol locale");

/*
 * Selects a system language from its file; leaves settings alone otherwise.
 * The loader accepts only [a-z]{2,3} codes, so a code never leaves languages.
 */
static bool select_language(struct fplinux_session_settings *settings,
			    const char *languages, const char *code)
{
	struct fplinux_language language;

	if (!fplinux_language_load(languages, code, &language) ||
	    !language.system)
		return false;
	strcpy(settings->locale, language.code);
	memcpy(settings->lang, language.lang, sizeof(settings->lang));
	return true;
}

static bool select_input_language(struct fplinux_session_settings *settings,
				  const char *languages, const char *code)
{
	struct fplinux_language language;

	if (!fplinux_language_load(languages, code, &language)) {
		if (strcmp(code, "en") || (languages && errno != ENOENT))
			return false;
		fplinux_language_english(&language);
	}
	if (!language.input)
		return false;
	strcpy(settings->input_locale, language.code);
	return true;
}

void fplinux_session_settings_defaults(
	struct fplinux_session_settings *settings, const char *languages)
{
	struct fplinux_language english;

	memset(settings, 0, sizeof(*settings));
	settings->brightness = 7;
	settings->volume = 10;
	settings->alarm_volume = 10;
	strcpy(settings->audio_output, "AUTO");
	settings->screen_off_seconds = 30;
	settings->auto_lock_seconds = 5;
	settings->theme = FPLINUX_UI_SESSION_THEME_LIGHT;
	strcpy(settings->midi_soundfont, FPLINUX_SESSION_SOUNDFONT_DEFAULT);
	fplinux_language_english(&english);
	if (!select_language(settings, languages, "en") &&
	    !select_language(settings, languages, "ru")) {
		strcpy(settings->locale, english.code);
		memcpy(settings->lang, english.lang, sizeof(settings->lang));
	}
	if (!select_input_language(settings, languages, settings->locale))
		strcpy(settings->input_locale, english.code);
}

enum fplinux_session_setting fplinux_session_setting_find(const char *name)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(setting_names); ++i)
		if (!strcmp(name, setting_names[i]))
			return i;
	return FPLINUX_SESSION_SETTING_UNKNOWN;
}

/* Canonical decimal only: no sign, spaces or leading zeros. */
static bool parse_decimal(const char *value, unsigned int *number)
{
	size_t length = strlen(value);
	size_t i;

	if (!length || length > 3 || (length > 1 && value[0] == '0'))
		return false;
	for (i = 0; i < length; ++i)
		if (value[i] < '0' || value[i] > '9')
			return false;
	*number = (unsigned int)strtoul(value, NULL, 10);
	return true;
}

static bool soundfont_id_valid(const char *id)
{
	size_t length = strnlen(id, FPLINUX_UI_SESSION_TEXT_BYTES);
	bool word_start = true;

	if (!length || length >= FPLINUX_UI_SESSION_TEXT_BYTES)
		return false;
	for (size_t i = 0; i < length; ++i) {
		char byte = id[i];

		if ((byte >= 'a' && byte <= 'z') ||
		    (byte >= '0' && byte <= '9'))
			word_start = false;
		else if (byte == '-' && !word_start)
			word_start = true;
		else
			return false;
	}
	return !word_start;
}

static bool select_soundfont(struct fplinux_session_settings *settings,
			     const char *id)
{
	if (!soundfont_id_valid(id))
		return false;
	strcpy(settings->midi_soundfont, id);
	return true;
}

bool fplinux_session_soundfont_path(const char *directory, const char *id,
				    char *path, size_t capacity)
{
	char header[12];
	struct stat status;
	ssize_t bytes;
	int length;
	int fd;

	if (!directory || directory[0] != '/')
		return false;
	length = snprintf(path, capacity, "%s/%s.sf2", directory, id);
	if (length < 0 || (size_t)length >= capacity)
		return false;
	fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
	if (fd < 0)
		return false;
	if (fstat(fd, &status) < 0 || !S_ISREG(status.st_mode) ||
	    status.st_size < (off_t)sizeof(header) ||
	    status.st_size > INT_MAX) {
		close(fd);
		return false;
	}
	do {
		bytes = read(fd, header, sizeof(header));
	} while (bytes < 0 && errno == EINTR);
	close(fd);
	return bytes == (ssize_t)sizeof(header) && !memcmp(header, "RIFF", 4) &&
	       !memcmp(header + 8, "sfbk", 4);
}

bool fplinux_session_settings_parse(struct fplinux_session_settings *settings,
				    const char *languages,
				    enum fplinux_session_setting setting,
				    const char *value)
{
	unsigned int number;
	unsigned int i;

	switch (setting) {
	case FPLINUX_SESSION_SETTING_BRIGHTNESS:
		if (!parse_decimal(value, &number) ||
		    number < FPLINUX_SETTINGS_BRIGHTNESS_MIN ||
		    number > FPLINUX_SETTINGS_BRIGHTNESS_MAX)
			return false;
		settings->brightness = number;
		settings->has_brightness = true;
		return true;
	case FPLINUX_SESSION_SETTING_VOLUME:
	case FPLINUX_SESSION_SETTING_ALARM_VOLUME:
		if (!parse_decimal(value, &number) || number > 10)
			return false;
		if (setting == FPLINUX_SESSION_SETTING_VOLUME) {
			settings->volume = number;
			settings->has_volume = true;
		} else {
			settings->alarm_volume = number;
			settings->has_alarm_volume = true;
		}
		return true;
	case FPLINUX_SESSION_SETTING_AUDIO_OUTPUT:
		if (strcmp(value, "AUTO") && strcmp(value, "SPEAKER") &&
		    strcmp(value, "HEADPHONES"))
			return false;
		strcpy(settings->audio_output, value);
		return true;
	case FPLINUX_SESSION_SETTING_SCREEN_OFF:
		if (!parse_decimal(value, &number))
			return false;
		for (i = 0; i < FPLINUX_SETTINGS_SCREEN_OFF_COUNT; ++i)
			if ((int)number ==
			    fplinux_settings_screen_off_values[i]) {
				settings->screen_off_seconds = number;
				return true;
			}
		return false;
	case FPLINUX_SESSION_SETTING_AUTO_LOCK:
		if (!strcmp(value, "off")) {
			settings->auto_lock_seconds =
				FPLINUX_SESSION_AUTO_LOCK_OFF;
			return true;
		}
		if (!parse_decimal(value, &number))
			return false;
		for (i = 1; i < FPLINUX_SETTINGS_AUTO_LOCK_COUNT; ++i)
			if ((int)number ==
			    fplinux_settings_auto_lock_values[i]) {
				settings->auto_lock_seconds = (int)number;
				return true;
			}
		return false;
	case FPLINUX_SESSION_SETTING_LOCALE:
		return select_language(settings, languages, value);
	case FPLINUX_SESSION_SETTING_INPUT_LOCALE:
		return select_input_language(settings, languages, value);
	case FPLINUX_SESSION_SETTING_THEME:
		for (i = 0; i < ARRAY_SIZE(theme_names); ++i)
			if (!strcmp(value, theme_names[i])) {
				settings->theme = i;
				return true;
			}
		return false;
	case FPLINUX_SESSION_SETTING_MIDI_SOUNDFONT:
		return select_soundfont(settings, value);
	default:
		return false;
	}
}

bool fplinux_session_settings_apply(struct fplinux_session_settings *settings,
				    const char *languages,
				    const struct fplinux_ui_setting *request)
{
	const union fplinux_ui_setting_value *value = &request->value;

	switch (request->kind) {
	case FPLINUX_UI_SETTING_SCREEN_OFF:
		for (size_t i = 0; i < FPLINUX_SETTINGS_SCREEN_OFF_COUNT; ++i)
			if (value->seconds ==
			    fplinux_settings_screen_off_values[i]) {
				settings->screen_off_seconds = value->seconds;
				return true;
			}
		return false;
	case FPLINUX_UI_SETTING_AUTO_LOCK:
		for (size_t i = 0; i < FPLINUX_SETTINGS_AUTO_LOCK_COUNT; ++i)
			if (value->seconds ==
			    fplinux_settings_auto_lock_values[i]) {
				settings->auto_lock_seconds = value->seconds;
				return true;
			}
		return false;
	case FPLINUX_UI_SETTING_LOCALE:
		return select_language(settings, languages, value->locale);
	case FPLINUX_UI_SETTING_INPUT_LOCALE:
		return select_input_language(settings, languages,
					     value->locale);
	case FPLINUX_UI_SETTING_THEME:
		settings->theme = value->theme;
		return true;
	case FPLINUX_UI_SETTING_MIDI_SOUNDFONT:
		return select_soundfont(settings, value->soundfont);
	default:
		return false;
	}
}

union fplinux_ui_setting_value
fplinux_session_settings_value(const struct fplinux_session_settings *settings,
			       enum fplinux_ui_setting_kind kind)
{
	union fplinux_ui_setting_value value = { 0 };

	switch (kind) {
	case FPLINUX_UI_SETTING_SCREEN_OFF:
		value.seconds = settings->screen_off_seconds;
		break;
	case FPLINUX_UI_SETTING_AUTO_LOCK:
		value.seconds = settings->auto_lock_seconds;
		break;
	case FPLINUX_UI_SETTING_LOCALE:
		strcpy(value.locale, settings->locale);
		break;
	case FPLINUX_UI_SETTING_INPUT_LOCALE:
		strcpy(value.locale, settings->input_locale);
		break;
	case FPLINUX_UI_SETTING_THEME:
		value.theme = settings->theme;
		break;
	case FPLINUX_UI_SETTING_MIDI_SOUNDFONT:
		strcpy(value.soundfont, settings->midi_soundfont);
		break;
	default:
		break;
	}
	return value;
}

bool fplinux_session_settings_format(
	const struct fplinux_session_settings *settings,
	enum fplinux_session_setting setting, char *value, size_t capacity)
{
	int length;

	switch (setting) {
	case FPLINUX_SESSION_SETTING_BRIGHTNESS:
		length = snprintf(value, capacity, "%u", settings->brightness);
		break;
	case FPLINUX_SESSION_SETTING_VOLUME:
		length = snprintf(value, capacity, "%u", settings->volume);
		break;
	case FPLINUX_SESSION_SETTING_ALARM_VOLUME:
		length =
			snprintf(value, capacity, "%u", settings->alarm_volume);
		break;
	case FPLINUX_SESSION_SETTING_AUDIO_OUTPUT:
		length =
			snprintf(value, capacity, "%s", settings->audio_output);
		break;
	case FPLINUX_SESSION_SETTING_SCREEN_OFF:
		length = snprintf(value, capacity, "%u",
				  settings->screen_off_seconds);
		break;
	case FPLINUX_SESSION_SETTING_AUTO_LOCK:
		if (settings->auto_lock_seconds ==
		    FPLINUX_SESSION_AUTO_LOCK_OFF)
			length = snprintf(value, capacity, "off");
		else
			length = snprintf(value, capacity, "%d",
					  settings->auto_lock_seconds);
		break;
	case FPLINUX_SESSION_SETTING_LOCALE:
		length = snprintf(value, capacity, "%s", settings->locale);
		break;
	case FPLINUX_SESSION_SETTING_INPUT_LOCALE:
		length =
			snprintf(value, capacity, "%s", settings->input_locale);
		break;
	case FPLINUX_SESSION_SETTING_THEME:
		length = snprintf(value, capacity, "%s",
				  theme_names[settings->theme]);
		break;
	case FPLINUX_SESSION_SETTING_MIDI_SOUNDFONT:
		length = snprintf(value, capacity, "%s",
				  settings->midi_soundfont);
		break;
	default:
		return false;
	}
	return length >= 0 && (size_t)length < capacity;
}

bool fplinux_session_settings_load(struct fplinux_session_settings *settings,
				   const char *languages, const char *path)
{
	char line[FPLINUX_UI_SESSION_TEXT_BYTES + FPLINUX_UI_SESSION_NAME_BYTES];
	bool truncated = false;
	FILE *file;

	fplinux_session_settings_defaults(settings, languages);
	file = fopen(path, "re");
	if (!file)
		return errno == ENOENT;
	while (fgets(line, sizeof(line), file)) {
		size_t length = strlen(line);
		bool complete = length && line[length - 1] == '\n';
		bool skip = truncated;
		enum fplinux_session_setting setting;
		char *value;

		/* The tail of an overlong line is not a line of its own. */
		truncated = !complete && !feof(file);
		if (skip || truncated)
			continue;
		line[strcspn(line, "\n")] = '\0';
		value = strchr(line, '=');
		if (!value)
			continue;
		*value++ = '\0';
		setting = fplinux_session_setting_find(line);
		if (setting != FPLINUX_SESSION_SETTING_TIME)
			fplinux_session_settings_parse(settings, languages,
						       setting, value);
	}
	if (ferror(file)) {
		fclose(file);
		fplinux_session_settings_defaults(settings, languages);
		errno = EIO;
		return false;
	}
	fclose(file);
	return true;
}

static bool write_all(int fd, const char *text, size_t length)
{
	while (length) {
		ssize_t written = write(fd, text, length);

		if (written < 0 && errno == EINTR)
			continue;
		if (written == 0)
			errno = EIO;
		if (written <= 0)
			return false;
		text += written;
		length -= (size_t)written;
	}
	return true;
}

static bool sync_parent(const char *path)
{
	char directory[PATH_MAX];
	const char *slash = strrchr(path, '/');
	int fd;
	int error;

	if (!slash) {
		strcpy(directory, ".");
	} else if (slash == path) {
		strcpy(directory, "/");
	} else {
		if ((size_t)(slash - path) >= sizeof(directory)) {
			errno = ENAMETOOLONG;
			return false;
		}
		memcpy(directory, path, (size_t)(slash - path));
		directory[slash - path] = '\0';
	}
	fd = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (fd < 0)
		return false;
	if (fsync(fd) < 0) {
		error = errno;
		close(fd);
		errno = error;
		return false;
	}
	close(fd);
	return true;
}

struct fplinux_storage_result
fplinux_session_settings_save(const struct fplinux_session_settings *settings,
			      const char *path)
{
	struct fplinux_storage_result result = {
		.state = FPLINUX_STORAGE_UNPUBLISHED,
	};
	char temporary[PATH_MAX];
	char text[256 + sizeof(settings->midi_soundfont)];
	char screen_off[16];
	char auto_lock[16];
	char theme[16];
	int length;
	int error;
	int fd;

	if (!fplinux_session_settings_format(settings,
					     FPLINUX_SESSION_SETTING_SCREEN_OFF,
					     screen_off, sizeof(screen_off)) ||
	    !fplinux_session_settings_format(settings,
					     FPLINUX_SESSION_SETTING_AUTO_LOCK,
					     auto_lock, sizeof(auto_lock)) ||
	    !fplinux_session_settings_format(settings,
					     FPLINUX_SESSION_SETTING_THEME,
					     theme, sizeof(theme))) {
		errno = EINVAL;
		goto failed;
	}
	length = snprintf(
		text, sizeof(text),
		"screen_off_seconds=%s\nauto_lock_seconds=%s\nlocale=%s\n"
		"input_locale=%s\ntheme=%s\n",
		screen_off, auto_lock, settings->locale, settings->input_locale,
		theme);
	if (strcmp(settings->audio_output, "AUTO") && length >= 0 &&
	    (size_t)length < sizeof(text))
		length += snprintf(text + length, sizeof(text) - length,
				   "audio_output=%s\n", settings->audio_output);
	if (settings->has_brightness && length >= 0 &&
	    (size_t)length < sizeof(text))
		length += snprintf(text + length, sizeof(text) - length,
				   "brightness=%u\n", settings->brightness);
	if (settings->has_volume && length >= 0 &&
	    (size_t)length < sizeof(text))
		length += snprintf(text + length, sizeof(text) - length,
				   "volume=%u\n", settings->volume);
	if (settings->has_alarm_volume && length >= 0 &&
	    (size_t)length < sizeof(text))
		length += snprintf(text + length, sizeof(text) - length,
				   "alarm_volume=%u\n", settings->alarm_volume);
	if (strcmp(settings->midi_soundfont,
		   FPLINUX_SESSION_SOUNDFONT_DEFAULT) &&
	    length >= 0 && (size_t)length < sizeof(text))
		length += snprintf(text + length, sizeof(text) - length,
				   "midi_soundfont=%s\n",
				   settings->midi_soundfont);
	if (length < 0 || (size_t)length >= sizeof(text) ||
	    snprintf(temporary, sizeof(temporary), "%s.XXXXXX", path) >=
		    (int)sizeof(temporary)) {
		errno = ENAMETOOLONG;
		goto failed;
	}
	fd = mkostemp(temporary, O_CLOEXEC);
	if (fd < 0)
		goto failed;
	/* mkostemp creates 0600 and the session umask is 0007. */
	if (!write_all(fd, text, (size_t)length) || fchmod(fd, 0644) < 0 ||
	    fsync(fd) < 0) {
		error = errno;
		close(fd);
		unlink(temporary);
		errno = error;
		goto failed;
	}
	if (close(fd) < 0 || rename(temporary, path) < 0) {
		error = errno;
		unlink(temporary);
		errno = error;
		goto failed;
	}
	result.state = FPLINUX_STORAGE_PUBLISHED_UNSYNCED;
	if (!sync_parent(path))
		goto failed;
	result.state = FPLINUX_STORAGE_DURABLE;
	return result;
failed:
	result.error = errno ? errno : EIO;
	errno = result.error;
	return result;
}

bool fplinux_session_time_setpoint(const struct fplinux_ui_setting_time *value,
				   time_t *seconds)
{
	struct tm time = { .tm_isdst = -1 };
	time_t result;

	if (value->year < FPLINUX_SETTINGS_YEAR_MIN ||
	    value->year > FPLINUX_SETTINGS_YEAR_MAX || value->month < 1 ||
	    value->month > 12 || value->day < 1 || value->day > 31 ||
	    value->hour < 0 || value->hour > 23 || value->minute < 0 ||
	    value->minute > 59 || value->second < 0 || value->second > 59)
		return false;
	time.tm_year = value->year - 1900;
	time.tm_mon = value->month - 1;
	time.tm_mday = value->day;
	time.tm_hour = value->hour;
	time.tm_min = value->minute;
	time.tm_sec = value->second;
	result = mktime(&time);
	/* mktime normalizes 31 April to 1 May; such a date is invalid. */
	if (result == (time_t)-1 || time.tm_year != value->year - 1900 ||
	    time.tm_mon != value->month - 1 || time.tm_mday != value->day ||
	    time.tm_hour != value->hour || time.tm_min != value->minute ||
	    time.tm_sec != value->second)
		return false;
	*seconds = result;
	return true;
}
