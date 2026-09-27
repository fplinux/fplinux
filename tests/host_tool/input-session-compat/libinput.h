/* SPDX-License-Identifier: GPL-2.0-only */
/* Fake libinput boundary for the input-session host component harness. */
#ifndef FPLINUX_TEST_LIBINPUT_H
#define FPLINUX_TEST_LIBINPUT_H

#include <stdint.h>

struct udev;
struct libinput;
struct libinput_device;
struct libinput_event;
struct libinput_event_keyboard;
struct libinput_event_pointer;

enum libinput_event_type {
	LIBINPUT_EVENT_DEVICE_ADDED,
	LIBINPUT_EVENT_DEVICE_REMOVED,
	LIBINPUT_EVENT_KEYBOARD_KEY,
	LIBINPUT_EVENT_POINTER_BUTTON,
	LIBINPUT_EVENT_POINTER_MOTION,
	LIBINPUT_EVENT_POINTER_SCROLL_WHEEL,
};

enum libinput_key_state {
	LIBINPUT_KEY_STATE_RELEASED,
	LIBINPUT_KEY_STATE_PRESSED,
};

enum libinput_button_state {
	LIBINPUT_BUTTON_STATE_RELEASED,
	LIBINPUT_BUTTON_STATE_PRESSED,
};

enum libinput_pointer_axis {
	LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL,
};

struct libinput_interface {
	int (*open_restricted)(const char *path, int flags, void *user_data);
	void (*close_restricted)(int fd, void *user_data);
};

struct libinput *
libinput_udev_create_context(const struct libinput_interface *interface,
			     void *user_data, struct udev *udev);
int libinput_udev_assign_seat(struct libinput *context, const char *seat);
struct libinput *libinput_unref(struct libinput *context);
int libinput_get_fd(struct libinput *context);
int libinput_dispatch(struct libinput *context);
void libinput_suspend(struct libinput *context);
int libinput_resume(struct libinput *context);
struct libinput_event *libinput_get_event(struct libinput *context);
void libinput_event_destroy(struct libinput_event *event);
struct libinput_device *libinput_event_get_device(struct libinput_event *event);
enum libinput_event_type libinput_event_get_type(struct libinput_event *event);
void *libinput_device_get_user_data(struct libinput_device *device);
void libinput_device_set_user_data(struct libinput_device *device, void *data);
const char *libinput_device_get_name(struct libinput_device *device);
const char *libinput_device_get_sysname(struct libinput_device *device);
struct libinput_event_keyboard *
libinput_event_get_keyboard_event(struct libinput_event *event);
uint32_t libinput_event_keyboard_get_key(struct libinput_event_keyboard *event);
uint64_t
libinput_event_keyboard_get_time_usec(struct libinput_event_keyboard *event);
enum libinput_key_state
libinput_event_keyboard_get_key_state(struct libinput_event_keyboard *event);
struct libinput_event_pointer *
libinput_event_get_pointer_event(struct libinput_event *event);
uint32_t
libinput_event_pointer_get_button(struct libinput_event_pointer *event);
enum libinput_button_state
libinput_event_pointer_get_button_state(struct libinput_event_pointer *event);
double libinput_event_pointer_get_dx_unaccelerated(
	struct libinput_event_pointer *event);
double libinput_event_pointer_get_dy_unaccelerated(
	struct libinput_event_pointer *event);
int libinput_event_pointer_has_axis(struct libinput_event_pointer *event,
				    enum libinput_pointer_axis axis);
double libinput_event_pointer_get_scroll_value_v120(
	struct libinput_event_pointer *event, enum libinput_pointer_axis axis);

#endif
