/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
/* Controlled udev/ioctl/libevdev boundaries; epoll and readiness fds are real. */
#include <assert.h>
#include <asm/ioctl.h>
#include <errno.h>
#include <fcntl.h>
#include <libevdev/libevdev.h>
#include <libudev.h>
#include <linux/input.h>
#include <poll.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "fplinux-input-session.h"

#define DEVICE_COUNT 6U
#define EVENT_COUNT 128U
#define BITS_PER_LONG (8U * sizeof(unsigned long))

struct udev {
	bool live;
};

struct udev_device {
	unsigned int index;
	const char *action;
};

struct udev_monitor {
	int pipe[2];
	struct udev_device notifications[EVENT_COUNT];
	size_t head;
	size_t tail;
};

struct udev_list_entry {
	unsigned int index;
	struct udev_list_entry *next;
};

struct udev_enumerate {
	struct udev_list_entry entries[DEVICE_COUNT];
	size_t count;
};

struct queued_event {
	struct input_event event;
	int result;
	unsigned int flags;
};

struct libevdev {
	unsigned int index;
	struct queued_event events[EVENT_COUNT];
	size_t head;
	size_t tail;
	bool live;
	bool pressed[KEY_CNT];
};

struct fake_device {
	const char *path;
	const char *phys;
	bool keyboard;
	bool pointer;
	bool present;
	bool grabbed;
	unsigned int busy;
	bool permanently_busy;
	int pipe[2];
	struct udev_device udev;
	struct libevdev evdev;
};

static struct udev fake_udev;
static struct udev_monitor fake_monitor;
static struct udev_enumerate fake_scan;
static bool scan_fails;
static struct fake_device devices[DEVICE_COUNT] = {
	{ .path = "/dev/input/event0", .phys = "fplinux/keypad0" },
	{ .path = "/dev/input/event1",
	  .phys = "usb/keyboard",
	  .keyboard = true },
	{ .path = "/dev/input/event2",
	  .phys = "bluetooth/combo",
	  .keyboard = true,
	  .pointer = true },
	{ .path = "/dev/input/event3", .phys = "usb/mouse", .pointer = true },
	{ .path = "/dev/input/event0",
	  .phys = "usb/replacement",
	  .keyboard = true },
	{ .path = "/dev/input/event5", .phys = "usb/key-only" },
};

int __real_close(int fd);
int __real_nanosleep(const struct timespec *requested,
		     struct timespec *remaining);

static void initialize(void)
{
	unsigned int i;

	for (i = 0; i < DEVICE_COUNT; ++i) {
		devices[i].pipe[0] = -1;
		devices[i].pipe[1] = -1;
		devices[i].udev.index = i;
		devices[i].evdev.index = i;
	}
}

static struct fake_device *device_by_fd(int fd)
{
	unsigned int i;

	for (i = 0; i < DEVICE_COUNT; ++i)
		if (devices[i].pipe[0] == fd)
			return &devices[i];
	assert(false);
	return NULL;
}

int __wrap_open(const char *path, int flags, ...)
{
	unsigned int i;

	assert((flags & (O_CLOEXEC | O_NONBLOCK)) == (O_CLOEXEC | O_NONBLOCK));
	for (i = 0; i < DEVICE_COUNT; ++i) {
		struct fake_device *device = &devices[i];

		if (device->present && !strcmp(path, device->path)) {
			assert(device->pipe[0] < 0);
			assert(pipe2(device->pipe, O_CLOEXEC | O_NONBLOCK) ==
			       0);
			return device->pipe[0];
		}
	}
	errno = ENOENT;
	return -1;
}

int __wrap_close(int fd)
{
	unsigned int i;

	for (i = 0; i < DEVICE_COUNT; ++i) {
		struct fake_device *device = &devices[i];

		if (device->pipe[0] == fd) {
			assert(!device->grabbed);
			device->pipe[0] = -1;
			assert(__real_close(device->pipe[1]) == 0);
			device->pipe[1] = -1;
			break;
		}
	}
	return __real_close(fd);
}

