// SPDX-License-Identifier: GPL-2.0-only
/* Shared UMS9117 volatile RAM boot flow parameterised per board. */
#ifndef UMS9117_BOOT_MAIN_H
#define UMS9117_BOOT_MAIN_H

#include <stdint.h>

#include "fplinux-boot-screen/boot-screen.h"

/* Every UMS9117 phone stages the same volatile layout in 64 MiB of RAM. */
#define UMS9117_BOOT_RAM_BASE 0x80000000u
#define UMS9117_BOOT_RAM_REQUIRED 0x04000000u
#define UMS9117_BOOT_ZIMAGE_STAGE 0x82000000u
#define UMS9117_BOOT_ZIMAGE_LIMIT 0x01200000u
#define UMS9117_BOOT_DTB_STAGE 0x83e00000u
#define UMS9117_BOOT_DTB_LIMIT 0x00010000u
#define UMS9117_BOOT_FRAMEBUFFER 0x83f00000u
#define UMS9117_BOOT_FRAMEBUFFER_BYTES 0x00100000u

/*
 * Optional board extensions.  Every callback may be NULL; quiesce_usb
 * replaces the default DMA channel quiesce when set.
 */
struct ums9117_boot_hooks {
	void *context;
	void (*entry)(void *context, uint32_t ram_bytes, uint32_t zimage_bytes,
		      uint32_t dtb_bytes);
	void (*record)(void *context, uint32_t stage, const char *message);
	void (*fail)(void *context, uint32_t code, const char *message);
	void (*quiesce_usb)(void *context);
	void (*pre_handoff)(void *context);
};

struct ums9117_boot_board {
	const char *marker; /* record prefix, e.g. "TA1618" */
	struct fplinux_boot_screen_identity identity;
	uint32_t display_width;
	uint32_t display_height;
	const struct ums9117_boot_hooks *hooks;
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
