/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_UMS9117_BOOTSTRAP_H
#define FPLINUX_UMS9117_BOOTSTRAP_H

#include <stddef.h>
#include <stdint.h>

#include "fplinux-handoff-protocol.h"

#define UMS9117_BOOTSTRAP_DMA_CLEAR_SEEN (1U << 0)
#define UMS9117_BOOTSTRAP_DMA_DISABLED (1U << 1)
#define UMS9117_BOOTSTRAP_DMA_OK \
	(UMS9117_BOOTSTRAP_DMA_CLEAR_SEEN | UMS9117_BOOTSTRAP_DMA_DISABLED)

enum ums9117_bootstrap_session_status {
	UMS9117_BOOTSTRAP_SESSION_OK = 0,
	UMS9117_BOOTSTRAP_SESSION_LAYOUT,
	UMS9117_BOOTSTRAP_SESSION_MAGIC,
	UMS9117_BOOTSTRAP_SESSION_SIZE,
	UMS9117_BOOTSTRAP_SESSION_CRC,
	UMS9117_BOOTSTRAP_SESSION_RESERVED,
	UMS9117_BOOTSTRAP_SESSION_ID,
	UMS9117_BOOTSTRAP_SESSION_SEED,
	UMS9117_BOOTSTRAP_SESSION_CLIENT_KEY,
	UMS9117_BOOTSTRAP_SESSION_USB_CONFIG,
	UMS9117_BOOTSTRAP_SESSION_DTB,
	UMS9117_BOOTSTRAP_SESSION_OUTPUT,
};

struct ums9117_bootstrap_timer_gates {
	uint32_t eb0_before;
	uint32_t eb0_after;
	uint32_t rtc_before;
	uint32_t rtc_after;
	uint32_t clk_before;
	uint32_t clk_after;
};

struct ums9117_bootstrap_timer_result {
	uint32_t ctl_before;
	uint32_t ctl;
	uint32_t syscnt_before;
	uint32_t syscnt_after;
	uint32_t int_status;
	uint32_t value;
	uint32_t shadow;
	unsigned long polls;
};

void ums9117_bootstrap_enable_timer_gates(
	struct ums9117_bootstrap_timer_gates *snapshot);
int ums9117_bootstrap_probe_timer(struct ums9117_bootstrap_timer_result *result);

uint32_t ums9117_bootstrap_quiesce_usb_dma_channel(unsigned channel);
void ums9117_bootstrap_cleanup_usb_dma_and_disconnect(void);

size_t ums9117_bootstrap_zimage_size(void);
size_t ums9117_bootstrap_dtb_size(void);
void ums9117_bootstrap_copy_zimage(uint32_t destination, size_t bytes);
void ums9117_bootstrap_copy_dtb(uint32_t destination, size_t bytes);
enum ums9117_bootstrap_session_status ums9117_bootstrap_personalize_dtb(
	uint32_t destination, size_t bytes,
	uint8_t session_id[FPLINUX_HANDOFF_SESSION_ID_BYTES]);
const char *
ums9117_bootstrap_session_error(enum ums9117_bootstrap_session_status status);

__attribute__((noreturn)) void ums9117_linux_handoff(uint32_t zimage,
						     uint32_t dtb);

void lcd_appinit(void);
void keytrn_init(void);

#endif
