/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_UMS9117_BOOT_CONTRACT_H
#define FPLINUX_UMS9117_BOOT_CONTRACT_H

#include <stdint.h>

#define FPLINUX_UBOOT_HANDOFF_MAGIC 0x46505548U
#define FPLINUX_STAGE0_OPS_MAGIC 0x4650534fU

#define FPLINUX_BOOT_ABI_ALIGNMENT 4U
#define FPLINUX_BOOT_FDT_ALIGNMENT 8U
#define FPLINUX_BOOT_INVALID_PHYS UINT32_MAX

#define FPLINUX_BOOT_RAM_SIZE_WORD_OFFSET 0x00100000U

#define FPLINUX_BOOT_ZIMAGE_HEADER_BYTES 0x30U
#define FPLINUX_BOOT_ZIMAGE_MAGIC_OFFSET 0x24U
#define FPLINUX_BOOT_ZIMAGE_SIZE_OFFSET 0x2cU
#define FPLINUX_BOOT_ZIMAGE_MAGIC 0x016f2818U

#define FPLINUX_BOOT_USB_TX_DMA_CHANNEL 5U
#define FPLINUX_BOOT_USB_RX_DMA_CHANNEL 21U
enum fplinux_stage0_checkpoint_code {
	FPLINUX_STAGE0_CHECKPOINT_UBOOT_READY = 1,
	FPLINUX_STAGE0_CHECKPOINT_FIT_LOADED,
	FPLINUX_STAGE0_CHECKPOINT_USB_PREP,
	FPLINUX_STAGE0_CHECKPOINT_LINUX_READY,
};

enum fplinux_stage0_failure_code {
	FPLINUX_STAGE0_FAILURE_UBOOT = 1,
	FPLINUX_STAGE0_FAILURE_SDBOOT,
	FPLINUX_STAGE0_FAILURE_STORAGE_CLEANUP = 14,
};

enum fplinux_sdboot_failure_detail {
	FPLINUX_SDBOOT_FAILURE_HANDOFF = 1,
	FPLINUX_SDBOOT_FAILURE_MMC,
	FPLINUX_SDBOOT_FAILURE_LOAD,
	FPLINUX_SDBOOT_FAILURE_BOOTM,
	FPLINUX_SDBOOT_FAILURE_RELEASE,
};

struct fplinux_stage0_ops {
	uint32_t magic;
	uint32_t size;
	int (*checkpoint)(uint32_t code, uint32_t value);
	void (*fail)(uint32_t code, uint32_t detail) __attribute__((noreturn));
	void (*finalize_and_boot)(uint32_t zimage_phys, uint32_t zimage_bytes,
				  uint32_t dtb_phys, uint32_t dtb_bytes)
		__attribute__((noreturn));
	int (*console_putc)(uint32_t byte);
	uint32_t (*timer_ms)(void);
};

struct fplinux_uboot_handoff {
	uint32_t magic;
	uint32_t size;
	uint32_t stage0_ops_phys;
	uint32_t resident_start;
	uint32_t resident_end;
	uint32_t zimage_phys;
	uint32_t zimage_limit;
	uint32_t dtb_phys;
	uint32_t dtb_limit;
	uint32_t fit_phys;
	uint32_t fit_limit;
};

#endif
