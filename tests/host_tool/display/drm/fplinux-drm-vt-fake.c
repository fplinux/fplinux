/* SPDX-License-Identifier: GPL-2.0-only */
/* File-backed substitute for VT ioctls in the host process test. */
#define _GNU_SOURCE
#include <errno.h>
#include <linux/kd.h>
#include <linux/vt.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

int __wrap_ioctl(int fd, unsigned long request, ...)
{
	va_list arguments;
	int active;

	va_start(arguments, request);
	switch (request) {
	case VT_GETSTATE: {
		struct vt_stat *state = va_arg(arguments, struct vt_stat *);

		if (pread(fd, &active, sizeof(active), 0) != sizeof(active))
			abort();
		memset(state, 0, sizeof(*state));
		state->v_active = active;
		break;
	}
	case VT_OPENQRY:
		*va_arg(arguments, int *) = 5;
		break;
	case VT_ACTIVATE:
		active = va_arg(arguments, int);
		if (pwrite(fd, &active, sizeof(active), 0) != sizeof(active))
			abort();
		break;
	case VT_SETMODE:
	case VT_WAITACTIVE:
	case VT_RELDISP:
	case VT_DISALLOCATE:
	case KDSETMODE:
		break;
	default:
		va_end(arguments);
		errno = ENOTTY;
		return -1;
	}
	va_end(arguments);
	return 0;
}
