/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "session-vibrator.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/ioctl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

/* Real directory traversal/open; only the character-device UAPI is replaced. */
struct device {
	int fd;
	bool character;
	bool phys;
	bool force_feedback;
	bool rumble;
	unsigned int opens;
	unsigned int closes;
	unsigned int ioctls;
};

static struct device devices[3];
static unsigned int failures;
static unsigned int uploads;
static unsigned int erases;
static struct ff_effect uploaded_effect;
static struct input_event events[16];
static unsigned int event_count;
static int upload_error;
static int write_error;
static int erase_error;
static int close_error;
static bool short_write;
static bool interrupt_write;
static bool fail_stat;

int __real_open(const char *path, int flags, ...);
int __real_fstat(int fd, struct stat *status);
int __real_close(int fd);
ssize_t __real_write(int fd, const void *buffer, size_t count);

static void check(bool condition, const char *message)
{
	if (!condition) {
		fprintf(stderr, "FAIL: %s\n", message);
		++failures;
	}
}

static struct device *device_for(int fd)
{
	unsigned int i;

	for (i = 0; i < 3; ++i) {
		if (devices[i].fd == fd)
			return &devices[i];
	}
	return NULL;
}

int __wrap_open(const char *path, int flags, ...)
{
	const char *name = strrchr(path, '/');
	int fd = __real_open(path, flags);
	unsigned int index;

	if (!name || fd < 0)
		return fd;
	++name;
	for (index = 0; index < 3; ++index) {
		char expected[16];

		snprintf(expected, sizeof(expected), "event%u", index);
		if (strcmp(name, expected))
			continue;
		check((flags & (O_RDWR | O_CLOEXEC | O_NONBLOCK)) ==
			      (O_RDWR | O_CLOEXEC | O_NONBLOCK),
		      "device descriptor is writable, nonblocking and close-on-exec");
		devices[index].fd = fd;
		++devices[index].opens;
		return fd;
	}
	check(false, "non-event names must never be opened");
	return fd;
}

int __wrap_fstat(int fd, struct stat *status)
{
	struct device *device = device_for(fd);
	int result = __real_fstat(fd, status);

	if (!device || result < 0)
		return result;
	if (fail_stat) {
		errno = EIO;
		return -1;
	}
	if (device->character)
		status->st_mode = (status->st_mode & ~S_IFMT) | S_IFCHR;
	return 0;
}

static void set_bit(void *buffer, unsigned int bit)
{
	unsigned long *bits = buffer;
	unsigned int width = 8U * sizeof(*bits);

	bits[bit / width] |= 1UL << (bit % width);
}

int __wrap_ioctl(int fd, unsigned long request, ...)
{
	struct device *device = device_for(fd);
	va_list arguments;
	void *buffer;

	check(device != NULL, "ioctl only receives an owned event descriptor");
	if (!device) {
		errno = EBADF;
		return -1;
	}
	++device->ioctls;
	va_start(arguments, request);
	if (request == EVIOCRMFF) {
		int effect_id = va_arg(arguments, int);

		va_end(arguments);
		check(effect_id == 17, "cleanup erases the assigned effect id");
		++erases;
		if (erase_error) {
			errno = erase_error;
			return -1;
		}
		return 0;
	}
	buffer = va_arg(arguments, void *);
	va_end(arguments);
	if (request == EVIOCSFF) {
		struct ff_effect *effect = buffer;

		++uploads;
		check(effect->type == FF_RUMBLE && effect->id == -1,
		      "upload creates one new rumble effect");
		uploaded_effect = *effect;
		if (upload_error) {
			errno = upload_error;
			return -1;
		}
		effect->id = 17;
		return 0;
	}
	memset(buffer, 0, _IOC_SIZE(request));
	if (_IOC_NR(request) == _IOC_NR(EVIOCGPHYS(1))) {
		snprintf(buffer, _IOC_SIZE(request), "%s",
			 device->phys ? "fplinux/vibrator0" : "other/device");
		return 0;
	}
	if (_IOC_NR(request) == _IOC_NR(EVIOCGBIT(0, 1))) {
		if (device->force_feedback)
			set_bit(buffer, EV_FF);
		return 0;
	}
	if (_IOC_NR(request) == _IOC_NR(EVIOCGBIT(EV_FF, 1))) {
		if (device->rumble)
			set_bit(buffer, FF_RUMBLE);
		return 0;
	}
	check(false, "backend uses only the evdev force-feedback UAPI");
	errno = ENOTTY;
	return -1;
}

