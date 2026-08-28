// SPDX-License-Identifier: GPL-2.0-only
/* Shared UMS9117 volatile RAM boot flow: stage, paint, verify, hand off. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "fplinux-boot-screen/boot-screen.h"
#include "fplinux-handoff-protocol.h"
#include "generated/fplinux-bootstrap-identity.h"
#include "generated/fplinux-boot-layout.h"
#include "syscode.h"
#include "usbio.h"
#include "ums9117-bootstrap/boot-common.h"
#include "ums9117-bootstrap/boot-main.h"
#include "ums9117-bootstrap/bootstrap.h"

#ifndef FPLINUX_BOOTSTRAP_DISPLAY_NAME
#error "generated bootstrap identity lacks FPLINUX_BOOTSTRAP_DISPLAY_NAME"
#endif
#ifndef FPLINUX_BOOTSTRAP_RECORD_PREFIX
#error "generated bootstrap identity lacks FPLINUX_BOOTSTRAP_RECORD_PREFIX"
#endif

enum ums9117_boot_stage {
	UMS9117_BOOT_STAGE_PINMAP = 0,
	UMS9117_BOOT_STAGE_DISPLAY,
	UMS9117_BOOT_STAGE_TIMER,
	UMS9117_BOOT_STAGE_KERNEL,
	UMS9117_BOOT_STAGE_DEVICE_TREE,
	UMS9117_BOOT_STAGE_PREPARE_LINUX,
	UMS9117_BOOT_STAGE_COUNT,
};

static const char *const boot_stage_labels[UMS9117_BOOT_STAGE_COUNT] = {
	"PINMAP", "DISPLAY", "TIMER", "KERNEL", "DEVICE TREE", "PREPARE LINUX",
};

static const struct fplinux_boot_screen_identity boot_identity = {
	.brand = "FPLinux",
	.model = FPLINUX_BOOTSTRAP_DISPLAY_NAME,
	.mode = "VOLATILE RAM BOOT",
};

static struct fplinux_boot_screen boot_screen;
static struct ums9117_boot_canvas boot_canvas;

static uint16_t *const framebuffer =
	(uint16_t *)(uintptr_t)FPLINUX_BOOT_LAYOUT_FRAMEBUFFER_PHYS;

static void set_stage(uint32_t stage, enum fplinux_boot_screen_status status)
{
	(void)fplinux_boot_screen_set_stage(&boot_screen, stage, status);
}

void ums9117_boot_checkpoint(const char *message,
			     enum fplinux_boot_screen_status status)
{
	(void)fplinux_boot_screen_set_checkpoint(&boot_screen, message, status);
}

static void record_stage(uint32_t stage, const char *message)
{
	fprintf(stderr, "%s_LINUX_BOOTSTRAP stage=%lu message=%s\n",
		FPLINUX_BOOTSTRAP_RECORD_PREFIX, (unsigned long)stage, message);
}

void ums9117_boot_fail(uint32_t code, const char *message)
{
	fprintf(stderr, "%s_LINUX_BOOTSTRAP stage=238 error=%lu message=%s\n",
		FPLINUX_BOOTSTRAP_RECORD_PREFIX, (unsigned long)code, message);
	fplinux_boot_screen_fail(&boot_screen, code, message);
	for (;;)
		;
}

static int enable_and_probe_sprd_timer(void)
{
	struct ums9117_bootstrap_timer_result timer;
	int timer_ok;

	ums9117_bootstrap_enable_timer_gates(NULL);
	timer_ok = ums9117_bootstrap_probe_timer(&timer);

	fprintf(stderr,
		"%s_SPRD_TIMER syscnt=%lu->%lu polls=%lu "
		"ctl=0x%08lx->0x%08lx value=%lu shadow=%lu int=0x%08lx\n",
		FPLINUX_BOOTSTRAP_RECORD_PREFIX,
		(unsigned long)timer.syscnt_before,
		(unsigned long)timer.syscnt_after, timer.polls,
		(unsigned long)timer.ctl_before, (unsigned long)timer.ctl,
		(unsigned long)timer.value, (unsigned long)timer.shadow,
		(unsigned long)timer.int_status);

	return timer_ok;
}

static __attribute__((noreturn)) void
ums9117_boot_halt_without_transport(uint32_t code, const char *message)
{
	fplinux_boot_screen_fail(&boot_screen, code, message);
	for (;;)
		;
}

static void prepare_usb_handoff(void)
{
	unsigned int channel = ums9117_bootstrap_prepare_usb_handoff();

	if (channel == FPLINUX_BOOT_USB_TX_DMA_CHANNEL)
		ums9117_boot_halt_without_transport(6, "USB DMA5 QUIESCE FAIL");
	if (channel == FPLINUX_BOOT_USB_RX_DMA_CHANNEL)
		ums9117_boot_halt_without_transport(7,
						    "USB DMA21 QUIESCE FAIL");
}

void ums9117_boot_main(const struct ums9117_boot_board *board)
{
	enum ums9117_bootstrap_session_status session_status;
	uint32_t ram_bytes = ums9117_bootstrap_ram_bytes();
	size_t zimage_bytes = ums9117_bootstrap_zimage_size();
	size_t dtb_bytes = ums9117_bootstrap_dtb_size();
	struct fplinux_boot_screen_canvas canvas;
	uint8_t session_id[FPLINUX_HANDOFF_SESSION_ID_BYTES];
	char note[48];

	fprintf(stderr,
		"%s_LINUX_BOOTSTRAP stage=0 message=ENTRY "
		"ram=0x%08lx zimage=%lu dtb=%lu\n",
		FPLINUX_BOOTSTRAP_RECORD_PREFIX, (unsigned long)ram_bytes,
		(unsigned long)zimage_bytes, (unsigned long)dtb_bytes);

	/* Fresh RAM is noise; clear the whole region the LCDC may scan. */
	memset(framebuffer, 0, FPLINUX_BOOT_LAYOUT_FRAMEBUFFER_BYTES);
	boot_canvas.pixels = framebuffer;
	boot_canvas.width = sys_data.display.w1;
	boot_canvas.height = sys_data.display.h1;
	boot_canvas.stride = sys_data.display.w1;
	if (!ums9117_boot_canvas_valid(&boot_canvas))
		ums9117_boot_fail(1, "BAD DISPLAY SIZE");
	canvas.width = boot_canvas.width;
	canvas.height = boot_canvas.height;
	canvas.context = &boot_canvas;
	canvas.fill_rect = ums9117_boot_canvas_fill_rect;
	canvas.present = ums9117_boot_canvas_present;

	/* Loads the board pin map over the host libc channel. */
	scan_firmware(0);
	sys_start();
	sys_framebuffer(boot_canvas.pixels);
	if (fplinux_boot_screen_init(&boot_screen, &canvas, &boot_identity,
				     boot_stage_labels,
				     UMS9117_BOOT_STAGE_COUNT) != 0)
		ums9117_boot_fail(8, "BOOT SCREEN INIT FAIL");

	set_stage(UMS9117_BOOT_STAGE_PINMAP, FPLINUX_BOOT_SCREEN_DONE);
	sprintf(note, "RAM %luM KERNEL %luK DTB %luK",
		(unsigned long)(ram_bytes >> 20),
		(unsigned long)(zimage_bytes >> 10),
		(unsigned long)(dtb_bytes >> 10));
	(void)fplinux_boot_screen_set_note(&boot_screen, note);
	set_stage(UMS9117_BOOT_STAGE_DISPLAY, FPLINUX_BOOT_SCREEN_ACTIVE);
	if (boot_canvas.width != board->display_width ||
	    boot_canvas.height != board->display_height)
		ums9117_boot_fail(1, "BAD DISPLAY SIZE");
	set_stage(UMS9117_BOOT_STAGE_DISPLAY, FPLINUX_BOOT_SCREEN_DONE);
	record_stage(1, "DISPLAY OK");

	set_stage(UMS9117_BOOT_STAGE_TIMER, FPLINUX_BOOT_SCREEN_ACTIVE);
	if (!enable_and_probe_sprd_timer())
		ums9117_boot_fail(5, "SPRD TIMER FAIL");
	set_stage(UMS9117_BOOT_STAGE_TIMER, FPLINUX_BOOT_SCREEN_DONE);
	record_stage(2, "SPRD TIMER OK");

	set_stage(UMS9117_BOOT_STAGE_KERNEL, FPLINUX_BOOT_SCREEN_ACTIVE);
	if (ram_bytes < FPLINUX_BOOT_LAYOUT_RAM_REQUIRED_BYTES)
		ums9117_boot_fail(2, "64MB RAM REQUIRED");
	if (!zimage_bytes ||
	    zimage_bytes > FPLINUX_BOOT_LAYOUT_ZIMAGE_LIMIT_BYTES)
		ums9117_boot_fail(3, "BAD KERNEL SIZE");
	record_stage(3, "COPY KERNEL");
	ums9117_bootstrap_copy_zimage(FPLINUX_BOOT_LAYOUT_ZIMAGE_PHYS,
				      zimage_bytes);
	set_stage(UMS9117_BOOT_STAGE_KERNEL, FPLINUX_BOOT_SCREEN_DONE);

	set_stage(UMS9117_BOOT_STAGE_DEVICE_TREE, FPLINUX_BOOT_SCREEN_ACTIVE);
	if (!dtb_bytes || dtb_bytes > FPLINUX_BOOT_LAYOUT_DTB_LIMIT_BYTES)
		ums9117_boot_fail(4, "BAD DTB SIZE");
	record_stage(4, "COPY DTB");
	ums9117_bootstrap_copy_dtb(FPLINUX_BOOT_LAYOUT_DTB_PHYS, dtb_bytes);
	ums9117_boot_checkpoint("SESSION VERIFY", FPLINUX_BOOT_SCREEN_ACTIVE);
	session_status = ums9117_bootstrap_personalize_dtb(
		FPLINUX_BOOT_LAYOUT_DTB_PHYS, dtb_bytes, session_id);
	if (session_status != UMS9117_BOOTSTRAP_SESSION_OK)
		ums9117_boot_fail(
			9, ums9117_bootstrap_session_error(session_status));
	ums9117_boot_checkpoint("SESSION READY", FPLINUX_BOOT_SCREEN_DONE);
	set_stage(UMS9117_BOOT_STAGE_DEVICE_TREE, FPLINUX_BOOT_SCREEN_DONE);

	set_stage(UMS9117_BOOT_STAGE_PREPARE_LINUX, FPLINUX_BOOT_SCREEN_ACTIVE);
	record_stage(5, "PREPARE LINUX");
	if (!ums9117_bootstrap_exchange_handoff_ack(session_id))
		ums9117_boot_halt_without_transport(10, "HANDOFF ACK FAIL");

	prepare_usb_handoff();
	ums9117_bootstrap_cleanup_usb_dma_and_disconnect();
	set_stage(UMS9117_BOOT_STAGE_PREPARE_LINUX, FPLINUX_BOOT_SCREEN_DONE);
	ums9117_bootstrap_handoff_to_linux(FPLINUX_BOOT_LAYOUT_ZIMAGE_PHYS,
					   FPLINUX_BOOT_LAYOUT_DTB_PHYS);
}
