// SPDX-License-Identifier: GPL-2.0-only
/* Shared UMS9117 volatile RAM boot flow: stage, paint, verify, hand off. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "fplinux-boot-screen/boot-screen.h"
#include "syscode.h"
#include "ums9117-bootstrap/boot-main.h"
#include "ums9117-bootstrap/bootstrap.h"

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

struct ums9117_boot_canvas {
	uint16_t *pixels;
	uint32_t width;
	uint32_t height;
	uint32_t stride;
};

static const struct ums9117_boot_board *active_board;
static struct fplinux_boot_screen boot_screen;
static struct ums9117_boot_canvas boot_canvas;

static uint16_t *const framebuffer =
	(uint16_t *)(uintptr_t)UMS9117_BOOT_FRAMEBUFFER_PHYS;

static int boot_canvas_safe(const struct ums9117_boot_canvas *canvas)
{
	const uint32_t limit = UMS9117_BOOT_FRAMEBUFFER_BYTES / 2U;

	return canvas != NULL && canvas->pixels != NULL && canvas->width != 0 &&
	       canvas->height != 0 && canvas->stride >= canvas->width &&
	       canvas->stride <= limit &&
	       canvas->height <= limit / canvas->stride;
}

static void boot_screen_fill_rect(void *context, uint32_t x, uint32_t y,
				  uint32_t width, uint32_t height,
				  uint16_t colour)
{
	struct ums9117_boot_canvas *canvas = context;
	uint32_t xx;
	uint32_t yy;

	if (!boot_canvas_safe(canvas) || width == 0 || height == 0 ||
	    x >= canvas->width || y >= canvas->height)
		return;
	if (width > canvas->width - x)
		width = canvas->width - x;
	if (height > canvas->height - y)
		height = canvas->height - y;
	for (yy = 0; yy < height; ++yy) {
		uint32_t offset = (y + yy) * canvas->stride + x;

		for (xx = 0; xx < width; ++xx)
			canvas->pixels[offset + xx] = colour;
	}
}

static void boot_screen_present(void *context)
{
	if (!boot_canvas_safe(context))
		return;
	sys_start_refresh();
	sys_wait_refresh();
}

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
		active_board->marker, (unsigned long)stage, message);
	if (active_board->hooks != NULL && active_board->hooks->record != NULL)
		active_board->hooks->record(active_board->hooks->context, stage,
					    message);
}

void ums9117_boot_fail(uint32_t code, const char *message)
{
	fprintf(stderr, "%s_LINUX_BOOTSTRAP stage=238 error=%lu message=%s\n",
		active_board->marker, (unsigned long)code, message);
	if (active_board->hooks != NULL && active_board->hooks->fail != NULL)
		active_board->hooks->fail(active_board->hooks->context, code,
					  message);
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
		active_board->marker, (unsigned long)timer.syscnt_before,
		(unsigned long)timer.syscnt_after, timer.polls,
		(unsigned long)timer.ctl_before, (unsigned long)timer.ctl,
		(unsigned long)timer.value, (unsigned long)timer.shadow,
		(unsigned long)timer.int_status);

	return timer_ok;
}

static void quiesce_usb(void)
{
	if (active_board->hooks != NULL &&
	    active_board->hooks->quiesce_usb != NULL) {
		active_board->hooks->quiesce_usb(active_board->hooks->context);
		return;
	}
	ums9117_boot_checkpoint("DMA 5 QUIESCE", FPLINUX_BOOT_SCREEN_ACTIVE);
	if ((ums9117_bootstrap_quiesce_usb_dma_channel(5) &
	     UMS9117_BOOTSTRAP_DMA_OK) != UMS9117_BOOTSTRAP_DMA_OK)
		ums9117_boot_fail(6, "USB DMA5 QUIESCE FAIL");
	ums9117_boot_checkpoint("DMA 21 QUIESCE", FPLINUX_BOOT_SCREEN_ACTIVE);
	if ((ums9117_bootstrap_quiesce_usb_dma_channel(21) &
	     UMS9117_BOOTSTRAP_DMA_OK) != UMS9117_BOOTSTRAP_DMA_OK)
		ums9117_boot_fail(7, "USB DMA21 QUIESCE FAIL");
	ums9117_boot_checkpoint("USB DISCONNECT", FPLINUX_BOOT_SCREEN_ACTIVE);
	ums9117_bootstrap_cleanup_usb_dma_and_disconnect();
}

void ums9117_boot_main(const struct ums9117_boot_board *board)
{
	uint32_t ram_bytes =
		*(volatile uint32_t *)(uintptr_t)(UMS9117_BOOT_RAM_BASE_PHYS +
						  0x00100000U);
	size_t zimage_bytes = ums9117_bootstrap_zimage_size();
	size_t dtb_bytes = ums9117_bootstrap_dtb_size();
	struct fplinux_boot_screen_canvas canvas;
	char note[48];

	active_board = board;

	fprintf(stderr,
		"%s_LINUX_BOOTSTRAP stage=0 message=ENTRY "
		"ram=0x%08lx zimage=%lu dtb=%lu\n",
		board->marker, (unsigned long)ram_bytes,
		(unsigned long)zimage_bytes, (unsigned long)dtb_bytes);
	if (board->hooks != NULL && board->hooks->entry != NULL)
		board->hooks->entry(board->hooks->context, ram_bytes,
				    (uint32_t)zimage_bytes,
				    (uint32_t)dtb_bytes);

	/* Fresh RAM is noise; clear the whole region the LCDC may scan. */
	memset(framebuffer, 0, UMS9117_BOOT_FRAMEBUFFER_BYTES);
	boot_canvas.pixels = framebuffer;
	boot_canvas.width = sys_data.display.w1;
	boot_canvas.height = sys_data.display.h1;
	boot_canvas.stride = sys_data.display.w1;
	if (!boot_canvas_safe(&boot_canvas))
		ums9117_boot_fail(1, "BAD DISPLAY SIZE");
	canvas.width = boot_canvas.width;
	canvas.height = boot_canvas.height;
	canvas.context = &boot_canvas;
	canvas.fill_rect = boot_screen_fill_rect;
	canvas.present = boot_screen_present;

	/* Loads the board pin map over the host libc channel. */
	scan_firmware(0);
	sys_start();
	sys_framebuffer(boot_canvas.pixels);
	if (fplinux_boot_screen_init(&boot_screen, &canvas, &board->identity,
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
	if (ram_bytes < UMS9117_BOOT_RAM_REQUIRED_BYTES)
		ums9117_boot_fail(2, "64MB RAM REQUIRED");
	if (!zimage_bytes || zimage_bytes > UMS9117_BOOT_ZIMAGE_LIMIT_BYTES)
		ums9117_boot_fail(3, "BAD KERNEL SIZE");
	record_stage(3, "COPY KERNEL");
	ums9117_bootstrap_copy_zimage(UMS9117_BOOT_ZIMAGE_STAGE_PHYS,
				      zimage_bytes);
	set_stage(UMS9117_BOOT_STAGE_KERNEL, FPLINUX_BOOT_SCREEN_DONE);

	set_stage(UMS9117_BOOT_STAGE_DEVICE_TREE, FPLINUX_BOOT_SCREEN_ACTIVE);
	if (!dtb_bytes || dtb_bytes > UMS9117_BOOT_DTB_LIMIT_BYTES)
		ums9117_boot_fail(4, "BAD DTB SIZE");
	record_stage(4, "COPY DTB");
	ums9117_bootstrap_copy_dtb(UMS9117_BOOT_DTB_STAGE_PHYS, dtb_bytes);
	set_stage(UMS9117_BOOT_STAGE_DEVICE_TREE, FPLINUX_BOOT_SCREEN_DONE);

	set_stage(UMS9117_BOOT_STAGE_PREPARE_LINUX, FPLINUX_BOOT_SCREEN_ACTIVE);
	/* The host stops libc_server after observing this final USB record. */
	record_stage(5, "PREPARE LINUX");
	quiesce_usb();
	set_stage(UMS9117_BOOT_STAGE_PREPARE_LINUX, FPLINUX_BOOT_SCREEN_DONE);
	if (board->hooks != NULL && board->hooks->pre_handoff != NULL)
		board->hooks->pre_handoff(board->hooks->context);
	clean_invalidate_dcache();
	invalidate_icache();

	ums9117_linux_handoff(UMS9117_BOOT_ZIMAGE_STAGE_PHYS,
			      UMS9117_BOOT_DTB_STAGE_PHYS);
}