static void set_bit(unsigned long *bits, unsigned int code)
{
	bits[code / BITS_PER_LONG] |= 1UL << (code % BITS_PER_LONG);
}

int __wrap_ioctl(int fd, unsigned long request, ...)
{
	struct fake_device *device = device_by_fd(fd);
	va_list arguments;
	void *buffer;
	unsigned long *bits;

	va_start(arguments, request);
	if (request == EVIOCGRAB) {
		int grab = va_arg(arguments, int);

		va_end(arguments);
		if (grab && (device->busy || device->permanently_busy)) {
			if (device->busy)
				--device->busy;
			errno = EBUSY;
			return -1;
		}
		device->grabbed = grab != 0;
		return 0;
	}
	buffer = va_arg(arguments, void *);
	va_end(arguments);
	memset(buffer, 0, _IOC_SIZE(request));
	if (_IOC_NR(request) == _IOC_NR(EVIOCGPHYS(1))) {
		snprintf(buffer, _IOC_SIZE(request), "%s", device->phys);
		return 0;
	}
	bits = buffer;
	if (_IOC_NR(request) == _IOC_NR(EVIOCGBIT(0, 1))) {
		set_bit(bits, EV_KEY);
		if (device->pointer)
			set_bit(bits, EV_REL);
	} else if (_IOC_NR(request) == _IOC_NR(EVIOCGBIT(EV_KEY, 1))) {
		set_bit(bits, KEY_F13);
		if (device->keyboard)
			set_bit(bits, KEY_ENTER);
		if (device->pointer)
			set_bit(bits, BTN_LEFT);
	} else if (_IOC_NR(request) == _IOC_NR(EVIOCGBIT(EV_REL, 1))) {
		assert(device->pointer);
		set_bit(bits, REL_X);
		set_bit(bits, REL_Y);
	} else {
		assert(false);
	}
	return 0;
}

int __wrap_nanosleep(const struct timespec *requested,
		     struct timespec *remaining)
{
	/* The caller's process deadline bounds retries with any sleep strategy. */
	return __real_nanosleep(requested, remaining);
}

struct udev *udev_new(void)
{
	fake_udev.live = true;
	return &fake_udev;
}

struct udev *udev_unref(struct udev *context)
{
	context->live = false;
	return NULL;
}

struct udev_monitor *udev_monitor_new_from_netlink(struct udev *context,
						   const char *name)
{
	assert(context->live && !strcmp(name, "udev"));
	memset(&fake_monitor, 0, sizeof(fake_monitor));
	assert(pipe2(fake_monitor.pipe, O_CLOEXEC | O_NONBLOCK) == 0);
	return &fake_monitor;
}

struct udev_monitor *udev_monitor_unref(struct udev_monitor *monitor)
{
	assert(__real_close(monitor->pipe[0]) == 0);
	assert(__real_close(monitor->pipe[1]) == 0);
	return NULL;
}

int udev_monitor_filter_add_match_subsystem_devtype(struct udev_monitor *monitor,
						    const char *subsystem,
						    const char *devtype)
{
	assert(monitor == &fake_monitor && !strcmp(subsystem, "input") &&
	       !devtype);
	return 0;
}

int udev_monitor_enable_receiving(struct udev_monitor *monitor)
{
	assert(monitor == &fake_monitor);
	return 0;
}

int udev_monitor_get_fd(struct udev_monitor *monitor)
{
	return monitor->pipe[0];
}

struct udev_device *udev_monitor_receive_device(struct udev_monitor *monitor)
{
	char ready;

	if (read(monitor->pipe[0], &ready, 1) < 0) {
		assert(errno == EAGAIN);
		return NULL;
	}
	assert(monitor->head < monitor->tail);
	return &monitor->notifications[monitor->head++];
}

struct udev_enumerate *udev_enumerate_new(struct udev *context)
{
	assert(context->live);
	memset(&fake_scan, 0, sizeof(fake_scan));
	return &fake_scan;
}

