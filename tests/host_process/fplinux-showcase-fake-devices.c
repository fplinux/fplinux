// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include "fplinux-drm-session.h"
#include "fplinux-keypad.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int keypad = -1;
static int vibrator = -1;
static int dispatches;
static int frames_presented;
static int led_brightness = -1;
static int led_trigger = -1;

FILE *__real_fopen(const char *path, const char *mode);
int __real_open(const char *path, int flags, ...);
ssize_t __real_read(int fd, void *buffer, size_t size);
ssize_t __real_write(int fd, const void *buffer, size_t size);
int __real_connect(int fd, const struct sockaddr *address, socklen_t length);
int __real_close(int fd);

/* Model the LED sysfs trigger selection and brightness-zero side effect only. */
static bool select_led_trigger(const char *selected)
{
	const char *directory = getenv("SHOWCASE_KEYPAD_LED");
	char path[512];
	FILE *file;
	bool ok;

	if (!directory || snprintf(path, sizeof(path), "%s/trigger",
				   directory) >= (int)sizeof(path)) {
		errno = EINVAL;
		return false;
	}
	file = __real_fopen(path, "w");
	if (!file)
		return false;
	if (!strcmp(selected, "none"))
		ok = fputs("[none] input-events timer\n", file) >= 0;
	else if (!strcmp(selected, "input-events"))
		ok = fputs("none [input-events] timer\n", file) >= 0;
	else {
		errno = EINVAL;
		ok = false;
	}
	if (fclose(file) < 0)
		ok = false;
	return ok;
}

FILE *__wrap_fopen(const char *path, const char *mode)
{
	const char *font_path = getenv("SHOWCASE_FONT_PATH");

	if (font_path && !strcmp(path, "/usr/share/fplinux/fonts/default.psf"))
		path = font_path;
	return __real_fopen(path, mode);
}

int __wrap_open(const char *path, int flags, ...)
{
	const char *directory = getenv("SHOWCASE_KEYPAD_LED");
	int descriptor;

	if (strstr(path, "/backlight/")) {
		fprintf(stderr, "Showcase opened an LCD backlight path: %s\n",
			path);
		exit(2);
	}
	if (!strcmp(path, "/dev/input/event0")) {
		if ((flags & O_ACCMODE) != O_RDONLY) {
			errno = ENOENT;
			return -1;
		}
		keypad = __real_open("/dev/null", flags);
		return keypad;
	}
	if (!strcmp(path, "/dev/input/event1")) {
		if ((flags & O_ACCMODE) != O_RDWR) {
			errno = ENOENT;
			return -1;
		}
		vibrator = __real_open("/dev/null", flags);
		return vibrator;
	}
	if (flags & O_CREAT) {
		va_list arguments;
		mode_t mode;

		va_start(arguments, flags);
		mode = va_arg(arguments, mode_t);
		va_end(arguments);
		return __real_open(path, flags, mode);
	}
	descriptor = __real_open(path, flags);
	if (descriptor >= 0 && directory &&
	    !strncmp(path, directory, strlen(directory))) {
		const char *attribute = path + strlen(directory);

		if (!strcmp(attribute, "/brightness"))
			led_brightness = descriptor;
		else if (!strcmp(attribute, "/trigger"))
			led_trigger = descriptor;
	}
	return descriptor;
}

int __wrap_close(int fd)
{
	if (fd == led_brightness)
		led_brightness = -1;
	if (fd == led_trigger)
		led_trigger = -1;
	return __real_close(fd);
}

