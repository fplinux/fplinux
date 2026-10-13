/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <linux/errno.h>

#include "marlin2-bluetooth-wire.h"

static void command_reset_uses_eight_byte_direct_write(void)
{
	const u8 reset[] = { 0x03, 0x0c, 0x00 };
	const u8 expected[] = { 0x01, 0x03, 0x0c, 0x00, 0, 0, 0, 0 };
	enum marlin2_bluetooth_tx_path path;
	size_t bytes;
	u8 data[16];

	memset(data, 0xa5, sizeof(data));
	assert(!marlin2_bluetooth_tx_build(data, sizeof(data), 1, reset,
					   sizeof(reset), &bytes, &path));
	assert(bytes == 8 && path == MARLIN2_BLUETOOTH_DIRECT);
	assert(!memcmp(data, expected, sizeof(expected)));
}

static void acl_preserves_handle_length_and_data(void)
{
	const u8 acl[] = { 0x01, 0x20, 0x04, 0x00, 0xde, 0xad, 0xbe, 0xef };
	const u8 expected[] = { 0x02, 0x01, 0x20, 0x04, 0x00, 0xde, 0xad, 0xbe,
				0xef, 0,    0,	  0,	0,    0,    0,	  0 };
	enum marlin2_bluetooth_tx_path path;
	size_t bytes;
	u8 data[16];

	assert(!marlin2_bluetooth_tx_build(data, sizeof(data), 2, acl,
					   sizeof(acl), &bytes, &path));
	assert(bytes == 16 && path == MARLIN2_BLUETOOTH_DIRECT);
	assert(!memcmp(data, expected, sizeof(expected)));
}

static void sco_uses_packet_path_without_direct_padding(void)
{
	const u8 sco[] = { 0x01, 0x00, 0x02, 0x11, 0x22 };
	const u8 expected[] = { 0x03, 0x01, 0x00, 0x02, 0x11, 0x22 };
	enum marlin2_bluetooth_tx_path path;
	size_t bytes;
	u8 data[16];

	assert(!marlin2_bluetooth_tx_build(data, sizeof(data), 3, sco,
					   sizeof(sco), &bytes, &path));
	assert(bytes == 6 && path == MARLIN2_BLUETOOTH_PACKET);
	assert(!memcmp(data, expected, sizeof(expected)));
}

static void inconsistent_or_truncated_payload_is_rejected(void)
{
	const u8 truncated_acl[] = { 0x01, 0x20, 0x04, 0x00, 0xde };
	const u8 reset[] = { 0x03, 0x0c, 0x00 };
	enum marlin2_bluetooth_tx_path path;
	size_t bytes;
	u8 data[16];

	assert(marlin2_bluetooth_tx_build(data, sizeof(data), 2, truncated_acl,
					  sizeof(truncated_acl), &bytes,
					  &path) == -EPROTO);
	assert(marlin2_bluetooth_tx_build(data, 7, 1, reset, sizeof(reset),
					  &bytes, &path) == -EMSGSIZE);
	assert(marlin2_bluetooth_tx_build(data, sizeof(data), 4, reset,
					  sizeof(reset), &bytes,
					  &path) == -EPROTO);
}

static void h4_lengths_use_packet_specific_headers(void)
{
	const u8 reset_complete[] = { 4, 0x0e, 4, 1, 3, 0x0c, 0 };
	const u8 acl_header[] = { 2, 1, 0x20, 0x01, 0x01 };
	const u8 sco_header[] = { 3, 1, 0, 2 };
	const u8 command_header[] = { 1, 3, 0x0c, 0 };
	const u8 invalid_type[] = { 5, 0, 0, 0, 0 };
	size_t bytes;
	size_t prefix;

	for (prefix = 0; prefix < 3; prefix++)
		assert(marlin2_bluetooth_frame_length(reset_complete, prefix,
						      &bytes) == -EAGAIN);
	assert(!marlin2_bluetooth_frame_length(reset_complete, 3, &bytes));
	assert(bytes == 7);
	assert(!marlin2_bluetooth_frame_length(acl_header, 5, &bytes));
	assert(bytes == 262);
	assert(!marlin2_bluetooth_frame_length(sco_header, 4, &bytes));
	assert(bytes == 6);
	assert(!marlin2_bluetooth_frame_length(command_header, 4, &bytes));
	assert(bytes == 4);
	assert(marlin2_bluetooth_frame_length(invalid_type, 5, &bytes) ==
	       -EPROTO);
}

static void module_enable_echo_is_not_a_standard_status(void)
{
	const u8 enabled[] = { 4, 0x0e, 6, 1, 0xa1, 0xfc, 1, 0, 1 };
	const u8 disabled[] = { 4, 0x0e, 6, 1, 0xa1, 0xfc, 0, 0, 0 };
	const u8 wrong_mode[] = { 4, 0x0e, 6, 1, 0xa1, 0xfc, 2, 0, 1 };
	const u8 unrelated[] = { 4, 0x0e, 4, 1, 3, 0x0c, 0 };
	const u8 accepted[] = { 4, 0x0f, 4, 0, 1, 0xa1, 0xfc };
	const u8 rejected[] = { 4, 0x0f, 4, 0x0c, 1, 0xa1, 0xfc };
	const u8 pskey_ok[] = { 4, 0x0e, 4, 1, 0xa0, 0xfc, 0 };
	const u8 pskey_bad[] = { 4, 0x0e, 4, 1, 0xa0, 0xfc, 0x12 };

	assert(!marlin2_bluetooth_setup_reply(0xfca1, 1, 1, enabled, 9));
	assert(!marlin2_bluetooth_setup_reply(0xfca1, 0, 0, disabled, 9));
	assert(marlin2_bluetooth_setup_reply(0xfca1, 1, 1, wrong_mode, 9) ==
	       -EREMOTEIO);
	assert(marlin2_bluetooth_setup_reply(0xfca1, 1, 1, unrelated, 7) ==
	       -EAGAIN);
	assert(marlin2_bluetooth_setup_reply(0xfca1, 1, 1, accepted, 7) ==
	       -EINPROGRESS);
	assert(marlin2_bluetooth_setup_reply(0xfca1, 1, 1, rejected, 7) ==
	       -EREMOTEIO);
	assert(!marlin2_bluetooth_setup_reply(0xfca0, 0, 0, pskey_ok, 7));
	assert(marlin2_bluetooth_setup_reply(0xfca0, 0, 0, pskey_bad, 7) ==
	       -EREMOTEIO);
}

int main(void)
{
	command_reset_uses_eight_byte_direct_write();
	acl_preserves_handle_length_and_data();
	sco_uses_packet_path_without_direct_padding();
	inconsistent_or_truncated_payload_is_rejected();
	h4_lengths_use_packet_specific_headers();
	module_enable_echo_is_not_a_standard_status();
	puts("Marlin2 Bluetooth wire host cases passed");
	return 0;
}