struct udev_enumerate *udev_enumerate_unref(struct udev_enumerate *scan)
{
	assert(scan == &fake_scan);
	return NULL;
}

int udev_enumerate_add_match_subsystem(struct udev_enumerate *scan,
				       const char *subsystem)
{
	assert(scan == &fake_scan && !strcmp(subsystem, "input"));
	return 0;
}

int udev_enumerate_scan_devices(struct udev_enumerate *scan)
{
	unsigned int i;

	if (scan_fails)
		return -EIO;
	for (i = 0; i < DEVICE_COUNT; ++i) {
		struct udev_list_entry *entry;

		if (!devices[i].present)
			continue;
		entry = &scan->entries[scan->count++];
		entry->index = i;
		if (scan->count > 1)
			entry[-1].next = entry;
	}
	return 0;
}

struct udev_list_entry *
udev_enumerate_get_list_entry(struct udev_enumerate *scan)
{
	return scan->count ? scan->entries : NULL;
}

struct udev_list_entry *udev_list_entry_get_next(struct udev_list_entry *entry)
{
	return entry->next;
}

const char *udev_list_entry_get_name(struct udev_list_entry *entry)
{
	return devices[entry->index].path;
}

struct udev_device *udev_device_new_from_syspath(struct udev *context,
						 const char *syspath)
{
	unsigned int i;

	assert(context->live);
	for (i = 0; i < DEVICE_COUNT; ++i)
		if (devices[i].present && !strcmp(devices[i].path, syspath))
			return &devices[i].udev;
	return NULL;
}

struct udev_device *udev_device_unref(struct udev_device *device)
{
	(void)device;
	return NULL;
}

const char *udev_device_get_devnode(struct udev_device *device)
{
	return devices[device->index].path;
}

const char *udev_device_get_sysname(struct udev_device *device)
{
	return strrchr(devices[device->index].path, '/') + 1;
}

const char *udev_device_get_action(struct udev_device *device)
{
	return device->action;
}

const char *udev_device_get_property_value(struct udev_device *device,
					   const char *key)
{
	(void)device;
	assert(!strcmp(key, "ID_SEAT"));
	return NULL;
}

int libevdev_new_from_fd(int fd, struct libevdev **device)
{
	struct fake_device *fake = device_by_fd(fd);
	unsigned int index = (unsigned int)(fake - devices);

	memset(&fake->evdev, 0, sizeof(fake->evdev));
	fake->evdev.index = index;
	fake->evdev.live = true;
	*device = &fake->evdev;
	return 0;
}

void libevdev_free(struct libevdev *device)
{
	assert(device->live);
	device->live = false;
}

int libevdev_set_clock_id(struct libevdev *device, int clock_id)
{
	assert(device->live && clock_id == CLOCK_MONOTONIC);
	return 0;
}

const char *libevdev_get_name(const struct libevdev *device)
{
	return devices[device->index].phys;
}

int libevdev_has_event_code(const struct libevdev *device, unsigned int type,
			    unsigned int code)
{
	return devices[device->index].pointer &&
	       ((type == EV_REL && (code == REL_X || code == REL_Y)) ||
		(type == EV_KEY && code == BTN_LEFT));
}

int libevdev_next_event(struct libevdev *device, unsigned int flags,
			struct input_event *event)
{
	struct queued_event *queued;
	char ready[EVENT_COUNT];
	ssize_t result;

	assert(device->live);
	/* Model library buffering that leaves the kernel fd no longer readable. */
	do {
		result = read(devices[device->index].pipe[0], ready,
			      sizeof(ready));
	} while (result > 0);
	assert(result < 0 && errno == EAGAIN);
	if (device->head == device->tail)
		return -EAGAIN;
	queued = &device->events[device->head++];
	assert(flags == queued->flags);
	*event = queued->event;
	if (event->type == EV_KEY && event->code < KEY_CNT &&
	    (event->value == 0 || event->value == 1))
		device->pressed[event->code] = event->value != 0;
	return queued->result;
}

