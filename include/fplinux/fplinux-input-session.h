/* SPDX-License-Identifier: GPL-2.0-only */
/* Exclusive evdev session over keyboards, pointers and the phone keypad. */

#ifndef FPLINUX_INPUT_SESSION_H
#define FPLINUX_INPUT_SESSION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <linux/input.h>

#define FPLINUX_INPUT_SESSION_DEVICE_COUNT 32U
#define FPLINUX_INPUT_SESSION_PATH_BYTES 32U
#define FPLINUX_INPUT_SESSION_NAME_BYTES 128U

struct libevdev;
struct udev;
struct udev_monitor;

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
 * reported. device_id identifies one device lifetime within the session and
 * changes on reconnect or resume. The name is valid only until the next call
 * on the session. Key events retain their CLOCK_MONOTONIC time in milliseconds;
 * delivery may be delayed and timestamps across devices need not be ordered.
 * Key repeat and text composition belong to the caller.
 */
struct fplinux_input_event {
	enum fplinux_input_event_type type;
	enum fplinux_input_source source;
	uint64_t device_id;
	unsigned int code;
	bool pressed;
	uint64_t time_ms;
	double dx;
	double dy;
	int wheel_clicks;
	const char *name;
};

struct fplinux_input_session_device {
	char path[FPLINUX_INPUT_SESSION_PATH_BYTES];
	int fd;
	enum fplinux_input_source source;
	uint64_t id;
	struct libevdev *device;
	char name[FPLINUX_INPUT_SESSION_NAME_BYTES];
	unsigned char pressed[(KEY_CNT + 7U) / 8U];
	bool added;
	bool removing;
	bool syncing;
	bool sync_pending;
	unsigned int sync_code;
	uint64_t sync_time_ms;
	struct input_event *keys;
	size_t key_count;
	size_t key_next;
	size_t key_capacity;
	bool frame_ready;
	bool has_pointer;
	bool high_resolution_wheel;
	bool motion_pending;
	double dx;
	double dy;
	int64_t wheel_v120;
	int64_t wheel_legacy;
	int wheel_pending;
};

struct fplinux_input_session {
	struct udev *udev;
	struct udev_monitor *monitor;
	int epoll_fd;
	int wake_fd;
	int active_device;
	unsigned int accepted_sources;
	uint64_t next_device_id;
	bool suspended;
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
int fplinux_input_session_get_fd(const struct fplinux_input_session *session);
/*
 * Releases all grabs and queues releases/removals. Drain next() before handing
 * input ownership to another application or VT. Resume scans and grabs current
 * devices again; their added events start new device lifetimes.
 */
void fplinux_input_session_suspend(struct fplinux_input_session *session);
bool fplinux_input_session_resume(struct fplinux_input_session *session,
				  char *error, size_t error_size);
void fplinux_input_session_close(struct fplinux_input_session *session);

#endif
