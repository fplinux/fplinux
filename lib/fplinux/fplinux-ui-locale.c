/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define _POSIX_C_SOURCE 200809L
#include "fplinux-ui-locale.h"
#include "fplinux-language.h"
#include "fplinux-text-input.h"
#include "fplinux-utf8.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wctype.h>

/* The longest formatted date before it is copied to the caller. */
#define DATE_TEXT_BYTES 256U

/* The runtime owns the manager's locale; formatting needs no LVGL types. */
const char *fplinux_ui_locale(const struct fplinux_ui *ui);

/*
 * The formats of the system language, read again when the manager changes
 * the language. The runtime runs one user interface per process. Without a
 * usable file the built-in English applies, as for messages.
 */
static const struct fplinux_language *
system_language(const struct fplinux_ui *ui)
{
	static struct fplinux_language language;
	static char loaded[FPLINUX_LANGUAGE_CODE_BYTES];
	const char *code = fplinux_ui_locale(ui);

	if (loaded[0] && !strcmp(loaded, code))
		return &language;
	if (!fplinux_language_load(FPLINUX_LANGUAGE_DIRECTORY, code,
				   &language) ||
	    !language.system)
		fplinux_language_english(&language);
	snprintf(loaded, sizeof(loaded), "%s", code);
	return &language;
}

/* Appends text; false once the date no longer fits. */
static bool append(char *out, size_t size, size_t *length, const char *text)
{
	size_t added = strlen(text);

	if (added >= size - *length)
		return false;
	memcpy(out + *length, text, added + 1);
	*length += added;
	return true;
}

enum date_field {
	FIELD_WEEKDAY,
	FIELD_DAY,
	FIELD_DAY_PADDED,
	FIELD_MONTH,
	FIELD_MONTH_GENITIVE,
	FIELD_MONTH_PADDED,
	FIELD_YEAR,
};

/* One placeholder of a date pattern; false for an unknown one. */
static bool expand(const struct fplinux_language *language,
		   const struct tm *time, const char *name, size_t name_length,
		   char *value, size_t size)
{
	static const struct {
		const char *name;
		enum date_field field;
	} fields[] = {
		{ "weekday", FIELD_WEEKDAY },
		{ "day", FIELD_DAY },
		{ "dd", FIELD_DAY_PADDED },
		{ "month", FIELD_MONTH },
		{ "month.genitive", FIELD_MONTH_GENITIVE },
		{ "mm", FIELD_MONTH_PADDED },
		{ "yyyy", FIELD_YEAR },
	};
	size_t i;

	for (i = 0; i < sizeof(fields) / sizeof(*fields); ++i)
		if (strlen(fields[i].name) == name_length &&
		    !strncmp(name, fields[i].name, name_length))
			break;
	if (i == sizeof(fields) / sizeof(*fields))
		return false;
	switch (fields[i].field) {
	case FIELD_WEEKDAY:
		snprintf(value, size, "%s", language->weekday[time->tm_wday]);
		break;
	case FIELD_DAY:
		snprintf(value, size, "%d", time->tm_mday);
		break;
	case FIELD_DAY_PADDED:
		snprintf(value, size, "%02d", time->tm_mday);
		break;
	case FIELD_MONTH:
		snprintf(value, size, "%s", language->month[time->tm_mon]);
		break;
	case FIELD_MONTH_GENITIVE:
		snprintf(value, size, "%s",
			 language->month_genitive[time->tm_mon]);
		break;
	case FIELD_MONTH_PADDED:
		snprintf(value, size, "%02d", time->tm_mon + 1);
		break;
	case FIELD_YEAR:
		snprintf(value, size, "%04d", time->tm_year + 1900);
		break;
	}
	return true;
}

