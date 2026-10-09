// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (C) 2015 Spreadtrum Communications Inc. */

#include <linux/errno.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "marlin2-wlan-wire.h"

#define MARLIN2_WLAN_PUH_EOF 0x00800000U

const u8 marlin2_wlan_extended_capabilities[11] = {};
/* Keep userspace-owned prefix bits; the radio accepts at most seven bytes. */
const u8 marlin2_wlan_extended_capabilities_mask[11] = {
	0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 0xff, 0xff,
};

int marlin2_wlan_ies_validate(const u8 *ies, size_t bytes)
{
	while (bytes) {
		size_t ie_bytes;

		if (bytes < 2)
			return -EINVAL;
		ie_bytes = 2 + ies[1];
		if (ie_bytes > bytes)
			return -EINVAL;
		if (ies[0] == 0x7f && ies[1] > 7)
			return -EOPNOTSUPP;
		ies += ie_bytes;
		bytes -= ie_bytes;
	}
	return 0;
}

static size_t aggregate_bytes(size_t payload_bytes)
{
	return (4 + ((payload_bytes + 3) & ~3U) + 4 + 511) & ~511U;
}

size_t marlin2_wlan_command_build(u8 *packet, size_t capacity, u8 mode,
				  u8 command, u32 token, const u8 *payload,
				  size_t payload_bytes)
{
	size_t message_bytes = MARLIN2_WLAN_COMMAND_BYTES + payload_bytes;
	size_t bytes;

	if (mode > 7 || payload_bytes > 0xffff - MARLIN2_WLAN_COMMAND_BYTES)
		return 0;
	bytes = aggregate_bytes(message_bytes);
	if (bytes > capacity)
		return 0;
	memset(packet, 0, bytes);
	put_unaligned_le32(0x20000000U | (message_bytes << 7), packet);
	packet[4] = (mode << 5) | 0x10;
	packet[5] = command;
	put_unaligned_le16(message_bytes, packet + 6);
	put_unaligned_le32(token, packet + 8);
	if (payload_bytes)
		memcpy(packet + 16, payload, payload_bytes);
	put_unaligned_le32(MARLIN2_WLAN_PUH_EOF,
			   packet + 4 + ((message_bytes + 3) & ~3U));
	return bytes;
}

size_t marlin2_wlan_data_build(u8 *packet, size_t capacity, u8 mode,
			       bool shared_credit, const u8 *frame,
			       size_t frame_bytes)
{
	size_t message_bytes = MARLIN2_WLAN_DATA_BYTES + frame_bytes;
	size_t bytes;

	if (mode > 7 || frame_bytes > 0xffff - MARLIN2_WLAN_DATA_BYTES)
		return 0;
	bytes = aggregate_bytes(message_bytes);
	if (bytes > capacity)
		return 0;
	memset(packet, 0, bytes);
	put_unaligned_le32(0x21000000U | (message_bytes << 7), packet);
	packet[4] = (mode << 5) | 2;
	put_unaligned_le16(message_bytes, packet + 6);
	packet[11] = shared_credit ? 0 : 1;
	memcpy(packet + 12, frame, frame_bytes);
	put_unaligned_le32(MARLIN2_WLAN_PUH_EOF,
			   packet + 4 + ((message_bytes + 3) & ~3U));
	return bytes;
}

size_t marlin2_wlan_beacon_build(u8 *payload, size_t capacity, const u8 *head,
				 size_t head_bytes, const u8 *tail,
				 size_t tail_bytes, const u8 *ssid,
				 size_t ssid_bytes, u8 hidden)
{
	size_t old_ssid_bytes;
	size_t beacon_bytes;
	size_t suffix_bytes;
	size_t offset;

	if (!ssid_bytes || ssid_bytes > 32 || hidden > 2 || head_bytes < 38 ||
	    head[36] != 0 || head[37] > head_bytes - 38)
		return 0;
	old_ssid_bytes = head[37];
	suffix_bytes = head_bytes - 38 - old_ssid_bytes;
	/* The fixed fields and SSID IE must fit the firmware's 768-byte limit. */
	if (suffix_bytes > 768 - 39 - ssid_bytes ||
	    tail_bytes > 768 - 39 - ssid_bytes - suffix_bytes)
		return 0;
	beacon_bytes = 39 + ssid_bytes + suffix_bytes + tail_bytes;
	if (capacity < beacon_bytes + 2)
		return 0;
	put_unaligned_le16(beacon_bytes, payload);
	memcpy(payload + 2, head, 37);
	payload[39] = ssid_bytes + 1;
	memcpy(payload + 40, ssid, ssid_bytes);
	payload[40 + ssid_bytes] = hidden;
	offset = 41 + ssid_bytes;
	if (suffix_bytes)
		memcpy(payload + offset, head + 38 + old_ssid_bytes,
		       suffix_bytes);
	offset += suffix_bytes;
	if (tail_bytes)
		memcpy(payload + offset, tail, tail_bytes);
	return beacon_bytes + 2;
}

