/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "fplinux-input-session.h"

#include <errno.h>
#include <fcntl.h>
#include <libinput.h>
#include <libudev.h>
#include <linux/input.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "fplinux-input-device.h"

#define FPLINUX_INPUT_SESSION_GRAB_ATTEMPTS 20U
#define FPLINUX_INPUT_SESSION_GRAB_RETRY_NS 50000000L
#define FPLINUX_INPUT_SESSION_PHYS_BYTES 64U
#define FPLINUX_INPUT_SESSION_WHEEL_CLICK_V120 120
#define BITS_PER_LONG (8U * sizeof(unsigned long))
#define BIT_WORDS(max) ((max) / BITS_PER_LONG + 1U)

static void set_message(char *error, size_t size, const char *message)
{
	if (size)
		snprintf(error, size, "%s", message);
}

static bool bit_is_set(const unsigned long *bits, unsigned int bit)
{
	return bits[bit / BITS_PER_LONG] & (1UL << (bit % BITS_PER_LONG));
}

static bool is_phone_keypad(int fd)
{
	char phys[FPLINUX_INPUT_SESSION_PHYS_BYTES] = { 0 };

	if (ioctl(fd, EVIOCGPHYS(sizeof(phys) - 1), phys) < 0)
		return false;
	return strcmp(phys, FPLINUX_INPUT_PHONE_PHYS) == 0;
}

/*
 * Devices are classified by what they report, never by their bus, so a
 * Bluetooth, USB or virtual keyboard is the same kind of source. A device with
 * keyboard keys is a keyboard even if it also moves a pointer, and a mouse that
 * publishes a separate key-only node contributes that node as a keyboard.
 */
static bool classify_device(int fd, enum fplinux_input_source *source)
{
	unsigned long events[BIT_WORDS(EV_MAX)] = { 0 };
	unsigned long keys[BIT_WORDS(KEY_MAX)] = { 0 };
	unsigned long relative[BIT_WORDS(REL_MAX)] = { 0 };

	if (is_phone_keypad(fd)) {
		*source = FPLINUX_INPUT_SOURCE_KEYPAD;
		return true;
	}
	if (ioctl(fd, EVIOCGBIT(0, sizeof(events)), events) < 0 ||
	    !bit_is_set(events, EV_KEY) ||
	    ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys) < 0)
		return false;
	if (bit_is_set(keys, KEY_ENTER)) {
		*source = FPLINUX_INPUT_SOURCE_KEYBOARD;
		return true;
	}
	if (bit_is_set(events, EV_REL) &&
	    ioctl(fd, EVIOCGBIT(EV_REL, sizeof(relative)), relative) >= 0 &&
	    bit_is_set(relative, REL_X) && bit_is_set(relative, REL_Y) &&
	    bit_is_set(keys, BTN_LEFT)) {
		*source = FPLINUX_INPUT_SOURCE_POINTER;
		return true;
	}
	return false;
}

static int grab_device(int fd)
{
	const struct timespec retry = {
		.tv_nsec = FPLINUX_INPUT_SESSION_GRAB_RETRY_NS,
	};
	unsigned int attempt;

	for (attempt = 1;; ++attempt) {
		if (ioctl(fd, EVIOCGRAB, 1) == 0)
			return 0;
		if (errno != EBUSY ||
		    attempt == FPLINUX_INPUT_SESSION_GRAB_ATTEMPTS)
			return -errno;
		nanosleep(&retry, NULL);
	}
}

static struct fplinux_input_session_device *
find_device_by_fd(struct fplinux_input_session *session, int fd)
{
	size_t i;

	for (i = 0; i < FPLINUX_INPUT_SESSION_DEVICE_COUNT; ++i)
		if (session->devices[i].fd == fd)
			return &session->devices[i];
	return NULL;
}

