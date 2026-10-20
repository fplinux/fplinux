/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define _GNU_SOURCE
#include "fplinux-ui-prefs.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#define KEY_MAX_BYTES 63U
#define VALUE_MAX_BYTES 65535U
/* The longest stored line: key, '=', a value with every byte escaped. */
#define LINE_MAX_BYTES (KEY_MAX_BYTES + 1U + 2U * VALUE_MAX_BYTES)

/*
 * Declared in fplinux-ui.h, which needs LVGL; the preferences do not. The
 * runtime library defines it.
 */
bool fplinux_ui_state_dir(const char *app_id, char *path, size_t capacity);

struct preference {
	char *key;
	char *value;
};

/*
 * The file holds one "key=value" line per preference in insertion order.
 * Values escape '\\' as "\\\\", a line feed as "\\n" and a carriage return
 * as "\\r"; every other byte is stored as it is.
 */
struct fplinux_ui_prefs {
	char directory[PATH_MAX];
	char path[PATH_MAX];
	struct preference *items;
	size_t count;
	size_t capacity;
	bool changed;
};

/* ASCII letters, digits, '.', '_' and '-'; independent of the C locale. */
static bool key_byte(char byte)
{
	return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
	       (byte >= '0' && byte <= '9') || byte == '.' || byte == '_' ||
	       byte == '-';
}

static bool valid_key(const char *key)
{
	size_t length = 0;

	if (!key)
		return false;
	for (; key[length]; ++length)
		if (length == KEY_MAX_BYTES || !key_byte(key[length]))
			return false;
	return length > 0;
}

