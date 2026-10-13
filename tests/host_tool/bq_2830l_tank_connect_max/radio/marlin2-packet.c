/* SPDX-License-Identifier: GPL-2.0-only */

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <linux/errno.h>

#include <marlin2-packet.h>

/* Literal protocol examples; this harness does not execute SDIO or firmware. */
static const u8 info_record[] = {
	0x7f, 0x14, 0x00, 0x20, 0x10, 0x01, 0x28, 0x00, 0x78, 0x56, 0x34,
	0x12, 0x00, 0x00, 0x00, 0x00, 0x32, 0x23, 0x00, 0x00, 0x07, 0x00,
	0x00, 0x00, 0x6e, 0xa0, 0x00, 0x00, 0x5f, 0x00, 0x00, 0x00, 0x7e,
	0x0f, 0x00, 0x00, 0x08, 0x08, 0x08, 0x00, 0x03, 0x00, 0x00, 0x00,
};

static void prepare_info(u8 *packet)
{
	memset(packet, 0, 2048);
	memcpy(packet, info_record, sizeof(info_record));
	packet[2040] = 40;
}

static void check_request(void)
{
	const u8 expected[] = {
		0x00, 0x06, 0x00, 0x20, 0x10, 0x01, 0x0c, 0x00, 0x78, 0x56,
		0x34, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00,
	};
	u8 packet[512];
	u8 zeroes[492] = {};

	marlin2_packet_build_get_info(packet, 0x12345678);
	assert(!memcmp(packet, expected, sizeof(expected)));
	assert(!memcmp(packet + 20, zeroes, sizeof(zeroes)));
}

static int check_response(void)
{
	struct marlin2_packet_result result = {};
	u8 packet[2048];
	int ret;

	prepare_info(packet);
	ret = marlin2_packet_decode(packet, sizeof(packet), 0x12345678,
				    &result);
	if (ret != 1) {
		fprintf(stderr, "40-byte GET_INFO decode returned %d\n", ret);
		return 1;
	}
	assert(result.info.chip_model == 0x2332);
	assert(result.info.chip_version == 7);
	assert(result.info.firmware_version == 0xa06e);
	assert(result.info.standards == 0x5f);
	assert(result.info.capabilities == 0xf7e);
	assert(result.info.max_ap_stations == 8);
	assert(result.info.max_acl_addresses == 8);
	assert(result.info.max_multicast_addresses == 8);
	assert(result.info.wnm_ft_support == 0);
	assert(result.info.extended_features == 3);
	assert(result.response_received);
	assert(result.response_length == 40);
	assert(!memcmp(result.response_header, info_record + 4, 12));
	assert(!memcmp(result.response_payload, info_record + 16, 28));
	assert(result.used_payload_bytes == 40);
	return 0;
}

static void check_response_errors(void)
{
	struct marlin2_packet_result result = {};
	u8 packet[2048];

	prepare_info(packet);
	assert(marlin2_packet_decode(packet, sizeof(packet), 0x87654321,
				     &result) == 0);
	assert(!result.response_received);
	packet[5] = 3;
	assert(marlin2_packet_decode(packet, sizeof(packet), 0x12345678,
				     &result) == 0);
	assert(!result.response_received);

	prepare_info(packet);
	packet[12] = 0xfe;
	assert(marlin2_packet_decode(packet, sizeof(packet), 0x12345678,
				     &result) == -EREMOTEIO);
	assert(result.status == -2);
	assert(result.response_length == 40);

	memset(&result, 0, sizeof(result));
	prepare_info(packet);
	packet[0] = 0x00;
	packet[1] = 0x12;
	packet[6] = 0x24;
	packet[2040] = 36;
	assert(marlin2_packet_decode(packet, sizeof(packet), 0x12345678,
				     &result) == -EPROTO);
	assert(result.response_length == 36);

	prepare_info(packet);
	packet[1] = 0x16;
	packet[6] = 0x2c;
	packet[2040] = 44;
	assert(marlin2_packet_decode(packet, sizeof(packet), 0x12345678,
				     &result) == -EPROTO);
}

