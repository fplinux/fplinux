// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "qin1splus-sipc-layout.h"

static struct qin_sipc_owner *find_owner(struct qin_sipc_owner *owners,
					 u8 channel)
{
	size_t index;

	for (index = 0; index < QIN_SIPC_OWNER_COUNT; index++) {
		if (owners[index].channel == channel)
			return &owners[index];
	}
	return NULL;
}

static void initialized_published_headers(void)
{
	static const struct published_header_case {
		u8 channel;
		size_t offset;
		size_t bytes;
		u8 header[36];
	} cases[] = {
		{
			.channel = 5,
			.offset = 0,
			.bytes = 0x38024,
			.header = {
				1, 0, 0, 0, 0x24, 0, 0x18, 0, 0, 0x80, 0, 0,
				0, 0, 0, 0, 0, 0, 0, 0, 0x24, 0x80, 0x18, 0,
				0, 0, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0,
			},
		},
		{
			.channel = 11,
			.offset = 0x39000,
			.bytes = 0x824,
			.header = {
				1, 0, 0, 0, 0x24, 0x90, 0x1b, 0, 0, 4, 0, 0,
				0, 0, 0, 0, 0, 0, 0, 0, 0x24, 0x94, 0x1b, 0,
				0, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
			},
		},
		{
			.channel = 12,
			.offset = 0x3a000,
			.bytes = 0x824,
			.header = {
				1, 0, 0, 0, 0x24, 0xa0, 0x1b, 0, 0, 4, 0, 0,
				0, 0, 0, 0, 0, 0, 0, 0, 0x24, 0xa4, 0x1b, 0,
				0, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
			},
		},
	};
	struct qin_sipc_owner owners[QIN_SIPC_OWNER_COUNT];
	u8 *allocation = malloc(0x100020);
	u8 *ram;
	size_t index;

	assert(allocation && !qin_sipc_layout(owners));
	ram = allocation + 16;
	for (index = 0; index < sizeof(cases) / sizeof(cases[0]); index++) {
		struct qin_sipc_owner *owner =
			find_owner(owners, cases[index].channel);

		assert(owner && owner->bytes == cases[index].bytes);
		memset(allocation, 0xa5, 0x100020);
		qin_sipc_init_owner(ram, owner);
		assert(!memcmp(ram + cases[index].offset, cases[index].header,
			       36));
		assert((ram + cases[index].offset)[-1] == 0xa5);
		assert(ram[cases[index].offset + cases[index].bytes] == 0xa5);
	}
	free(allocation);
}

