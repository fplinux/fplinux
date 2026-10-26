// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stddef.h>
#include <string.h>

#include "sdhci-sprd-emmc-ro.h"

static void check_reads(void)
{
	struct mmc_command cmd = { .opcode = 17 };
	struct mmc_command stop = { .opcode = 12 };
	struct mmc_data data = { .flags = 1U << 9, .blksz = 512, .blocks = 1 };
	struct mmc_request request = { .cmd = &cmd, .data = &data };

	assert(sc9820e_emmc_ro_request_check(&request) == 0);
	cmd.opcode = 18;
	data.blocks = 128;
	request.stop = &stop;
	assert(sc9820e_emmc_ro_request_check(&request) == 0);
	cmd.opcode = 8;
	data.blocks = 1;
	request.stop = NULL;
	assert(sc9820e_emmc_ro_request_check(&request) == 0);
	data.blocks = 2;
	assert(sc9820e_emmc_ro_request_check(&request) == -EROFS);
	data.blocks = 1;
	data.flags = 1U << 8;
	assert(sc9820e_emmc_ro_request_check(&request) == -EROFS);
	data.flags |= 1U << 9;
	assert(sc9820e_emmc_ro_request_check(&request) == -EROFS);
	data.flags = 1U << 9;
	data.blksz = 256;
	assert(sc9820e_emmc_ro_request_check(&request) == -EROFS);
	request.data = NULL;
	assert(sc9820e_emmc_ro_request_check(&request) == -EROFS);
}

static void check_blocked_requests(void)
{
	const unsigned int blocked[] = { 4,  19, 20, 23, 24, 25, 26, 27, 28,
					 29, 30, 32, 33, 35, 36, 38, 42, 56 };
	struct mmc_command cmd = { .opcode = 13 };
	struct mmc_command extra = { .opcode = 24 };
	struct mmc_request request = { .cmd = &cmd };
	size_t i;

	for (i = 0; i < sizeof(blocked) / sizeof(blocked[0]); i++) {
		cmd.opcode = blocked[i];
		assert(sc9820e_emmc_ro_request_check(&request) == -EROFS);
	}
	cmd.opcode = 13;
	request.sbc = &extra;
	assert(sc9820e_emmc_ro_request_check(&request) == -EROFS);
	request.sbc = NULL;
	request.stop = &extra;
	assert(sc9820e_emmc_ro_request_check(&request) == -EROFS);
	request.stop = NULL;
	assert(sc9820e_emmc_ro_request_check(&request) == 0);
	cmd.opcode = 0;
	cmd.arg = 0xf0f0f0f0;
	assert(sc9820e_emmc_ro_request_check(&request) == -EROFS);
	cmd.arg = 0;
	assert(sc9820e_emmc_ro_request_check(&request) == 0);
	cmd.opcode = 16;
	cmd.arg = 512;
	assert(sc9820e_emmc_ro_request_check(&request) == 0);
	cmd.arg = 1024;
	assert(sc9820e_emmc_ro_request_check(&request) == -EROFS);
}

static void check_configuration(void)
{
	const struct {
		unsigned int index;
		unsigned int max_value;
	} allowed[] = {
		{ 32, 1 }, { 33, 1 }, { 34, 3 }, { 161, 1 }, { 175, 1 }
	};
	struct mmc_command cmd = { .opcode = 6 };
	struct mmc_request request = { .cmd = &cmd };
	unsigned int index, value;
	size_t i;

	/* Exhaustive byte operands protect the persistent-settings boundary. */
	for (index = 0; index < 256; index++) {
		for (value = 0; value < 256; value++) {
			bool valid = false;

			for (i = 0; i < sizeof(allowed) / sizeof(allowed[0]);
			     i++)
				if (index == allowed[i].index && value >= 1 &&
				    value <= allowed[i].max_value)
					valid = true;
			cmd.arg = 0x03000001 | index << 16 | value << 8;
			assert(sc9820e_emmc_ro_request_check(&request) ==
			       (valid ? 0 : -EROFS));
		}
	}
	cmd.arg = 0x01af0101;
	assert(sc9820e_emmc_ro_request_check(&request) == -EROFS);
	cmd.arg = 0x03af0100;
	assert(sc9820e_emmc_ro_request_check(&request) == -EROFS);
	cmd.arg = 0x03af0181;
	assert(sc9820e_emmc_ro_request_check(&request) == -EROFS);
}

int main(int argc, char **argv)
{
	assert(argc == 2);
	if (!strcmp(argv[1], "reads"))
		check_reads();
	else if (!strcmp(argv[1], "blocked"))
		check_blocked_requests();
	else if (!strcmp(argv[1], "configuration"))
		check_configuration();
	else
		return 2;
	return 0;
}
