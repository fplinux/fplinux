// SPDX-License-Identifier: GPL-2.0-only
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/string.h>

#include "qin1splus-bt-protocol.h"

static int packet_header(u8 type)
{
	switch (type) {
	case 1:
	case 3:
		return 3;
	case 2:
		return 4;
	case 4:
		return 2;
	default:
		return -EILSEQ;
	}
}

static size_t packet_bytes(u8 type, const u8 *data)
{
	switch (type) {
	case 1:
	case 3:
		return 3 + data[2];
	case 2:
		return 4 + data[2] + ((u32)data[3] << 8);
	default:
		return 2 + data[1];
	}
}

int qin_bt_pack(u8 type, const u8 *data, size_t bytes, u8 *output,
		size_t capacity)
{
	size_t total, padding = 0;
	int header = packet_header(type);

	if (header < 0 || type == 4)
		return -EILSEQ;
	if (bytes < (size_t)header || bytes > QIN_BT_FRAME_BYTES ||
	    packet_bytes(type, data) != bytes)
		return -EMSGSIZE;
	total = bytes + 1;
	if (type == 1 || type == 2)
		padding = 8 - (total & 7);
	if (total + padding > capacity)
		return -ENOSPC;
	output[0] = type;
	memcpy(output + 1, data, bytes);
	memset(output + total, 0, padding);
	return total + padding;
}

int qin_bt_stream_receive(struct qin_bt_stream *stream, const u8 *data,
			  size_t bytes,
			  int (*receive)(void *, u8, const u8 *, size_t),
			  void *context)
{
	int header, ret;
	size_t count;

	while (bytes) {
		if (!stream->type) {
			stream->type = *data++;
			bytes--;
			header = packet_header(stream->type);
			if (header < 0 || stream->type == 1) {
				stream->bad_type = stream->type;
				return -EILSEQ;
			}
			stream->needed = header;
		}
		count = min(bytes, stream->needed - stream->used);
		memcpy(stream->frame + stream->used, data, count);
		stream->used += count;
		data += count;
		bytes -= count;
		if (stream->used != stream->needed)
			continue;
		header = packet_header(stream->type);
		if (stream->used == (size_t)header) {
			stream->needed =
				packet_bytes(stream->type, stream->frame);
			if (stream->needed > sizeof(stream->frame)) {
				stream->bad_length = stream->needed;
				return -EMSGSIZE;
			}
			if (stream->used != stream->needed)
				continue;
		}
		ret = receive(context, stream->type, stream->frame,
			      stream->used);
		stream->type = 0;
		stream->used = 0;
		stream->needed = 0;
		if (ret)
			return ret;
	}
	return 0;
}

int qin_bt_validate_pskey(const u8 *data, size_t bytes)
{
	static const u8 response[] = { 0, 0x93, 0x61, 0x25, 3, 0x15, 0x20 };

	if (bytes != sizeof(response) ||
	    memcmp(data, response, sizeof(response)))
		return -EPROTO;
	return 0;
}

int qin_bt_validate_enable(const u8 *data, size_t bytes, bool enabled)
{
	/* These are the echoed mode/request bytes, not a success flag. */
	if (bytes != 3 || data[0] || data[1] || data[2] != enabled)
		return -EPROTO;
	return 0;
}

int qin_bt_validate_event(const u8 *data, size_t bytes)
{
	size_t expected;

	if (bytes < 5 || data[0] != 0x0e || data[4] != 0xfc ||
	    (data[3] != 0xa0 && data[3] != 0xa1))
		return 0;
	expected = data[3] == 0xa0 ? 12 : 8;
	if (bytes != expected || data[1] != expected - 2 || data[2] != 1)
		return -EPROTO;
	return 0;
}
