// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (C) 2015 Spreadtrum Communications Inc. */

#include <linux/errno.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "marlin2-packet.h"

#define MARLIN2_PUH_BYTES 4U
#define MARLIN2_PUH_EOF 0x00800000U
#define MARLIN2_PUH_WIFI 2U
#define MARLIN2_PUH_DUMMY 15U
#define MARLIN2_COMMAND_BYTES 12U
#define MARLIN2_GET_INFO_BYTES 28U
#define MARLIN2_GET_INFO_ID 1U

void marlin2_packet_build_get_info(u8 packet[MARLIN2_PACKET_TX_BYTES],
				   u32 token)
{
	memset(packet, 0, MARLIN2_PACKET_TX_BYTES);
	put_unaligned_le32(0x20000600U, packet);
	packet[4] = 0x10;
	packet[5] = MARLIN2_GET_INFO_ID;
	put_unaligned_le16(MARLIN2_COMMAND_BYTES, packet + 6);
	put_unaligned_le32(token, packet + 8);
	put_unaligned_le32(MARLIN2_PUH_EOF, packet + 16);
}

static int decode_get_info(const u8 *command, size_t bytes, u32 token,
			   struct marlin2_packet_result *result)
{
	const u8 *payload;
	u16 command_bytes;

	if (bytes < MARLIN2_COMMAND_BYTES)
		return -EBADMSG;
	if ((command[0] & 7) || command[1] != MARLIN2_GET_INFO_ID ||
	    get_unaligned_le32(command + 4) != token)
		return 0;
	command_bytes = get_unaligned_le16(command + 2);
	result->response_length = command_bytes;
	memcpy(result->response_header, command,
	       sizeof(result->response_header));
	result->status = (s8)command[8];
	result->response_received = true;
	if (command_bytes > bytes || command_bytes < MARLIN2_COMMAND_BYTES)
		return -EBADMSG;
	if (result->status)
		return -EREMOTEIO;
	if (bytes != MARLIN2_COMMAND_BYTES + MARLIN2_GET_INFO_BYTES ||
	    command_bytes != bytes)
		return -EPROTO;
	payload = command + MARLIN2_COMMAND_BYTES;
	memcpy(result->response_payload, payload,
	       sizeof(result->response_payload));
	result->info.chip_model = get_unaligned_le32(payload);
	result->info.chip_version = get_unaligned_le32(payload + 4);
	result->info.firmware_version = get_unaligned_le32(payload + 8);
	result->info.standards = get_unaligned_le32(payload + 12);
	result->info.capabilities = get_unaligned_le32(payload + 16);
	result->info.max_ap_stations = payload[20];
	result->info.max_acl_addresses = payload[21];
	result->info.max_multicast_addresses = payload[22];
	result->info.wnm_ft_support = payload[23];
	result->info.extended_features = get_unaligned_le32(payload + 24);
	return 1;
}

int marlin2_packet_decode(const u8 *packet, size_t read_bytes, u32 token,
			  struct marlin2_packet_result *result)
{
	u32 parsed_payload_bytes = 0;
	u32 used_payload_bytes;
	size_t offset = 0;
	size_t record_bytes;
	size_t payload_bytes;
	size_t data_bytes;
	u32 header;
	u8 subtype;
	u8 type;
	int ret;

	if (read_bytes < 8 || read_bytes > MARLIN2_PACKET_RX_MAX_BYTES ||
	    read_bytes % 512)
		return -EINVAL;
	data_bytes = read_bytes - 8;
	used_payload_bytes = get_unaligned_le32(packet + data_bytes);
	result->used_payload_bytes = used_payload_bytes;
	result->next_payload_bytes =
		get_unaligned_le32(packet + data_bytes + 4);
	result->aggregates++;
	while (parsed_payload_bytes < used_payload_bytes) {
		if (data_bytes - offset < MARLIN2_PUH_BYTES)
			return -EBADMSG;
		header = get_unaligned_le32(packet + offset);
		result->last_puh = header;
		if (header & MARLIN2_PUH_EOF)
			return -EBADMSG;
		payload_bytes = (header >> 7) & 0xffff;
		type = header >> 28;
		subtype = (header >> 24) & 15;
		record_bytes = MARLIN2_PUH_BYTES + ((payload_bytes + 3) & ~3U);
		if (record_bytes > data_bytes - offset)
			return -EBADMSG;
		if (type != MARLIN2_PUH_DUMMY) {
			if (!payload_bytes ||
			    payload_bytes >
				    used_payload_bytes - parsed_payload_bytes)
				return -EBADMSG;
			parsed_payload_bytes += payload_bytes;
			result->records++;
			if (type == MARLIN2_PUH_WIFI && !subtype) {
				ret = decode_get_info(
					packet + offset + MARLIN2_PUH_BYTES,
					payload_bytes, token, result);
				if (ret)
					return ret;
			}
			result->ignored_records++;
		}
		offset += record_bytes;
	}
	return 0;
}

size_t marlin2_packet_next_read(u32 pending_payload_bytes)
{
	u64 bytes;

	if (!pending_payload_bytes)
		return MARLIN2_PACKET_RX_INITIAL_BYTES;
	bytes = pending_payload_bytes;
	bytes += 8 * ((pending_payload_bytes >> 10) + 1) + 64 + 511;
	bytes &= ~511ULL;
	if (bytes > MARLIN2_PACKET_RX_MAX_BYTES)
		return MARLIN2_PACKET_RX_MAX_BYTES;
	return bytes;
}
