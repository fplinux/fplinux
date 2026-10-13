/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2015 Spreadtrum Communications Inc. */
#ifndef FPLINUX_MARLIN2_WLAN_WIRE_H
#define FPLINUX_MARLIN2_WLAN_WIRE_H

#include <linux/types.h>

#define MARLIN2_WLAN_COMMAND_BYTES 12U
#define MARLIN2_WLAN_DATA_BYTES 8U
#define MARLIN2_WLAN_TX_MAX_BYTES 4096U
#define MARLIN2_WLAN_MODE_STA 1U
#define MARLIN2_WLAN_MODE_AP 2U
#define MARLIN2_WLAN_SCAN_COLOCATED_6GHZ_FLAG 0x4000U

struct marlin2_wlan_station_event {
	const u8 *mac;
	const u8 *ies;
	size_t ie_bytes;
	bool connected;
};

struct marlin2_wlan_data_message {
	const u8 *frame;
	const u8 *credits;
	const u8 *management_metadata;
	size_t frame_bytes;
	u8 mode;
	u8 frame_type;
	bool has_credits;
};

struct marlin2_wlan_ssid {
	const u8 *data;
	size_t bytes;
};

extern const u8 marlin2_wlan_extended_capabilities[11];
extern const u8 marlin2_wlan_extended_capabilities_mask[11];

int marlin2_wlan_ies_validate(const u8 *ies, size_t bytes);
size_t marlin2_wlan_command_build(u8 *packet, size_t capacity, u8 mode,
				  u8 command, u32 token, const u8 *payload,
				  size_t payload_bytes);
size_t marlin2_wlan_data_build(u8 *packet, size_t capacity, u8 mode,
			       bool shared_credit, const u8 *frame,
			       size_t frame_bytes);
size_t marlin2_wlan_beacon_build(u8 *payload, size_t capacity, const u8 *head,
				 size_t head_bytes, const u8 *tail,
				 size_t tail_bytes, const u8 *ssid,
				 size_t ssid_bytes, u8 hidden);
int marlin2_wlan_station_decode(const u8 *payload, size_t bytes,
				struct marlin2_wlan_station_event *event);
int marlin2_wlan_data_decode(const u8 *payload, size_t bytes,
			     struct marlin2_wlan_data_message *message);
bool marlin2_wlan_event_for_mode(u8 common, u8 id, u8 mode);
bool marlin2_wlan_scan_flags_supported(u32 flags);
size_t marlin2_wlan_scan_build(u8 *payload, size_t capacity, u32 channels,
			       const struct marlin2_wlan_ssid *ssids,
			       size_t count);
void marlin2_wlan_connect_build(u8 payload[82], bool wpa2, const u8 *bssid,
				u8 channel, const u8 *ssid, size_t ssid_bytes);
int marlin2_wlan_key_build(u8 payload[35], u8 index, bool pairwise,
			   const u8 *mac, const u8 *sequence,
			   size_t sequence_bytes, const u8 key[16]);

#endif
