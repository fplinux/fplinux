// SPDX-License-Identifier: GPL-2.0-only
#include <linux/errno.h>
#include <linux/string.h>

#include "marlin2-bluetooth-wire.h"

int marlin2_bluetooth_frame_length(const u8 *data, size_t bytes,
				   size_t *frame_bytes)
{
	size_t header_bytes;
	size_t payload_bytes;

	if (!bytes)
		return -EAGAIN;
	switch (data[0]) {
	case 1:
	case 3:
		header_bytes = 4;
		break;
	case 2:
		header_bytes = 5;
		break;
	case 4:
		header_bytes = 3;
		break;
	default:
		return -EPROTO;
	}
	if (bytes < header_bytes)
		return -EAGAIN;
	if (data[0] == 2)
		payload_bytes = data[3] | ((size_t)data[4] << 8);
	else
		payload_bytes = data[header_bytes - 1];
	*frame_bytes = header_bytes + payload_bytes;
	return 0;
}

int marlin2_bluetooth_tx_build(u8 *data, size_t capacity, u8 type,
			       const u8 *payload, size_t payload_bytes,
			       size_t *wire_bytes,
			       enum marlin2_bluetooth_tx_path *path)
{
	size_t header_bytes;
	size_t declared_bytes;
	size_t frame_bytes;
	size_t aligned_bytes;

	switch (type) {
	case 1:
	case 3:
		header_bytes = 3;
		break;
	case 2:
		header_bytes = 4;
		break;
	default:
		return -EPROTO;
	}
	if (payload_bytes < header_bytes)
		return -EPROTO;
	if (type == 2)
		declared_bytes = payload[2] | ((size_t)payload[3] << 8);
	else
		declared_bytes = payload[2];
	if (declared_bytes != payload_bytes - header_bytes)
		return -EPROTO;
	frame_bytes = 1 + header_bytes + declared_bytes;
	aligned_bytes = type == 3 ? frame_bytes : (frame_bytes + 7) & ~7UL;
	if (aligned_bytes > capacity)
		return -EMSGSIZE;
	data[0] = type;
	memcpy(data + 1, payload, payload_bytes);
	memset(data + frame_bytes, 0, aligned_bytes - frame_bytes);
	*wire_bytes = aligned_bytes;
	*path = type == 3 ? MARLIN2_BLUETOOTH_PACKET : MARLIN2_BLUETOOTH_DIRECT;
	return 0;
}

int marlin2_bluetooth_pskey_build(u8 pskey[MARLIN2_BLUETOOTH_PSKEY_BYTES],
				  const u8 *configuration, size_t bytes,
				  const u8 device_address[6])
{
	size_t index;

	if (bytes != MARLIN2_BLUETOOTH_CONFIG_BYTES)
		return -EINVAL;
	memset(pskey, 0, MARLIN2_BLUETOOTH_PSKEY_BYTES);
	memcpy(pskey, configuration + 0x26c, 0x3c);
	memcpy(pskey + 0x3c, configuration + 0x310, 0x10);
	memcpy(pskey + 0x4c, configuration + 0x320, 0x38);
	memcpy(pskey + 0x4c, device_address, 6);
	memcpy(pskey + 0x84, configuration + 0x2a8, 0x20);
	memcpy(pskey + 0xa4, configuration + 0x2c8, 0x20);
	memcpy(pskey + 0xc4, configuration + 0x2e8, 4);
	for (index = 0; index < 8; index++)
		memcpy(pskey + 0xc8 + index * 2,
		       configuration + 0x2ec + index * 4, 2);
	return 0;
}

int marlin2_bluetooth_setup_reply(u16 opcode, u16 mode, u8 state,
				  const u8 *event, size_t bytes)
{
	u16 returned_opcode;
	u16 returned_mode;

	if (bytes < 3 || event[0] != 4)
		return -EAGAIN;
	if (event[1] == 0x0e) {
		if (bytes < 6)
			return -EAGAIN;
		returned_opcode = event[4] | ((u16)event[5] << 8);
	} else if (event[1] == 0x0f) {
		if (bytes < 7)
			return -EAGAIN;
		returned_opcode = event[5] | ((u16)event[6] << 8);
	} else {
		return -EAGAIN;
	}
	if (returned_opcode != opcode)
		return -EAGAIN;
	if ((size_t)event[2] + 3 != bytes)
		return -EPROTO;
	if (event[1] == 0x0f)
		return event[3] ? -EREMOTEIO : -EINPROGRESS;
	switch (opcode) {
	case MARLIN2_BLUETOOTH_WRITE_PSKEY:
		if (bytes < 7)
			return -EPROTO;
		return event[6] ? -EREMOTEIO : 0;
	case MARLIN2_BLUETOOTH_MODULE_ENABLE:
		/* The return parameters contain mode and state, without status. */
		if (bytes < 9)
			return -EPROTO;
		returned_mode = event[6] | ((u16)event[7] << 8);
		return returned_mode == mode && event[8] == state ? 0 :
								    -EREMOTEIO;
	default:
		return -EINVAL;
	}
}
