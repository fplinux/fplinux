/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef FPLINUX_AUDIO_CONTROL_H
#define FPLINUX_AUDIO_CONTROL_H

#include "fplinux-audio-protocol.h"

#include <stdbool.h>
#include <sys/types.h>

/*
 * On the control socket, a packet is one byte of type followed by the fields
 * listed below. Multibyte numbers are little-endian. APP and CLOCK replace
 * a process's policy; a new APP background grant revokes the old one.
 * GONE ends a process's connections. ELIGIBLE follows the foreground state
 * of an APP or the lifetime of the single registered CLOCK helper.
 *
 * The manager waits for the volume and output acknowledgements before
 * replaying processes and sending SYNC. Until SYNC the streams are paused.
 * The service reports APP activity after SYNC and on each change.
 */
enum fplinux_audio_control_type {
	FPLINUX_AUDIO_SET_VOLUME = 1, /* class u8, level u8 */
	FPLINUX_AUDIO_SET_OUTPUT = 2, /* route u8 */
	FPLINUX_AUDIO_CONTROL_APP = 3, /* pid u32, uid u32, background u8 */
	FPLINUX_AUDIO_CONTROL_CLOCK = 4, /* pid u32, uid u32 */
	FPLINUX_AUDIO_CONTROL_GONE = 5, /* pid u32 */
	FPLINUX_AUDIO_CONTROL_ELIGIBLE = 6, /* pid u32, eligible u8 */
	FPLINUX_AUDIO_CONTROL_SYNC = 7,
	FPLINUX_AUDIO_VOLUME_APPLIED = 8, /* class u8, level u8 */
	FPLINUX_AUDIO_OUTPUT_APPLIED = 9, /* route u8 */
	FPLINUX_AUDIO_OUTPUT_ERROR = 10, /* errno u16 */
	FPLINUX_AUDIO_CONTROL_ACTIVITY = 11, /* pid u32, uid u32, state u8 */
};

enum fplinux_audio_control_direction {
	FPLINUX_AUDIO_TO_SERVICE,
	FPLINUX_AUDIO_TO_MANAGER,
};

enum fplinux_audio_class {
	FPLINUX_AUDIO_MEDIA = 1,
	FPLINUX_AUDIO_ALARM = 2,
};

enum fplinux_audio_activity {
	FPLINUX_AUDIO_ACTIVITY_NONE = 0,
	FPLINUX_AUDIO_ACTIVITY_PAUSED = 1,
	FPLINUX_AUDIO_ACTIVITY_HELD = 2,
	FPLINUX_AUDIO_ACTIVITY_UNAVAILABLE = 3,
	FPLINUX_AUDIO_ACTIVITY_PLAYING = 4,
};

struct fplinux_audio_control {
	enum fplinux_audio_control_type type;
	union {
		struct {
			enum fplinux_audio_class class;
			unsigned int level;
		} volume;
		struct {
			pid_t pid;
			uid_t uid;
			bool background;
		} app;
		struct {
			pid_t pid;
			uid_t uid;
		} clock;
		pid_t gone;
		struct {
			pid_t pid;
			bool enabled;
		} eligible;
		struct {
			pid_t pid;
			uid_t uid;
			enum fplinux_audio_activity state;
		} activity;
		enum fplinux_audio_route route;
		int error;
	};
};

int fplinux_audio_control_send(int fd,
			       const struct fplinux_audio_control *message);
/* Nonblocking. Returns 1 on success, 0 on EOF, or -1 with errno. */
int fplinux_audio_control_receive(int fd,
				  enum fplinux_audio_control_direction direction,
				  struct fplinux_audio_control *message);

const char *fplinux_audio_route_name(enum fplinux_audio_route route);
bool fplinux_audio_route_parse(const char *name,
			       enum fplinux_audio_route *route);

#endif
