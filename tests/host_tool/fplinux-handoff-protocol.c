/* SPDX-License-Identifier: GPL-2.0-only */
/* Host C99 harness for the fixed bootstrap-to-bridge acknowledgement codec. */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../../platforms/ums9117/bootstrap/fplinux-handoff-protocol.h"

static const uint8_t session_id_vector[] = {
	0x03, 0x0a, 0x11, 0x18, 0x1f, 0x26, 0x2d, 0x34, 0x3b, 0x42, 0x49,
	0x50, 0x57, 0x5e, 0x65, 0x6c, 0x73, 0x7a, 0x81, 0x88, 0x8f, 0x96,
	0x9d, 0xa4, 0xab, 0xb2, 0xb9, 0xc0, 0xc7, 0xce, 0xd5, 0xdc,
};

static const uint8_t request_vector[] = {
	0x03, 0x0a, 0x11, 0x18, 0x1f, 0x26, 0x2d, 0x34, 0x3b, 0x42, 0x49, 0x50,
	0x57, 0x5e, 0x65, 0x6c, 0x73, 0x7a, 0x81, 0x88, 0x8f, 0x96, 0x9d, 0xa4,
	0xab, 0xb2, 0xb9, 0xc0, 0xc7, 0xce, 0xd5, 0xdc, 0xab, 0x91,
};

static const uint8_t ack_vector[] = {
	0x00, 0x03, 0x0a, 0x11, 0x18, 0x1f, 0x26, 0x2d, 0x34, 0x3b, 0x42, 0x49,
	0x50, 0x57, 0x5e, 0x65, 0x6c, 0x73, 0x7a, 0x81, 0x88, 0x8f, 0x96, 0x9d,
	0xa4, 0xab, 0xb2, 0xb9, 0xc0, 0xc7, 0xce, 0xd5, 0xdc, 0xab, 0x91,
};

static const uint8_t nack_vector[] = {
	0x01, 0x03, 0x0a, 0x11, 0x18, 0x1f, 0x26, 0x2d, 0x34, 0x3b, 0x42, 0x49,
	0x50, 0x57, 0x5e, 0x65, 0x6c, 0x73, 0x7a, 0x81, 0x88, 0x8f, 0x96, 0x9d,
	0xa4, 0xab, 0xb2, 0xb9, 0xc0, 0xc7, 0xce, 0xd5, 0xdc, 0xab, 0x92,
};

static const uint8_t other_session_ack_vector[] = {
	0x00, 0x03, 0x0a, 0x11, 0x18, 0x1f, 0x26, 0x2d, 0xb4, 0x3b, 0x42, 0x49,
	0x50, 0x57, 0x5e, 0x65, 0x6c, 0x73, 0x7a, 0x81, 0x88, 0x8f, 0x96, 0x9d,
	0xa4, 0xab, 0xb2, 0xb9, 0xc0, 0xc7, 0xce, 0xd5, 0xdc, 0xac, 0x11,
};

static int test_roundtrip(void)
{
	uint8_t request[sizeof(request_vector)];
	uint8_t response_storage[sizeof(ack_vector) + 1U];
	uint8_t *response = response_storage + 1U;

	if (FPLINUX_HANDOFF_SESSION_ID_BYTES != sizeof(session_id_vector) ||
	    FPLINUX_HANDOFF_REQUEST_PAYLOAD_BYTES != sizeof(request_vector) ||
	    FPLINUX_HANDOFF_RESPONSE_BYTES != sizeof(ack_vector))
		return EXIT_FAILURE;
	fplinux_handoff_encode_request(request, session_id_vector);
	if (memcmp(request, request_vector, sizeof(request_vector)) != 0)
		return EXIT_FAILURE;
	if (!fplinux_handoff_validate_request(request_vector,
					      session_id_vector))
		return EXIT_FAILURE;
	fplinux_handoff_encode_response(response, FPLINUX_HANDOFF_STATUS_ACK,
					session_id_vector);
	if (memcmp(response, ack_vector, sizeof(ack_vector)) != 0)
		return EXIT_FAILURE;
	if (fplinux_handoff_validate_response(ack_vector, session_id_vector) !=
	    FPLINUX_HANDOFF_RESPONSE_ACK)
		return EXIT_FAILURE;

	return EXIT_SUCCESS;
}

static int test_tamper(void)
{
	uint8_t request[sizeof(request_vector)];
	uint8_t response[sizeof(ack_vector)];

	memcpy(request, request_vector, sizeof(request));
	request[9] ^= 0x40U;
	if (fplinux_handoff_validate_request(request, session_id_vector))
		return EXIT_FAILURE;

	if (fplinux_handoff_validate_response(other_session_ack_vector,
					      session_id_vector) !=
	    FPLINUX_HANDOFF_RESPONSE_SESSION_MISMATCH)
		return EXIT_FAILURE;

	memcpy(response, ack_vector, sizeof(response));
	response[sizeof(response) - 1U] ^= 0x01U;
	if (fplinux_handoff_validate_response(response, session_id_vector) !=
	    FPLINUX_HANDOFF_RESPONSE_BAD_CHECKSUM)
		return EXIT_FAILURE;

	return EXIT_SUCCESS;
}

static int test_nack(void)
{
	if (fplinux_handoff_validate_response(nack_vector, session_id_vector) !=
	    FPLINUX_HANDOFF_RESPONSE_NACK)
		return EXIT_FAILURE;

	return EXIT_SUCCESS;
}

int main(int argc, char **argv)
{
	if (argc != 2)
		return EXIT_FAILURE;
	if (strcmp(argv[1], "roundtrip") == 0)
		return test_roundtrip();
	if (strcmp(argv[1], "tamper") == 0)
		return test_tamper();
	if (strcmp(argv[1], "nack") == 0)
		return test_nack();

	return EXIT_FAILURE;
}
