/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define _POSIX_C_SOURCE 200809L
#include "fplinux-language.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

/*
 * A file holds "key=value" lines; a line starting with '#' is a comment and
 * empty lines are skipped. Values are UTF-8 without control characters and
 * use the preferences escapes "\\\\", "\\n" and "\\r"; nothing is trimmed, so
 * "multitap.0= 0" starts with a space. Unknown keys are ignored; a repeated
 * known key makes the file invalid.
 */

/* Texts read before they are validated into flags and the separator. */
struct reader {
	struct fplinux_language *language;
	char system[FPLINUX_LANGUAGE_TEXT_BYTES];
	char input[FPLINUX_LANGUAGE_TEXT_BYTES];
	char decimal[FPLINUX_LANGUAGE_TEXT_BYTES];
	uint64_t seen;
};

/* Each key has a bit in reader.seen. */
enum slot {
	SLOT_NAME,
	SLOT_LANG,
	SLOT_SYSTEM,
	SLOT_INPUT,
	SLOT_DECIMAL,
	SLOT_DATE_LONG,
	SLOT_DATE_SHORT,
	SLOT_XKB,
	SLOT_INDICATOR,
	SLOT_WEEKDAY = SLOT_INDICATOR + 3,
	SLOT_MONTH = SLOT_WEEKDAY + 7,
	SLOT_MONTH_GENITIVE = SLOT_MONTH + 12,
	SLOT_MULTITAP = SLOT_MONTH_GENITIVE + 12,
	SLOT_COUNT = SLOT_MULTITAP + FPLINUX_LANGUAGE_KEYS,
};

_Static_assert(SLOT_COUNT <= 64, "every key has a bit in reader.seen");

/* Where a key's value is stored. */
struct target {
	char *text;
	size_t size;
	unsigned int slot;
};

/* Placeholders of the date patterns, as fplinux_ui_format_date() uses them. */
static const char *const date_fields[] = {
	"weekday", "day", "dd", "month", "month.genitive", "mm", "yyyy",
};

static const struct fplinux_language english = {
	.code = "en",
	.name = "English",
	.lang = "en_US.UTF-8",
	.system = true,
	.input = true,
	.decimal = '.',
	.date_long = "{weekday}, {month} {day}",
	.date_short = "{mm}/{dd}/{yyyy}",
	.weekday = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday",
		     "Friday", "Saturday" },
	.month = { "January", "February", "March", "April", "May", "June",
		   "July", "August", "September", "October", "November",
		   "December" },
	.month_genitive = { "January", "February", "March", "April", "May",
			    "June", "July", "August", "September", "October",
			    "November", "December" },
	.indicator = {
		[FPLINUX_LANGUAGE_SENTENCE] = "Abc",
		[FPLINUX_LANGUAGE_LOWER] = "abc",
		[FPLINUX_LANGUAGE_UPPER] = "ABC",
	},
	.multitap = { " 0", ".,?!1@'\"-()/:_;+&%*=$#^", "abc2", "def3", "ghi4",
		      "jkl5", "mno6", "pqrs7", "tuv8", "wxyz9" },
	.xkb = "us",
};

void fplinux_language_english(struct fplinux_language *language)
{
	*language = english;
}

/* [a-z]{2,3}: a language code is also a file name. */
static bool is_code(const char *code)
{
	size_t length = strspn(code, "abcdefghijklmnopqrstuvwxyz");

	return (length == 2 || length == 3) && !code[length];
}

/* ASCII letters, digits, '.', '_' and '-', as preference keys. */
static bool is_key(const char *key)
{
	size_t length = strspn(key, "abcdefghijklmnopqrstuvwxyz"
				    "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-");

	return length && !key[length];
}

/* Strict UTF-8 without control characters. */
static bool is_text(const char *text)
{
	const unsigned char *bytes = (const unsigned char *)text;

	while (*bytes) {
		unsigned char lead = *bytes;
		uint32_t code;
		uint32_t minimum;
		size_t extra;

		if (lead < 0x80) {
			if (lead < 0x20 || lead == 0x7f)
				return false;
			++bytes;
			continue;
		}
		if (lead >= 0xc2 && lead <= 0xdf) {
			extra = 1;
			minimum = 0x80;
			code = lead & 0x1f;
		} else if (lead >= 0xe0 && lead <= 0xef) {
			extra = 2;
			minimum = 0x800;
			code = lead & 0x0f;
		} else if (lead >= 0xf0 && lead <= 0xf4) {
			extra = 3;
			minimum = 0x10000;
			code = lead & 0x07;
		} else {
			return false;
		}
		for (size_t i = 1; i <= extra; ++i) {
			if ((bytes[i] & 0xc0) != 0x80)
				return false;
			code = code << 6 | (bytes[i] & 0x3f);
		}
		/* C1 controls are control characters too. */
		if (code < minimum || code > 0x10ffff ||
		    (code >= 0xd800 && code <= 0xdfff) ||
		    (code >= 0x80 && code <= 0x9f))
			return false;
		bytes += extra + 1;
	}
	return true;
}