int libevdev_get_event_value(const struct libevdev *device, unsigned int type,
			     unsigned int code)
{
	assert(device->live && type == EV_KEY && code < KEY_CNT);
	return device->pressed[code];
}

static void hotplug(unsigned int index, bool present)
{
	struct udev_device *notification;
	char ready = 1;

	assert(fake_monitor.tail < EVENT_COUNT);
	devices[index].present = present;
	notification = &fake_monitor.notifications[fake_monitor.tail++];
	notification->index = index;
	notification->action = present ? "add" : "remove";
	assert(write(fake_monitor.pipe[1], &ready, 1) == 1);
}

static void raw_event(unsigned int index, unsigned int type, unsigned int code,
		      int value, int result, unsigned int flags,
		      uint64_t time_us)
{
	struct libevdev *device = &devices[index].evdev;
	struct queued_event *queued;
	char ready = 1;

	assert(device->live);
	if (device->head == device->tail)
		device->head = device->tail = 0;
	assert(device->tail < EVENT_COUNT);
	queued = &device->events[device->tail++];
	memset(queued, 0, sizeof(*queued));
	queued->event.type = type;
	queued->event.code = code;
	queued->event.value = value;
	queued->event.input_event_sec = time_us / 1000000U;
	queued->event.input_event_usec = time_us % 1000000U;
	queued->result = result;
	queued->flags = flags;
	assert(write(devices[index].pipe[1], &ready, 1) == 1);
}

static void input(unsigned int index, unsigned int type, unsigned int code,
		  int value)
{
	raw_event(index, type, code, value, 0, 2, 0);
}

static void frame(unsigned int index)
{
	input(index, EV_SYN, SYN_REPORT, 0);
}

static struct fplinux_input_event next(struct fplinux_input_session *session,
				       enum fplinux_input_event_type type,
				       enum fplinux_input_source source,
				       unsigned int code, bool pressed)
{
	struct fplinux_input_event event;

	assert(fplinux_input_session_next(session, &event));
	assert(event.type == type && event.source == source);
	assert(event.code == code && event.pressed == pressed);
	assert(event.device_id != 0);
	return event;
}

static void empty(struct fplinux_input_session *session)
{
	struct fplinux_input_event event;
	struct pollfd ready = { .fd = fplinux_input_session_get_fd(session),
				.events = POLLIN };

	assert(!fplinux_input_session_next(session, &event));
	assert(poll(&ready, 1, 0) == 0);
}

static void open_session(struct fplinux_input_session *session,
			 unsigned int mask)
{
	char error[128];
	struct pollfd ready;

	assert(fplinux_input_session_open(session, mask, error, sizeof(error)));
	ready = (struct pollfd){ .fd = fplinux_input_session_get_fd(session),
				 .events = POLLIN };
	assert(ready.fd >= 0);
	assert(poll(&ready, 1, 0) == 1);
}

static void closed(struct fplinux_input_session *session)
{
	struct fplinux_input_event event;
	unsigned int i;

	fplinux_input_session_close(session);
	assert(!fake_udev.live);
	assert(fplinux_input_session_get_fd(session) == -1);
	assert(!fplinux_input_session_next(session, &event));
	for (i = 0; i < DEVICE_COUNT; ++i)
		assert(!devices[i].grabbed && devices[i].pipe[0] < 0 &&
		       !devices[i].evdev.live);
}

