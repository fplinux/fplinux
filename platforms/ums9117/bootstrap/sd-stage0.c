// SPDX-License-Identifier: GPL-2.0-only
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "fplinux-boot-screen/boot-screen.h"
#include "generated/fplinux-bootstrap-identity.h"
#include "generated/fplinux-boot-layout.h"
#include "generated/fplinux-uboot-build.h"
#include "syscode.h"
#include "ums9117-bootstrap/bootstrap.h"
#include "ums9117-bootstrap/boot-common.h"
#include "ums9117-bootstrap/sd-stage0.h"
#include "ums9117-common/ums9117-boot-contract.h"

extern const unsigned char uboot_payload_start[];
extern const unsigned char uboot_payload_end[];
extern unsigned char __image_start[];
extern unsigned char __bss_end[];

void ums9117_uboot_handoff(uint32_t entry, uint32_t handoff)
	__attribute__((noreturn));

enum sd_stage {
	SD_STAGE_PINMAP = 0,
	SD_STAGE_DISPLAY,
	SD_STAGE_TIMER,
	SD_STAGE_UBOOT,
	SD_STAGE_MICROSD,
	SD_STAGE_VERIFY,
	SD_STAGE_LINUX,
	SD_STAGE_COUNT,
};

static const char *const stage_labels[SD_STAGE_COUNT] = {
	"PINMAP", "DISPLAY", "TIMER", "U-BOOT", "MICROSD", "VERIFY", "LINUX",
};

enum ums9117_sd_stage0_failure_code {
	UMS9117_SD_STAGE0_FAILURE_DISPLAY_MEMORY = 3,
	UMS9117_SD_STAGE0_FAILURE_BOOT_SCREEN,
	UMS9117_SD_STAGE0_FAILURE_DISPLAY_SIZE,
	UMS9117_SD_STAGE0_FAILURE_TIMER,
	UMS9117_SD_STAGE0_FAILURE_UBOOT_IMAGE,
	UMS9117_SD_STAGE0_FAILURE_SYSTEM_IMAGE,
	UMS9117_SD_STAGE0_FAILURE_SESSION,
	UMS9117_SD_STAGE0_FAILURE_USB_HANDOFF,
	UMS9117_SD_STAGE0_FAILURE_USB_RX_CLEANUP,
	UMS9117_SD_STAGE0_FAILURE_USB_TX_CLEANUP,
};

static const struct fplinux_boot_screen_identity boot_identity = {
	.brand = "FPLinux",
	.model = FPLINUX_BOOTSTRAP_DISPLAY_NAME,
	.mode = "MICROSD BOOT",
};

static struct fplinux_boot_screen boot_screen;
static struct ums9117_boot_canvas boot_canvas;
static struct fplinux_uboot_handoff uboot_handoff;

static const char *stage0_failure_detail(uint32_t code, uint32_t detail)
{
	if (code == FPLINUX_STAGE0_FAILURE_SDBOOT) {
		switch (detail) {
		case FPLINUX_SDBOOT_FAILURE_MMC:
			return "MICROSD NOT FOUND OR UNREADABLE. POWER OFF AND CHECK THE CARD";
		case FPLINUX_SDBOOT_FAILURE_LOAD:
			return "FPLINUX.ITB NOT FOUND OR UNREADABLE. REWRITE THE BOOT CARD";
		case FPLINUX_SDBOOT_FAILURE_BOOTM:
			return "SYSTEM IMAGE INVALID. REWRITE THE BOOT CARD";
		case FPLINUX_SDBOOT_FAILURE_RELEASE:
			return "STORAGE CLEANUP FAILED. POWER OFF THE PHONE";
		default:
			return "U-BOOT STOPPED. POWER OFF AND TRY AGAIN";
		}
	}
	switch (code) {
	case FPLINUX_STAGE0_FAILURE_UBOOT:
		return "U-BOOT STOPPED. POWER OFF AND TRY AGAIN";
	case UMS9117_SD_STAGE0_FAILURE_DISPLAY_MEMORY:
		return "DISPLAY MEMORY INVALID. POWER OFF THE PHONE";
	case UMS9117_SD_STAGE0_FAILURE_BOOT_SCREEN:
		return "BOOT SCREEN FAILED. POWER OFF AND TRY AGAIN";
	case UMS9117_SD_STAGE0_FAILURE_DISPLAY_SIZE:
		return "DISPLAY SIZE MISMATCH. POWER OFF THE PHONE";
	case UMS9117_SD_STAGE0_FAILURE_TIMER:
		return "TIMER START FAILED. POWER OFF AND TRY AGAIN";
	case UMS9117_SD_STAGE0_FAILURE_UBOOT_IMAGE:
		return "U-BOOT IMAGE INVALID. REBUILD AND TRY AGAIN";
	case UMS9117_SD_STAGE0_FAILURE_SYSTEM_IMAGE:
		return "SYSTEM IMAGE INVALID. REWRITE THE BOOT CARD";
	case UMS9117_SD_STAGE0_FAILURE_SESSION:
		return "USB SESSION INVALID. POWER OFF AND TRY AGAIN";
	case UMS9117_SD_STAGE0_FAILURE_USB_HANDOFF:
		return "USB HANDOFF FAILED. POWER OFF AND TRY AGAIN";
	case UMS9117_SD_STAGE0_FAILURE_USB_RX_CLEANUP:
		return "USB RECEIVE CLEANUP FAILED. POWER OFF THE PHONE";
	case UMS9117_SD_STAGE0_FAILURE_USB_TX_CLEANUP:
		return "USB TRANSMIT CLEANUP FAILED. POWER OFF THE PHONE";
	case FPLINUX_STAGE0_FAILURE_STORAGE_CLEANUP:
		return "STORAGE CLEANUP FAILED. POWER OFF THE PHONE";
	default:
		return "MICROSD BOOT FAILED. POWER OFF AND TRY AGAIN";
	}
}

