// SPDX-License-Identifier: GPL-2.0-only
/* Shared UMS9117 volatile RAM boot flow parameterised per board. */
#ifndef FPLINUX_UMS9117_BOOT_MAIN_H
#define FPLINUX_UMS9117_BOOT_MAIN_H

#include <stdint.h>

#include "fplinux-boot-screen/boot-screen.h"

/* Every UMS9117 phone stages the same volatile layout in 64 MiB of RAM. */
#define UMS9117_BOOT_RAM_BASE_PHYS 0x80000000U
#define UMS9117_BOOT_RAM_REQUIRED_BYTES 0x04000000U
#define UMS9117_BOOT_ZIMAGE_STAGE_PHYS 0x82000000U
#define UMS9117_BOOT_ZIMAGE_LIMIT_BYTES 0x01200000U
#define UMS9117_BOOT_DTB_STAGE_PHYS 0x83e00000U
#define UMS9117_BOOT_DTB_LIMIT_BYTES 0x00010000U
#define UMS9117_BOOT_FRAMEBUFFER_PHYS 0x83f00000U
#define UMS9117_BOOT_FRAMEBUFFER_BYTES 0x00100000U

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
