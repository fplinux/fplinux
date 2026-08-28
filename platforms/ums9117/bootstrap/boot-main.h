// SPDX-License-Identifier: GPL-2.0-only
/* Shared UMS9117 volatile RAM boot flow parameterised per board. */
#ifndef FPLINUX_UMS9117_BOOT_MAIN_H
#define FPLINUX_UMS9117_BOOT_MAIN_H

#include <stdint.h>

#include "fplinux-boot-screen/boot-screen.h"

struct ums9117_boot_board {
	uint32_t display_width;
	uint32_t display_height;
};

/* Paint a transient checkpoint line on the boot screen. */
void ums9117_boot_checkpoint(const char *message,
			     enum fplinux_boot_screen_status status);

/* Record the failure, paint it on the boot screen, and halt. */
void ums9117_boot_fail(uint32_t code, const char *message)
	__attribute__((noreturn));

/* Run the shared boot flow; never returns (hands off to Linux or halts). */
void ums9117_boot_main(const struct ums9117_boot_board *board)
	__attribute__((noreturn));

#endif