static void lifecycle(void)
{
	struct fplinux_input_session session;
	struct fplinux_input_event event;
	uint64_t phone_id, keyboard_id, replacement_id;
	char error[128];

	devices[0].present = devices[1].present = true;
	open_session(&session, 3U);
	phone_id = next(&session, FPLINUX_INPUT_EVENT_DEVICE_ADDED, 0, 0, false)
			   .device_id;
	keyboard_id =
		next(&session, FPLINUX_INPUT_EVENT_DEVICE_ADDED, 1, 0, false)
			.device_id;
	assert(phone_id != keyboard_id);
	/* Literal F13 stays source-specific and preserves its monotonic timestamp. */
	raw_event(0, EV_KEY, 183, 1, 0, 2, 1234999);
	frame(0);
	event = next(&session, FPLINUX_INPUT_EVENT_KEY, 0, 183, true);
	assert(event.device_id == phone_id && event.time_ms == 1234);
	empty(&session);
	raw_event(1, EV_KEY, 183, 1, 0, 2, 923456);
	frame(1);
	event = next(&session, FPLINUX_INPUT_EVENT_KEY, 1, 183, true);
	assert(event.device_id == keyboard_id && event.time_ms == 923);
	empty(&session);
	hotplug(0, false);
	hotplug(4, true);
	assert(next(&session, FPLINUX_INPUT_EVENT_KEY, 0, 183, false)
		       .device_id == phone_id);
	assert(next(&session, FPLINUX_INPUT_EVENT_DEVICE_REMOVED, 0, 0, false)
		       .device_id == phone_id);
	replacement_id =
		next(&session, FPLINUX_INPUT_EVENT_DEVICE_ADDED, 1, 0, false)
			.device_id;
	assert(replacement_id != phone_id && replacement_id != keyboard_id);
	hotplug(4, false);
	next(&session, FPLINUX_INPUT_EVENT_DEVICE_REMOVED, 1, 0, false);
	fplinux_input_session_suspend(&session);
	assert(!devices[1].grabbed && devices[1].pipe[0] < 0);
	assert(next(&session, FPLINUX_INPUT_EVENT_KEY, 1, 183, false)
		       .device_id == keyboard_id);
	next(&session, FPLINUX_INPUT_EVENT_DEVICE_REMOVED, 1, 0, false);
	empty(&session);
	/* Resume scans current devices and ignores obsolete queued notifications. */
	hotplug(2, true);
	hotplug(2, false);
	assert(fplinux_input_session_resume(&session, error, sizeof(error)));
	assert(next(&session, FPLINUX_INPUT_EVENT_DEVICE_ADDED, 1, 0, false)
		       .device_id != keyboard_id);
	empty(&session);
	closed(&session);
}

static void classification(void)
{
	struct fplinux_input_session session;
	unsigned int i;

	for (i = 0; i < DEVICE_COUNT; ++i)
		devices[i].present = i != 4;
	open_session(&session, 7U);
	next(&session, FPLINUX_INPUT_EVENT_DEVICE_ADDED, 0, 0, false);
	next(&session, FPLINUX_INPUT_EVENT_DEVICE_ADDED, 1, 0, false);
	next(&session, FPLINUX_INPUT_EVENT_DEVICE_ADDED, 1, 0, false);
	next(&session, FPLINUX_INPUT_EVENT_DEVICE_ADDED, 2, 0, false);
	assert(!devices[5].grabbed);
	empty(&session);
	input(2, EV_KEY, 183, 1);
	frame(2);
	next(&session, FPLINUX_INPUT_EVENT_KEY, 1, 183, true);
	input(2, EV_REL, REL_X, 2);
	frame(2);
	assert(next(&session, FPLINUX_INPUT_EVENT_MOTION, 1, 0, false).dx == 2);
	empty(&session);
	closed(&session);
	open_session(&session, 3U);
	next(&session, FPLINUX_INPUT_EVENT_DEVICE_ADDED, 0, 0, false);
	next(&session, FPLINUX_INPUT_EVENT_DEVICE_ADDED, 1, 0, false);
	next(&session, FPLINUX_INPUT_EVENT_DEVICE_ADDED, 1, 0, false);
	assert(!devices[3].grabbed);
	empty(&session);
	closed(&session);
}