static void check_record_boundaries(void)
{
	struct marlin2_packet_result result = {};
	u8 packet[2048];
	const u8 dummy[] = { 0x80, 0x01, 0x00, 0xf0, 0xaa, 0xbb, 0xcc, 0x00 };
	const u8 event[] = {
		0x00, 0x06, 0x00, 0x20, 0x01, 0x82, 0x0c, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	};

	memset(packet, 0, sizeof(packet));
	memcpy(packet, dummy, sizeof(dummy));
	memcpy(packet + 8, event, sizeof(event));
	memcpy(packet + 24, info_record, sizeof(info_record));
	packet[2040] = 52;
	assert(marlin2_packet_decode(packet, sizeof(packet), 0x12345678,
				     &result) == 1);
	assert(result.records == 2);
	assert(result.ignored_records == 1);

	memset(&result, 0, sizeof(result));
	prepare_info(packet);
	packet[0] = 0x80;
	packet[1] = 0xff;
	packet[2] = 0x7f;
	assert(marlin2_packet_decode(packet, sizeof(packet), 0x12345678,
				     &result) == -EBADMSG);
	assert(result.last_puh == 0x207fff80 && !result.records);

	prepare_info(packet);
	packet[2040] = 39;
	assert(marlin2_packet_decode(packet, sizeof(packet), 0x12345678,
				     &result) == -EBADMSG);
	assert(result.last_puh == 0x2000147f && !result.records);

	prepare_info(packet);
	packet[2] |= 0x80;
	assert(marlin2_packet_decode(packet, sizeof(packet), 0x12345678,
				     &result) == -EBADMSG);
	assert(result.last_puh == 0x2080147f && !result.records);
}

static void check_adaptive_reads(void)
{
	assert(marlin2_packet_next_read(0) == 2048);
	assert(marlin2_packet_next_read(36) == 512);
	assert(marlin2_packet_next_read(1024) == 1536);
	assert(marlin2_packet_next_read(20000) == 10240);
	assert(marlin2_packet_next_read(0xffffffff) == 10240);
}

static void check_diagnostics_and_early_match(void)
{
	struct marlin2_packet_result result = {
		.aggregates = 3,
		.records = 4,
		.ignored_records = 5,
		.last_puh = 0x11223344,
	};
	u8 packet[2048];

	prepare_info(packet);
	/* A matching reply ends this decoder before the malformed next record. */
	packet[44 + 2] = 0x80;
	packet[2040] = 41;
	packet[2044] = 1;
	packet[2045] = 2;
	assert(marlin2_packet_decode(packet, sizeof(packet), 0x12345678,
				     &result) == 1);
	assert(result.aggregates == 4 && result.records == 5);
	assert(result.ignored_records == 5 && result.last_puh == 0x2000147f);
	assert(result.used_payload_bytes == 41 &&
	       result.next_payload_bytes == 513);

	assert(marlin2_packet_decode(packet, sizeof(packet), 0x87654321,
				     &result) == -EBADMSG);
	assert(result.aggregates == 5 && result.records == 6);
	assert(result.ignored_records == 6 && result.last_puh == 0x00800000);
	assert(result.used_payload_bytes == 41 &&
	       result.next_payload_bytes == 513);

	memset(packet, 0, sizeof(packet));
	packet[2044] = 17;
	assert(!marlin2_packet_decode(packet, sizeof(packet), 0x12345678,
				      &result));
	assert(result.aggregates == 6 && result.records == 6);
	assert(result.ignored_records == 6 && result.last_puh == 0x00800000);
	assert(!result.used_payload_bytes && result.next_payload_bytes == 17);
	assert(marlin2_packet_decode(packet, 511, 0x12345678, &result) ==
	       -EINVAL);
	assert(marlin2_packet_decode(packet, 10241, 0x12345678, &result) ==
	       -EINVAL);
	assert(result.aggregates == 6 && result.next_payload_bytes == 17);
}

int main(void)
{
	check_request();
	if (check_response())
		return 1;
	check_response_errors();
	check_record_boundaries();
	check_adaptive_reads();
	check_diagnostics_and_early_match();
	puts("Marlin2 packet codec host cases passed");
	return 0;
}