/* Undoes the preferences escapes in place; false for an unknown one. */
static bool unescape(char *text)
{
	char *out = text;

	for (const char *in = text; *in; ++in) {
		char byte = *in;

		if (byte == '\\') {
			++in;
			if (*in == 'n')
				byte = '\n';
			else if (*in == 'r')
				byte = '\r';
			else if (*in == '\\')
				byte = '\\';
			else
				return false;
		}
		*out++ = byte;
	}
	*out = '\0';
	return true;
}

/*
 * The decimal index after prefix, without leading zeros, as an offset from
 * first; false when key has another prefix or the index is out of range.
 */
static bool key_index(const char *key, const char *prefix, unsigned int first,
		      unsigned int count, unsigned int *index)
{
	size_t length = strlen(prefix);
	const char *digits = key + length;
	unsigned int value = 0;

	if (strncmp(key, prefix, length) || !digits[0] ||
	    (digits[0] == '0' && digits[1]))
		return false;
	for (; *digits; ++digits) {
		if (*digits < '0' || *digits > '9' || value > 100)
			return false;
		value = value * 10 + (unsigned int)(*digits - '0');
	}
	if (value < first || value - first >= count)
		return false;
	*index = value - first;
	return true;
}

static bool set_target(struct target *target, char *text, size_t size,
		       unsigned int slot)
{
	*target = (struct target){ .text = text, .size = size, .slot = slot };
	return true;
}

/* False for a key that this reader does not know. */
static bool find_target(struct reader *reader, const char *key,
			struct target *target)
{
	static const char *const cases[] = {
		[FPLINUX_LANGUAGE_SENTENCE] = "input.indicator.sentence",
		[FPLINUX_LANGUAGE_LOWER] = "input.indicator.lower",
		[FPLINUX_LANGUAGE_UPPER] = "input.indicator.upper",
	};
	struct fplinux_language *language = reader->language;
	const size_t text = FPLINUX_LANGUAGE_TEXT_BYTES;
	unsigned int i;

	if (!strcmp(key, "name"))
		return set_target(target, language->name, text, SLOT_NAME);
	if (!strcmp(key, "lang"))
		return set_target(target, language->lang, text, SLOT_LANG);
	if (!strcmp(key, "system"))
		return set_target(target, reader->system, text, SLOT_SYSTEM);
	if (!strcmp(key, "input"))
		return set_target(target, reader->input, text, SLOT_INPUT);
	if (!strcmp(key, "decimal"))
		return set_target(target, reader->decimal, text, SLOT_DECIMAL);
	if (!strcmp(key, "date.long"))
		return set_target(target, language->date_long, text,
				  SLOT_DATE_LONG);
	if (!strcmp(key, "date.short"))
		return set_target(target, language->date_short, text,
				  SLOT_DATE_SHORT);
	if (!strcmp(key, "xkb"))
		return set_target(target, language->xkb, sizeof(language->xkb),
				  SLOT_XKB);
	for (i = 0; i < 3; ++i)
		if (!strcmp(key, cases[i]))
			return set_target(target, language->indicator[i], text,
					  SLOT_INDICATOR + i);
	if (key_index(key, "weekday.", 0, 7, &i))
		return set_target(target, language->weekday[i], text,
				  SLOT_WEEKDAY + i);
	if (key_index(key, "month.genitive.", 1, 12, &i))
		return set_target(target, language->month_genitive[i], text,
				  SLOT_MONTH_GENITIVE + i);
	if (key_index(key, "month.", 1, 12, &i))
		return set_target(target, language->month[i], text,
				  SLOT_MONTH + i);
	if (key_index(key, "multitap.", 0, FPLINUX_LANGUAGE_KEYS, &i))
		return set_target(target, language->multitap[i], text,
				  SLOT_MULTITAP + i);
	return false;
}

static bool read_line(struct reader *reader, char *line)
{
	char *equals = strchr(line, '=');
	struct target target;
	char *value;

	if (!equals)
		return false;
	*equals = '\0';
	value = equals + 1;
	if (!is_key(line) || !unescape(value) || !is_text(value))
		return false;
	if (!find_target(reader, line, &target))
		return true;
	if (reader->seen & (UINT64_C(1) << target.slot) ||
	    strlen(value) >= target.size)
		return false;
	reader->seen |= UINT64_C(1) << target.slot;
	strcpy(target.text, value);
	return true;
}

/* Literal text and the date_fields placeholders in braces. */
static bool is_date_pattern(const char *pattern)
{
	if (!pattern[0])
		return false;
	while (*pattern) {
		const char *close;
		size_t length;
		bool known = false;

		if (*pattern == '}')
			return false;
		if (*pattern != '{') {
			++pattern;
			continue;
		}
		close = strchr(pattern, '}');
		if (!close)
			return false;
		length = (size_t)(close - pattern - 1);
		for (size_t i = 0;
		     i < sizeof(date_fields) / sizeof(*date_fields); ++i)
			known = known ||
				(strlen(date_fields[i]) == length &&
				 !strncmp(pattern + 1, date_fields[i], length));
		if (!known)
			return false;
		pattern = close + 1;
	}
	return true;
}

