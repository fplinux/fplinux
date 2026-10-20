/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_UMS9117_FM_WIRE_H
#define FPLINUX_UMS9117_FM_WIRE_H

#include <linux/types.h>

#define UMS9117_FM_COMMAND_BYTES 259U
#define UMS9117_FM_EVENT_BYTES 258U

struct ums9117_fm_wire {
	u8 event[UMS9117_FM_EVENT_BYTES];
	u8 *reply;
	size_t event_bytes;
	size_t reply_bytes;
	size_t expected_bytes;
	bool seek;
	bool acknowledged;
	bool complete;
};

size_t ums9117_fm_command_build(u8 *command, size_t capacity, u8 subcommand,
				const u8 *payload, size_t payload_bytes);
/*
 * The reply buffer remains valid through receive. Begin retains any partial
 * event; admission and draining remain with the caller.
 */
int ums9117_fm_wire_begin(struct ums9117_fm_wire *wire, u8 *reply,
			  size_t capacity, size_t expected_bytes, bool seek);
int ums9117_fm_wire_receive(struct ums9117_fm_wire *wire, bool awaiting_reply,
			    const u8 *input, size_t length);

#endif
