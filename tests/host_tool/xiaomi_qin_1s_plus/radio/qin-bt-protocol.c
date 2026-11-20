// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "qin1splus-bt-protocol.h"

struct received {
	u8 types[8];
	u8 bytes[8][32];
	size_t lengths[8];
	unsigned int count;
};

static int receive(void *context, u8 type, const u8 *data, size_t bytes)
{
	struct received *received = context;
	unsigned int index = received->count++;

	assert(index < 8 && bytes <= 32);
	received->types[index] = type;
	received->lengths[index] = bytes;
	memcpy(received->bytes[index], data, bytes);
	return 0;
}

static void transmit_literal_records(void)
{
	const u8 reset[] = { 3, 0x0c, 0 };
	const u8 aligned_acl[] = { 1, 0x20, 3, 0, 0x11, 0x22, 0x33 };
	const u8 sco[] = { 1, 0, 1, 0xaa };
	const u8 reset_wire[] = { 1, 3, 0x0c, 0, 0, 0, 0, 0 };
	const u8 acl_wire[] = {
		2, 1, 0x20, 3, 0, 0x11, 0x22, 0x33, 0, 0, 0, 0, 0, 0, 0, 0,
	};
	const u8 sco_wire[] = { 3, 1, 0, 1, 0xaa };
	u8 output[32];

	assert(qin_bt_pack(1, reset, sizeof(reset), output, sizeof(output)) ==
	       8);
	assert(!memcmp(output, reset_wire, 8));
	assert(qin_bt_pack(2, aligned_acl, 7, output, sizeof(output)) == 16);
	assert(!memcmp(output, acl_wire, 16));
	assert(qin_bt_pack(3, sco, 4, output, sizeof(output)) == 5);
	assert(!memcmp(output, sco_wire, 5));
	assert(qin_bt_pack(1, reset, 3, output, 7) == -ENOSPC);
	assert(qin_bt_pack(2, aligned_acl, 6, output, 32) == -EMSGSIZE);
	assert(qin_bt_pack(5, reset, 3, output, 32) == -EILSEQ);
}

static void transmit_setup_records(void)
{
	const u8 pskey[221] = { 0xa0, 0xfc, 218, 0x11, [220] = 0x22 };
	const u8 pskey_wire[224] = { 1, 0xa0, 0xfc, 218, 0x11, [221] = 0x22 };
	const u8 enable[] = { 0xa1, 0xfc, 3, 0, 0, 1 };
	const u8 enable_wire[] = { 1, 0xa1, 0xfc, 3, 0, 0, 1, 0 };
	const u8 disable[] = { 0xa1, 0xfc, 3, 0, 0, 0 };
	const u8 disable_wire[] = { 1, 0xa1, 0xfc, 3, 0, 0, 0, 0 };
	u8 output[224];

	assert(qin_bt_pack(1, pskey, sizeof(pskey), output, sizeof(output)) ==
	       224);
	assert(!memcmp(output, pskey_wire, sizeof(pskey_wire)));
	assert(qin_bt_pack(1, enable, sizeof(enable), output, sizeof(output)) ==
	       8);
	assert(!memcmp(output, enable_wire, sizeof(enable_wire)));
	assert(qin_bt_pack(1, disable, sizeof(disable), output,
			   sizeof(output)) == 8);
	assert(!memcmp(output, disable_wire, sizeof(disable_wire)));
}

