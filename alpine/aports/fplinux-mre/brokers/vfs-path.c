/* SPDX-License-Identifier: 0BSD */
#define _GNU_SOURCE
#include "vfs-path.h"
#include "../ipc/mre-vfs.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>

static bool utf8_valid(const unsigned char *text, size_t bytes)
{
	for (size_t i = 0; i < bytes;) {
		uint32_t scalar = text[i++];
		unsigned int remaining;
		uint32_t minimum;

		if (scalar < 0x80)
			continue;
		if (scalar >= 0xc2 && scalar <= 0xdf) {
			remaining = 1;
			minimum = 0x80;
			scalar &= 0x1f;
		} else if (scalar >= 0xe0 && scalar <= 0xef) {
			remaining = 2;
			minimum = 0x800;
			scalar &= 0x0f;
		} else if (scalar >= 0xf0 && scalar <= 0xf4) {
			remaining = 3;
			minimum = 0x10000;
			scalar &= 7;
		} else {
			return false;
		}
		if (remaining > bytes - i)
			return false;
		while (remaining--) {
			unsigned char continuation = text[i++];

			if ((continuation & 0xc0) != 0x80)
				return false;
			scalar = (scalar << 6) | (continuation & 0x3f);
		}
		if (scalar < minimum || scalar > 0x10ffff ||
		    (scalar >= 0xd800 && scalar <= 0xdfff))
			return false;
	}
	return true;
}

bool mre_vfs_component_valid(const char *start, size_t bytes, bool pattern)
{
	static const char staging_prefix[] = ".mre-vfs-";
	bool staging_name = bytes >= sizeof(staging_prefix) - 1;

	if (!bytes || bytes >= MRE_VFS_NAME_BYTES ||
	    (bytes == 1 && start[0] == '.') ||
	    (bytes == 2 && start[0] == '.' && start[1] == '.') ||
	    start[bytes - 1] == '.' || start[bytes - 1] == ' ')
		return false;
	if (!utf8_valid((const unsigned char *)start, bytes))
		return false;
	for (size_t i = 0; i < bytes; ++i) {
		unsigned char value = (unsigned char)start[i];

		if (value < 32 || value == 127 || value == '/' ||
		    value == '\\' || value == ':' || value == '"' ||
		    value == '<' || value == '>' || value == '|' ||
		    (!pattern && (value == '*' || value == '?')))
			return false;
		if (i < sizeof(staging_prefix) - 1) {
			unsigned char folded = value >= 'A' && value <= 'Z' ?
						       value + ('a' - 'A') :
						       value;

			if (folded != (unsigned char)staging_prefix[i])
				staging_name = false;
		}
	}
	/* Broker staging names are never part of the application's namespace. */
	return !staging_name;
}

int mre_vfs_normalize_path(const char *path, bool pattern, char *relative)
{
	size_t bytes = strnlen(path, MRE_VFS_PATH_BYTES);
	size_t cursor = 3;
	size_t output = 0;

	memset(relative, 0, MRE_VFS_PATH_BYTES);
	if (bytes >= MRE_VFS_PATH_BYTES || bytes < 3 || path[1] != ':' ||
	    path[2] != '\\' ||
	    (path[0] != 'E' && path[0] != 'e' && path[0] != 'C' &&
	     path[0] != 'c')) {
		errno = EINVAL;
		return -1;
	}
	/* The internal prefix selects a pinned directory, never a host path. */
	relative[output++] = path[0] == 'C' || path[0] == 'c' ? 'C' : 'E';
	relative[output++] = '/';
	while (cursor < bytes) {
		size_t start = cursor;

		while (cursor < bytes && path[cursor] != '\\')
			++cursor;
		if (!mre_vfs_component_valid(path + start, cursor - start,
					     pattern && cursor == bytes)) {
			errno = EINVAL;
			return -1;
		}
		if (output && relative[output - 1] != '/')
			relative[output++] = '/';
		memcpy(relative + output, path + start, cursor - start);
		output += cursor - start;
		if (cursor < bytes)
			++cursor;
	}
	relative[output] = '\0';
	return 0;
}

bool mre_vfs_ascii_case_equal(const char *first, const char *second)
{
	while (*first && *second) {
		unsigned char a = (unsigned char)*first++;
		unsigned char b = (unsigned char)*second++;

		if (a >= 'A' && a <= 'Z')
			a += 'a' - 'A';
		if (b >= 'A' && b <= 'Z')
			b += 'a' - 'A';
		if (a != b)
			return false;
	}
	return !*first && !*second;
}

static const char *next_character(const char *text)
{
	++text;
	while (((unsigned char)*text & 0xc0) == 0x80)
		++text;
	return text;
}

static unsigned char ascii_fold(unsigned char value)
{
	return value >= 'A' && value <= 'Z' ? value + ('a' - 'A') : value;
}

bool mre_vfs_name_matches(const char *pattern, const char *name)
{
	const char *star = NULL;
	const char *retry = NULL;

	/* DOS *.* includes extensionless files and directories. */
	if (strcmp(pattern, "*.*") == 0)
		pattern = "*";
	while (*name) {
		if (*pattern == '?') {
			++pattern;
			name = next_character(name);
		} else if (*pattern == *name ||
			   ascii_fold((unsigned char)*pattern) ==
				   ascii_fold((unsigned char)*name)) {
			++pattern;
			++name;
		} else if (*pattern == '*') {
			star = ++pattern;
			retry = name;
		} else if (star) {
			pattern = star;
			retry = next_character(retry);
			name = retry;
		} else {
			return false;
		}
	}
	while (*pattern == '*')
		++pattern;
	return !*pattern;
}
