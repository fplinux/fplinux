// SPDX-License-Identifier: GPL-2.0-only
/*
 * Device doubles for the Showcase brightness host test. The test links this
 * file with -Wl,--wrap=open,ioctl,read,write,connect, so only direct calls to
 * those functions from the linked objects reach the wrappers below:
 *
 * - open() backs /dev/input/event0 (read-only keypad) and event1 (read-write
 *   vibrator) with /dev/null. SHOWCASE_RIGHT_SOFT_KEY adds a second keypad at
 *   event2 and selects which keypad exposes the exit key (event0, event2 or
 *   none). A path containing "/backlight/" exits with status 2;
 * - ioctl() answers grab, physical path, event bits and force-feedback upload
 *   and removal for those descriptors, matching only the request number;
 * - read() reports a right soft key press once one frame, or two with
 *   SHOWCASE_EXTRA_FRAME set, has been presented;
 * - write() discards vibrator events;
 * - connect() redirects AF_UNIX connections to SHOWCASE_BRIGHTNESS_SOCKET.
 *
 * Strong fplinux_drm_session_* definitions replace the DRM and VT session with
 * a 128x160 memory buffer. SHOWCASE_VT_CYCLE makes the second dispatch release
 * and reacquire the display in one call.
 * The GPU output opener rejects all initialization unless the case requests
 * SHOWCASE_GPU_INIT_FAIL, which supplies a controlled initialization error.
 *
 * These doubles do not prove real evdev semantics, VT switching with an
 * inactive interval, DRM page presentation or sysfs LED file behavior. The
 * backlight guard does not see fopen(), openat() or calls made inside libc.
 */
#define _GNU_SOURCE

#include "fplinux-drm-session.h"
#include "fplinux-gpu-output.h"
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

static int keypads[2] = { -1, -1 };
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

struct fplinux_gpu_output *
fplinux_gpu_output_open(struct fplinux_drm_session *session,
			enum fplinux_gpu_api api, unsigned int width,
			unsigned int height, bool clockwise, char *error,
			size_t error_size)
{
	(void)session;
	(void)api;
	(void)width;
	(void)height;
	(void)clockwise;
	if (!getenv("SHOWCASE_GPU_INIT_FAIL")) {
		fputs("Showcase unexpectedly initialized GPU\n", stderr);
		exit(2);
	}
	snprintf(error, error_size, "controlled GPU initialization failure");
	return NULL;
}

static _Noreturn void unexpected_gpu_output(void)
{
	fputs("Showcase unexpectedly used GPU output\n", stderr);
	exit(2);
}

struct fplinux_gpu *
fplinux_gpu_output_gpu(const struct fplinux_gpu_output *output)
{
	if (output)
		unexpected_gpu_output();
	return NULL;
}

bool fplinux_gpu_output_render(struct fplinux_gpu_output *output,
			       unsigned int texture)
{
	(void)output;
	(void)texture;
	unexpected_gpu_output();
}

bool fplinux_gpu_output_begin_cpu(struct fplinux_gpu_output *output,
				  void **pixels, size_t *stride)
{
	(void)output;
	(void)pixels;
	(void)stride;
	unexpected_gpu_output();
}

bool fplinux_gpu_output_end_cpu(struct fplinux_gpu_output *output)
{
	(void)output;
	unexpected_gpu_output();
}

bool fplinux_gpu_output_present(struct fplinux_gpu_output *output)
{
	(void)output;
	unexpected_gpu_output();
}

const char *fplinux_gpu_output_error(const struct fplinux_gpu_output *output)
{
	(void)output;
	unexpected_gpu_output();
}

bool fplinux_gpu_output_close(struct fplinux_gpu_output *output)
{
	if (output)
		unexpected_gpu_output();
	return true;
}

static bool keypad_has_right_soft_key(int fd)
{
	const char *device = getenv("SHOWCASE_RIGHT_SOFT_KEY");

	return !device || (!strcmp(device, "event0") && fd == keypads[0]) ||
	       (!strcmp(device, "event2") && fd == keypads[1]);
}

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
	const char *right_soft_key = getenv("SHOWCASE_RIGHT_SOFT_KEY");
	int descriptor;

	if (strstr(path, "/backlight/")) {
		fprintf(stderr, "Showcase opened an LCD backlight path: %s\n",
			path);
		exit(2);
	}
	if (!strcmp(path, "/dev/input/event0") ||
	    (right_soft_key && !strcmp(path, "/dev/input/event2"))) {
		int index = !strcmp(path, "/dev/input/event0") ? 0 : 1;

		if ((flags & O_ACCMODE) != O_RDONLY) {
			errno = ENOENT;
			return -1;
		}
		keypads[index] = __real_open("/dev/null", flags);
		return keypads[index];
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
	if (fd == keypads[0])
		keypads[0] = -1;
	if (fd == keypads[1])
		keypads[1] = -1;
	if (fd == vibrator)
		vibrator = -1;
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
	bool is_keypad = fd == keypads[0] || fd == keypads[1];

	if (!is_keypad && fd != vibrator) {
		errno = ENOTTY;
		return -1;
	}
	if (number == _IOC_NR(EVIOCRMFF) || number == _IOC_NR(EVIOCGRAB))
		return 0;
	va_start(arguments, request);
	value = va_arg(arguments, void *);
	va_end(arguments);
	if (number == _IOC_NR(EVIOCGPHYS(0))) {
		const char *phys = is_keypad ? "fplinux/keypad0" :
					       "fplinux/vibrator0";

		snprintf(value, bytes, "%s", phys);
		return (int)strlen(phys) + 1;
	}
	if (number >= _IOC_NR(EVIOCGBIT(0, 0)) &&
	    number <= _IOC_NR(EVIOCGBIT(EV_MAX, 0))) {
		unsigned int type = number - _IOC_NR(EVIOCGBIT(0, 0));
		unsigned int bit = is_keypad ? FPLINUX_KEY_SOFT_RIGHT :
					       FF_RUMBLE;
		unsigned char *bits = value;

		memset(bits, 0, bytes);
		if (type == 0)
			bit = is_keypad ? EV_KEY : EV_FF;
		if (type == 0 ||
		    (is_keypad && type == EV_KEY &&
		     keypad_has_right_soft_key(fd)) ||
		    (fd == vibrator && type == EV_FF)) {
			if (bit / 8U < bytes)
				bits[bit / 8U] |= 1U << (bit % 8U);
		}
		return (int)bytes;
	}
	if (number == _IOC_NR(EVIOCSFF)) {
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

	if (fd != keypads[0] && fd != keypads[1])
		return __real_read(fd, buffer, size);
	if (!keypad_has_right_soft_key(fd) ||
	    frames_presented < (extra_frame ? 2 : 1)) {
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
	if (getenv("SHOWCASE_GPU_INIT_FAIL")) {
		const uint16_t *pixels =
			(const uint16_t *)((const unsigned char *)
						   session->mapping +
					   page * session->page_bytes);
		size_t count = session->page_bytes / sizeof(*pixels);
		size_t different = 0;

		for (size_t index = 1; index < count; ++index)
			if (pixels[index] != pixels[0])
				++different;
		if (different <= count / 16U) {
			fputs("Showcase fallback presented a blank frame\n",
			      stderr);
			return false;
		}
	}
	session->shown_page = page;
	++frames_presented;
	return true;
}

bool fplinux_drm_session_close(struct fplinux_drm_session *session)
{
	free(session->mapping);
	return true;
}
