/* SPDX-License-Identifier: GPL-2.0-only */

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "marlin2-transport-wire.h"

struct received {
	unsigned int count;
	u8 types[3];
	u8 subtypes[3];
	u8 payloads[3][40];
	size_t lengths[3];
};

static int collect(void *context,
		   const struct marlin2_transport_message *message)
{
	struct received *received = context;
	unsigned int index = received->count++;

	assert(index < 3);
	assert(message->length <= sizeof(received->payloads[index]));
	received->types[index] = message->type;
	received->subtypes[index] = message->subtype;
	received->lengths[index] = message->length;
	memcpy(received->payloads[index], message->data, message->length);
	return 0;
}

static void check_single_records(void)
{
	static const u8 sco[] = { 0x03, 0x01, 0x00, 0x03, 0x10, 0x20, 0x30 };
	static const u8 expected_sco[] = {
		0x80, 0x03, 0x00, 0x01, 0x03, 0x01, 0x00, 0x03,
		0x10, 0x20, 0x30, 0x00, 0x00, 0x00, 0x80, 0x00,
	};
	static const u8 fm[] = { 0x01, 0x8c, 0xfc, 0x03, 0x00, 0x00, 0x01 };
	static const u8 expected_fm[] = {
		0x80, 0x03, 0x00, 0x10, 0x01, 0x8c, 0xfc, 0x03,
		0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x80, 0x00,
	};
	static const u8 wlan[] = {
		0x10, 0x01, 0x0c, 0x00, 0x44, 0x33,
		0x22, 0x11, 0x00, 0x00, 0x00, 0x00,
	};
	static const u8 expected_wlan[] = {
		0x00, 0x06, 0x00, 0x20, 0x10, 0x01, 0x0c, 0x00, 0x44, 0x33,
		0x22, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00,
	};
	u8 packet[1024];
	u8 boundary[505] = {};
	size_t index;

	assert(marlin2_transport_record_build(packet, sizeof(packet), 0, 1, sco,
					      sizeof(sco)) == 512);
	assert(!memcmp(packet, expected_sco, sizeof(expected_sco)));
	for (index = sizeof(expected_sco); index < 512; index++)
		assert(!packet[index]);
	assert(marlin2_transport_record_build(packet, sizeof(packet), 1, 0, fm,
					      sizeof(fm)) == 512);
	assert(!memcmp(packet, expected_fm, sizeof(expected_fm)));
	assert(marlin2_transport_record_build(packet, sizeof(packet), 2, 0,
					      wlan, sizeof(wlan)) == 512);
	assert(!memcmp(packet, expected_wlan, sizeof(expected_wlan)));
	assert(marlin2_transport_record_build(packet, 512, 1, 0, boundary,
					      504) == 512);
	assert(!marlin2_transport_record_build(packet, 512, 1, 0, boundary,
					       505));
	assert(marlin2_transport_record_build(packet, 1024, 1, 0, boundary,
					      505) == 1024);
	assert(!marlin2_transport_record_build(packet, 1024, 1, 0, boundary,
					       0));
	assert(!marlin2_transport_record_build(packet, 1024, 1, 4, boundary,
					       1));
	assert(!marlin2_transport_record_build(packet, 1024, 4, 0, boundary,
					       1));
}

