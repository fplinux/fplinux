/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
/* Fake evdev, udev and libinput boundaries around the production session. */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <libinput.h>
#include <libudev.h>
#include <linux/input.h>
#include <linux/ioctl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>

#include "fplinux-input-session.h"
#include "fplinux-keypad.h"

#define DEVICE_COUNT 4U
#define EVENT_COUNT 64U
#define BITS_PER_LONG (8U * sizeof(unsigned long))

struct udev {
	bool live;
};

struct libinput_device {
	const char *sysname;
	const char *phys;
	void *data;
	unsigned int held;
	int fd;
	bool grabbed;
};

struct libinput_event_keyboard {
	unsigned int code;
	enum libinput_key_state state;
	uint64_t time_usec;
};

struct libinput_event_pointer {
	int unused;
};

struct libinput_event {
	enum libinput_event_type type;
	struct libinput_device *device;
	struct libinput_event_keyboard keyboard;
};

struct libinput {
	const struct libinput_interface *interface;
	void *data;
	struct libinput_event events[EVENT_COUNT];
	size_t head;
	size_t tail;
};

static struct udev fake_udev;
static struct libinput fake_input;
static struct libinput_device devices[DEVICE_COUNT] = {
	{ .sysname = "event0", .phys = "fplinux/keypad0", .fd = -1 },
	{ .sysname = "event1", .phys = "usb/keyboard", .fd = -1 },
	{ .sysname = "event2", .phys = "bluetooth/keyboard", .fd = -1 },
	{ .sysname = "event0", .phys = "usb/replacement", .fd = -1 },
};
static unsigned int opening_device;

static void queue_event(struct libinput_device *device,
			enum libinput_event_type type, unsigned int code,
			bool pressed)
{
	struct libinput_event *event;

	assert(fake_input.tail < EVENT_COUNT);
	event = &fake_input.events[fake_input.tail++];
	event->type = type;
	event->device = device;
	event->keyboard.code = code;
	event->keyboard.state = pressed ? LIBINPUT_KEY_STATE_PRESSED :
					  LIBINPUT_KEY_STATE_RELEASED;
}

static void add_device(unsigned int index)
{
	struct libinput_device *device = &devices[index];
	char path[64];

	opening_device = index;
	snprintf(path, sizeof(path), "/dev/input/%s", device->sysname);
	device->fd = fake_input.interface->open_restricted(
		path, O_RDONLY | O_NONBLOCK, fake_input.data);
	assert(device->fd >= 0);
	assert(device->grabbed);
	queue_event(device, LIBINPUT_EVENT_DEVICE_ADDED, 0, false);
}

static void key(unsigned int index, unsigned int code, bool pressed)
{
	devices[index].held = pressed ? code : 0;
	queue_event(&devices[index], LIBINPUT_EVENT_KEYBOARD_KEY, code,
		    pressed);
}

static void remove_device(unsigned int index)
{
	struct libinput_device *device = &devices[index];

	if (device->held)
		key(index, device->held, false);
	fake_input.interface->close_restricted(device->fd, fake_input.data);
	device->fd = -1;
	assert(!device->grabbed);
	queue_event(device, LIBINPUT_EVENT_DEVICE_REMOVED, 0, false);
}

int __wrap_open(const char *path, int flags, ...)
{
	(void)path;
	assert(flags & O_CLOEXEC);
	return 100 + (int)opening_device;
}

int __wrap_close(int fd)
{
	assert(fd >= 100 && fd < 100 + (int)DEVICE_COUNT);
	return 0;
}