int marlin2_wlan_station_decode(const u8 *payload, size_t bytes,
				struct marlin2_wlan_station_event *event)
{
	if (bytes < 7 || bytes == 8)
		return -EBADMSG;
	event->connected = payload[0] != 0;
	event->mac = payload + 1;
	event->ie_bytes = bytes == 7 ? 0 : get_unaligned_le16(payload + 7);
	if (bytes > 7 && event->ie_bytes > bytes - 9)
		return -EBADMSG;
	event->ies = event->ie_bytes ? payload + 9 : NULL;
	return 0;
}

int marlin2_wlan_data_decode(const u8 *payload, size_t bytes,
			     struct marlin2_wlan_data_message *message)
{
	u8 offset;

	if (bytes < MARLIN2_WLAN_DATA_BYTES || (payload[0] & 7) != 2 ||
	    get_unaligned_le16(payload + 2) != bytes)
		return -EBADMSG;
	offset = payload[1] & 31;
	if (offset > bytes - MARLIN2_WLAN_DATA_BYTES)
		return -EBADMSG;
	message->mode = payload[0] >> 5;
	message->frame_type = payload[1] & 0xe0;
	message->has_credits = (payload[0] & 0x10) != 0;
	message->credits = payload + 4;
	message->frame = payload + MARLIN2_WLAN_DATA_BYTES + offset;
	message->frame_bytes = bytes - MARLIN2_WLAN_DATA_BYTES - offset;
	message->management_metadata = NULL;
	if (message->frame_type == 0x40) {
		size_t frame_bytes;

		/* Management offsets include the twelve bytes stored at the tail. */
		if (offset < 12)
			return -EBADMSG;
		message->management_metadata = payload + bytes - 12;
		frame_bytes =
			get_unaligned_le16(message->management_metadata + 10);
		if (frame_bytes > message->frame_bytes)
			return -EBADMSG;
		message->frame -= 12;
		message->frame_bytes = frame_bytes;
	}
	return 0;
}

bool marlin2_wlan_event_for_mode(u8 common, u8 id, u8 mode)
{
	if ((common & 7) != 1)
		return false;
	/* Credits describe the shared transport pools, independent of AP/STA. */
	return id == 0xb3 || !(common >> 5) || (common >> 5) == mode;
}

bool marlin2_wlan_scan_flags_supported(u32 flags)
{
	/* The co-location hint does not affect a scan of 2.4 GHz channels. */
	return !(flags & ~MARLIN2_WLAN_SCAN_COLOCATED_6GHZ_FLAG);
}

size_t marlin2_wlan_scan_build(u8 *payload, size_t capacity, u32 channels,
			       const struct marlin2_wlan_ssid *ssids,
			       size_t count)
{
	size_t bytes = 10;
	size_t index;

	if (count > 4 || capacity < 10)
		return 0;
	for (index = 0; index < count; index++) {
		if (ssids[index].bytes > 32)
			return 0;
		if (!ssids[index].bytes)
			continue;
		if (capacity - bytes < 1 + ssids[index].bytes)
			return 0;
		payload[bytes++] = ssids[index].bytes;
		memcpy(payload + bytes, ssids[index].data, ssids[index].bytes);
		bytes += ssids[index].bytes;
	}
	put_unaligned_le32(channels, payload);
	put_unaligned_le32(0, payload + 4);
	put_unaligned_le16(bytes - 10, payload + 8);
	return bytes;
}

void marlin2_wlan_connect_build(u8 payload[82], bool wpa2, const u8 *bssid,
				u8 channel, const u8 *ssid, size_t ssid_bytes)
{
	memset(payload, 0, 82);
	payload[12] = 0x80;
	payload[13] = 0x80;
	payload[14] = 0x80;
	if (wpa2) {
		put_unaligned_le32(2, payload);
		payload[12] = 0x84;
		payload[13] = 0x84;
		payload[14] = 0x82;
	}
	if (bssid)
		memcpy(payload + 4, bssid, 6);
	payload[10] = channel;
	payload[17] = ssid_bytes;
	memcpy(payload + 50, ssid, ssid_bytes);
}

int marlin2_wlan_key_build(u8 payload[35], u8 index, bool pairwise,
			   const u8 *mac, const u8 *sequence,
			   size_t sequence_bytes, const u8 key[16])
{
	if (index > 3 || sequence_bytes > 8 || (pairwise && !mac))
		return -EINVAL;
	memset(payload, 0, 35);
	payload[0] = 3;
	payload[1] = index;
	payload[2] = pairwise;
	if (mac)
		memcpy(payload + 3, mac, 6);
	if (sequence_bytes)
		memcpy(payload + 9, sequence, sequence_bytes);
	payload[17] = 4;
	payload[18] = 16;
	memcpy(payload + 19, key, 16);
	return 0;
}