static void stage0_fail(uint32_t code, uint32_t detail)
{
	fprintf(stderr, "%s_UBOOT_STAGE0 error=%lu detail=%lu\n",
		FPLINUX_BOOTSTRAP_RECORD_PREFIX, (unsigned long)code,
		(unsigned long)detail);
	fplinux_boot_screen_fail(&boot_screen, code,
				 stage0_failure_detail(code, detail));
	for (;;)
		;
}

static void stage0_fail_without_transport(uint32_t code)
	__attribute__((noreturn));

static void stage0_fail_without_transport(uint32_t code)
{
	fplinux_boot_screen_fail(&boot_screen, code,
				 stage0_failure_detail(code, 0U));
	for (;;)
		;
}

static int stage0_checkpoint(uint32_t code, uint32_t value)
{
	char message[32];

	switch (code) {
	case FPLINUX_STAGE0_CHECKPOINT_UBOOT_READY:
		(void)fplinux_boot_screen_set_stage(
			&boot_screen, SD_STAGE_UBOOT, FPLINUX_BOOT_SCREEN_DONE);
		(void)fplinux_boot_screen_set_stage(&boot_screen,
						    SD_STAGE_MICROSD,
						    FPLINUX_BOOT_SCREEN_ACTIVE);
		(void)fplinux_boot_screen_set_note(&boot_screen,
						   "KEEP USB CONNECTED");
		strcpy(message, "READING MICROSD");
		break;
	case FPLINUX_STAGE0_CHECKPOINT_FIT_LOADED:
		(void)fplinux_boot_screen_set_stage(&boot_screen,
						    SD_STAGE_MICROSD,
						    FPLINUX_BOOT_SCREEN_DONE);
		(void)fplinux_boot_screen_set_stage(&boot_screen,
						    SD_STAGE_VERIFY,
						    FPLINUX_BOOT_SCREEN_ACTIVE);
		sprintf(message, "VERIFYING SYSTEM %luK",
			(unsigned long)(value >> 10));
		break;
	case FPLINUX_STAGE0_CHECKPOINT_LINUX_READY:
		(void)fplinux_boot_screen_set_stage(&boot_screen,
						    SD_STAGE_VERIFY,
						    FPLINUX_BOOT_SCREEN_DONE);
		(void)fplinux_boot_screen_set_stage(&boot_screen,
						    SD_STAGE_LINUX,
						    FPLINUX_BOOT_SCREEN_ACTIVE);
		strcpy(message, "PREPARING LINUX");
		break;
	case FPLINUX_STAGE0_CHECKPOINT_USB_PREP:
		strcpy(message, "PREPARING USB");
		break;
	default:
		strcpy(message, "PREPARING LINUX");
		break;
	}
	(void)fplinux_boot_screen_set_checkpoint(&boot_screen, message,
						 FPLINUX_BOOT_SCREEN_ACTIVE);
	fprintf(stderr, "%s_UBOOT_STAGE0 checkpoint=%lu value=0x%08lx\n",
		FPLINUX_BOOTSTRAP_RECORD_PREFIX, (unsigned long)code,
		(unsigned long)value);
	return 0;
}

static int stage0_console_putc(uint32_t byte)
{
	return fputc((int)(byte & 0xffU), stdout) == EOF ||
			       fflush(stdout) != 0 ?
		       -1 :
		       0;
}

static uint32_t stage0_timer_ms(void)
{
	return (uint32_t)sys_timer_ms();
}