int __wrap_ioctl(int fd, unsigned long request, ...)
{
	struct libinput_device *device = &devices[fd - 100];
	va_list arguments;
	void *buffer;
	unsigned long *bits;

	va_start(arguments, request);
	if (request == EVIOCGRAB) {
		device->grabbed = va_arg(arguments, int) != 0;
		va_end(arguments);
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
	if (_IOC_NR(request) == _IOC_NR(EVIOCGBIT(0, 1)))
		bits[EV_KEY / BITS_PER_LONG] |= 1UL << (EV_KEY % BITS_PER_LONG);
	else if (_IOC_NR(request) == _IOC_NR(EVIOCGBIT(EV_KEY, 1)))
		bits[KEY_ENTER / BITS_PER_LONG] |=
			1UL << (KEY_ENTER % BITS_PER_LONG);
	else
		assert(false);
	return 0;
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

struct libinput *
libinput_udev_create_context(const struct libinput_interface *interface,
			     void *data, struct udev *udev)
{
	assert(udev->live);
	memset(&fake_input, 0, sizeof(fake_input));
	fake_input.interface = interface;
	fake_input.data = data;
	return &fake_input;
}

int libinput_udev_assign_seat(struct libinput *context, const char *seat)
{
	assert(context == &fake_input && !strcmp(seat, "seat0"));
	return 0;
}

void libinput_suspend(struct libinput *context)
{
	unsigned int i;

	assert(context == &fake_input);
	for (i = 0; i < DEVICE_COUNT; ++i)
		if (devices[i].fd >= 0)
			remove_device(i);
}

int libinput_resume(struct libinput *context)
{
	assert(context == &fake_input);
	add_device(1);
	return 0;
}

struct libinput *libinput_unref(struct libinput *context)
{
	libinput_suspend(context);
	return NULL;
}

int libinput_dispatch(struct libinput *context)
{
	assert(context == &fake_input);
	return 0;
}

int libinput_get_fd(struct libinput *context)
{
	assert(context == &fake_input);
	return 23;
}

struct libinput_event *libinput_get_event(struct libinput *context)
{
	if (context->head == context->tail) {
		context->head = 0;
		context->tail = 0;
		return NULL;
	}
	return &context->events[context->head++];
}

void libinput_event_destroy(struct libinput_event *event)
{
	(void)event;
}

struct libinput_device *libinput_event_get_device(struct libinput_event *event)
{
	return event->device;
}

enum libinput_event_type libinput_event_get_type(struct libinput_event *event)
{
	return event->type;
}

void *libinput_device_get_user_data(struct libinput_device *device)
{
	return device->data;
}

void libinput_device_set_user_data(struct libinput_device *device, void *data)
{
	device->data = data;
}

const char *libinput_device_get_name(struct libinput_device *device)
{
	return device->phys;
}

const char *libinput_device_get_sysname(struct libinput_device *device)
{
	return device->sysname;
}

struct libinput_event_keyboard *
libinput_event_get_keyboard_event(struct libinput_event *event)
{
	return &event->keyboard;
}

uint32_t libinput_event_keyboard_get_key(struct libinput_event_keyboard *event)
{
	return event->code;
}

uint64_t
libinput_event_keyboard_get_time_usec(struct libinput_event_keyboard *event)
{
	return event->time_usec;
}

enum libinput_key_state
libinput_event_keyboard_get_key_state(struct libinput_event_keyboard *event)
{
	return event->state;
}

/* No pointer events are queued in these keyboard lifecycle scenarios. */
struct libinput_event_pointer *
libinput_event_get_pointer_event(struct libinput_event *event)
{
	(void)event;
	assert(false);
	return NULL;
}

uint32_t libinput_event_pointer_get_button(struct libinput_event_pointer *event)
{
	(void)event;
	return 0;
}

enum libinput_button_state
libinput_event_pointer_get_button_state(struct libinput_event_pointer *event)
{
	(void)event;
	return LIBINPUT_BUTTON_STATE_RELEASED;
}

double libinput_event_pointer_get_dx_unaccelerated(
	struct libinput_event_pointer *event)
{
	(void)event;
	return 0;
}

double libinput_event_pointer_get_dy_unaccelerated(
	struct libinput_event_pointer *event)
{
	(void)event;
	return 0;
}

int libinput_event_pointer_has_axis(struct libinput_event_pointer *event,
				    enum libinput_pointer_axis axis)
{
	(void)event;
	(void)axis;
	return 0;
}

double libinput_event_pointer_get_scroll_value_v120(
	struct libinput_event_pointer *event, enum libinput_pointer_axis axis)
{
	(void)event;
	(void)axis;
	return 0;
}

static struct fplinux_input_event next(struct fplinux_input_session *session,
				       enum fplinux_input_event_type type,
				       enum fplinux_input_source source,
				       unsigned int code, bool pressed)
{
	struct fplinux_input_event event;

	assert(fplinux_input_session_next(session, &event));
	assert(event.type == type);
	assert(event.source == source);
	assert(event.code == code && event.pressed == pressed);
	assert(event.device_id != 0);
	return event;
}

int main(void)
{
	struct fplinux_input_session session;
	struct fplinux_input_event event;
	uint64_t phone_id, keyboard_id, replacement_id;
	char error[128];

	assert(fplinux_input_session_open(&session, 3U, error, sizeof(error)));
	assert(fplinux_input_session_get_fd(&session) == 23);
	add_device(0);
	add_device(1);
	phone_id = next(&session, FPLINUX_INPUT_EVENT_DEVICE_ADDED,
			FPLINUX_INPUT_SOURCE_KEYPAD, 0, false)
			   .device_id;
	keyboard_id = next(&session, FPLINUX_INPUT_EVENT_DEVICE_ADDED,
			   FPLINUX_INPUT_SOURCE_KEYBOARD, 0, false)
			      .device_id;
	assert(phone_id != keyboard_id);
	/* Identical Linux codes preserve their independently classified source. */
	key(0, 183, true);
	fake_input.events[fake_input.tail - 1].keyboard.time_usec = 1234999;
	key(1, 183, true);
	fake_input.events[fake_input.tail - 1].keyboard.time_usec = 923456;
	event = next(&session, FPLINUX_INPUT_EVENT_KEY,
		     FPLINUX_INPUT_SOURCE_KEYPAD, FPLINUX_KEY_SOFT_LEFT, true);
	assert(event.device_id == phone_id && event.time_ms == 1234);
	event = next(&session, FPLINUX_INPUT_EVENT_KEY,
		     FPLINUX_INPUT_SOURCE_KEYBOARD, 183, true);
	assert(event.device_id == keyboard_id && event.time_ms == 923);
	/* Closing an fd must not erase the identity of its queued release. */
	remove_device(0);
	assert(next(&session, FPLINUX_INPUT_EVENT_KEY,
		    FPLINUX_INPUT_SOURCE_KEYPAD, 183, false)
		       .device_id == phone_id);
	assert(next(&session, FPLINUX_INPUT_EVENT_DEVICE_REMOVED,
		    FPLINUX_INPUT_SOURCE_KEYPAD, 0, false)
		       .device_id == phone_id);
	/* A node reused before its ADDED event is read remains distinguishable. */
	add_device(0);
	remove_device(0);
	add_device(3);
	phone_id = next(&session, FPLINUX_INPUT_EVENT_DEVICE_ADDED,
			FPLINUX_INPUT_SOURCE_KEYPAD, 0, false)
			   .device_id;
	assert(next(&session, FPLINUX_INPUT_EVENT_DEVICE_REMOVED,
		    FPLINUX_INPUT_SOURCE_KEYPAD, 0, false)
		       .device_id == phone_id);
	replacement_id = next(&session, FPLINUX_INPUT_EVENT_DEVICE_ADDED,
			      FPLINUX_INPUT_SOURCE_KEYBOARD, 0, false)
				 .device_id;
	assert(replacement_id != phone_id);
	remove_device(3);
	next(&session, FPLINUX_INPUT_EVENT_DEVICE_REMOVED,
	     FPLINUX_INPUT_SOURCE_KEYBOARD, 0, false);
	/* Suspend drains the held keyboard before resume starts a new lifetime. */
	fplinux_input_session_suspend(&session);
	assert(next(&session, FPLINUX_INPUT_EVENT_KEY,
		    FPLINUX_INPUT_SOURCE_KEYBOARD, 183, false)
		       .device_id == keyboard_id);
	assert(next(&session, FPLINUX_INPUT_EVENT_DEVICE_REMOVED,
		    FPLINUX_INPUT_SOURCE_KEYBOARD, 0, false)
		       .device_id == keyboard_id);
	assert(!fplinux_input_session_next(&session, &event));
	assert(fplinux_input_session_resume(&session, error, sizeof(error)));
	assert(next(&session, FPLINUX_INPUT_EVENT_DEVICE_ADDED,
		    FPLINUX_INPUT_SOURCE_KEYBOARD, 0, false)
		       .device_id != keyboard_id);
	fplinux_input_session_close(&session);
	assert(!fake_udev.live && !devices[1].grabbed);
	assert(fplinux_input_session_get_fd(&session) == -1);
	assert(!fplinux_input_session_next(&session, &event));
	return 0;
}
