// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (C) 2015 Spreadtrum Communications Inc. */

#include <linux/errno.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "marlin2-packet.h"
#include "marlin2-transport-wire.h"

#define MARLIN2_TRANSPORT_PUH_EOF 0x00800000U

size_t marlin2_transport_record_build(u8 *packet, size_t capacity, u8 type,
				      u8 subtype, const u8 *payload,
				      size_t payload_bytes)
{
	size_t record_bytes;
	size_t bytes;

	if (type > 3 || subtype > 3 || !payload_bytes || payload_bytes > 0xffff)
		return 0;
	record_bytes = 4 + ((payload_bytes + 3) & ~3U);
	bytes = (record_bytes + 4 + 511) & ~511U;
	if (bytes > capacity)
		return 0;
	memset(packet, 0, bytes);
	put_unaligned_le32((u32)type << 28 | (u32)subtype << 24 |
				   (u32)payload_bytes << 7,
			   packet);
	memcpy(packet + 4, payload, payload_bytes);
	put_unaligned_le32(MARLIN2_TRANSPORT_PUH_EOF, packet + record_bytes);
	return bytes;
}

int marlin2_transport_aggregate_decode(const u8 *packet, size_t read_bytes,
				       marlin2_transport_receive_fn receive,
				       void *context, u32 *next_payload_bytes)
{
	size_t offset = 0;
	size_t data_bytes;
	u32 used_bytes;
	u32 parsed_bytes = 0;

	if (read_bytes < 512 || read_bytes > MARLIN2_PACKET_RX_MAX_BYTES ||
	    read_bytes % 512)
		return -EINVAL;
	data_bytes = read_bytes - 8;
	used_bytes = get_unaligned_le32(packet + data_bytes);
	*next_payload_bytes = get_unaligned_le32(packet + data_bytes + 4);
	while (parsed_bytes < used_bytes) {
		struct marlin2_transport_message message;
		size_t record_bytes;
		u32 header;
		int ret;

		if (offset > data_bytes || data_bytes - offset < 4)
			return -EBADMSG;
		header = get_unaligned_le32(packet + offset);
		if (header & MARLIN2_TRANSPORT_PUH_EOF)
			return -EBADMSG;
		message.length = (header >> 7) & 0xffff;
		message.type = header >> 28;
		message.subtype = (header >> 24) & 15;
		message.data = packet + offset + 4;
		record_bytes = 4 + ((message.length + 3) & ~3U);
		if (record_bytes > data_bytes - offset)
			return -EBADMSG;
		if (message.type != 15) {
			if (!message.length ||
			    message.length > used_bytes - parsed_bytes)
				return -EBADMSG;
			parsed_bytes += message.length;
			ret = receive(context, &message);
			if (ret)
				return ret;
		}
		offset += record_bytes;
	}
	return 0;
}
