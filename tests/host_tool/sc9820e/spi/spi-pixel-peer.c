// SPDX-License-Identifier: GPL-2.0-only
#include <stdio.h>
#include <stdlib.h>

#include "drm_mipi_dbi-spi.h"
#include "spi-sprd-word.h"

#define CHECK(condition)                                                   \
	do {                                                               \
		if (!(condition)) {                                        \
			fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, \
				#condition);                               \
			exit(1);                                           \
		}                                                          \
	} while (0)

static struct {
	const void *source;
	size_t length;
	void *allocation;
	unsigned int expected_bpw, allocations, frees, transfers;
	bool allocation_error;
	int transfer_error;
} peer;

void *kmalloc(size_t length, int flags)
{
	CHECK(flags == 0 && length == peer.length);
	peer.allocations++;
	if (peer.allocation_error)
		return NULL;
	peer.allocation = malloc(length);
	CHECK(peer.allocation);
	return peer.allocation;
}

void kfree(const void *buffer)
{
	CHECK(buffer == peer.allocation && peer.transfers == 1);
	peer.frees++;
	free(peer.allocation);
	peer.allocation = NULL;
}

int mipi_dbi_spi_transfer(struct spi_device *spi, u32 speed_hz, u8 bpw,
			  const void *buffer, size_t length)
{
	static const u8 expected_wire[] = {
		0x12, 0x34, 0xab, 0xcd, 0x07, 0xe0, 0xf8, 0x00,
	};
	const u32 *words = buffer;
	u8 wire[8];
	size_t i;

	(void)spi;
	CHECK(speed_hz == 96000000 && length == peer.length);
	CHECK(bpw == peer.expected_bpw);
	peer.transfers++;
	if (bpw == 32) {
		CHECK(buffer == peer.allocation && buffer != peer.source);
		CHECK(words[0] == 0x1234abcd && words[1] == 0x07e0f800);
		for (i = 0; i < 2; i++) {
			wire[4 * i] = words[i] >> 24;
			wire[4 * i + 1] = words[i] >> 16;
			wire[4 * i + 2] = words[i] >> 8;
			wire[4 * i + 3] = words[i];
		}
		CHECK(!memcmp(wire, expected_wire, sizeof(wire)));
	} else {
		CHECK(buffer == peer.source && !peer.allocations);
	}
	return peer.transfer_error;
}

static void check_word_control(void)
{
	CHECK(sprd_spi_word_control(0x2f83, 8) == 0x2fa3);
	CHECK(sprd_spi_word_control(0x2fa3, 16) == 0x2fc3);
	CHECK(sprd_spi_word_control(0x2fc3, 32) == 0x2f83);
	CHECK(sprd_spi_word_control(0x0f03, 32) == 0x0f03);
}

static void check_word_counts(void)
{
	CHECK(sprd_spi_word_count(65536, 32) == 16384);
	CHECK(sprd_spi_word_count(22528, 32) == 5632);
	CHECK(sprd_spi_word_count(153600, 32) == 38400);
	CHECK(sprd_spi_word_count(65536, 16) == 32768);
	CHECK(sprd_spi_word_count(22528, 16) == 11264);
	CHECK(sprd_spi_word_count(6, 16) == 3);
	CHECK(sprd_spi_word_count(1, 8) == 1);
}

int main(int argc, char **argv)
{
	const u16 pixels[] = { 0x1234, 0xabcd, 0x07e0, 0xf800 };
	struct spi_device spi = { .supports_32 = true, .max_transfer = 65536 };
	u8 source[10], before[10];
	unsigned int bpw = 16;
	size_t length = 8, offset = 0;
	int ret;

	CHECK(argc == 2);
	if (!strcmp(argv[1], "word-control")) {
		check_word_control();
		return 0;
	}
	if (!strcmp(argv[1], "word-counts")) {
		check_word_counts();
		return 0;
	}
	peer.expected_bpw = 32;
	if (!strcmp(argv[1], "unaligned32"))
		offset = 1;
	if (!strcmp(argv[1], "error32"))
		peer.transfer_error = -EIO;
	if (!strcmp(argv[1], "allocation-error"))
		peer.allocation_error = true;
	if (!strcmp(argv[1], "odd16")) {
		length = 6;
		peer.expected_bpw = 16;
	}
	if (!strcmp(argv[1], "single16")) {
		length = 2;
		peer.expected_bpw = 16;
	}
	if (!strcmp(argv[1], "no32")) {
		spi.supports_32 = false;
		peer.expected_bpw = 16;
	}
	if (!strcmp(argv[1], "bytes8")) {
		bpw = 8;
		peer.expected_bpw = 8;
	}
	if (!strcmp(argv[1], "lsb16")) {
		spi.mode = SPI_LSB_FIRST;
		peer.expected_bpw = 16;
	}
	if (!strcmp(argv[1], "limit16")) {
		spi.max_transfer = 2;
		peer.expected_bpw = 16;
	}

	memset(source, 0xa5, sizeof(source));
	memcpy(source + offset, pixels, length);
	memcpy(before, source, sizeof(before));
	peer.source = source + offset;
	peer.length = length;
	ret = mipi_dbi_spi_write_memory(&spi, 96000000, bpw, peer.source,
					length);
	CHECK(ret == (peer.allocation_error ? -ENOMEM : peer.transfer_error));
	CHECK(!memcmp(source, before, sizeof(source)));
	CHECK(!peer.allocation);
	if (peer.allocation_error) {
		CHECK(peer.allocations == 1 && !peer.frees && !peer.transfers);
	} else if (peer.expected_bpw == 32) {
		CHECK(peer.allocations == 1 && peer.frees == 1);
	} else {
		CHECK(!peer.allocations && !peer.frees && peer.transfers == 1);
	}
	return 0;
}
