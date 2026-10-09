/* SPDX-License-Identifier: GPL-2.0-only */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include "bch.h"
#include "fplinux-cli.h"

#define SECTOR_BYTES 1024
#define PARITY_BYTES 21
#define CORRECTION_BITS 12
#define RECORD_BYTES (SECTOR_BYTES + PARITY_BYTES)

static bool is_erased(const uint8_t *record)
{
	for (size_t i = 0; i < RECORD_BYTES; i++) {
		if (record[i] != 0xff)
			return false;
	}
	return true;
}

static int correct_sector(struct bch_control *bch, uint8_t *record)
{
	uint8_t reversed[SECTOR_BYTES];
	uint8_t encoded[PARITY_BYTES] = { 0 };
	uint8_t *parity = record + SECTOR_BYTES;
	unsigned int locations[CORRECTION_BITS];
	int count;

	if (is_erased(record))
		return 0;
	for (size_t i = 0; i < SECTOR_BYTES; i++)
		reversed[i] = record[SECTOR_BYTES - 1 - i];
	count = bch_decode(bch, reversed, SECTOR_BYTES, parity, NULL, NULL,
			   locations);
	if (count < 0 || count > CORRECTION_BITS)
		return -1;
	for (int i = 0; i < count; i++) {
		unsigned int bit = locations[i];

		if (bit >= RECORD_BYTES * 8)
			return -1;
		if (bit < SECTOR_BYTES * 8)
			reversed[bit / 8] ^= 1U << (bit % 8);
		else {
			bit -= SECTOR_BYTES * 8;
			parity[bit / 8] ^= 1U << (bit % 8);
		}
	}
	bch_encode(bch, reversed, SECTOR_BYTES, encoded);
	if (memcmp(encoded, parity, PARITY_BYTES))
		return -1;
	for (size_t i = 0; i < SECTOR_BYTES; i++)
		record[i] = reversed[SECTOR_BYTES - 1 - i];
	return count;
}

static int self_test(struct bch_control *bch)
{
	static const uint8_t parity[PARITY_BYTES] = {
		0x8c, 0x5c, 0xa7, 0x0a, 0x22, 0x42, 0x42,
		0xfb, 0x43, 0x60, 0x1b, 0x19, 0x7b, 0x1b,
		0x65, 0x73, 0x2a, 0x4b, 0x6e, 0x7e, 0xea,
	};
	static const unsigned int error_counts[] = { 0, 1, 12 };
	uint8_t original[RECORD_BYTES];
	uint8_t record[RECORD_BYTES];

	for (size_t i = 0; i < SECTOR_BYTES; i++)
		original[i] = i % 256;
	memcpy(original + SECTOR_BYTES, parity, PARITY_BYTES);
	for (size_t case_id = 0;
	     case_id < sizeof(error_counts) / sizeof(error_counts[0]);
	     case_id++) {
		unsigned int errors = error_counts[case_id];

		memcpy(record, original, sizeof(record));
		for (unsigned int i = 0; i < errors; i++)
			record[i * 73] ^= 1U << (i % 8);
		if (correct_sector(bch, record) != (int)errors ||
		    memcmp(record, original, sizeof(record)))
			return 1;
	}
	memcpy(record, original, sizeof(record));
	for (unsigned int i = 0; i < 6; i++) {
		record[i * 73] ^= 1U << i;
		record[SECTOR_BYTES + i * 3] ^= 1U << i;
	}
	if (correct_sector(bch, record) != 12 ||
	    memcmp(record, original, sizeof(record)))
		return 1;
	memcpy(record, original, sizeof(record));
	for (unsigned int i = 0; i < 13; i++)
		record[i * 73] ^= 1U << (i % 8);
	if (correct_sector(bch, record) >= 0)
		return 1;
	memset(record, 0xff, sizeof(record));
	if (correct_sector(bch, record) != 0 || !is_erased(record))
		return 1;
	record[0] = 0xfe;
	if (correct_sector(bch, record) >= 0)
		return 1;
	return 0;
}

static int correct_stream(struct bch_control *bch)
{
	uint8_t record[RECORD_BYTES];
	size_t sector = 0;
	size_t count;

#ifdef _WIN32
	if (_setmode(_fileno(stdin), _O_BINARY) == -1 ||
	    _setmode(_fileno(stdout), _O_BINARY) == -1) {
		fprintf(stderr, "cannot set binary NAND ECC stream mode\n");
		return 1;
	}
#endif
	while ((count = fread(record, 1, sizeof(record), stdin))) {
		if (count != sizeof(record)) {
			fprintf(stderr,
				"incomplete NAND ECC record at sector %zu\n",
				sector);
			return 1;
		}
		if (correct_sector(bch, record) < 0) {
			fprintf(stderr, "uncorrectable NAND ECC sector %zu\n",
				sector);
			return 1;
		}
		if (fwrite(record, 1, SECTOR_BYTES, stdout) != SECTOR_BYTES) {
			fprintf(stderr,
				"cannot write corrected NAND main data\n");
			return 1;
		}
		sector++;
	}
	if (ferror(stdin) || fflush(stdout)) {
		fprintf(stderr, "cannot read or write NAND ECC stream\n");
		return 1;
	}
	return 0;
}

int main(int argc, char **argv)
{
	struct fplinux_cli_option options[] = {
		{ "self-test", NULL, "Check synthetic BCH12 codewords", 0, NULL,
		  0 },
	};
	struct fplinux_cli cli = {
		.program = "ums9117-nand-ecc",
		.description = "Correct 1024-byte main plus 21-byte parity "
			       "records from stdin to main data on stdout.",
		.options = options,
		.option_count = sizeof(options) / sizeof(options[0]),
	};
	struct bch_control *bch;
	enum fplinux_cli_result parsed;
	int result;

	parsed = fplinux_cli_parse(&cli, argc, argv);
	if (parsed != FPLINUX_CLI_READY)
		return parsed;
	bch = bch_init(14, CORRECTION_BITS, 0x6111, false);
	if (!bch) {
		fprintf(stderr, "cannot initialize NAND BCH12 decoder\n");
		return 1;
	}
	if (bch->ecc_bits != PARITY_BYTES * 8 ||
	    bch->ecc_bytes != PARITY_BYTES) {
		fprintf(stderr, "unexpected NAND BCH12 parity size\n");
		bch_free(bch);
		return 1;
	}
	result = options[0].count ? self_test(bch) : correct_stream(bch);
	if (options[0].count && result)
		fprintf(stderr, "NAND BCH12 self-test failed\n");
	bch_free(bch);
	return result;
}