int __wrap_connect(int fd, const struct sockaddr *address, socklen_t length)
{
	struct sockaddr_un redirected = { .sun_family = AF_UNIX };
	const char *socket_path = getenv("SHOWCASE_BRIGHTNESS_SOCKET");

	if (address->sa_family != AF_UNIX || !socket_path)
		return __real_connect(fd, address, length);
	if (strlen(socket_path) >= sizeof(redirected.sun_path)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	strcpy(redirected.sun_path, socket_path);
	return __real_connect(fd, (const struct sockaddr *)&redirected,
			      sizeof(redirected));
}

int __wrap_ioctl(int fd, unsigned long request, ...)
{
	va_list arguments;
	void *value;
	unsigned int number = _IOC_NR(request);
	unsigned int bytes = _IOC_SIZE(request);

	if (fd != keypad && fd != vibrator) {
		errno = ENOTTY;
		return -1;
	}
	if (number == 0x81 || number == 0x90)
		return 0;
	va_start(arguments, request);
	value = va_arg(arguments, void *);
	va_end(arguments);
	if (number == 0x07) {
		const char *phys = fd == keypad ? "fplinux/keypad0" :
						  "fplinux/vibrator0";

		snprintf(value, bytes, "%s", phys);
		return (int)strlen(phys) + 1;
	}
	if (number >= 0x20 && number <= 0x20 + EV_MAX) {
		unsigned int type = number - 0x20;
		unsigned int bit = fd == keypad ? FPLINUX_KEY_SOFT_RIGHT :
						  FF_RUMBLE;
		unsigned char *bits = value;

		memset(bits, 0, bytes);
		if (type == 0)
			bit = fd == keypad ? EV_KEY : EV_FF;
		if (type == 0 || (fd == keypad && type == EV_KEY) ||
		    (fd == vibrator && type == EV_FF)) {
			if (bit / 8U < bytes)
				bits[bit / 8U] |= 1U << (bit % 8U);
		}
		return (int)bytes;
	}
	if (number == 0x80) {
		((struct ff_effect *)value)->id = 1;
		return 0;
	}
	errno = EINVAL;
	return -1;
}

ssize_t __wrap_read(int fd, void *buffer, size_t size)
{
	struct input_event event = {
		.type = EV_KEY,
		.code = FPLINUX_KEY_SOFT_RIGHT,
		.value = 1,
	};
	const char *extra_frame = getenv("SHOWCASE_EXTRA_FRAME");

	if (fd != keypad)
		return __real_read(fd, buffer, size);
	if (frames_presented < (extra_frame ? 2 : 1)) {
		errno = EAGAIN;
		return -1;
	}
	if (size < sizeof(event)) {
		errno = EINVAL;
		return -1;
	}
	memcpy(buffer, &event, sizeof(event));
	return sizeof(event);
}

ssize_t __wrap_write(int fd, const void *buffer, size_t size)
{
	ssize_t written;

	if (fd == vibrator)
		return (ssize_t)size;
	if (fd == led_trigger) {
		char selected[32];

		if (size >= sizeof(selected)) {
			errno = EINVAL;
			return -1;
		}
		memcpy(selected, buffer, size);
		selected[size] = '\0';
		selected[strcspn(selected, "\n")] = '\0';
		return select_led_trigger(selected) ? (ssize_t)size : -1;
	}
	written = __real_write(fd, buffer, size);
	if (fd == led_brightness && written == (ssize_t)size && size &&
	    ((const char *)buffer)[0] == '0' && !select_led_trigger("none"))
		return -1;
	return written;
}

bool fplinux_drm_session_open(struct fplinux_drm_session *session,
			      const char *drm_path, const char *tty_path,
			      uint32_t format, char *error, size_t error_size)
{
	(void)drm_path;
	(void)tty_path;
	(void)format;
	(void)error;
	(void)error_size;
	session->width = 128;
	session->height = 160;
	session->stride = session->width * 2U;
	session->page_bytes = session->stride * session->height;
	session->mapping = calloc(2U, session->page_bytes);
	session->pages = 2;
	session->active = true;
	return session->mapping != NULL;
}

bool fplinux_drm_session_set_active_handler(struct fplinux_drm_session *session,
					    fplinux_drm_active_handler handler,
					    void *data)
{
	session->active_handler = handler;
	session->active_data = data;
	return !handler || handler(session, true, data);
}

bool fplinux_drm_session_dispatch(struct fplinux_drm_session *session)
{
	if (getenv("SHOWCASE_VT_CYCLE") && ++dispatches == 2) {
		if (!session->active_handler(session, false,
					     session->active_data))
			return false;
		session->active = false;
		if (!session->active_handler(session, true,
					     session->active_data))
			return false;
		session->active = true;
	}
	return true;
}

bool fplinux_drm_session_present(struct fplinux_drm_session *session,
				 unsigned int page)
{
	session->shown_page = page;
	++frames_presented;
	return true;
}

bool fplinux_drm_session_close(struct fplinux_drm_session *session)
{
	free(session->mapping);
	return true;
}
