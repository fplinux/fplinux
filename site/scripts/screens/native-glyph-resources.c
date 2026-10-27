/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

int __real_open(const char *path, int flags, ...);

/* Preserve resource lookup and decoding; the capture owns the installed tree. */
int __wrap_open(const char *path, int flags, ...)
{
	static const char prefix[] = "/usr/share/fplinux/glyphs/";
	const char *root = getenv("FPLINUX_NATIVE_GLYPHS");
	char mapped[1024];
	mode_t mode = 0;
	va_list arguments;

	if (root && !strncmp(path, prefix, sizeof(prefix) - 1)) {
		if (snprintf(mapped, sizeof(mapped), "%s/%s", root,
			     path + sizeof(prefix) - 1) >=
		    (int)sizeof(mapped)) {
			errno = ENAMETOOLONG;
			return -1;
		}
		path = mapped;
	}
	if ((flags & O_CREAT) || (flags & O_TMPFILE) == O_TMPFILE) {
		va_start(arguments, flags);
		mode = va_arg(arguments, mode_t);
		va_end(arguments);
	}
	return __real_open(path, flags, mode);
}
