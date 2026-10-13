/* SPDX-License-Identifier: GPL-2.0-only */
/* Fixed binary acknowledgement exchanged before a RAM boot disconnects USB. */
#ifndef FPLINUX_HANDOFF_PROTOCOL_H
#define FPLINUX_HANDOFF_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#define FPLINUX_HANDOFF_OPCODE 0x8aU
#define FPLINUX_HANDOFF_CHECKSUM_INIT 0x5a5aU
#define FPLINUX_HANDOFF_SESSION_ID_BYTES 32U
#define FPLINUX_HANDOFF_REQUEST_PAYLOAD_BYTES \
	(FPLINUX_HANDOFF_SESSION_ID_BYTES + 2U)
#define FPLINUX_HANDOFF_RESPONSE_BYTES \
	(1U + FPLINUX_HANDOFF_SESSION_ID_BYTES + 2U)
#define FPLINUX_HANDOFF_ACK_TIMEOUT_MS 5000U
#define FPLINUX_HANDOFF_STATUS_ACK 0U
#define FPLINUX_HANDOFF_STATUS_NACK 1U

enum fplinux_handoff_response_status {
	FPLINUX_HANDOFF_RESPONSE_ACK = 0,
	FPLINUX_HANDOFF_RESPONSE_NACK,
	FPLINUX_HANDOFF_RESPONSE_SESSION_MISMATCH,
	FPLINUX_HANDOFF_RESPONSE_BAD_CHECKSUM,
};

static inline uint16_t
fplinux_handoff_fastchk16(uint32_t checksum, const uint8_t *bytes, size_t count)
{
	const volatile uint8_t *cursor = bytes;

	while (count > 1U) {
		checksum += (uint32_t)cursor[0] | ((uint32_t)cursor[1] << 8);
		cursor += 2;
		count -= 2U;
	}
	if (count != 0U)
		checksum += cursor[0];
	checksum = (checksum >> 16) + (checksum & 0xffffU);
	checksum += checksum >> 16;

	return (uint16_t)checksum;
}

static inline void fplinux_handoff_write_le16(uint8_t *destination,
					      uint16_t value)
{
	volatile uint8_t *bytes = destination;

	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
}

static inline uint16_t fplinux_handoff_read_le16(const uint8_t *source)
{
	const volatile uint8_t *bytes = source;

	return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
}

static inline uint16_t fplinux_handoff_request_checksum(
	const uint8_t session_id[FPLINUX_HANDOFF_SESSION_ID_BYTES])
{
	return fplinux_handoff_fastchk16(
		FPLINUX_HANDOFF_CHECKSUM_INIT + FPLINUX_HANDOFF_OPCODE,
		session_id, FPLINUX_HANDOFF_SESSION_ID_BYTES);
}

static inline uint16_t fplinux_handoff_response_checksum(
	uint8_t status,
	const uint8_t session_id[FPLINUX_HANDOFF_SESSION_ID_BYTES])
{
	return fplinux_handoff_fastchk16(
		FPLINUX_HANDOFF_CHECKSUM_INIT + FPLINUX_HANDOFF_OPCODE +
			((uint32_t)status << 8),
		session_id, FPLINUX_HANDOFF_SESSION_ID_BYTES);
}

static inline void fplinux_handoff_encode_request(
	uint8_t payload[FPLINUX_HANDOFF_REQUEST_PAYLOAD_BYTES],
	const uint8_t session_id[FPLINUX_HANDOFF_SESSION_ID_BYTES])
{
	size_t index;

	for (index = 0; index < FPLINUX_HANDOFF_SESSION_ID_BYTES; ++index)
		payload[index] = session_id[index];
	fplinux_handoff_write_le16(
		payload + FPLINUX_HANDOFF_SESSION_ID_BYTES,
		fplinux_handoff_request_checksum(session_id));
}

static inline void fplinux_handoff_encode_response(
	uint8_t response[FPLINUX_HANDOFF_RESPONSE_BYTES], uint8_t status,
	const uint8_t session_id[FPLINUX_HANDOFF_SESSION_ID_BYTES])
{
	size_t index;

	response[0] = status;
	for (index = 0; index < FPLINUX_HANDOFF_SESSION_ID_BYTES; ++index)
		response[1U + index] = session_id[index];
	fplinux_handoff_write_le16(
		response + 1U + FPLINUX_HANDOFF_SESSION_ID_BYTES,
		fplinux_handoff_response_checksum(status, session_id));
}

static inline int fplinux_handoff_validate_request(
	const uint8_t payload[FPLINUX_HANDOFF_REQUEST_PAYLOAD_BYTES],
	const uint8_t expected_session_id[FPLINUX_HANDOFF_SESSION_ID_BYTES])
{
	size_t index;

	if (fplinux_handoff_read_le16(payload +
				      FPLINUX_HANDOFF_SESSION_ID_BYTES) !=
	    fplinux_handoff_request_checksum(payload))
		return 0;
	for (index = 0; index < FPLINUX_HANDOFF_SESSION_ID_BYTES; ++index) {
		if (payload[index] != expected_session_id[index])
			return 0;
	}

	return 1;
}

static inline enum fplinux_handoff_response_status
fplinux_handoff_validate_response(
	const uint8_t response[FPLINUX_HANDOFF_RESPONSE_BYTES],
	const uint8_t expected_session_id[FPLINUX_HANDOFF_SESSION_ID_BYTES])
{
	const uint8_t status = response[0];
	const uint8_t *session_id = response + 1U;
	const uint16_t expected_checksum =
		fplinux_handoff_response_checksum(status, session_id);
	size_t index;

	if (fplinux_handoff_read_le16(response + 1U +
				      FPLINUX_HANDOFF_SESSION_ID_BYTES) !=
	    expected_checksum)
		return FPLINUX_HANDOFF_RESPONSE_BAD_CHECKSUM;
	for (index = 0; index < FPLINUX_HANDOFF_SESSION_ID_BYTES; ++index) {
		if (session_id[index] != expected_session_id[index])
			return FPLINUX_HANDOFF_RESPONSE_SESSION_MISMATCH;
	}
	if (status != FPLINUX_HANDOFF_STATUS_ACK)
		return FPLINUX_HANDOFF_RESPONSE_NACK;

	return FPLINUX_HANDOFF_RESPONSE_ACK;
}

#endif