static void stage0_finalize_and_boot(uint32_t zimage_phys,
				     uint32_t zimage_bytes, uint32_t dtb_phys,
				     uint32_t dtb_bytes)
	__attribute__((noreturn));

static void stage0_finalize_and_boot(uint32_t zimage_phys,
				     uint32_t zimage_bytes, uint32_t dtb_phys,
				     uint32_t dtb_bytes)
{
	enum ums9117_bootstrap_session_status session_status;
	uint8_t session_id[FPLINUX_HANDOFF_SESSION_ID_BYTES];
	uint32_t dma_status;

	if (zimage_phys != FPLINUX_BOOT_LAYOUT_ZIMAGE_PHYS || !zimage_bytes ||
	    zimage_bytes > FPLINUX_BOOT_LAYOUT_ZIMAGE_LIMIT_BYTES ||
	    dtb_phys != FPLINUX_BOOT_LAYOUT_DTB_PHYS || !dtb_bytes ||
	    dtb_bytes > FPLINUX_BOOT_LAYOUT_DTB_LIMIT_BYTES ||
	    zimage_bytes < FPLINUX_BOOT_ZIMAGE_HEADER_BYTES ||
	    *(const uint32_t *)(uintptr_t)(zimage_phys +
					   FPLINUX_BOOT_ZIMAGE_MAGIC_OFFSET) !=
		    FPLINUX_BOOT_ZIMAGE_MAGIC ||
	    *(const uint32_t *)(uintptr_t)(zimage_phys +
					   FPLINUX_BOOT_ZIMAGE_SIZE_OFFSET) !=
		    zimage_bytes)
		stage0_fail(UMS9117_SD_STAGE0_FAILURE_SYSTEM_IMAGE, 0U);
	session_status = ums9117_bootstrap_personalize_dtb(dtb_phys, dtb_bytes,
							   session_id);
	if (session_status != UMS9117_BOOTSTRAP_SESSION_OK)
		stage0_fail(UMS9117_SD_STAGE0_FAILURE_SESSION,
			    (uint32_t)session_status);
	stage0_checkpoint(FPLINUX_STAGE0_CHECKPOINT_USB_PREP, zimage_bytes);
	if (!ums9117_bootstrap_exchange_handoff_ack(session_id))
		stage0_fail_without_transport(
			UMS9117_SD_STAGE0_FAILURE_USB_HANDOFF);
	dma_status = ums9117_bootstrap_quiesce_usb_dma_channel(
		FPLINUX_BOOT_USB_RX_DMA_CHANNEL);
	if ((dma_status & UMS9117_BOOTSTRAP_DMA_OK) != UMS9117_BOOTSTRAP_DMA_OK)
		stage0_fail_without_transport(
			UMS9117_SD_STAGE0_FAILURE_USB_RX_CLEANUP);
	dma_status = ums9117_bootstrap_quiesce_usb_dma_channel(
		FPLINUX_BOOT_USB_TX_DMA_CHANNEL);
	if (!(dma_status & UMS9117_BOOTSTRAP_DMA_DISABLED))
		stage0_fail_without_transport(
			UMS9117_SD_STAGE0_FAILURE_USB_TX_CLEANUP);
	ums9117_bootstrap_cleanup_usb_dma_and_disconnect();
	(void)fplinux_boot_screen_set_stage(&boot_screen, SD_STAGE_LINUX,
					    FPLINUX_BOOT_SCREEN_DONE);
	(void)fplinux_boot_screen_set_checkpoint(&boot_screen, "STARTING LINUX",
						 FPLINUX_BOOT_SCREEN_DONE);
	ums9117_bootstrap_handoff_to_linux(zimage_phys, dtb_phys);
}

static const struct fplinux_stage0_ops stage0_ops = {
	.magic = FPLINUX_STAGE0_OPS_MAGIC,
	.size = sizeof(struct fplinux_stage0_ops),
	.checkpoint = stage0_checkpoint,
	.fail = stage0_fail,
	.finalize_and_boot = stage0_finalize_and_boot,
	.console_putc = stage0_console_putc,
	.timer_ms = stage0_timer_ms,
};

static size_t uboot_payload_size(void)
{
	return (size_t)(uboot_payload_end - uboot_payload_start);
}