static bool format_pattern(const struct fplinux_language *language,
			   const char *pattern, const struct tm *time,
			   char *out, size_t size)
{
	size_t length = 0;

	out[0] = '\0';
	while (*pattern) {
		char value[FPLINUX_LANGUAGE_TEXT_BYTES];
		const char *close;
		size_t literal = strcspn(pattern, "{");

		if (literal) {
			if (literal >= size - length)
				return false;
			memcpy(out + length, pattern, literal);
			length += literal;
			out[length] = '\0';
			pattern += literal;
			continue;
		}
		close = strchr(pattern, '}');
		if (!close ||
		    !expand(language, time, pattern + 1,
			    (size_t)(close - pattern - 1), value,
			    sizeof(value)) ||
		    !append(out, size, &length, value))
			return false;
		pattern = close + 1;
	}
	return true;
}

/*
 * Names are stored as they appear inside a sentence ("вторник"); a date
 * that begins with one starts with a capital ("Вторник, 30 августа").
 * musl's towupper() maps every script regardless of the locale.
 */
static bool copy_capitalized(const char *text, char *out, size_t size)
{
	const unsigned char *cursor = (const unsigned char *)text;
	const unsigned char *end = cursor + strlen(text);
	char first[FPLINUX_TEXT_INPUT_UTF8_BYTES];
	uint32_t code;
	size_t first_bytes;
	size_t length;
	size_t rest;

	if (!text[0]) {
		out[0] = '\0';
		return true;
	}
	if (!fplinux_utf8_next(&cursor, end, &code))
		return false;
	first_bytes = (size_t)(cursor - (const unsigned char *)text);
	rest = strlen(text + first_bytes);
	length = fplinux_text_input_encode_utf8(
		(uint32_t)towupper((wint_t)code), first);
	if (length + rest >= size)
		return false;
	memcpy(out, first, length);
	memcpy(out + length, text + first_bytes, rest + 1);
	return true;
}

bool fplinux_ui_format_date(const struct fplinux_ui *ui, time_t when,
			    enum fplinux_ui_date_style style, char *out,
			    size_t size)
{
	const struct fplinux_language *language = system_language(ui);
	char text[DATE_TEXT_BYTES];
	const char *pattern;
	struct tm time;

	if (!out || !size) {
		errno = ERANGE;
		return false;
	}
	if (!localtime_r(&when, &time))
		return false;
	switch (style) {
	case FPLINUX_UI_DATE_LONG:
		pattern = language->date_long;
		break;
	case FPLINUX_UI_DATE_SHORT:
		pattern = language->date_short;
		break;
	case FPLINUX_UI_DATE_CLOCK:
		pattern = NULL;
		break;
	default:
		errno = EINVAL;
		return false;
	}
	if (!pattern) {
		if (snprintf(out, size, "%02d:%02d", time.tm_hour,
			     time.tm_min) < (int)size)
			return true;
		errno = ERANGE;
		return false;
	}
	/* The reader accepted the pattern, so only the space can run out. */
	if (!format_pattern(language, pattern, &time, text, sizeof(text)) ||
	    !copy_capitalized(text, out, size)) {
		out[0] = '\0';
		errno = ERANGE;
		return false;
	}
	return true;
}

/* musl prints numbers in the C locale, always with '.'. */
bool fplinux_ui_format_number(const struct fplinux_ui *ui, double value,
			      int decimals, char *out, size_t size)
{
	char *point;
	int written;

	if (decimals < 0 || decimals > 9) {
		errno = EINVAL;
		return false;
	}
	if (!out || !size) {
		errno = ERANGE;
		return false;
	}
	written = snprintf(out, size, "%.*f", decimals, value);
	if (written < 0 || (size_t)written >= size) {
		out[0] = '\0';
		errno = ERANGE;
		return false;
	}
	point = strchr(out, '.');
	if (point)
		*point = fplinux_ui_decimal_separator(ui);
	return true;
}

char fplinux_ui_decimal_separator(const struct fplinux_ui *ui)
{
	return system_language(ui)->decimal;
}