static int open_restricted(const char *path, int flags, void *user_data)
{
	struct fplinux_input_session *session = user_data;
	struct fplinux_input_session_device *slot;
	enum fplinux_input_source source;
	size_t path_bytes = strlen(path) + 1;
	int result;
	int fd;

	slot = find_device_by_fd(session, -1);
	if (!slot)
		return -EMFILE;
	if (path_bytes > sizeof(slot->path))
		return -ENAMETOOLONG;
	fd = open(path, flags | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	if (!classify_device(fd, &source) ||
	    !(session->accepted_sources & FPLINUX_INPUT_SOURCE_MASK(source))) {
		close(fd);
		return -ENODEV;
	}
	result = grab_device(fd);
	if (result < 0) {
		close(fd);
		return result;
	}
	memcpy(slot->path, path, path_bytes);
	slot->fd = fd;
	slot->source = source;
	return fd;
}

static void close_restricted(int fd, void *user_data)
{
	struct fplinux_input_session *session = user_data;
	struct fplinux_input_session_device *slot =
		find_device_by_fd(session, fd);

	/* A device that has disappeared cannot be released; ENODEV is fine. */
	ioctl(fd, EVIOCGRAB, 0);
	close(fd);
	if (slot) {
		slot->path[0] = '\0';
		slot->fd = -1;
	}
}

static const struct libinput_interface session_interface = {
	.open_restricted = open_restricted,
	.close_restricted = close_restricted,
};

bool fplinux_input_session_open(struct fplinux_input_session *session,
				unsigned int accepted_sources, char *error,
				size_t error_size)
{
	size_t i;

	memset(session, 0, sizeof(*session));
	for (i = 0; i < FPLINUX_INPUT_SESSION_DEVICE_COUNT; ++i)
		session->devices[i].fd = -1;
	session->accepted_sources = accepted_sources;

	session->udev = udev_new();
	if (!session->udev) {
		set_message(error, error_size, "cannot create udev context");
		return false;
	}
	session->libinput = libinput_udev_create_context(
		&session_interface, session, session->udev);
	if (!session->libinput) {
		set_message(error, error_size, "cannot create input context");
		fplinux_input_session_close(session);
		return false;
	}
	if (libinput_udev_assign_seat(session->libinput, "seat0") < 0) {
		set_message(error, error_size, "cannot scan input devices");
		fplinux_input_session_close(session);
		return false;
	}
	return true;
}

static enum fplinux_input_source
source_for_sysname(const struct fplinux_input_session *session,
		   const char *sysname)
{
	size_t sysname_length = strlen(sysname);
	size_t i;

	for (i = 0; i < FPLINUX_INPUT_SESSION_DEVICE_COUNT; ++i) {
		const struct fplinux_input_session_device *slot =
			&session->devices[i];
		size_t path_length = strlen(slot->path);

		if (slot->fd >= 0 && path_length > sysname_length &&
		    slot->path[path_length - sysname_length - 1] == '/' &&
		    !strcmp(slot->path + path_length - sysname_length, sysname))
			return slot->source;
	}
	return FPLINUX_INPUT_SOURCE_KEYBOARD;
}

/* The source is stored off by one so that an unknown device reads as NULL. */
static enum fplinux_input_source device_source(struct libinput_device *device)
{
	uintptr_t stored = (uintptr_t)libinput_device_get_user_data(device);

	return stored ? (enum fplinux_input_source)(stored - 1U) :
			FPLINUX_INPUT_SOURCE_KEYBOARD;
}

static void describe_device(struct fplinux_input_session *session,
			    struct libinput_device *device,
			    struct fplinux_input_event *event)
{
	snprintf(session->name, sizeof(session->name), "%s",
		 libinput_device_get_name(device));
	event->name = session->name;
	event->source = device_source(device);
}

static bool translate_wheel(struct fplinux_input_session *session,
			    struct libinput_event_pointer *pointer,
			    struct fplinux_input_event *event)
{
	int clicks;

	if (!libinput_event_pointer_has_axis(
		    pointer, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL))
		return false;
	/* libinput reports scrolling toward the user as positive. */
	session->wheel_remainder -=
		(int)libinput_event_pointer_get_scroll_value_v120(
			pointer, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL);
	clicks = session->wheel_remainder /
		 FPLINUX_INPUT_SESSION_WHEEL_CLICK_V120;
	if (!clicks)
		return false;
	session->wheel_remainder -=
		clicks * FPLINUX_INPUT_SESSION_WHEEL_CLICK_V120;
	event->type = FPLINUX_INPUT_EVENT_WHEEL;
	event->wheel_clicks = clicks;
	return true;
}

static bool translate_event(struct fplinux_input_session *session,
			    struct libinput_event *raw,
			    struct fplinux_input_event *event)
{
	struct libinput_device *device = libinput_event_get_device(raw);
	struct libinput_event_keyboard *keyboard;
	struct libinput_event_pointer *pointer;

	memset(event, 0, sizeof(*event));
	switch (libinput_event_get_type(raw)) {
	case LIBINPUT_EVENT_DEVICE_ADDED:
		libinput_device_set_user_data(
			device,
			(void *)(uintptr_t)(source_for_sysname(
						    session,
						    libinput_device_get_sysname(
							    device)) +
					    1U));
		event->type = FPLINUX_INPUT_EVENT_DEVICE_ADDED;
		describe_device(session, device, event);
		return true;
	case LIBINPUT_EVENT_DEVICE_REMOVED:
		event->type = FPLINUX_INPUT_EVENT_DEVICE_REMOVED;
		describe_device(session, device, event);
		return true;
	case LIBINPUT_EVENT_KEYBOARD_KEY:
		keyboard = libinput_event_get_keyboard_event(raw);
		event->type = FPLINUX_INPUT_EVENT_KEY;
		event->source = device_source(device);
		event->code = libinput_event_keyboard_get_key(keyboard);
		event->pressed =
			libinput_event_keyboard_get_key_state(keyboard) ==
			LIBINPUT_KEY_STATE_PRESSED;
		return true;
	case LIBINPUT_EVENT_POINTER_BUTTON:
		pointer = libinput_event_get_pointer_event(raw);
		event->type = FPLINUX_INPUT_EVENT_BUTTON;
		event->source = device_source(device);
		event->code = libinput_event_pointer_get_button(pointer);
		event->pressed =
			libinput_event_pointer_get_button_state(pointer) ==
			LIBINPUT_BUTTON_STATE_PRESSED;
		return true;
	case LIBINPUT_EVENT_POINTER_MOTION:
		pointer = libinput_event_get_pointer_event(raw);
		event->type = FPLINUX_INPUT_EVENT_MOTION;
		event->source = device_source(device);
		event->dx =
			libinput_event_pointer_get_dx_unaccelerated(pointer);
		event->dy =
			libinput_event_pointer_get_dy_unaccelerated(pointer);
		return true;
	case LIBINPUT_EVENT_POINTER_SCROLL_WHEEL:
		event->source = device_source(device);
		return translate_wheel(
			session, libinput_event_get_pointer_event(raw), event);
	default:
		return false;
	}
}

bool fplinux_input_session_next(struct fplinux_input_session *session,
				struct fplinux_input_event *event)
{
	struct libinput_event *raw;

	libinput_dispatch(session->libinput);
	while ((raw = libinput_get_event(session->libinput))) {
		bool translated = translate_event(session, raw, event);

		libinput_event_destroy(raw);
		if (translated)
			return true;
	}
	return false;
}

void fplinux_input_session_close(struct fplinux_input_session *session)
{
	if (session->libinput)
		libinput_unref(session->libinput);
	if (session->udev)
		udev_unref(session->udev);
	session->libinput = NULL;
	session->udev = NULL;
}