static void modifiers_and_repeat(void)
{
	struct fplinux_input_session session;
	struct fplinux_input_event releases[2];
	uint64_t first, second;
	struct pollfd ready;
	unsigned int i;

	devices[1].present = devices[2].present = true;
	open_session(&session, 2U);
	first = next(&session, FPLINUX_INPUT_EVENT_DEVICE_ADDED, 1, 0, false)
			.device_id;
	second = next(&session, FPLINUX_INPUT_EVENT_DEVICE_ADDED, 1, 0, false)
			 .device_id;
	input(1, EV_KEY, 42, 1);
	input(1, EV_KEY, 42, 2);
	input(1, EV_KEY, 29, 1);
	frame(1);
	assert(next(&session, FPLINUX_INPUT_EVENT_KEY, 1, 42, true).device_id ==
	       first);
	ready = (struct pollfd){ .fd = fplinux_input_session_get_fd(&session),
				 .events = POLLIN };
	assert(poll(&ready, 1, 0) == 1);
	next(&session, FPLINUX_INPUT_EVENT_KEY, 1, 29, true);
	empty(&session);
	input(2, EV_KEY, 42, 1);
	input(2, EV_KEY, 29, 1);
	frame(2);
	assert(next(&session, FPLINUX_INPUT_EVENT_KEY, 1, 42, true).device_id ==
	       second);
	next(&session, FPLINUX_INPUT_EVENT_KEY, 1, 29, true);
	empty(&session);
	hotplug(1, false);
	assert(fplinux_input_session_next(&session, &releases[0]));
	assert(fplinux_input_session_next(&session, &releases[1]));
	for (i = 0; i < 2; ++i) {
		assert(releases[i].type == FPLINUX_INPUT_EVENT_KEY &&
		       releases[i].source == FPLINUX_INPUT_SOURCE_KEYBOARD &&
		       !releases[i].pressed && releases[i].device_id == first);
		assert(releases[i].code == 29 || releases[i].code == 42);
	}
	assert(releases[0].code != releases[1].code);
	assert(next(&session, FPLINUX_INPUT_EVENT_DEVICE_REMOVED, 1, 0, false)
		       .device_id == first);
	empty(&session);
	input(2, EV_KEY, 42, 0);
	input(2, EV_KEY, 29, 0);
	frame(2);
	assert(next(&session, FPLINUX_INPUT_EVENT_KEY, 1, 42, false).device_id ==
	       second);
	assert(next(&session, FPLINUX_INPUT_EVENT_KEY, 1, 29, false).device_id ==
	       second);
	empty(&session);
	closed(&session);
}

static void keyboard_frames(void)
{
	struct fplinux_input_session session;
	uint64_t first, second;

	devices[1].present = devices[2].present = true;
	open_session(&session, 2U);
	first = next(&session, FPLINUX_INPUT_EVENT_DEVICE_ADDED, 1, 0, false)
			.device_id;
	second = next(&session, FPLINUX_INPUT_EVENT_DEVICE_ADDED, 1, 0, false)
			 .device_id;
	/* Partial frames stay attached to their device while another one reports. */
	input(1, EV_KEY, 30, 1);
	empty(&session);
	input(2, EV_KEY, 48, 1);
	frame(2);
	assert(next(&session, FPLINUX_INPUT_EVENT_KEY, 1, 48, true).device_id ==
	       second);
	empty(&session);
	frame(1);
	assert(next(&session, FPLINUX_INPUT_EVENT_KEY, 1, 30, true).device_id ==
	       first);
	empty(&session);
	input(1, EV_KEY, 30, 0);
	input(1, EV_KEY, 30, 1);
	frame(1);
	next(&session, FPLINUX_INPUT_EVENT_KEY, 1, 30, false);
	next(&session, FPLINUX_INPUT_EVENT_KEY, 1, 30, true);
	empty(&session);
	input(1, EV_KEY, 46, 1);
	empty(&session);
	input(2, EV_KEY, 48, 0);
	frame(2);
	assert(next(&session, FPLINUX_INPUT_EVENT_KEY, 1, 48, false).device_id ==
	       second);
	empty(&session);
	/* C is still held: there is no library delta for its uncommitted press. */
	raw_event(1, EV_SYN, SYN_DROPPED, 0, 1, 2, 3000000);
	raw_event(1, EV_KEY, 30, 0, 1, 1, 3000000);
	raw_event(1, EV_SYN, SYN_REPORT, 0, 1, 1, 3000000);
	raw_event(1, 0, 0, 0, -EAGAIN, 1, 0);
	next(&session, FPLINUX_INPUT_EVENT_KEY, 1, 30, false);
	assert(next(&session, FPLINUX_INPUT_EVENT_KEY, 1, 46, true).time_ms ==
	       3000);
	empty(&session);
	/* A key that pressed and released inside the lost frame never reaches us. */
	input(1, EV_KEY, 32, 1);
	raw_event(1, EV_SYN, SYN_DROPPED, 0, 1, 2, 3001000);
	raw_event(1, EV_KEY, 32, 0, 1, 1, 3001000);
	raw_event(1, EV_SYN, SYN_REPORT, 0, 1, 1, 3001000);
	raw_event(1, 0, 0, 0, -EAGAIN, 1, 0);
	empty(&session);
	fplinux_input_session_suspend(&session);
	next(&session, FPLINUX_INPUT_EVENT_KEY, 1, 46, false);
	next(&session, FPLINUX_INPUT_EVENT_DEVICE_REMOVED, 1, 0, false);
	next(&session, FPLINUX_INPUT_EVENT_DEVICE_REMOVED, 1, 0, false);
	empty(&session);
	closed(&session);
}