ssize_t __wrap_write(int fd, const void *buffer, size_t count)
{
	if (!device_for(fd))
		return __real_write(fd, buffer, count);
	check(count == sizeof(struct input_event),
	      "state write is one input event");
	if (interrupt_write) {
		interrupt_write = false;
		errno = EINTR;
		return -1;
	}
	if (write_error) {
		int error = write_error;

		write_error = 0;
		errno = error;
		return -1;
	}
	if (short_write) {
		short_write = false;
		return (ssize_t)count - 1;
	}
	if (event_count < 16)
		memcpy(&events[event_count++], buffer, sizeof(*events));
	else
		check(false, "unexpected repeated state writes");
	return (ssize_t)count;
}

int __wrap_close(int fd)
{
	struct device *device = device_for(fd);
	int result = __real_close(fd);

	if (!device)
		return result;
	device->fd = -1;
	++device->closes;
	if (close_error) {
		errno = close_error;
		return -1;
	}
	return result;
}

static void check_closed(const struct fplinux_session_vibrator *vibrator)
{
	unsigned int i;

	check(!fplinux_session_vibrator_available(vibrator) &&
		      vibrator->fd == -1 && vibrator->effect_id == -1 &&
		      !vibrator->on,
	      "released backend is unavailable and has no stale output state");
	for (i = 0; i < 3; ++i)
		check(devices[i].opens == devices[i].closes,
		      "every opened candidate descriptor is closed exactly once");
}

static bool is_rejection(const char *scenario)
{
	return !strcmp(scenario, "absent") ||
	       !strcmp(scenario, "noncharacter") ||
	       !strcmp(scenario, "wrong-phys") || !strcmp(scenario, "no-ff") ||
	       !strcmp(scenario, "no-rumble") ||
	       !strcmp(scenario, "stat-fails") ||
	       !strcmp(scenario, "ambiguous") ||
	       !strcmp(scenario, "upload-fails") ||
	       !strcmp(scenario, "zero-duration");
}

static void configure(const char *scenario)
{
	unsigned int i;

	for (i = 0; i < 3; ++i) {
		devices[i] = (struct device){
			.fd = -1,
			.character = true,
			.phys = true,
			.force_feedback = true,
			.rumble = true,
		};
	}
	if (!strcmp(scenario, "noncharacter"))
		devices[0].character = false;
	if (!strcmp(scenario, "wrong-phys"))
		devices[0].phys = false;
	if (!strcmp(scenario, "no-ff"))
		devices[0].force_feedback = false;
	if (!strcmp(scenario, "no-rumble"))
		devices[0].rumble = false;
	if (!strcmp(scenario, "stat-fails"))
		fail_stat = true;
	if (!strcmp(scenario, "upload-fails"))
		upload_error = ENOSPC;
	if (!strcmp(scenario, "unique")) {
		devices[1].phys = false;
		devices[2].character = false;
	}
}

static void test_rejection(const char *scenario, const char *directory)
{
	struct fplinux_session_vibrator vibrator;
	int expected = ENODEV;
	uint16_t duration_ms = 3000;

	if (!strcmp(scenario, "ambiguous"))
		expected = ENOTUNIQ;
	if (!strcmp(scenario, "upload-fails"))
		expected = ENOSPC;
	if (!strcmp(scenario, "zero-duration")) {
		expected = EINVAL;
		duration_ms = 0;
	}
	fplinux_session_vibrator_init(&vibrator);
	check(!fplinux_session_vibrator_open(&vibrator, directory, duration_ms,
					     UINT16_MAX) &&
		      errno == expected,
	      "unusable or ambiguous device reports the discovery/upload failure");
	check(uploads == (unsigned int)(expected == ENOSPC) && !erases,
	      "discovery never uploads to an unqualified or ambiguous device");
	if (uploads)
		check(uploaded_effect.replay.length == 3000 &&
			      uploaded_effect.u.rumble.strong_magnitude ==
				      65535 &&
			      uploaded_effect.u.rumble.weak_magnitude == 65535,
		      "failed upload still received the bounded session effect");
	if (!strcmp(scenario, "zero-duration"))
		check(!devices[0].opens,
		      "unbounded effect is rejected before opening an input device");
	if (!strcmp(scenario, "noncharacter") ||
	    !strcmp(scenario, "stat-fails"))
		check(!devices[0].ioctls,
		      "non-character input is rejected before ioctl");
	check_closed(&vibrator);
}

