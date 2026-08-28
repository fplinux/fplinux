/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_UMS9117_SD_STAGE0_H
#define FPLINUX_UMS9117_SD_STAGE0_H

#include <stdint.h>

struct ums9117_sd_stage0_board {
	uint32_t display_width;
	uint32_t display_height;
};

void ums9117_sd_stage0_main(const struct ums9117_sd_stage0_board *board)
	__attribute__((noreturn));

#endif