static void wheel(struct fplinux_input_session *session, int legacy, int high,
		  bool has_high, int expected)
{
	if (legacy)
		input(3, EV_REL, REL_WHEEL, legacy);
	if (has_high)
		input(3, EV_REL, REL_WHEEL_HI_RES, high);
	frame(3);
	if (expected)
		assert(next(session, FPLINUX_INPUT_EVENT_WHEEL, 2, 0, false)
			       .wheel_clicks == expected);
	empty(session);
}

static void pointer_frames_and_wheel(void)
{
	struct fplinux_input_session session;
	struct fplinux_input_event event;
	uint64_t pointer_id;

	devices[3].present = true;
	open_session(&session, 4U);
	pointer_id =
		next(&session, FPLINUX_INPUT_EVENT_DEVICE_ADDED, 2, 0, false)
			.device_id;
	input(3, EV_REL, REL_X, 2);
	input(3, EV_REL, REL_X, 3);
	input(3, EV_REL, REL_Y, -4);
	empty(&session);
	frame(3);
	event = next(&session, FPLINUX_INPUT_EVENT_MOTION, 2, 0, false);
	assert(event.dx == 5 && event.dy == -4 &&
	       event.device_id == pointer_id);
	empty(&session);
	wheel(&session, 1, 0, false, 1);
	wheel(&session, -2, 0, false, -2);
	wheel(&session, 0, 30, true, 0);
	wheel(&session, 0, 30, true, 0);
	wheel(&session, 1, 60, true, 1);
	wheel(&session, -1, -30, true, 0);
	wheel(&session, -1, -90, true, -1);
	wheel(&session, 0, 60, true, 0);
	wheel(&session, -1, -120, true, 0);
	wheel(&session, 0, -60, true, -1);
	wheel(&session, 1, 0, true, 0);
	input(3, EV_KEY, 272, 1);
	frame(3);
	next(&session, FPLINUX_INPUT_EVENT_BUTTON, 2, 272, true);
	empty(&session);
	hotplug(3, false);
	assert(next(&session, FPLINUX_INPUT_EVENT_BUTTON, 2, 272, false)
		       .device_id == pointer_id);
	next(&session, FPLINUX_INPUT_EVENT_DEVICE_REMOVED, 2, 0, false);
	empty(&session);
	closed(&session);
}

