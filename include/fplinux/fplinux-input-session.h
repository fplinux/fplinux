/* SPDX-License-Identifier: GPL-2.0-only */
/* Exclusive libinput session over keyboards, pointers and the phone keypad. */

#ifndef FPLINUX_INPUT_SESSION_H
#define FPLINUX_INPUT_SESSION_H

#include <stdbool.h>
#include <stddef.h>

#define FPLINUX_INPUT_SESSION_DEVICE_COUNT 32U
#define FPLINUX_INPUT_SESSION_PATH_BYTES 32U
#define FPLINUX_INPUT_SESSION_NAME_BYTES 128U

struct libinput;
struct udev;

enum fplinux_input_source {
	FPLINUX_INPUT_SOURCE_KEYPAD,
	FPLINUX_INPUT_SOURCE_KEYBOARD,
	FPLINUX_INPUT_SOURCE_POINTER,
};

#define FPLINUX_INPUT_SOURCE_MASK(source) (1U << (source))

enum fplinux_input_event_type {
	FPLINUX_INPUT_EVENT_KEY,
	FPLINUX_INPUT_EVENT_BUTTON,
	FPLINUX_INPUT_EVENT_MOTION,
	FPLINUX_INPUT_EVENT_WHEEL,
	FPLINUX_INPUT_EVENT_DEVICE_ADDED,
	FPLINUX_INPUT_EVENT_DEVICE_REMOVED,
};

/*
 * One translated event. Key and button codes are Linux input codes; a device
 * that disappears has all of its pressed keys released before its removal is
 * reported. The name is valid only until the next call on the session.
 */
struct fplinux_input_event {
	enum fplinux_input_event_type type;
	enum fplinux_input_source source;
	unsigned int code;
	bool pressed;
	double dx;
	double dy;
	int wheel_clicks;
	const char *name;
};

struct fplinux_input_session_device {
	char path[FPLINUX_INPUT_SESSION_PATH_BYTES];
	int fd;
	enum fplinux_input_source source;
};

struct fplinux_input_session {
	struct udev *udev;
	struct libinput *libinput;
	unsigned int accepted_sources;
	int wheel_remainder;
	char name[FPLINUX_INPUT_SESSION_NAME_BYTES];
	struct fplinux_input_session_device
		devices[FPLINUX_INPUT_SESSION_DEVICE_COUNT];
};

/*
 * Opens every present and later connected device whose source is in
 * accepted_sources and grabs it exclusively for the life of the session.
 */
bool fplinux_input_session_open(struct fplinux_input_session *session,
				unsigned int accepted_sources, char *error,
				size_t error_size);
bool fplinux_input_session_next(struct fplinux_input_session *session,
				struct fplinux_input_event *event);
void fplinux_input_session_close(struct fplinux_input_session *session);

#endif