void ums9117_sd_stage0_main(const struct ums9117_sd_stage0_board *board)
{
	struct fplinux_boot_screen_canvas canvas;
	struct ums9117_bootstrap_timer_result timer;
	uint32_t ram_bytes = ums9117_bootstrap_ram_bytes();
	size_t uboot_bytes = uboot_payload_size();
	char note[48];

	memset((void *)(uintptr_t)FPLINUX_BOOT_LAYOUT_FRAMEBUFFER_PHYS, 0,
	       FPLINUX_BOOT_LAYOUT_FRAMEBUFFER_BYTES);
	boot_canvas.pixels =
		(uint16_t *)(uintptr_t)FPLINUX_BOOT_LAYOUT_FRAMEBUFFER_PHYS;
	boot_canvas.width = sys_data.display.w1;
	boot_canvas.height = sys_data.display.h1;
	boot_canvas.stride = sys_data.display.w1;
	if (!ums9117_boot_canvas_valid(&boot_canvas))
		stage0_fail(UMS9117_SD_STAGE0_FAILURE_DISPLAY_MEMORY, 0U);
	canvas.width = boot_canvas.width;
	canvas.height = boot_canvas.height;
	canvas.context = &boot_canvas;
	canvas.fill_rect = ums9117_boot_canvas_fill_rect;
	canvas.present = ums9117_boot_canvas_present;

	scan_firmware(0);
	sys_start();
	sys_framebuffer(boot_canvas.pixels);
	if (fplinux_boot_screen_init(&boot_screen, &canvas, &boot_identity,
				     stage_labels, SD_STAGE_COUNT) != 0)
		stage0_fail(UMS9117_SD_STAGE0_FAILURE_BOOT_SCREEN, 0U);
	(void)fplinux_boot_screen_set_stage(&boot_screen, SD_STAGE_PINMAP,
					    FPLINUX_BOOT_SCREEN_DONE);
	if (boot_canvas.width != board->display_width ||
	    boot_canvas.height != board->display_height)
		stage0_fail(UMS9117_SD_STAGE0_FAILURE_DISPLAY_SIZE, 0U);
	(void)fplinux_boot_screen_set_stage(&boot_screen, SD_STAGE_DISPLAY,
					    FPLINUX_BOOT_SCREEN_DONE);

	ums9117_bootstrap_enable_timer_gates(NULL);
	if (!ums9117_bootstrap_probe_timer(&timer))
		stage0_fail(UMS9117_SD_STAGE0_FAILURE_TIMER, 0U);
	(void)fplinux_boot_screen_set_stage(&boot_screen, SD_STAGE_TIMER,
					    FPLINUX_BOOT_SCREEN_DONE);

	if (ram_bytes < FPLINUX_BOOT_LAYOUT_RAM_REQUIRED_BYTES ||
	    !uboot_bytes ||
	    uboot_bytes > FPLINUX_BOOT_LAYOUT_UBOOT_LIMIT_BYTES ||
	    uboot_bytes != FPLINUX_UBOOT_BINARY_BYTES)
		stage0_fail(UMS9117_SD_STAGE0_FAILURE_UBOOT_IMAGE,
			    (uint32_t)uboot_bytes);
	memcpy((void *)(uintptr_t)FPLINUX_BOOT_LAYOUT_UBOOT_LOAD_PHYS,
	       uboot_payload_start, uboot_bytes);
	(void)fplinux_boot_screen_set_stage(&boot_screen, SD_STAGE_UBOOT,
					    FPLINUX_BOOT_SCREEN_ACTIVE);
	sprintf(note, "U-BOOT %luK ENTRY %08lx",
		(unsigned long)(uboot_bytes >> 10),
		(unsigned long)FPLINUX_UBOOT_ENTRY_PHYS);
	(void)fplinux_boot_screen_set_note(&boot_screen, note);

	uboot_handoff.magic = FPLINUX_UBOOT_HANDOFF_MAGIC;
	uboot_handoff.size = sizeof(uboot_handoff);
	uboot_handoff.stage0_ops_phys = (uint32_t)(uintptr_t)&stage0_ops;
	uboot_handoff.resident_start = (uint32_t)(uintptr_t)__image_start;
	uboot_handoff.resident_end = (uint32_t)(uintptr_t)__bss_end;
	uboot_handoff.zimage_phys = FPLINUX_BOOT_LAYOUT_ZIMAGE_PHYS;
	uboot_handoff.zimage_limit = FPLINUX_BOOT_LAYOUT_ZIMAGE_LIMIT_BYTES;
	uboot_handoff.dtb_phys = FPLINUX_BOOT_LAYOUT_DTB_PHYS;
	uboot_handoff.dtb_limit = FPLINUX_BOOT_LAYOUT_DTB_LIMIT_BYTES;
	uboot_handoff.fit_phys = FPLINUX_BOOT_LAYOUT_FIT_PHYS;
	uboot_handoff.fit_limit = FPLINUX_BOOT_LAYOUT_FIT_LIMIT_BYTES;

	clean_invalidate_dcache();
	invalidate_icache();
	ums9117_uboot_handoff(FPLINUX_UBOOT_ENTRY_PHYS,
			      (uint32_t)(uintptr_t)&uboot_handoff);
}
