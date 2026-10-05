/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "fplinux-input-session.h"

#include <errno.h>
#include <fcntl.h>
#include <libevdev/libevdev.h>
#include <libudev.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "fplinux-input-device.h"

#define FPLINUX_INPUT_SESSION_GRAB_ATTEMPTS 20U
#define FPLINUX_INPUT_SESSION_GRAB_RETRY_NS 50000000L
#define FPLINUX_INPUT_SESSION_WHEEL_CLICK_V120 120

static void set_message(char *error, size_t size, const char *message)
{
	if (size)
		snprintf(error, size, "%s", message);
}

static bool has_pointer(const struct libevdev *device)
{
	return libevdev_has_event_code(device, EV_REL, REL_X) &&
	       libevdev_has_event_code(device, EV_REL, REL_Y) &&
	       libevdev_has_event_code(device, EV_KEY, BTN_LEFT);
}

static bool is_phone_keypad(const struct libevdev *device)
{
	const char *phys = libevdev_get_phys(device);

	return phys && strcmp(phys, FPLINUX_INPUT_PHONE_PHYS) == 0;
}

/* Keyboard capabilities take priority over relative pointer capabilities. */
static bool classify_device(const struct libevdev *device,
			    enum fplinux_input_source *source)
{
	if (is_phone_keypad(device)) {
		*source = FPLINUX_INPUT_SOURCE_KEYPAD;
		return true;
	}
	if (libevdev_has_event_code(device, EV_KEY, KEY_ENTER)) {
		*source = FPLINUX_INPUT_SOURCE_KEYBOARD;
		return true;
	}
	if (has_pointer(device)) {
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

static void forget_device(struct fplinux_input_session_device *slot)
{
	memset(slot, 0, sizeof(*slot));
	slot->fd = -1;
}

static void wake_session(struct fplinux_input_session *session)
{
	uint64_t ready = 1;

	while (write(session->wake_fd, &ready, sizeof(ready)) < 0 &&
	       errno == EINTR)
		;
}

static int watch_fd(struct fplinux_input_session *session, int fd)
{
	struct epoll_event event = { .events = EPOLLIN, .data.fd = fd };

	return epoll_ctl(session->epoll_fd, EPOLL_CTL_ADD, fd, &event);
}

static void release_device(struct fplinux_input_session *session,
			   struct fplinux_input_session_device *slot)
{
	if (slot->fd >= 0) {
		epoll_ctl(session->epoll_fd, EPOLL_CTL_DEL, slot->fd, NULL);
		/* A disappeared device may return ENODEV when releasing its grab. */
		ioctl(slot->fd, EVIOCGRAB, 0);
		close(slot->fd);
		slot->fd = -1;
	}
	if (slot->device) {
		libevdev_free(slot->device);
		slot->device = NULL;
	}
	free(slot->keys);
	slot->keys = NULL;
	slot->key_count = 0;
	slot->key_next = 0;
	slot->key_capacity = 0;
	slot->frame_ready = false;
	if (session->active_device == slot - session->devices)
		session->active_device = -1;
}

static void remove_device(struct fplinux_input_session *session,
			  struct fplinux_input_session_device *slot)
{
	release_device(session, slot);
	slot->removing = true;
	slot->dx = 0;
	slot->dy = 0;
	slot->motion_pending = false;
	slot->wheel_pending = 0;
	wake_session(session);
}

static void add_device(struct fplinux_input_session *session,
		       struct udev_device *device)
{
	const char *path = udev_device_get_devnode(device);
	const char *sysname = udev_device_get_sysname(device);
	const char *seat = udev_device_get_property_value(device, "ID_SEAT");
	struct fplinux_input_session_device *slot = NULL;
	struct libevdev *evdev = NULL;
	enum fplinux_input_source source;
	size_t i;
	int fd;

	if (!path || !sysname || strncmp(sysname, "event", 5) ||
	    (seat && strcmp(seat, "seat0")) || session->suspended)
		return;
	for (i = 0; i < FPLINUX_INPUT_SESSION_DEVICE_COUNT; ++i) {
		struct fplinux_input_session_device *candidate =
			&session->devices[i];

		if (candidate->fd >= 0 && !strcmp(candidate->path, path))
			return;
		if (!slot && !candidate->path[0])
			slot = candidate;
	}
	if (!slot || strlen(path) >= sizeof(slot->path))
		return;
	fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		return;
	if (libevdev_new_from_fd(fd, &evdev) < 0) {
		close(fd);
		return;
	}
	if (!classify_device(evdev, &source) ||
	    !(session->accepted_sources & FPLINUX_INPUT_SOURCE_MASK(source))) {
		libevdev_free(evdev);
		close(fd);
		return;
	}
	if (libevdev_set_clock_id(evdev, CLOCK_MONOTONIC) < 0 ||
	    grab_device(fd) < 0 || watch_fd(session, fd) < 0) {
		ioctl(fd, EVIOCGRAB, 0);
		libevdev_free(evdev);
		close(fd);
		return;
	}
	snprintf(slot->path, sizeof(slot->path), "%s", path);
	snprintf(slot->name, sizeof(slot->name), "%s",
		 libevdev_get_name(evdev));
	slot->fd = fd;
	slot->source = source;
	slot->id = ++session->next_device_id;
	slot->device = evdev;
	slot->added = true;
	slot->has_pointer = has_pointer(evdev);
	wake_session(session);
}

static bool scan_devices(struct fplinux_input_session *session)
{
	struct udev_enumerate *scan = udev_enumerate_new(session->udev);
	struct udev_list_entry *entry;
	bool scanned = false;

	if (!scan)
		return false;
	if (udev_enumerate_add_match_subsystem(scan, "input") < 0 ||
	    udev_enumerate_scan_devices(scan) < 0)
		goto done;
	for (entry = udev_enumerate_get_list_entry(scan); entry;
	     entry = udev_list_entry_get_next(entry)) {
		struct udev_device *device = udev_device_new_from_syspath(
			session->udev, udev_list_entry_get_name(entry));

		if (device) {
			add_device(session, device);
			udev_device_unref(device);
		}
	}
	scanned = true;
done:
	udev_enumerate_unref(scan);
	return scanned;
}

bool fplinux_input_session_open(struct fplinux_input_session *session,
				unsigned int accepted_sources, char *error,
				size_t error_size)
{
	size_t i;

	memset(session, 0, sizeof(*session));
	session->epoll_fd = -1;
	session->wake_fd = -1;
	session->active_device = -1;
	for (i = 0; i < FPLINUX_INPUT_SESSION_DEVICE_COUNT; ++i)
		session->devices[i].fd = -1;
	session->accepted_sources = accepted_sources;
	session->udev = udev_new();
	if (!session->udev) {
		set_message(error, error_size, "cannot create udev context");
		return false;
	}
	session->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
	session->wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	session->monitor = udev_monitor_new_from_netlink(session->udev, "udev");
	if (session->epoll_fd < 0 || session->wake_fd < 0 ||
	    !session->monitor ||
	    udev_monitor_filter_add_match_subsystem_devtype(
		    session->monitor, "input", NULL) < 0 ||
	    udev_monitor_enable_receiving(session->monitor) < 0 ||
	    watch_fd(session, session->wake_fd) < 0 ||
	    watch_fd(session, udev_monitor_get_fd(session->monitor)) < 0) {
		set_message(error, error_size, "cannot create input context");
		goto fail;
	}
	/* Monitor first, then scan; duplicate add notifications are harmless. */
	if (!scan_devices(session)) {
		set_message(error, error_size, "cannot scan input devices");
		goto fail;
	}
	return true;
fail:
	fplinux_input_session_close(session);
	return false;
}

static void describe_event(struct fplinux_input_session *session,
			   struct fplinux_input_session_device *slot,
			   struct fplinux_input_event *event)
{
	memset(event, 0, sizeof(*event));
	event->source = slot->source;
	event->device_id = slot->id;
	snprintf(session->name, sizeof(session->name), "%s", slot->name);
	event->name = session->name;
}

static bool is_pressed(const struct fplinux_input_session_device *slot,
		       unsigned int code)
{
	return slot->pressed[code / 8U] & (1U << (code % 8U));
}

static void set_pressed(struct fplinux_input_session_device *slot,
			unsigned int code, bool pressed)
{
	if (pressed)
		slot->pressed[code / 8U] |= 1U << (code % 8U);
	else
		slot->pressed[code / 8U] &= ~(1U << (code % 8U));
}

static bool is_pointer_button(unsigned int code)
{
	return code >= BTN_LEFT && code <= BTN_TASK;
}

static bool translate_key(struct fplinux_input_session *session,
			  struct fplinux_input_session_device *slot,
			  const struct input_event *raw,
			  struct fplinux_input_event *event);

static bool next_pending(struct fplinux_input_session *session,
			 struct fplinux_input_event *event)
{
	size_t i;

	for (i = 0; i < FPLINUX_INPUT_SESSION_DEVICE_COUNT; ++i) {
		struct fplinux_input_session_device *slot =
			&session->devices[i];

		if (!slot->path[0])
			continue;
		if (slot->added) {
			describe_event(session, slot, event);
			event->type = FPLINUX_INPUT_EVENT_DEVICE_ADDED;
			slot->added = false;
			return true;
		}
		if (slot->removing) {
			unsigned int code;

			describe_event(session, slot, event);
			for (code = 0; code < KEY_CNT; ++code) {
				if (!is_pressed(slot, code))
					continue;
				set_pressed(slot, code, false);
				event->type =
					is_pointer_button(code) ?
						FPLINUX_INPUT_EVENT_BUTTON :
						FPLINUX_INPUT_EVENT_KEY;
				event->code = code;
				return true;
			}
			event->type = FPLINUX_INPUT_EVENT_DEVICE_REMOVED;
			forget_device(slot);
			return true;
		}
		if (slot->frame_ready) {
			while (slot->key_next < slot->key_count)
				if (translate_key(session, slot,
						  &slot->keys[slot->key_next++],
						  event))
					return true;
			slot->key_count = 0;
			slot->key_next = 0;
			slot->frame_ready = false;
		}
		if (slot->sync_pending) {
			while (slot->sync_code < KEY_CNT) {
				struct input_event raw = {
					.type = EV_KEY,
					.code = slot->sync_code++,
				};

				raw.value = libevdev_get_event_value(
					slot->device, EV_KEY, raw.code);
				if (translate_key(session, slot, &raw, event)) {
					if (event->type ==
					    FPLINUX_INPUT_EVENT_KEY)
						event->time_ms =
							slot->sync_time_ms;
					return true;
				}
			}
			slot->sync_pending = false;
		}
		if (slot->motion_pending) {
			describe_event(session, slot, event);
			event->type = FPLINUX_INPUT_EVENT_MOTION;
			event->dx = slot->dx;
			event->dy = slot->dy;
			slot->dx = 0;
			slot->dy = 0;
			slot->motion_pending = false;
			return true;
		}
		if (slot->wheel_pending) {
			describe_event(session, slot, event);
			event->type = FPLINUX_INPUT_EVENT_WHEEL;
			event->wheel_clicks = slot->wheel_pending;
			slot->wheel_pending = 0;
			return true;
		}
	}
	return false;
}

static void finish_frame(struct fplinux_input_session *session,
			 struct fplinux_input_session_device *slot)
{
	int64_t wheel_v120 =
		slot->high_resolution_wheel ?
			slot->wheel_v120 :
			slot->wheel_legacy *
				FPLINUX_INPUT_SESSION_WHEEL_CLICK_V120;
	int64_t total = session->wheel_remainder + wheel_v120;
	int64_t clicks = total / FPLINUX_INPUT_SESSION_WHEEL_CLICK_V120;

	slot->motion_pending = slot->dx != 0 || slot->dy != 0;
	/* Positive evdev wheel values scroll away from the user. */
	if (clicks > INT_MAX)
		clicks = INT_MAX;
	else if (clicks < INT_MIN)
		clicks = INT_MIN;
	slot->wheel_pending = (int)clicks;
	session->wheel_remainder =
		(int)(total % FPLINUX_INPUT_SESSION_WHEEL_CLICK_V120);
	slot->wheel_v120 = 0;
	slot->wheel_legacy = 0;
	slot->high_resolution_wheel = false;
}

static bool translate_key(struct fplinux_input_session *session,
			  struct fplinux_input_session_device *slot,
			  const struct input_event *raw,
			  struct fplinux_input_event *event)
{
	bool button = is_pointer_button(raw->code);
	bool pressed = raw->value == 1;

	if (raw->code >= KEY_CNT || (raw->value != 0 && raw->value != 1) ||
	    (button ? !slot->has_pointer :
		      slot->source == FPLINUX_INPUT_SOURCE_POINTER) ||
	    is_pressed(slot, raw->code) == pressed)
		return false;
	set_pressed(slot, raw->code, pressed);
	describe_event(session, slot, event);
	event->type = button ? FPLINUX_INPUT_EVENT_BUTTON :
			       FPLINUX_INPUT_EVENT_KEY;
	event->code = raw->code;
	event->pressed = pressed;
	if (!button)
		event->time_ms = (uint64_t)raw->input_event_sec * 1000U +
				 (uint64_t)raw->input_event_usec / 1000U;
	return true;
}

static bool queue_key(struct fplinux_input_session_device *slot,
		      const struct input_event *raw)
{
	if (slot->key_count == slot->key_capacity) {
		size_t capacity = slot->key_capacity ? slot->key_capacity * 2U :
						       16U;
		struct input_event *keys;

		if (capacity < slot->key_capacity ||
		    capacity > SIZE_MAX / sizeof(*keys))
			return false;
		keys = realloc(slot->keys, capacity * sizeof(*keys));
		if (!keys)
			return false;
		slot->keys = keys;
		slot->key_capacity = capacity;
	}
	slot->keys[slot->key_count++] = *raw;
	return true;
}

static bool read_device(struct fplinux_input_session *session,
			struct fplinux_input_session_device *slot,
			struct fplinux_input_event *event)
{
	for (;;) {
		struct input_event raw;
		unsigned int flags = slot->syncing ? LIBEVDEV_READ_FLAG_SYNC :
						     LIBEVDEV_READ_FLAG_NORMAL;
		int result = libevdev_next_event(slot->device, flags, &raw);

		if (result == -EINTR)
			continue;
		if (result == -EAGAIN) {
			if (slot->syncing) {
				slot->syncing = false;
				slot->sync_pending = true;
				slot->sync_code = 0;
				if (next_pending(session, event))
					return true;
				continue;
			}
			session->active_device = -1;
			return false;
		}
		if (result < 0) {
			remove_device(session, slot);
			return next_pending(session, event);
		}
		if (result == LIBEVDEV_READ_STATUS_SYNC && !slot->syncing) {
			/* Discard the incomplete frame; lost relative motion is unrecoverable. */
			slot->key_count = 0;
			slot->key_next = 0;
			slot->dx = 0;
			slot->dy = 0;
			slot->wheel_v120 = 0;
			slot->wheel_legacy = 0;
			slot->high_resolution_wheel = false;
			slot->syncing = true;
			slot->sync_time_ms =
				(uint64_t)raw.input_event_sec * 1000U +
				(uint64_t)raw.input_event_usec / 1000U;
			continue;
		}
		if (slot->syncing) {
			slot->sync_time_ms =
				(uint64_t)raw.input_event_sec * 1000U +
				(uint64_t)raw.input_event_usec / 1000U;
			continue;
		}
		if (raw.type == EV_KEY) {
			if (raw.value != 0 && raw.value != 1)
				continue;
			if (!queue_key(slot, &raw)) {
				remove_device(session, slot);
				return next_pending(session, event);
			}
			continue;
		}
		if (raw.type == EV_SYN && raw.code == SYN_REPORT) {
			slot->frame_ready = true;
			if (slot->has_pointer)
				finish_frame(session, slot);
			if (next_pending(session, event))
				return true;
			continue;
		}
		if (!slot->has_pointer)
			continue;
		if (raw.type == EV_REL) {
			switch (raw.code) {
			case REL_X:
				slot->dx += raw.value;
				break;
			case REL_Y:
				slot->dy += raw.value;
				break;
			case REL_WHEEL:
				slot->wheel_legacy += raw.value;
				break;
			case REL_WHEEL_HI_RES:
				slot->wheel_v120 += raw.value;
				slot->high_resolution_wheel = true;
				break;
			}
		}
	}
}

static void receive_device(struct fplinux_input_session *session)
{
	struct udev_device *device =
		udev_monitor_receive_device(session->monitor);
	const char *action;
	const char *path;
	size_t i;

	if (!device)
		return;
	action = udev_device_get_action(device);
	path = udev_device_get_devnode(device);
	if (action && !strcmp(action, "add")) {
		add_device(session, device);
	} else if (action && !strcmp(action, "remove") && path) {
		for (i = 0; i < FPLINUX_INPUT_SESSION_DEVICE_COUNT; ++i) {
			struct fplinux_input_session_device *slot =
				&session->devices[i];

			if (slot->fd >= 0 && !strcmp(slot->path, path)) {
				remove_device(session, slot);
				break;
			}
		}
	}
	udev_device_unref(device);
}

bool fplinux_input_session_next(struct fplinux_input_session *session,
				struct fplinux_input_event *event)
{
	if (!session->udev || session->epoll_fd < 0)
		return false;
	for (;;) {
		struct epoll_event ready;
		int result;
		size_t i;

		if (next_pending(session, event))
			return true;
		if (session->active_device >= 0 &&
		    read_device(session,
				&session->devices[session->active_device],
				event)) {
			/* libevdev may still hold events after draining the kernel fd. */
			wake_session(session);
			return true;
		}
		result = epoll_wait(session->epoll_fd, &ready, 1, 0);
		if (result < 0 && errno == EINTR)
			continue;
		if (result <= 0)
			return false;
		if (ready.data.fd == session->wake_fd) {
			uint64_t count;

			while (read(session->wake_fd, &count, sizeof(count)) <
				       0 &&
			       errno == EINTR)
				;
		} else if (ready.data.fd ==
			   udev_monitor_get_fd(session->monitor)) {
			receive_device(session);
		} else {
			for (i = 0; i < FPLINUX_INPUT_SESSION_DEVICE_COUNT; ++i)
				if (session->devices[i].fd == ready.data.fd) {
					session->active_device = (int)i;
					break;
				}
		}
	}
}

int fplinux_input_session_get_fd(const struct fplinux_input_session *session)
{
	return session->udev ? session->epoll_fd : -1;
}

void fplinux_input_session_suspend(struct fplinux_input_session *session)
{
	size_t i;

	if (!session->udev || session->epoll_fd < 0 || session->suspended)
		return;
	session->suspended = true;
	for (i = 0; i < FPLINUX_INPUT_SESSION_DEVICE_COUNT; ++i)
		if (session->devices[i].fd >= 0)
			remove_device(session, &session->devices[i]);
}

bool fplinux_input_session_resume(struct fplinux_input_session *session,
				  char *error, size_t error_size)
{
	if (!session->udev || session->epoll_fd < 0) {
		set_message(error, error_size, "input session is closed");
		return false;
	}
	if (!session->suspended)
		return true;
	/* Discard notifications accumulated while another owner held the devices. */
	for (;;) {
		struct udev_device *device =
			udev_monitor_receive_device(session->monitor);

		if (!device)
			break;
		udev_device_unref(device);
	}
	session->suspended = false;
	if (!scan_devices(session)) {
		fplinux_input_session_suspend(session);
		set_message(error, error_size, "cannot resume input devices");
		return false;
	}
	return true;
}

void fplinux_input_session_close(struct fplinux_input_session *session)
{
	size_t i;

	if (!session->udev)
		return;
	for (i = 0; i < FPLINUX_INPUT_SESSION_DEVICE_COUNT; ++i) {
		release_device(session, &session->devices[i]);
		forget_device(&session->devices[i]);
	}
	if (session->monitor)
		udev_monitor_unref(session->monitor);
	if (session->wake_fd >= 0)
		close(session->wake_fd);
	if (session->epoll_fd >= 0)
		close(session->epoll_fd);
	if (session->udev)
		udev_unref(session->udev);
	session->monitor = NULL;
	session->udev = NULL;
	session->wake_fd = -1;
	session->epoll_fd = -1;
}
