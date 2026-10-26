/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SC9820E_EMMC_TEST_CORE_H
#define SC9820E_EMMC_TEST_CORE_H

#include <stdbool.h>
#include <stdint.h>

typedef uint32_t u32;
typedef uint8_t u8;

#define MMC_DATA_WRITE (1U << 8)
#define MMC_DATA_READ (1U << 9)

struct mmc_command {
	u32 opcode;
	u32 arg;
};

struct mmc_data {
	unsigned int flags;
	unsigned int blksz;
	unsigned int blocks;
};

struct mmc_request {
	struct mmc_command *sbc;
	struct mmc_command *cmd;
	struct mmc_command *stop;
	struct mmc_data *data;
};

#endif