int main(void)
{
	struct qin_sipc_owner owners[QIN_SIPC_OWNER_COUNT];
	struct qin_sipc_owner *owner;
	u8 *ram = malloc(0x100000);
	u8 output[8] = {};
	const u8 bytes[] = { 0x11, 0x22, 0x33, 0x44 };
	const u8 count[] = { 16, 0, 0, 0 };
	const u8 tx_address[] = { 4, 0x12, 0x1d, 0 };
	const u8 rx_address[] = { 4, 2, 0x1d, 0 };
	u8 *base, *tx, *rx;
	struct qin_sbuf_ring *tx_ring, *rx_ring;
	bool notify;

	initialized_published_headers();
	assert(ram && !qin_sipc_layout(owners));
	owner = find_owner(owners, 4);
	assert(owner);
	memset(ram, 0xa5, 0x100000);
	qin_sipc_init_owner(ram, owner);
	assert(!memcmp(ram + 0x3b000, count, 4));
	assert(!memcmp(ram + 0x3b164, tx_address, 4));
	assert(!memcmp(ram + 0x3b154, rx_address, 4));
	base = ram + 0x3b000;
	/* Literal offsets independently pin channel 4's 16-ring wire layout. */
	tx_ring = (void *)(base + 356);
	rx_ring = (void *)(base + 324);
	tx = base + 90628;
	rx = base + 86532;
	assert(!qin_sipc_bt_idle(ram, owner));
	assert(qin_sipc_bt_write_ring(ram, owner, bytes, 4, &notify) == 4);
	assert(qin_sipc_bt_idle(ram, owner) == -EBUSY);
	assert(notify && readl(&tx_ring->tx_write) == 4);
	assert(!memcmp(tx, bytes, 4));
	assert(base[4 + 512] == 0 && base[4 + 512 + 8192] == 0);
	assert(qin_sipc_bt_write_ring(ram, owner, bytes, 4, &notify) == 4);
	assert(!notify && readl(&tx_ring->tx_write) == 8);
	writel(0xfffffffe, &tx_ring->tx_write);
	writel(0xfffffffe, &tx_ring->tx_read);
	assert(qin_sipc_bt_write_ring(ram, owner, bytes, 4, &notify) == 4);
	assert(notify && readl(&tx_ring->tx_write) == 2);
	assert(tx[4094] == 0x11 && tx[4095] == 0x22);
	assert(tx[0] == 0x33 && tx[1] == 0x44);
	writel(4096, &tx_ring->tx_write);
	writel(0, &tx_ring->tx_read);
	assert(qin_sipc_bt_write_ring(ram, owner, bytes, 4, &notify) ==
	       -EAGAIN);
	assert(readl(&tx_ring->tx_write) == 4096 && !notify);
	writel(4097, &tx_ring->tx_write);
	assert(qin_sipc_bt_write_ring(ram, owner, bytes, 4, &notify) ==
	       -EPROTO);

	writel(0xfffffffe, &rx_ring->rx_read);
	writel(2, &rx_ring->rx_write);
	rx[4094] = 0x11;
	rx[4095] = 0x22;
	rx[0] = 0x33;
	rx[1] = 0x44;
	assert(qin_sipc_bt_read_ring(ram, owner, output, 8, &notify) == 4);
	assert(!notify && readl(&rx_ring->rx_read) == 2);
	assert(!memcmp(output, bytes, 4));
	assert(qin_sipc_bt_read_ring(ram, owner, output, 8, &notify) ==
	       -EAGAIN);
	writel(0, &rx_ring->rx_read);
	writel(4096, &rx_ring->rx_write);
	assert(qin_sipc_bt_read_ring(ram, owner, output, 3, &notify) == 3);
	assert(notify && readl(&rx_ring->rx_read) == 3);
	writel(8192, &rx_ring->rx_write);
	assert(qin_sipc_bt_read_ring(ram, owner, output, 8, &notify) ==
	       -EPROTO);
	assert(readl(&rx_ring->rx_read) == 3);
	/* The pointers embedded by CP cannot redirect either AP copy. */
	writel(0, &tx_ring->tx_addr);
	writel(0, &tx_ring->tx_size);
	writel(0, &tx_ring->tx_read);
	writel(0, &tx_ring->tx_write);
	assert(qin_sipc_bt_write_ring(ram, owner, bytes, 4, &notify) == 4);
	assert(!memcmp(tx, bytes, 4));
	writel(4, &tx_ring->tx_read);
	writel(8192, &rx_ring->rx_read);
	assert(!qin_sipc_bt_idle(ram, owner));
	writel(0, &rx_ring->rx_addr);
	writel(0, &rx_ring->rx_size);
	rx[0] = 0x55;
	writel(8193, &rx_ring->rx_write);
	assert(qin_sipc_bt_read_ring(ram, owner, output, 1, &notify) == 1);
	assert(output[0] == 0x55 && readl(&rx_ring->rx_read) == 8193);
	assert(!qin_sipc_bt_idle(ram, owner));
	assert(qin_sipc_bt_write_ring(ram, owner, bytes, 4097, &notify) ==
	       -EINVAL);
	assert(qin_sipc_bt_read_ring(ram, owner, output, 0, &notify) ==
	       -EINVAL);
	free(ram);
	puts("Qin SIPC BT ring host cases passed");
	return 0;
}