static bool read_flag(const char *text, bool *flag)
{
	*flag = !strcmp(text, "yes");
	return *flag || !text[0] || !strcmp(text, "no");
}

/* A LANG value such as ru_RU.UTF-8 or sr_RS@latin. */
static bool is_lang(const char *lang)
{
	size_t length = strspn(lang,
			       "abcdefghijklmnopqrstuvwxyz"
			       "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._@-");

	return length && !lang[length];
}

static bool is_xkb_layout(const char *layout)
{
	size_t length =
		strspn(layout, "abcdefghijklmnopqrstuvwxyz0123456789_-");

	return !layout[length];
}

/*
 * A system language needs everything that formats dates and numbers; an
 * input language its mode indicators. The key maps are checked by the
 * multi-tap engine, which knows what a usable map is.
 */
static bool finish(struct reader *reader)
{
	struct fplinux_language *language = reader->language;

	if (!language->name[0] ||
	    !read_flag(reader->system, &language->system) ||
	    !read_flag(reader->input, &language->input) ||
	    !is_xkb_layout(language->xkb))
		return false;
	if (!reader->decimal[0] && !language->system)
		language->decimal = '.';
	else if (!strcmp(reader->decimal, ".") || !strcmp(reader->decimal, ","))
		language->decimal = reader->decimal[0];
	else
		return false;
	if (language->system) {
		if (!is_lang(language->lang) ||
		    !is_date_pattern(language->date_long) ||
		    !is_date_pattern(language->date_short))
			return false;
		for (size_t i = 0; i < 7; ++i)
			if (!language->weekday[i][0])
				return false;
		for (size_t i = 0; i < 12; ++i)
			if (!language->month[i][0] ||
			    !language->month_genitive[i][0])
				return false;
	}
	if (language->input)
		for (size_t i = 0; i < 3; ++i)
			if (!language->indicator[i][0])
				return false;
	return true;
}

static int read_file(FILE *file, struct reader *reader)
{
	char *line = NULL;
	size_t size = 0;
	ssize_t length;
	int error = 0;

	errno = 0;
	while ((length = getline(&line, &size, file)) >= 0) {
		if (length && line[length - 1] == '\n')
			line[--length] = '\0';
		if ((size_t)length != strlen(line)) {
			error = EINVAL;
			break;
		}
		if (!length || line[0] == '#')
			continue;
		if (!read_line(reader, line)) {
			error = EINVAL;
			break;
		}
	}
	if (!error && ferror(file))
		error = errno ? errno : EIO;
	else if (!error && errno == ENOMEM)
		error = ENOMEM;
	free(line);
	if (!error && !finish(reader))
		error = EINVAL;
	return error;
}

bool fplinux_language_load(const char *directory, const char *code,
			   struct fplinux_language *language)
{
	struct reader reader = { .language = language };
	char path[PATH_MAX];
	FILE *file;
	int error;

	memset(language, 0, sizeof(*language));
	if (!directory || !code || !is_code(code)) {
		errno = EINVAL;
		return false;
	}
	if (snprintf(path, sizeof(path), "%s/%s", directory, code) >=
	    (int)sizeof(path)) {
		errno = ENAMETOOLONG;
		return false;
	}
	file = fopen(path, "re");
	if (!file)
		return false;
	error = read_file(file, &reader);
	fclose(file);
	if (error) {
		memset(language, 0, sizeof(*language));
		errno = error;
		return false;
	}
	strcpy(language->code, code);
	return true;
}

/* Lists codes whose files load, keeping the first capacity in byte order. */
size_t fplinux_language_list(const char *directory,
			     char codes[][FPLINUX_LANGUAGE_CODE_BYTES],
			     size_t capacity)
{
	struct fplinux_language language;
	struct dirent *entry;
	size_t count = 0;
	DIR *entries;

	if (!directory) {
		errno = EINVAL;
		return 0;
	}
	entries = opendir(directory);
	if (!entries)
		return 0;
	while ((entry = readdir(entries))) {
		size_t at;

		if (!is_code(entry->d_name) ||
		    !fplinux_language_load(directory, entry->d_name, &language))
			continue;
		at = count < capacity ? count : capacity;
		++count;
		/* Insertion in order; a code past the capacity drops out. */
		while (at && strcmp(entry->d_name, codes[at - 1]) < 0) {
			if (at < capacity)
				strcpy(codes[at], codes[at - 1]);
			--at;
		}
		if (at < capacity)
			strcpy(codes[at], entry->d_name);
	}
	closedir(entries);
	return count;
}
