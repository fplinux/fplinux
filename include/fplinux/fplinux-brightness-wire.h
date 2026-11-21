/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_BRIGHTNESS_WIRE_H
#define FPLINUX_BRIGHTNESS_WIRE_H

#define FPLINUX_BRIGHTNESS_LEVEL_MAX 10U

/*
 * Each request and reply occupies one SOCK_SEQPACKET packet. GET reads the
 * baseline and returns LEVEL. SET changes the baseline and CLAIM, SHOW and
 * RELEASE manipulate a temporary display lease tied to the connection.
 * The service returns OK after a change or ERROR on failure.
 */
enum fplinux_brightness_type {
	FPLINUX_BRIGHTNESS_GET = 1,
	FPLINUX_BRIGHTNESS_SET = 2, /* level u8 */
	FPLINUX_BRIGHTNESS_CLAIM = 3,
	FPLINUX_BRIGHTNESS_SHOW = 4, /* level u8 */
	FPLINUX_BRIGHTNESS_RELEASE = 5,
	FPLINUX_BRIGHTNESS_LEVEL = 6, /* level u8 */
	FPLINUX_BRIGHTNESS_OK = 7,
	FPLINUX_BRIGHTNESS_ERROR = 8, /* positive errno, u32le */
};

enum fplinux_brightness_direction {
	FPLINUX_BRIGHTNESS_REQUEST,
	FPLINUX_BRIGHTNESS_REPLY,
};

struct fplinux_brightness_message {
	enum fplinux_brightness_type type;
	union {
		unsigned int level;
		int error;
	};
};

int fplinux_brightness_wire_send(
	int fd, const struct fplinux_brightness_message *message);
/* Uses the fd's blocking mode. Returns 1, 0 on EOF, or -1 with errno. */
int fplinux_brightness_wire_receive(int fd,
				    enum fplinux_brightness_direction direction,
				    struct fplinux_brightness_message *message);

#endif