/* Strict UTF-8: no overlong forms, surrogates or code points past U+10FFFF. */
static bool valid_utf8(const char *text, size_t length)
{
	const unsigned char *bytes = (const unsigned char *)text;
	size_t index = 0;

	while (index < length) {
		unsigned char lead = bytes[index];
		unsigned int minimum;
		unsigned int code;
		size_t extra;

		if (lead < 0x80) {
			++index;
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
		if (length - index <= extra)
			return false;
		for (size_t i = 1; i <= extra; ++i) {
			if ((bytes[index + i] & 0xc0) != 0x80)
				return false;
			code = code << 6 | (bytes[index + i] & 0x3f);
		}
		if (code < minimum || code > 0x10ffff ||
		    (code >= 0xd800 && code <= 0xdfff))
			return false;
		index += extra + 1;
	}
	return true;
}

static struct preference *find(const struct fplinux_ui_prefs *prefs,
			       const char *key)
{
	for (size_t i = 0; i < prefs->count; ++i)
		if (!strcmp(prefs->items[i].key, key))
			return &prefs->items[i];
	return NULL;
}

/* Sets an already validated value; false with errno on allocation failure. */
static bool store(struct fplinux_ui_prefs *prefs, const char *key,
		  const char *value)
{
	struct preference *item = find(prefs, key);
	char *copy;

	if (item && !strcmp(item->value, value))
		return true;
	copy = strdup(value);
	if (!copy)
		return false;
	if (item) {
		free(item->value);
		item->value = copy;
		prefs->changed = true;
		return true;
	}
	if (prefs->count == prefs->capacity) {
		size_t capacity = prefs->capacity ? 2 * prefs->capacity : 8;
		struct preference *items =
			reallocarray(prefs->items, capacity, sizeof(*items));

		if (!items) {
			free(copy);
			return false;
		}
		prefs->items = items;
		prefs->capacity = capacity;
	}
	item = &prefs->items[prefs->count];
	item->key = strdup(key);
	if (!item->key) {
		free(copy);
		return false;
	}
	item->value = copy;
	++prefs->count;
	prefs->changed = true;
	return true;
}

/* Undoes the escaping in place; false for an unknown or unfinished escape. */
static bool unescape(char *text, size_t *length)
{
	size_t out = 0;

	for (size_t in = 0; in < *length; ++in) {
		char byte = text[in];

		if (byte == '\\') {
			if (++in == *length)
				return false;
			if (text[in] == 'n')
				byte = '\n';
			else if (text[in] == 'r')
				byte = '\r';
			else if (text[in] == '\\')
				byte = '\\';
			else
				return false;
		}
		text[out++] = byte;
	}
	text[out] = '\0';
	*length = out;
	return true;
}

enum line_result {
	LINE_STORED,
	LINE_MALFORMED,
	LINE_FAILED,
};

static enum line_result parse_line(struct fplinux_ui_prefs *prefs, char *line,
				   size_t length)
{
	char *equals = memchr(line, '=', length);
	char *value;
	size_t value_length;

	if (!equals || length > LINE_MAX_BYTES || memchr(line, '\0', length))
		return LINE_MALFORMED;
	*equals = '\0';
	value = equals + 1;
	value_length = length - (size_t)(value - line);
	if (!valid_key(line) || !unescape(value, &value_length) ||
	    value_length > VALUE_MAX_BYTES || !valid_utf8(value, value_length))
		return LINE_MALFORMED;
	return store(prefs, line, value) ? LINE_STORED : LINE_FAILED;
}

/* A missing file is empty; malformed lines are reported and skipped. */
static bool load(struct fplinux_ui_prefs *prefs)
{
	FILE *file = fopen(prefs->path, "re");
	char *line = NULL;
	size_t size = 0;
	size_t number = 0;
	ssize_t read;
	int error = 0;

	if (!file)
		return errno == ENOENT;
	while ((read = getline(&line, &size, file)) >= 0) {
		size_t length = (size_t)read;
		enum line_result result;

		++number;
		if (length && line[length - 1] == '\n')
			line[--length] = '\0';
		if (!length)
			continue;
		result = parse_line(prefs, line, length);
		if (result == LINE_FAILED) {
			error = errno;
			break;
		}
		if (result == LINE_MALFORMED)
			fprintf(stderr,
				"fplinux-ui: %s:%zu: malformed preference ignored\n",
				prefs->path, number);
	}
	if (!error && ferror(file))
		error = errno ? errno : EIO;
	free(line);
	fclose(file);
	/* The file as loaded is the saved state. */
	prefs->changed = false;
	errno = error;
	return !error;
}

struct fplinux_ui_prefs *fplinux_ui_prefs_open(const char *app_id)
{
	struct fplinux_ui_prefs *prefs = calloc(1, sizeof(*prefs));
	int error;

	if (!prefs)
		return NULL;
	if (!fplinux_ui_state_dir(app_id, prefs->directory,
				  sizeof(prefs->directory))) {
		error = errno;
		goto fail;
	}
	if (snprintf(prefs->path, sizeof(prefs->path), "%s/preferences",
		     prefs->directory) >= (int)sizeof(prefs->path)) {
		error = ENAMETOOLONG;
		goto fail;
	}
	if (!load(prefs)) {
		error = errno;
		goto fail;
	}
	return prefs;
fail:
	fplinux_ui_prefs_close(prefs);
	errno = error;
	return NULL;
}

void fplinux_ui_prefs_close(struct fplinux_ui_prefs *prefs)
{
	if (!prefs)
		return;
	for (size_t i = 0; i < prefs->count; ++i) {
		free(prefs->items[i].key);
		free(prefs->items[i].value);
	}
	free(prefs->items);
	free(prefs);
}

const char *fplinux_ui_prefs_get(const struct fplinux_ui_prefs *prefs,
				 const char *key, const char *fallback)
{
	const struct preference *item;

	if (!prefs || !valid_key(key))
		return fallback;
	item = find(prefs, key);
	return item ? item->value : fallback;
}

/* The whole value must be a decimal number within long long. */
long long fplinux_ui_prefs_get_int(const struct fplinux_ui_prefs *prefs,
				   const char *key, long long fallback)
{
	const char *text = fplinux_ui_prefs_get(prefs, key, NULL);
	int saved = errno;
	long long value;
	char *end;

	if (!text || !text[0] || isspace((unsigned char)text[0]))
		return fallback;
	errno = 0;
	value = strtoll(text, &end, 10);
	if (errno == ERANGE || *end)
		value = fallback;
	errno = saved;
	return value;
}

bool fplinux_ui_prefs_set(struct fplinux_ui_prefs *prefs, const char *key,
			  const char *value)
{
	size_t length;

	if (!prefs || !valid_key(key) || !value) {
		errno = EINVAL;
		return false;
	}
	length = strlen(value);
	if (length > VALUE_MAX_BYTES) {
		errno = E2BIG;
		return false;
	}
	if (!valid_utf8(value, length)) {
		errno = EILSEQ;
		return false;
	}
	return store(prefs, key, value);
}

bool fplinux_ui_prefs_set_int(struct fplinux_ui_prefs *prefs, const char *key,
			      long long value)
{
	char text[32];

	snprintf(text, sizeof(text), "%lld", value);
	return fplinux_ui_prefs_set(prefs, key, text);
}

/* Removing a key that is not there succeeds. */
bool fplinux_ui_prefs_remove(struct fplinux_ui_prefs *prefs, const char *key)
{
	struct preference *item;
	size_t index;

	if (!prefs || !valid_key(key)) {
		errno = EINVAL;
		return false;
	}
	item = find(prefs, key);
	if (!item)
		return true;
	index = (size_t)(item - prefs->items);
	free(item->key);
	free(item->value);
	memmove(item, item + 1,
		(prefs->count - index - 1) * sizeof(*prefs->items));
	--prefs->count;
	prefs->changed = true;
	return true;
}

static void write_value(FILE *file, const char *value)
{
	for (; *value; ++value) {
		if (*value == '\\')
			fputs("\\\\", file);
		else if (*value == '\n')
			fputs("\\n", file);
		else if (*value == '\r')
			fputs("\\r", file);
		else
			fputc(*value, file);
	}
}

static bool write_items(FILE *file, const struct fplinux_ui_prefs *prefs)
{
	for (size_t i = 0; i < prefs->count; ++i) {
		fputs(prefs->items[i].key, file);
		fputc('=', file);
		write_value(file, prefs->items[i].value);
		fputc('\n', file);
	}
	return !ferror(file) && !fflush(file);
}

/* Makes the rename itself durable. */
static bool sync_directory(const char *path)
{
	int directory = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	int error;

	if (directory < 0)
		return false;
	if (fsync(directory) < 0) {
		error = errno;
		close(directory);
		errno = error;
		return false;
	}
	close(directory);
	return true;
}

/*
 * A new mode-0600 file in the same directory replaces the old one by rename,
 * so a crash leaves either the previous or the new preferences.
 */
bool fplinux_ui_prefs_save(struct fplinux_ui_prefs *prefs)
{
	char temporary[PATH_MAX];
	FILE *file;
	int error;
	int fd;

	if (!prefs) {
		errno = EINVAL;
		return false;
	}
	if (!prefs->changed)
		return true;
	if (snprintf(temporary, sizeof(temporary), "%s/.preferences.XXXXXX",
		     prefs->directory) >= (int)sizeof(temporary)) {
		errno = ENAMETOOLONG;
		return false;
	}
	fd = mkostemp(temporary, O_CLOEXEC);
	if (fd < 0)
		return false;
	file = fdopen(fd, "w");
	if (!file) {
		error = errno;
		close(fd);
		goto fail;
	}
	errno = 0;
	if (!write_items(file, prefs) || fsync(fd) < 0) {
		error = errno ? errno : EIO;
		fclose(file);
		goto fail;
	}
	if (fclose(file)) {
		error = errno;
		goto fail;
	}
	if (rename(temporary, prefs->path) < 0) {
		error = errno;
		goto fail;
	}
	if (!sync_directory(prefs->directory))
		return false;
	prefs->changed = false;
	return true;
fail:
	unlink(temporary);
	errno = error;
	return false;
}