static void synchronization(void)
{
	struct fplinux_input_session session;
	struct fplinux_input_event event;

	devices[2].present = true;
	open_session(&session, 2U);
	next(&session, FPLINUX_INPUT_EVENT_DEVICE_ADDED, 1, 0, false);
	input(2, EV_KEY, 42, 1);
	input(2, EV_KEY, 272, 1);
	frame(2);
	next(&session, FPLINUX_INPUT_EVENT_KEY, 1, 42, true);
	next(&session, FPLINUX_INPUT_EVENT_BUTTON, 1, 272, true);
	empty(&session);
	input(2, EV_REL, REL_X, 999);
	input(2, EV_REL, REL_WHEEL, 8);
	empty(&session);
	/* Library-generated deltas release held state and press a newly held Ctrl. */
	raw_event(2, EV_SYN, SYN_DROPPED, 0, 1, 2, 0);
	raw_event(2, EV_KEY, 42, 0, 1, 1, 2000000);
	raw_event(2, EV_KEY, 272, 0, 1, 1, 0);
	raw_event(2, EV_KEY, 29, 1, 1, 1, 2000000);
	raw_event(2, EV_KEY, 273, 1, 1, 1, 0);
	raw_event(2, EV_SYN, SYN_REPORT, 0, 1, 1, 2000000);
	raw_event(2, 0, 0, 0, -EAGAIN, 1, 0);
	input(2, EV_REL, REL_X, 4);
	frame(2);
	next(&session, FPLINUX_INPUT_EVENT_KEY, 1, 29, true);
	assert(next(&session, FPLINUX_INPUT_EVENT_KEY, 1, 42, false).time_ms ==
	       2000);
	next(&session, FPLINUX_INPUT_EVENT_BUTTON, 1, 272, false);
	next(&session, FPLINUX_INPUT_EVENT_BUTTON, 1, 273, true);
	event = next(&session, FPLINUX_INPUT_EVENT_MOTION, 1, 0, false);
	assert(event.dx == 4 && event.dy == 0);
	empty(&session);
	fplinux_input_session_suspend(&session);
	next(&session, FPLINUX_INPUT_EVENT_KEY, 1, 29, false);
	next(&session, FPLINUX_INPUT_EVENT_BUTTON, 1, 273, false);
	next(&session, FPLINUX_INPUT_EVENT_DEVICE_REMOVED, 1, 0, false);
	empty(&session);
	closed(&session);
}

static void grab_retry_and_failed_scan(void)
{
	struct fplinux_input_session session = { 0 };
	struct fplinux_input_event event;
	char error[128];

	assert(fplinux_input_session_get_fd(&session) == -1);
	assert(!fplinux_input_session_next(&session, &event));
	assert(!fplinux_input_session_resume(&session, error, sizeof(error)));
	assert(!strcmp(error, "input session is closed"));
	fplinux_input_session_suspend(&session);
	fplinux_input_session_close(&session);
	devices[1].present = true;
	devices[1].busy = 2;
	open_session(&session, 2U);
	assert(devices[1].grabbed);
	next(&session, FPLINUX_INPUT_EVENT_DEVICE_ADDED, 1, 0, false);
	empty(&session);
	closed(&session);
	scan_fails = true;
	assert(!fplinux_input_session_open(&session, 2U, error, sizeof(error)));
	assert(!strcmp(error, "cannot scan input devices"));
	closed(&session);
	scan_fails = false;
	devices[1].permanently_busy = true;
	assert(fplinux_input_session_open(&session, 2U, error, sizeof(error)));
	assert(!devices[1].grabbed && devices[1].pipe[0] < 0 &&
	       !devices[1].evdev.live);
	empty(&session);
	closed(&session);
}

int main(int argc, char **argv)
{
	assert(argc == 2);
	initialize();
	if (!strcmp(argv[1], "lifecycle"))
		lifecycle();
	else if (!strcmp(argv[1], "classification"))
		classification();
	else if (!strcmp(argv[1], "modifiers"))
		modifiers_and_repeat();
	else if (!strcmp(argv[1], "frames"))
		keyboard_frames();
	else if (!strcmp(argv[1], "pointer"))
		pointer_frames_and_wheel();
	else if (!strcmp(argv[1], "sync"))
		synchronization();
	else if (!strcmp(argv[1], "retry"))
		grab_retry_and_failed_scan();
	else
		assert(false);
	return 0;
}
