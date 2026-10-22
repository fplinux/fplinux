// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/ioctl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

int __real_open(const char *path, int flags, ...);
DIR *__real_opendir(const char *path);
int __real_close(int descriptor);
ssize_t __real_write(int descriptor, const void *bytes, size_t size);
int __real_ioctl(int descriptor, unsigned long request, ...);
int __real_connect(int descriptor, const struct sockaddr *address,
		   socklen_t size);

static int vibrator = -1;

static void vibration_event(const char *event)
{
	const char *path = getenv("CLOCK_SIGNAL_VIBRATION_EVENTS");
	int descriptor = __real_open(
		path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0600);

	if (descriptor < 0)
		abort();
	if (__real_write(descriptor, event, strlen(event)) !=
	    (ssize_t)strlen(event))
		abort();
	__real_close(descriptor);
}

int __wrap_connect(int descriptor, const struct sockaddr *address,
		   socklen_t size)
{
	puts("audio-connect");
	fflush(stdout);
	return __real_connect(descriptor, address, size);
}

DIR *__wrap_opendir(const char *path)
{
	const char *input_tree = getenv("CLOCK_SIGNAL_INPUT_TREE");

	if (strcmp(path, "/dev/input") != 0)
		return __real_opendir(path);
	puts("input-open");
	fflush(stdout);
	if (!input_tree) {
		errno = ENOENT;
		return NULL;
	}
	return __real_opendir(input_tree);
}

int __wrap_open(const char *path, int flags, ...)
{
	const char *input_prefix = "/dev/input/event";
	size_t prefix_length = strlen(input_prefix);
	const char *suffix;
	mode_t mode;
	va_list arguments;

	suffix = strncmp(path, input_prefix, prefix_length) == 0 ?
			 path + prefix_length :
			 "";
	if (*suffix && strspn(suffix, "0123456789") == strlen(suffix)) {
		if (strcmp(suffix, "0") == 0 &&
		    getenv("CLOCK_SIGNAL_VIBRATION_EVENTS")) {
			vibrator = __real_open("/dev/null", O_RDWR | O_CLOEXEC);
			return vibrator;
		}
		errno = ENOENT;
		return -1;
	}
	if (!(flags & O_CREAT) && (flags & O_TMPFILE) != O_TMPFILE)
		return __real_open(path, flags);
	va_start(arguments, flags);
	mode = va_arg(arguments, mode_t);
	va_end(arguments);
	return __real_open(path, flags, mode);
}

int __wrap_ioctl(int descriptor, unsigned long request, ...)
{
	va_list arguments;
	void *value;

	va_start(arguments, request);
	if (descriptor == vibrator && request == EVIOCRMFF) {
		int effect = va_arg(arguments, int);

		va_end(arguments);
		if (effect != 7)
			abort();
		vibration_event("remove\n");
		return 0;
	}
	value = va_arg(arguments, void *);
	va_end(arguments);
	if (descriptor != vibrator)
		return __real_ioctl(descriptor, request, value);
	if (_IOC_NR(request) == _IOC_NR(EVIOCGPHYS(0))) {
		snprintf(value, _IOC_SIZE(request), "fplinux/vibrator0");
		return 0;
	}
	if (_IOC_NR(request) == _IOC_NR(EVIOCGBIT(0, 0)) ||
	    _IOC_NR(request) == _IOC_NR(EVIOCGBIT(EV_FF, 0))) {
		unsigned long *bits = value;
		unsigned int bit = _IOC_NR(request) ==
						   _IOC_NR(EVIOCGBIT(0, 0)) ?
					   EV_FF :
					   FF_RUMBLE;

		memset(value, 0, _IOC_SIZE(request));
		bits[bit / (8U * sizeof(*bits))] |=
			1UL << (bit % (8U * sizeof(*bits)));
		return 0;
	}
	if (request == EVIOCSFF) {
		struct ff_effect *effect = value;

		if (effect->type != FF_RUMBLE || effect->id != -1 ||
		    effect->replay.length != 250 ||
		    effect->u.rumble.strong_magnitude != 32767 ||
		    effect->u.rumble.weak_magnitude != 32767)
			abort();
		effect->id = 7;
		vibration_event("upload 250 32767 32767\n");
		return 0;
	}
	errno = EINVAL;
	return -1;
}

ssize_t __wrap_write(int descriptor, const void *bytes, size_t size)
{
	const struct input_event *event = bytes;

	if (descriptor != vibrator)
		return __real_write(descriptor, bytes, size);
	if (size != sizeof(*event) || event->type != EV_FF ||
	    event->code != 7 || (event->value != 0 && event->value != 1))
		abort();
	vibration_event(event->value ? "start\n" : "stop\n");
	return (ssize_t)size;
}

int __wrap_close(int descriptor)
{
	if (descriptor == vibrator) {
		vibration_event("close\n");
		vibrator = -1;
	}
	return __real_close(descriptor);
}
