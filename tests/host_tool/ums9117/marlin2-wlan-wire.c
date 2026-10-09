/* SPDX-License-Identifier: GPL-2.0-only */

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "marlin2-wlan-wire.h"

static void command_and_data_packets(void)
{
	static const u8 mac[] = { 0x02, 0x34, 0x56, 0x78, 0x9a, 0xbc };
	static const u8 open[] = { 2, 0, 0x02, 0x34, 0x56, 0x78, 0x9a, 0xbc };
	static const u8 expected_open[] = {
		0x00, 0x0a, 0x00, 0x20, 0x50, 0x03, 0x14, 0x00, 0x78, 0x56,
		0x34, 0x12, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x02, 0x34,
		0x56, 0x78, 0x9a, 0xbc, 0x00, 0x00, 0x80, 0x00,
	};
	u8 packet[4096];
	u8 frame[1514] = {};
	size_t index;

	assert(marlin2_wlan_command_build(packet, sizeof(packet), 2, 3,
					  0x12345678, open,
					  sizeof(open)) == 512);
	assert(!memcmp(packet, expected_open, sizeof(expected_open)));
	for (index = sizeof(expected_open); index < 512; index++)
		assert(packet[index] == 0);
	memcpy(frame, mac, sizeof(mac));
	frame[12] = 0x88;
	frame[13] = 0x8e;
	assert(marlin2_wlan_data_build(packet, sizeof(packet), 1, true, frame,
				       sizeof(frame)) == 1536);
	assert(!memcmp(packet,
		       (u8[]){ 0x00, 0xf9, 0x02, 0x21, 0x22, 0x00, 0xf2, 0x05,
			       0x00, 0x00, 0x00, 0x00 },
		       12));
	assert(!memcmp(packet + 12, frame, sizeof(frame)));
	assert(!memcmp(packet + 1528, (u8[]){ 0, 0, 0x80, 0 }, 4));
	assert(marlin2_wlan_data_build(packet, 512, 1, false, frame,
				       sizeof(frame)) == 0);
	assert(marlin2_wlan_data_build(packet, sizeof(packet), 1, false, frame,
				       14) == 512);
	assert(packet[11] == 1);
}

static void connection_and_key_payloads(void)
{
	const u8 bssid[] = { 2, 3, 4, 5, 6, 7 };
	const u8 key[] = {
		0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
	};
	const u8 sequence[] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 };
	u8 connect[82];
	u8 payload[35];
	size_t index;

	marlin2_wlan_connect_build(connect, true, bssid, 1, (const u8 *)"test",
				   4);
	assert(!memcmp(connect,
		       (u8[]){ 2, 0, 0, 0, 2, 3, 4, 5, 6, 7, 1, 0, 0x84, 0x84,
			       0x82, 0, 0, 4 },
		       18));
	for (index = 18; index < 50; index++)
		assert(connect[index] == 0);
	assert(!memcmp(connect + 50, "test", 4));
	marlin2_wlan_connect_build(connect, false, NULL, 1, (const u8 *)"open",
				   4);
	assert(connect[0] == 0 && connect[11] == 0);
	assert(connect[12] == 0x80 && connect[13] == 0x80 &&
	       connect[14] == 0x80);
	assert(marlin2_wlan_key_build(payload, 2, true, bssid, sequence,
				      sizeof(sequence), key) == 0);
	assert(!memcmp(payload,
		       (u8[]){ 3, 2, 1, 2, 3, 4, 5, 6, 7, 0x11, 0x22, 0x33,
			       0x44, 0x55, 0x66, 0, 0, 4, 16 },
		       19));
	assert(!memcmp(payload + 19, key, sizeof(key)));
	assert(marlin2_wlan_key_build(payload, 0, true, NULL, NULL, 0, key) <
	       0);
}

