// SPDX-License-Identifier: GPL-2.0-only
/* Production H4 parser with literal packets; no Bluetooth core or radio fake. */
#include <assert.h>
#include <linux/errno.h>
#include <stdio.h>
#include <string.h>

#include "cm4-h4.h"

static void mixed_frames(void)
{
	/* One complete wire packet per line. */
	/* clang-format off */
	static const unsigned char input[] = {
		4, 0x0e, 4, 1, 3, 0x0c, 0,
		2, 1, 0x20, 4, 0, 0xaa, 4, 0x0e, 0xbb,
		3, 1, 0, 2, 0xcc, 0xdd,
		4, 0xff, 0,
	};
	/* clang-format on */
	static const struct {
		unsigned char type;
		unsigned int size;
		unsigned char data[8];
	} expected[] = {
		{ 4, 6, { 0x0e, 4, 1, 3, 0x0c, 0 } },
		{ 2, 8, { 1, 0x20, 4, 0, 0xaa, 4, 0x0e, 0xbb } },
		{ 3, 5, { 1, 0, 2, 0xcc, 0xdd } },
		{ 4, 2, { 0xff, 0 } },
	};
	struct ums9117_h4_rx rx = { 0 };
	unsigned int frames = 0;
	size_t i;
	int ret;

	for (i = 0; i < sizeof(input); i++) {
		ret = ums9117_h4_rx_byte(&rx, input[i]);
		assert(ret >= 0);
		if (!ret)
			continue;
		assert(frames < 4);
		assert(rx.type == expected[frames].type);
		assert(rx.length == expected[frames].size);
		assert(!memcmp(rx.data, expected[frames].data, rx.length));
		frames++;
		ums9117_h4_rx_reset(&rx);
	}
	assert(frames == 4);
}

static void acl_size_boundary(void)
{
	static const unsigned char header[] = { 2, 1, 0x20, 0, 4 };
	struct ums9117_h4_rx rx = { 0 };
	size_t i;
	int ret;

	for (i = 0; i < sizeof(header); i++)
		assert(ums9117_h4_rx_byte(&rx, header[i]) == 0);
	for (i = 0; i < 1024; i++) {
		ret = ums9117_h4_rx_byte(&rx, (unsigned char)i);
		assert(ret == (i == 1023));
	}
	assert(rx.length == 1028);
	for (i = 0; i < 1024; i++)
		assert(rx.data[4 + i] == (unsigned char)i);

	ums9117_h4_rx_reset(&rx);
	assert(ums9117_h4_rx_byte(&rx, 2) == 0);
	assert(ums9117_h4_rx_byte(&rx, 1) == 0);
	assert(ums9117_h4_rx_byte(&rx, 0x20) == 0);
	assert(ums9117_h4_rx_byte(&rx, 1) == 0);
	assert(ums9117_h4_rx_byte(&rx, 4) == -EMSGSIZE);
}

static void partial_event_prefix(void)
{
	static const unsigned char prefix[] = { 4, 0x0e, 4, 1, 3 };
	static const unsigned char remainder[] = { 0x0c, 0 };
	static const unsigned char expected[] = { 0x0e, 4, 1, 3, 0x0c, 0 };
	struct ums9117_h4_rx rx = { 0 };
	size_t i;

	for (i = 0; i < sizeof(prefix); i++)
		assert(ums9117_h4_rx_byte(&rx, prefix[i]) == 0);
	for (i = 0; i < sizeof(remainder); i++)
		assert(ums9117_h4_rx_byte(&rx, remainder[i]) == (i == 1));
	assert(rx.type == 4);
	assert(rx.length == sizeof(expected));
	assert(!memcmp(rx.data, expected, sizeof(expected)));

	ums9117_h4_rx_reset(&rx);
	assert(ums9117_h4_rx_byte(&rx, 4) == 0);
	assert(ums9117_h4_rx_byte(&rx, 0xff) == 0);
	assert(ums9117_h4_rx_byte(&rx, 0) == 1);
	assert(rx.length == 2);
	assert(rx.data[0] == 0xff && rx.data[1] == 0);
}

static void sco_and_event_lengths(void)
{
	struct ums9117_h4_rx rx = { 0 };
	unsigned int i;
	int ret;

	assert(ums9117_h4_rx_byte(&rx, 3) == 0);
	assert(ums9117_h4_rx_byte(&rx, 1) == 0);
	assert(ums9117_h4_rx_byte(&rx, 0) == 0);
	assert(ums9117_h4_rx_byte(&rx, 252) == 0);
	for (i = 0; i < 252; i++) {
		ret = ums9117_h4_rx_byte(&rx, 0xaa);
		assert(ret == (i == 251));
	}
	assert(rx.length == 255);

	ums9117_h4_rx_reset(&rx);
	assert(ums9117_h4_rx_byte(&rx, 3) == 0);
	assert(ums9117_h4_rx_byte(&rx, 1) == 0);
	assert(ums9117_h4_rx_byte(&rx, 0) == 0);
	assert(ums9117_h4_rx_byte(&rx, 253) == -EMSGSIZE);

	ums9117_h4_rx_reset(&rx);
	assert(ums9117_h4_rx_byte(&rx, 4) == 0);
	assert(ums9117_h4_rx_byte(&rx, 0xff) == 0);
	assert(ums9117_h4_rx_byte(&rx, 255) == 0);
	for (i = 0; i < 255; i++) {
		ret = ums9117_h4_rx_byte(&rx, 0xbb);
		assert(ret == (i == 254));
	}
	assert(rx.length == 257);
}

static void invalid_rx_types(void)
{
	static const unsigned char unsupported[] = { 0, 1, 5, 0xff };
	struct ums9117_h4_rx rx = { 0 };
	size_t i;

	for (i = 0; i < sizeof(unsupported); i++)
		assert(ums9117_h4_rx_byte(&rx, unsupported[i]) == -EILSEQ);
}

static void tx_lengths_and_types(void)
{
	/* The TX API accepts just the header plus the full packet's byte count. */
	static const unsigned char reset[] = { 3, 0x0c, 0 };
	static const unsigned char large_command[] = { 1, 0xfc, 255 };
	static const unsigned char acl[] = { 1, 0x20, 0, 4 };
	static const unsigned char sco[] = { 1, 0, 240 };

	assert(ums9117_h4_tx_validate(1, reset, 3) == 0);
	assert(ums9117_h4_tx_validate(1, reset, 2) == -EMSGSIZE);
	assert(ums9117_h4_tx_validate(1, reset, 4) == -EPROTO);
	assert(ums9117_h4_tx_validate(1, large_command, 258) == 0);
	assert(ums9117_h4_tx_validate(1, large_command, 259) == -EMSGSIZE);
	assert(ums9117_h4_tx_validate(2, acl, 1028) == 0);
	assert(ums9117_h4_tx_validate(2, acl, 1029) == -EMSGSIZE);
	assert(ums9117_h4_tx_validate(3, sco, 243) == 0);
	assert(ums9117_h4_tx_validate(3, sco, 242) == -EPROTO);
	assert(ums9117_h4_tx_validate(4, reset, 3) == -EILSEQ);
	assert(ums9117_h4_tx_validate(5, acl, 4) == -EILSEQ);
}

int main(void)
{
	mixed_frames();
	partial_event_prefix();
	acl_size_boundary();
	sco_and_event_lengths();
	invalid_rx_types();
	tx_lengths_and_types();
	puts("PASS: H4 mixed packets, partial input, length boundaries and TX validation");
	return 0;
}
