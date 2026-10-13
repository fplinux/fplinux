/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2015 Spreadtrum Communications Inc. */

#ifndef FPLINUX_MARLIN2_PACKET_H
#define FPLINUX_MARLIN2_PACKET_H

#include <linux/types.h>

#define MARLIN2_PACKET_TX_BYTES 512U
#define MARLIN2_PACKET_RX_INITIAL_BYTES 2048U
#define MARLIN2_PACKET_RX_MAX_BYTES 10240U

struct gpio_desc;
struct sdio_func;

struct marlin2_info {
	u32 chip_model;
	u32 chip_version;
	u32 firmware_version;
	u32 standards;
	u32 capabilities;
	u8 max_ap_stations;
	u8 max_acl_addresses;
	u8 max_multicast_addresses;
	u8 wnm_ft_support;
	u32 extended_features;
};

struct marlin2_packet_result {
	struct marlin2_info info;
	u32 token;
	u32 aggregates;
	u32 records;
	u32 ignored_records;
	u32 used_payload_bytes;
	u32 next_payload_bytes;
	u32 last_puh;
	u16 response_length;
	u8 response_header[12];
	u8 response_payload[28];
	s8 status;
	bool multi_block;
	bool request_sent;
	bool rx_request_seen;
	bool response_received;
};

void marlin2_packet_build_get_info(u8 packet[MARLIN2_PACKET_TX_BYTES],
				   u32 token);
int marlin2_packet_decode(const u8 *packet, size_t read_bytes, u32 token,
			  struct marlin2_packet_result *result);
size_t marlin2_packet_next_read(u32 pending_payload_bytes);

/*
 * Caller owns power and holds the SDIO host claim. Function 1 is enabled,
 * uses 512-byte blocks, and the firmware is awake. rx_request is its dedicated
 * packet request input. The caller retains interrupt and power cleanup.
 */
int marlin2_packet_get_info_locked(struct sdio_func *func,
				   struct gpio_desc *rx_request, u32 token,
				   unsigned long deadline,
				   struct marlin2_packet_result *result);

#endif
