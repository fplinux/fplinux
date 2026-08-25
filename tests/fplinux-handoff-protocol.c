/* SPDX-License-Identifier: GPL-2.0-only */
/* Host C99 harness for the fixed bootstrap-to-bridge acknowledgement codec. */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../platforms/ums9117/bootstrap/fplinux-handoff-protocol.h"

static void
fill_session_id(uint8_t session_id[FPLINUX_HANDOFF_SESSION_ID_BYTES])
{
	size_t index;

	for (index = 0; index < FPLINUX_HANDOFF_SESSION_ID_BYTES; ++index)
		session_id[index] = (uint8_t)(index * 7U + 3U);
}

static int test_roundtrip(void)
{
	uint8_t session_id[FPLINUX_HANDOFF_SESSION_ID_BYTES];
	uint8_t request[FPLINUX_HANDOFF_REQUEST_PAYLOAD_BYTES];
	uint8_t response[FPLINUX_HANDOFF_RESPONSE_BYTES];

	fill_session_id(session_id);
	fplinux_handoff_encode_request(request, session_id);
	if (request[FPLINUX_HANDOFF_SESSION_ID_BYTES] != 0xabU ||
	    request[FPLINUX_HANDOFF_SESSION_ID_BYTES + 1U] != 0x91U)
		return EXIT_FAILURE;
	if (!fplinux_handoff_validate_request(request, session_id))
		return EXIT_FAILURE;
	fplinux_handoff_encode_response(response, FPLINUX_HANDOFF_STATUS_ACK,
					session_id);
	if (response[FPLINUX_HANDOFF_RESPONSE_BYTES - 2U] != 0xabU ||
	    response[FPLINUX_HANDOFF_RESPONSE_BYTES - 1U] != 0x91U)
		return EXIT_FAILURE;
	if (fplinux_handoff_validate_response(response, session_id) !=
	    FPLINUX_HANDOFF_RESPONSE_ACK)
		return EXIT_FAILURE;

	return EXIT_SUCCESS;
}

static int test_tamper(void)
{
	uint8_t session_id[FPLINUX_HANDOFF_SESSION_ID_BYTES];
	uint8_t other_session_id[FPLINUX_HANDOFF_SESSION_ID_BYTES];
	uint8_t request[FPLINUX_HANDOFF_REQUEST_PAYLOAD_BYTES];
	uint8_t response[FPLINUX_HANDOFF_RESPONSE_BYTES];

	fill_session_id(session_id);
	fplinux_handoff_encode_request(request, session_id);
	request[9] ^= 0x40U;
	if (fplinux_handoff_validate_request(request, session_id))
		return EXIT_FAILURE;

	memcpy(other_session_id, session_id, sizeof(other_session_id));
	other_session_id[7] ^= 0x80U;
	fplinux_handoff_encode_response(response, FPLINUX_HANDOFF_STATUS_ACK,
					other_session_id);
	if (fplinux_handoff_validate_response(response, session_id) !=
	    FPLINUX_HANDOFF_RESPONSE_SESSION_MISMATCH)
		return EXIT_FAILURE;

	fplinux_handoff_encode_response(response, FPLINUX_HANDOFF_STATUS_ACK,
					session_id);
	response[FPLINUX_HANDOFF_RESPONSE_BYTES - 1U] ^= 0x01U;
	if (fplinux_handoff_validate_response(response, session_id) !=
	    FPLINUX_HANDOFF_RESPONSE_BAD_CHECKSUM)
		return EXIT_FAILURE;

	return EXIT_SUCCESS;
}

static int test_nack(void)
{
	uint8_t session_id[FPLINUX_HANDOFF_SESSION_ID_BYTES];
	uint8_t response[FPLINUX_HANDOFF_RESPONSE_BYTES];

	fill_session_id(session_id);
	fplinux_handoff_encode_response(response, FPLINUX_HANDOFF_STATUS_NACK,
					session_id);
	if (fplinux_handoff_validate_response(response, session_id) !=
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