static void check_aggregate_dispatch(void)
{
	static const u8 records[] = {
		0x80, 0x01, 0x00, 0xf0, 0xd0, 0xd1, 0xd2, 0x00, 0x80, 0x03,
		0x00, 0x00, 0x04, 0x0e, 0x04, 0x01, 0x03, 0x0c, 0x00, 0x00,
		0x80, 0x03, 0x00, 0x10, 0x04, 0x0e, 0x04, 0x01, 0x8c, 0xfc,
		0x00, 0x00, 0x00, 0x06, 0x00, 0x20, 0x00, 0x01, 0x0c, 0x00,
		0x44, 0x33, 0x22, 0x11, 0x00, 0x00, 0x00, 0x00,
	};
	static const u8 expected_bt[] = { 0x04, 0x0e, 0x04, 0x01,
					  0x03, 0x0c, 0x00 };
	static const u8 expected_fm[] = { 0x04, 0x0e, 0x04, 0x01,
					  0x8c, 0xfc, 0x00 };
	static const u8 expected_wlan[] = {
		0x00, 0x01, 0x0c, 0x00, 0x44, 0x33,
		0x22, 0x11, 0x00, 0x00, 0x00, 0x00,
	};
	u8 packet[512] = {};
	struct received received = {};
	u32 next = 0;

	memcpy(packet, records, sizeof(records));
	/* The used count is 26 payload bytes; physical records occupy 48 bytes. */
	packet[504] = 0x1a;
	packet[508] = 0x01;
	packet[509] = 0x02;
	assert(!marlin2_transport_aggregate_decode(packet, sizeof(packet),
						   collect, &received, &next));
	assert(next == 513);
	assert(received.count == 3);
	assert(received.types[0] == 0 && received.subtypes[0] == 0);
	assert(received.types[1] == 1 && received.subtypes[1] == 0);
	assert(received.types[2] == 2 && received.subtypes[2] == 0);
	assert(received.lengths[0] == 7 && received.lengths[1] == 7 &&
	       received.lengths[2] == 12);
	assert(!memcmp(received.payloads[0], expected_bt, sizeof(expected_bt)));
	assert(!memcmp(received.payloads[1], expected_fm, sizeof(expected_fm)));
	assert(!memcmp(received.payloads[2], expected_wlan,
		       sizeof(expected_wlan)));

	received.count = 0;
	packet[10] = 0x80;
	assert(marlin2_transport_aggregate_decode(packet, sizeof(packet),
						  collect, &received,
						  &next) == -EBADMSG);
	assert(!received.count);
	packet[10] = 0;
	packet[504] = 0x06;
	assert(marlin2_transport_aggregate_decode(packet, sizeof(packet),
						  collect, &received,
						  &next) == -EBADMSG);
	assert(!received.count);
	memset(packet, 0, sizeof(packet));
	assert(!marlin2_transport_aggregate_decode(packet, sizeof(packet),
						   collect, &received, &next));
	assert(!received.count && !next);
	assert(marlin2_transport_aggregate_decode(packet, 511, collect,
						  &received, &next) == -EINVAL);
}

static void check_wlan_aggregate_drain(void)
{
	/* A command response and a flow event share the same 2048-byte read. */
	u8 packet[2048] = {
		0x7f,	     0x14, 0x00,	0x20, 0x20,	   0x01,
		0x28,	     0x00, 0x78,	0x56, 0x34,	   0x12,
		[44] = 0x00, 0x08, 0x00,	0x20, 0x21,	   0xb3,
		0x10,	     0x00, [60] = 0x08, 0x04, [2040] = 56, [2044] = 1,
	};
	const u8 expected_response[40] = {
		0x20, 0x01, 0x28, 0x00, 0x78, 0x56, 0x34, 0x12,
	};
	const u8 expected_flow[16] = {
		0x21, 0xb3, 0x10, 0x00, [12] = 0x08, 0x04,
	};
	struct received received = {};
	u32 next = 0;

	assert(!marlin2_transport_aggregate_decode(packet, sizeof(packet),
						   collect, &received, &next));
	assert(received.count == 2);
	assert(received.types[0] == 2 && received.subtypes[0] == 0);
	assert(received.types[1] == 2 && received.subtypes[1] == 0);
	assert(received.lengths[0] == 40 && received.lengths[1] == 16);
	assert(!memcmp(received.payloads[0], expected_response, 40));
	assert(!memcmp(received.payloads[1], expected_flow, 16));
	assert(next == 1);

	received.count = 0;
	packet[44] = 0;
	packet[45] = 0;
	packet[46] = 0x80;
	assert(marlin2_transport_aggregate_decode(packet, sizeof(packet),
						  collect, &received,
						  &next) == -EBADMSG);
	assert(received.count == 1);
}

int main(void)
{
	check_single_records();
	check_aggregate_dispatch();
	check_wlan_aggregate_drain();
	puts("Marlin2 shared wire host cases passed");
	return 0;
}