static void test_playback(const char *scenario, const char *directory)
{
	struct fplinux_session_vibrator vibrator;
	bool failed = false;
	bool clock_effect = !strcmp(scenario, "clock-effect");

	fplinux_session_vibrator_init(&vibrator);
	check(fplinux_session_vibrator_open(
		      &vibrator, directory, clock_effect ? 250 : 3000,
		      clock_effect ? UINT16_MAX / 2U : UINT16_MAX) &&
		      fplinux_session_vibrator_available(&vibrator),
	      "successful effect upload makes the backend available");
	check(uploads == 1 && !event_count,
	      "open uploads once without starting");
	if (clock_effect)
		check(uploaded_effect.replay.length == 250 &&
			      uploaded_effect.u.rumble.strong_magnitude ==
				      32767 &&
			      uploaded_effect.u.rumble.weak_magnitude == 32767,
		      "clock alert uploads a 250 ms half-strength effect");
	else
		check(uploaded_effect.replay.length == 3000 &&
			      uploaded_effect.u.rumble.strong_magnitude ==
				      65535 &&
			      uploaded_effect.u.rumble.weak_magnitude == 65535,
		      "session uploads a 3000 ms full-strength effect");
	if (!strcmp(scenario, "start-fails"))
		write_error = EPIPE;
	if (!strcmp(scenario, "short-write"))
		short_write = true;
	if (!strcmp(scenario, "interrupted-write"))
		interrupt_write = true;
	if (!strcmp(scenario, "start-fails") ||
	    !strcmp(scenario, "short-write")) {
		int expected = write_error ? EPIPE : EIO;

		erase_error = EPERM;
		close_error = EBUSY;
		check(!fplinux_session_vibrator_set(&vibrator, true) &&
			      errno == expected,
		      "state-write failure preserves its error across cleanup");
		check(erases == 1, "failed start removes its effect");
		check_closed(&vibrator);
		check(!fplinux_session_vibrator_set(&vibrator, true) &&
			      errno == ENODEV,
		      "lost device cannot silently accept a later start");
		return;
	}
	check(fplinux_session_vibrator_set(&vibrator, true) && vibrator.on &&
		      event_count == 1 && events[0].type == EV_FF &&
		      events[0].code == 17 && events[0].value == 1,
	      "start writes the assigned effect id and records success");
	check(fplinux_session_vibrator_set(&vibrator, true) && event_count == 1,
	      "unchanged output does not restart the bounded effect");
	if (!strcmp(scenario, "stop-fails")) {
		write_error = EAGAIN;
		check(!fplinux_session_vibrator_set(&vibrator, false) &&
			      errno == EAGAIN && erases == 1,
		      "failed stop reports its error and releases the effect");
		check_closed(&vibrator);
		return;
	}
	check(fplinux_session_vibrator_set(&vibrator, false) && !vibrator.on &&
		      event_count == 2 && events[1].value == 0,
	      "stop writes off and records the successful output state");
	if (!strcmp(scenario, "erase-fails")) {
		erase_error = EPERM;
		failed = true;
	}
	if (!strcmp(scenario, "close-fails")) {
		close_error = EIO;
		failed = true;
	}
	if (!strcmp(scenario, "close-stop-fails")) {
		write_error = EPIPE;
		erase_error = EPERM;
		close_error = EBUSY;
		failed = true;
	}
	check(fplinux_session_vibrator_close(&vibrator) == !failed,
	      "close reports stop, erase or descriptor release errors");
	if (!strcmp(scenario, "close-stop-fails"))
		check(errno == EPIPE, "cleanup retains the first error");
	if (!strcmp(scenario, "erase-fails"))
		check(errno == EPERM, "erase error reaches the caller");
	if (!strcmp(scenario, "close-fails"))
		check(errno == EIO, "close error reaches the caller");
	check(erases == 1 && uploads == 1,
	      "cleanup erases the one effect without re-uploading");
	check_closed(&vibrator);
	check(fplinux_session_vibrator_close(&vibrator) && erases == 1,
	      "repeated close does not use released resources");
}

int main(int argc, char **argv)
{
	if (argc != 3)
		return 2;
	configure(argv[1]);
	if (is_rejection(argv[1]))
		test_rejection(argv[1], argv[2]);
	else
		test_playback(argv[1], argv[2]);
	return failures ? 1 : 0;
}
