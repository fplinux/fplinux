// SPDX-License-Identifier: GPL-2.0-only
/* Replace only the kernel character-device open with a captured pipe. */
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int __wrap_open(const char *path, int flags, ...);

int __wrap_open(const char *path, int flags, ...)
{
	if (strcmp(path, "/dev/kmsg") || !(flags & O_WRONLY) ||
	    getenv("FPLINUX_TEST_KMSG_DENY_OPEN")) {
		errno = EACCES;
		return -1;
	}
	return dup(STDOUT_FILENO);
}