static void data_credits_preserve_ethernet(void)
{
	/* Credit grants may accompany a real Ethernet frame. */
	const u8 payload[] = {
		0x32, 0x02, 0x19, 0x00, 3, 4, 0,  0,  0xaa, 0xbb, 2,	3, 4,
		5,    6,    7,	  2,	8, 9, 10, 11, 12,   0x88, 0x8e, 1,
	};
	struct marlin2_wlan_data_message received;

	assert(marlin2_wlan_data_decode(payload, sizeof(payload), &received) ==
	       0);
	assert(received.mode == 1 && received.frame_type == 0);
	assert(received.has_credits && received.credits[0] == 3 &&
	       received.credits[1] == 4);
	assert(received.frame_bytes == 15);
	assert(!memcmp(received.frame, payload + 10, 15));
	assert(marlin2_wlan_data_decode((u8[]){ 0x32, 0, 8, 0, 3, 4, 0, 0 }, 8,
					&received) == 0);
	assert(received.has_credits && received.frame_bytes == 0);
}

static void global_credits_and_wildcard_scan(void)
{
	const struct marlin2_wlan_ssid wildcard[] = { { NULL, 0 } };
	const struct marlin2_wlan_ssid directed[] = {
		{ (const u8 *)"test", 4 },
		{ NULL, 0 },
	};
	u8 payload[144];

	assert(marlin2_wlan_scan_flags_supported(0));
	assert(marlin2_wlan_scan_flags_supported(0x4000));
	assert(!marlin2_wlan_scan_flags_supported(8));
	assert(!marlin2_wlan_scan_flags_supported(0x4008));
	assert(marlin2_wlan_event_for_mode(0x21, 0xb3, 2));
	assert(!marlin2_wlan_event_for_mode(0x21, 0x80, 2));
	assert(marlin2_wlan_event_for_mode(0x21, 0x80, 1));
	assert(marlin2_wlan_scan_build(payload, sizeof(payload), 1, wildcard,
				       1) == 10);
	assert(!memcmp(payload, (u8[]){ 1, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, 10));
	assert(marlin2_wlan_scan_build(payload, sizeof(payload), 0x401,
				       directed, 2) == 15);
	assert(!memcmp(payload,
		       (u8[]){ 1, 4, 0, 0, 0, 0, 0, 0, 5, 0, 4, 't', 'e', 's',
			       't' },
		       15));
}

static void management_data_frame_precedes_trailing_metadata(void)
{
	const u8 packet[56] = {
		0x22, 0x4c, 0x38, 0, 0, 0, 0, 0, [8] = 0x80, [44] = 4, 1,
		0xd8, 0,    2,	  3, 4, 5, 6, 7, 36,	     0,
	};
	struct marlin2_wlan_data_message received;

	assert(marlin2_wlan_data_decode(packet, sizeof(packet), &received) ==
	       0);
	assert(received.frame_type == 0x40 && received.frame_bytes == 36);
	assert(received.frame == packet + 8);
	assert(received.frame[0] == 0x80);
	assert(received.management_metadata == packet + 44);
	assert(received.management_metadata[0] == 4);
	assert(received.management_metadata[1] == 1);
	assert(received.management_metadata[2] == 0xd8);
	assert(marlin2_wlan_data_decode((u8[]){ 0x22, 0x40, 8, 0, 0, 0, 0, 0 },
					8, &received) < 0);
	{
		u8 too_long[56];

		memcpy(too_long, packet, sizeof(too_long));
		too_long[54] = 37;
		assert(marlin2_wlan_data_decode(too_long, sizeof(too_long),
						&received) < 0);
	}
}

static void beacon_and_station_events(void)
{
	u8 head[47] = {};
	u8 payload[770];
	struct marlin2_wlan_station_event event;
	const u8 disconnected[] = { 0, 2, 3, 4, 5, 6, 7 };
	const u8 connected[] = { 1, 2, 3, 4, 5, 6, 7, 3, 0, 1, 1, 0x82 };
	size_t bytes;

	memcpy(head + 36, (u8[]){ 0, 4, 'o', 'l', 'd', 'x', 3, 1, 1, 1, 0 },
	       11);
	bytes = marlin2_wlan_beacon_build(payload, sizeof(payload), head,
					  sizeof(head), NULL, 0,
					  (const u8 *)"new", 3, 0);
	assert(bytes == 49);
	assert(payload[0] == 47 && payload[1] == 0);
	assert(!memcmp(payload + 38,
		       (u8[]){ 0, 4, 'n', 'e', 'w', 0, 3, 1, 1, 1, 0 }, 11));
	assert(marlin2_wlan_beacon_build(payload, sizeof(payload), head,
					 sizeof(head), NULL, 0,
					 (const u8 *)"new", 3, 1) == 49);
	assert(payload[43] == 1);
	assert(marlin2_wlan_station_decode(disconnected, sizeof(disconnected),
					   &event) == 0);
	assert(!event.connected && event.ie_bytes == 0 && event.ies == NULL);
	assert(marlin2_wlan_station_decode(connected, sizeof(connected),
					   &event) == 0);
	assert(event.connected && event.ie_bytes == 3 &&
	       !memcmp(event.ies, (u8[]){ 1, 1, 0x82 }, 3));
	assert(marlin2_wlan_station_decode(connected, 8, &event) < 0);
	assert(marlin2_wlan_station_decode(connected, 11, &event) < 0);
}

static void extended_capabilities_fit_radio_and_keep_security_ies(void)
{
	const u8 unsupported[] = {
		0x7f, 0x0b, 0x00, 0x00, 0x0a, 0x02, 0x00,
		0x00, 0x40, 0x00, 0x00, 0x01, 0x20,
	};
	const u8 supported[] = {
		0x30, 0x14, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x04,
		0x01, 0x00, 0x00, 0x0f, 0xac, 0x04, 0x01, 0x00,
		0x00, 0x0f, 0xac, 0x02, 0x00, 0x00, 0x7f, 0x07,
		0x00, 0x00, 0x0a, 0x02, 0x00, 0x00, 0x40,
	};
	const u8 first_unsupported[] = {
		0x7f, 0x08, 0x00, 0x00, 0x0a, 0x02, 0x00, 0x00, 0x40, 0x01,
	};
	const u8 expected_capabilities[] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
	const u8 expected_mask[] = {
		0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 0xff, 0xff,
	};

	assert(marlin2_wlan_ies_validate(unsupported, sizeof(unsupported)) ==
	       -EOPNOTSUPP);
	assert(marlin2_wlan_ies_validate(first_unsupported,
					 sizeof(first_unsupported)) ==
	       -EOPNOTSUPP);
	assert(marlin2_wlan_ies_validate(supported, sizeof(supported)) == 0);
	assert(marlin2_wlan_ies_validate((u8[]){ 0xdd, 4, 0, 0x50, 0xf2, 4 },
					 6) == 0);
	assert(marlin2_wlan_ies_validate(NULL, 0) == 0);
	assert(marlin2_wlan_ies_validate((u8[]){ 0x7f, 7, 0 }, 3) == -EINVAL);
	assert(marlin2_wlan_ies_validate((u8[]){ 0x7f }, 1) == -EINVAL);
	assert(sizeof(marlin2_wlan_extended_capabilities) == 11);
	assert(sizeof(marlin2_wlan_extended_capabilities_mask) == 11);
	assert(!memcmp(marlin2_wlan_extended_capabilities,
		       expected_capabilities, 11));
	assert(!memcmp(marlin2_wlan_extended_capabilities_mask, expected_mask,
		       11));
}

int main(void)
{
	command_and_data_packets();
	connection_and_key_payloads();
	data_credits_preserve_ethernet();
	global_credits_and_wildcard_scan();
	management_data_frame_precedes_trailing_metadata();
	beacon_and_station_events();
	extended_capabilities_fit_radio_and_keep_security_ies();
	puts("marlin2 WLAN wire host cases passed");
	return 0;
}
