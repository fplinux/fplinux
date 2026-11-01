/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/types.h>

static const struct {
	const char *prefix;
	const char *variable;
} trees[] = {
	{ "/usr/share/icons/hicolor/", "FPLINUX_LAUNCHER_CAPTURE_ICONS" },
	{ "/usr/share/fplinux/glyphs/", "FPLINUX_NATIVE_GLYPHS" },
};

/*
 * The app keeps its resource policy; only its external icon and glyph trees
 * are owned here, each by the capture that sets its variable.
 */
static const char *resource_path(const char *path, char *buffer,
				 size_t capacity)
{
	for (size_t i = 0; i < sizeof(trees) / sizeof(*trees); ++i) {
		const char *root = getenv(trees[i].variable);
		size_t length = strlen(trees[i].prefix);

		if (!root || strncmp(path, trees[i].prefix, length))
			continue;
		if (snprintf(buffer, capacity, "%s/%s", root, path + length) >=
		    (int)capacity) {
			errno = ENAMETOOLONG;
			return NULL;
		}
		return buffer;
	}
	return path;
}

int __real_open(const char *path, int flags, ...);

int __wrap_open(const char *path, int flags, ...)
{
	char buffer[PATH_MAX];
	const char *mapped = resource_path(path, buffer, sizeof(buffer));
	mode_t mode = 0;
	va_list arguments;

	if (!mapped)
		return -1;
	if ((flags & O_CREAT) || (flags & O_TMPFILE) == O_TMPFILE) {
		va_start(arguments, flags);
		mode = va_arg(arguments, mode_t);
		va_end(arguments);
	}
	return __real_open(mapped, flags, mode);
}

int __real_inotify_add_watch(int fd, const char *path, uint32_t mask);

int __wrap_inotify_add_watch(int fd, const char *path, uint32_t mask)
{
	char buffer[PATH_MAX];
	const char *mapped = resource_path(path, buffer, sizeof(buffer));

	return mapped ? __real_inotify_add_watch(fd, mapped, mask) : -1;
}