static void receive_fragments_and_back_to_back_records(void)
{
	const u8 wire[] = {
		4,    0x0e, 0x0a, 1,	0xa0, 0xfc, 0,	  0x93, 0x61,
		0x25, 3,    0x15, 0x20, 4,    0x0e, 6,	  1,	0xa1,
		0xfc, 0,    0,	  1,	2,    1,    0x20, 3,	0,
		0x11, 0x22, 0x33, 3,	1,    0,    1,	  0xaa,
	};
	const u8 first_event[] = { 0x0e, 0x0a, 1,    0xa0, 0xfc, 0,
				   0x93, 0x61, 0x25, 3,	   0x15, 0x20 };
	const u8 second_event[] = { 0x0e, 6, 1, 0xa1, 0xfc, 0, 0, 1 };
	struct qin_bt_stream stream = {};
	struct received received = {};
	size_t split;

	/* Every possible split crosses the actual stream API, including headers. */
	for (split = 0; split <= sizeof(wire); split++) {
		memset(&stream, 0, sizeof(stream));
		memset(&received, 0, sizeof(received));
		assert(!qin_bt_stream_receive(&stream, wire, split, receive,
					      &received));
		assert(!qin_bt_stream_receive(&stream, wire + split,
					      sizeof(wire) - split, receive,
					      &received));
		assert(received.count == 4);
		assert(received.types[0] == 4 && received.lengths[0] == 12);
		assert(!memcmp(received.bytes[0], first_event, 12));
		assert(received.types[1] == 4 && received.lengths[1] == 8);
		assert(!memcmp(received.bytes[1], second_event, 8));
		assert(received.types[2] == 2 && received.lengths[2] == 7);
		assert(received.types[3] == 3 && received.lengths[3] == 4);
		assert(stream.type == 0 && stream.used == 0);
	}
	memset(&received, 0, sizeof(received));
	for (split = 0; split < sizeof(wire); split++)
		assert(!qin_bt_stream_receive(&stream, wire + split, 1, receive,
					      &received));
	assert(received.count == 4);
}

static void reject_unknown_padding_and_oversized_payload(void)
{
	const u8 padded[] = { 4, 0x0e, 0, 0 };
	const u8 oversized[] = { 2, 1, 0x20, 1, 4 };
	struct qin_bt_stream stream = {};
	struct received received = {};

	assert(qin_bt_stream_receive(&stream, padded, 4, receive, &received) ==
	       -EILSEQ);
	assert(received.count == 1 && stream.bad_type == 0);
	memset(&stream, 0, sizeof(stream));
	assert(qin_bt_stream_receive(&stream, oversized, 5, receive,
				     &received) == -EMSGSIZE);
	assert(stream.bad_length == 1029);
}

static void complete_return_parameters(void)
{
	u8 pskey[] = { 0, 0x93, 0x61, 0x25, 3, 0x15, 0x20 };
	u8 enabled[] = { 0, 0, 1 };
	const u8 disabled[] = { 0, 0, 0 };
	u8 event[] = { 0x0e, 6, 1, 0xa1, 0xfc, 0, 0, 1 };
	u8 pskey_event[] = { 0x0e, 0x0a, 1,    0xa0, 0xfc, 0,
			     0x93, 0x61, 0x25, 3,    0x15, 0x20 };
	size_t index;

	assert(!qin_bt_validate_pskey(pskey, 7));
	assert(qin_bt_validate_pskey(pskey, 1) == -EPROTO);
	for (index = 0; index < sizeof(pskey); index++) {
		pskey[index] ^= 0x80;
		assert(qin_bt_validate_pskey(pskey, 7) == -EPROTO);
		pskey[index] ^= 0x80;
	}
	assert(!qin_bt_validate_enable(enabled, 3, true));
	assert(!qin_bt_validate_enable(disabled, 3, false));
	assert(qin_bt_validate_enable(enabled, 1, true) == -EPROTO);
	assert(qin_bt_validate_enable(enabled, 3, false) == -EPROTO);
	enabled[1] = 1;
	assert(qin_bt_validate_enable(enabled, 3, true) == -EPROTO);
	assert(!qin_bt_validate_event(event, 8));
	event[2] = 2;
	assert(qin_bt_validate_event(event, 8) == -EPROTO);
	event[2] = 1;
	assert(qin_bt_validate_event(event, 7) == -EPROTO);
	event[1] = 7;
	assert(qin_bt_validate_event(event, 8) == -EPROTO);
	assert(!qin_bt_validate_event(pskey_event, 12));
	assert(qin_bt_validate_event(pskey_event, 11) == -EPROTO);
	pskey_event[1] = 9;
	assert(qin_bt_validate_event(pskey_event, 12) == -EPROTO);
	pskey_event[1] = 10;
	pskey_event[2] = 2;
	assert(qin_bt_validate_event(pskey_event, 12) == -EPROTO);
}

int main(void)
{
	transmit_literal_records();
	transmit_setup_records();
	receive_fragments_and_back_to_back_records();
	reject_unknown_padding_and_oversized_payload();
	complete_return_parameters();
	puts("Qin Bluetooth protocol host cases passed");
	return 0;
}
