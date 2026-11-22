/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_MARLIN2_BLUETOOTH_WIRE_H
#define FPLINUX_MARLIN2_BLUETOOTH_WIRE_H

#include <linux/types.h>

#define MARLIN2_BLUETOOTH_PSKEY_BYTES 220U
#define MARLIN2_BLUETOOTH_WRITE_PSKEY 0xfca0
#define MARLIN2_BLUETOOTH_MODULE_ENABLE 0xfca1

enum marlin2_bluetooth_tx_path {
	MARLIN2_BLUETOOTH_DIRECT,
	MARLIN2_BLUETOOTH_PACKET,
};

/* frame_bytes includes the H4 packet indicator. */
int marlin2_bluetooth_frame_length(const u8 *data, size_t bytes,
				   size_t *frame_bytes);
int marlin2_bluetooth_tx_build(u8 *data, size_t capacity, u8 type,
			       const u8 *payload, size_t payload_bytes,
			       size_t *wire_bytes,
			       enum marlin2_bluetooth_tx_path *path);
/* A successful Command Status still requires Command Complete. */
int marlin2_bluetooth_setup_reply(u16 opcode, u16 mode, u8 state,
				  const u8 *event, size_t bytes);

#endif
