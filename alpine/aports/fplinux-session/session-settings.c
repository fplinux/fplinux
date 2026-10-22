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
	[FPLINUX_SESSION_SETTING_SCREEN_OFF] = "screen_off_seconds",
	[FPLINUX_SESSION_SETTING_AUTO_LOCK] = "auto_lock_seconds",
	[FPLINUX_SESSION_SETTING_LOCALE] = "locale",
	[FPLINUX_SESSION_SETTING_INPUT_LOCALE] = "input_locale",
	[FPLINUX_SESSION_SETTING_TIME] = "time",
	[FPLINUX_SESSION_SETTING_THEME] = "theme",
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
	settings->screen_off_seconds = 30;
	settings->auto_lock_seconds = 5;
	settings->theme = FPLINUX_UI_SESSION_THEME_LIGHT;
	fplinux_language_english(&english);
	if (!select_language(settings, languages, "ru") &&
	    !select_language(settings, languages, "en")) {
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
	case FPLINUX_SESSION_SETTING_SCREEN_OFF:
		if (!parse_decimal(value, &number))
			return false;
		for (i = 0; i < FPLINUX_SETTINGS_SCREEN_OFF_COUNT; ++i)
			if (!strcmp(value,
				    fplinux_settings_screen_off_values[i])) {
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
			if (!strcmp(value,
				    fplinux_settings_auto_lock_values[i])) {
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
	default:
		return false;
	}
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
	char text[256];
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
	if (settings->has_brightness && length >= 0 &&
	    (size_t)length < sizeof(text))
		length += snprintf(text + length, sizeof(text) - length,
				   "brightness=%u\n", settings->brightness);
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

static bool has_digits(const char *value, size_t first, size_t count)
{
	size_t i;

	for (i = first; i < first + count; ++i)
		if (value[i] < '0' || value[i] > '9')
			return false;
	return true;
}

bool fplinux_session_time_parse(const char *value, time_t *seconds)
{
	struct tm time = { .tm_isdst = -1 };
	size_t length = strlen(value);
	int year, month, day, hour, minute;
	int second = 0;
	time_t result;

	/* Seconds are optional; a date-only edit keeps the running seconds. */
	if ((length != 16 && length != 19) || !has_digits(value, 0, 4) ||
	    value[4] != '-' || !has_digits(value, 5, 2) || value[7] != '-' ||
	    !has_digits(value, 8, 2) || value[10] != ' ' ||
	    !has_digits(value, 11, 2) || value[13] != ':' ||
	    !has_digits(value, 14, 2) ||
	    (length == 19 && (value[16] != ':' || !has_digits(value, 17, 2))) ||
	    sscanf(value, "%4d-%2d-%2d %2d:%2d", &year, &month, &day, &hour,
		   &minute) != 5 ||
	    (length == 19 && sscanf(value + 17, "%2d", &second) != 1))
		return false;
	if (year < FPLINUX_SETTINGS_YEAR_MIN ||
	    year > FPLINUX_SETTINGS_YEAR_MAX || month < 1 || month > 12 ||
	    day < 1 || day > 31 || hour > 23 || minute > 59 || second > 59)
		return false;
	time.tm_year = year - 1900;
	time.tm_mon = month - 1;
	time.tm_mday = day;
	time.tm_hour = hour;
	time.tm_min = minute;
	time.tm_sec = second;
	result = mktime(&time);
	/* mktime normalizes 31 April to 1 May; such a date is invalid. */
	if (result == (time_t)-1 || time.tm_year != year - 1900 ||
	    time.tm_mon != month - 1 || time.tm_mday != day ||
	    time.tm_hour != hour || time.tm_min != minute ||
	    time.tm_sec != second)
		return false;
	*seconds = result;
	return true;
}

bool fplinux_session_time_format(time_t seconds, char *value, size_t capacity)
{
	struct tm time;

	return localtime_r(&seconds, &time) &&
	       strftime(value, capacity, "%Y-%m-%d %H:%M", &time) == 16;
}
