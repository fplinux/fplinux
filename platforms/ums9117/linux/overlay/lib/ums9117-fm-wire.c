// SPDX-License-Identifier: GPL-2.0-only
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/soc/ums9117/fm-wire.h>

size_t ums9117_fm_command_build(u8 *command, size_t capacity, u8 subcommand,
				const u8 *payload, size_t payload_bytes)
{
	if (payload_bytes > 254 || capacity < 5 + payload_bytes ||
	    (!payload && payload_bytes))
		return 0;
	command[0] = 0x01;
	command[1] = 0x8c;
	command[2] = 0xfc;
	command[3] = payload_bytes + 1;
	command[4] = subcommand;
	if (payload_bytes)
		memcpy(command + 5, payload, payload_bytes);
	return 5 + payload_bytes;
}

int ums9117_fm_wire_begin(struct ums9117_fm_wire *wire, u8 *reply,
			  size_t capacity, size_t expected_bytes, bool seek)
{
	if (!reply || expected_bytes < 7 || expected_bytes > capacity ||
	    expected_bytes > UMS9117_FM_EVENT_BYTES ||
	    (seek && expected_bytes != 9))
		return -EINVAL;
	wire->reply = reply;
	wire->reply_bytes = 0;
	wire->expected_bytes = expected_bytes;
	wire->seek = seek;
	wire->acknowledged = false;
	wire->complete = false;
	return 0;
}

static int complete_event(struct ums9117_fm_wire *wire)
{
	const u8 *event = wire->event;
	size_t bytes = wire->event_bytes;

	if (event[1] == 0x0e && bytes >= 7 && event[4] == 0x8c &&
	    event[5] == 0xfc) {
		if (wire->seek && !event[6]) {
			if (bytes != 7 || wire->acknowledged)
				return -EPROTO;
			wire->acknowledged = true;
			return 0;
		}
		/* A firmware error may omit the success payload. */
		if (bytes != wire->expected_bytes && !(bytes == 7 && event[6]))
			return -EPROTO;
	} else if (wire->seek && event[1] == 0xff && bytes >= 4 &&
		   event[3] == 0x30) {
		if (!wire->acknowledged || bytes != 9)
			return -EPROTO;
	} else {
		return 0;
	}
	memcpy(wire->reply, event, bytes);
	wire->reply_bytes = bytes;
	wire->complete = true;
	return 0;
}

int ums9117_fm_wire_receive(struct ums9117_fm_wire *wire, bool awaiting_reply,
			    const u8 *input, size_t length)
{
	size_t index;
	int ret;

	for (index = 0; index < length; index++) {
		if (!wire->event_bytes && input[index] != 0x04)
			return -EPROTO;
		wire->event[wire->event_bytes++] = input[index];
		if (wire->event_bytes < 3 ||
		    wire->event_bytes < 3U + wire->event[2])
			continue;
		/* Complete events may be split across reads or share one read. */
		if (awaiting_reply && !wire->complete) {
			ret = complete_event(wire);
			if (ret)
				return ret;
		}
		wire->event_bytes = 0;
	}
	return 0;
}
