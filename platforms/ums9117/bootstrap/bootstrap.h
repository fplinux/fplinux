/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_UMS9117_BOOTSTRAP_H
#define FPLINUX_UMS9117_BOOTSTRAP_H

#include <stddef.h>
#include <stdint.h>

#define UMS9117_BOOTSTRAP_DMA_CLEAR_SEEN (1U << 0)
#define UMS9117_BOOTSTRAP_DMA_DISABLED (1U << 1)
#define UMS9117_BOOTSTRAP_DMA_OK \
	(UMS9117_BOOTSTRAP_DMA_CLEAR_SEEN | UMS9117_BOOTSTRAP_DMA_DISABLED)

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

__attribute__((noreturn)) void ums9117_linux_handoff(uint32_t zimage,
						     uint32_t dtb);

void lcd_appinit(void);
void keytrn_init(void);

#endif
