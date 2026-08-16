// SPDX-License-Identifier: GPL-2.0-only
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "fplinux-boot-screen/boot-screen.h"
#include "syscode.h"
#include "ta1618-hwdiag.h"
#include "ums9117-bootstrap/bootstrap.h"

#define RAM_BASE 0x80000000u
#define RAM_REQUIRED 0x04000000u
#define ZIMAGE_STAGE 0x82000000u
#define DTB_STAGE 0x83e00000u
#define DIAG_BASE 0x83ef0000u
#define FRAMEBUFFER 0x83f00000u
#define AON_TIMER_BASE 0x40050000u
#define AP_SYSCNT 0x4023000cu
#define AON_APB_EB0 0x402e0000u
#define AON_APB_RTC_EB 0x402e0010u
#define AON_APB_CLK_EB0 0x402e0134u
#define GICD_BASE 0x12001000u
#define MUSB_BASE 0x20200000u
#define FB_WIDTH 240u
#define FB_HEIGHT 320u
#define FB_PIXELS (FB_WIDTH * FB_HEIGHT)
#define FB_BYTES (FB_PIXELS * 2u)

#define BIT(n) (1u << (n))
#define TIMER_EB0_BITS (BIT(11) | BIT(10))
#define TIMER_RTC_BITS (BIT(4) | BIT(3))
#define TIMER_CLK_BITS BIT(11)

#define TIMER_LOAD_LO 0x00u
#define TIMER_LOAD_HI 0x04u
#define TIMER_VALUE_LO 0x08u
#define TIMER_CTL 0x10u
#define TIMER_INT 0x14u
#define TIMER_SHDW_LO 0x18u
#define TIMER_CTL_PERIOD BIT(0)
#define TIMER_CTL_ENABLE BIT(1)
#define TIMER_CTL_64BIT BIT(16)
#define TIMER_INT_EN BIT(0)
#define TIMER_INT_RAW BIT(1)
#define TIMER_INT_MASK BIT(2)
#define TIMER_INT_CLR BIT(3)

#define GICD_CTLR 0x000u
#define GICD_TYPER 0x004u
#define GICD_IIDR 0x008u
#define GICD_IGROUPR 0x080u
#define GICD_ISENABLER 0x100u
#define GICD_ISPENDR 0x200u
#define GICD_ICPENDR 0x280u
#define GICD_ISACTIVER 0x300u

#define MMU_L1_TABLE_BASE RAM_BASE
#define MMU_SECTION_BASE_MASK 0xfff00000u
#define MMU_SECTION_AP_RW (3u << 10)
#define MMU_SECTION_DESCRIPTOR 2u

#define MUSB_FADDR 0x000u
#define MUSB_POWER 0x001u
#define MUSB_INTRTX 0x002u
#define MUSB_INTRRX 0x004u
#define MUSB_INTRTXE 0x006u
#define MUSB_INTRRXE 0x008u
#define MUSB_INTRUSB 0x00au
#define MUSB_INTRUSBE 0x00bu
#define MUSB_FRAME 0x00cu
#define MUSB_INDEX 0x00eu
#define MUSB_TESTMODE 0x00fu
#define MUSB_DEVCTL 0x060u
#define MUSB_BABBLE_CTL 0x061u
#define MUSB_TXFIFOSZ 0x062u
#define MUSB_RXFIFOSZ 0x063u
#define MUSB_TXFIFOADD 0x064u
#define MUSB_RXFIFOADD 0x066u
#define MUSB_HWVERS 0x06cu
#define MUSB_EPINFO 0x078u
#define MUSB_RAMINFO 0x079u
#define MUSB_LINKINFO 0x07au
#define MUSB_VPLEN 0x07bu
#define MUSB_DMA_RAW_STATUS 0x1008u
#define MUSB_DMA_MASK_STATUS 0x100cu
#define MUSB_DMA_REQ_STATUS 0x1010u
#define MUSB_DMA_EN_STATUS 0x1014u
#define MUSB_DMA_DEBUG 0x1018u
#define MUSB_DMA_CHANNEL(n) (0x1c00u + ((n) - 1u) * 0x20u)
#define MUSB_DMA_PAUSE 0x00u
#define MUSB_DMA_CFG 0x04u
#define MUSB_DMA_INTR 0x08u
#define MUSB_DMA_LLIST_PTR 0x14u
#define MUSB_DMA_CHN_EN BIT(0)
#define MUSB_DMA_CLEAR_INT_EN BIT(5)
#define MUSB_DMA_CHN_CLR BIT(15)
#define MUSB_DMA_CLR_STATUS BIT(21)
#define MUSB_DMA_INTR_CLEAR (BIT(24) | BIT(25) | BIT(26) | BIT(27) | BIT(28))
#define MUSB_POWER_SOFTCONN BIT(6)
#define MUSB_TXCSR_DMA_BITS (BIT(15) | BIT(12) | BIT(10))
#define MUSB_RXCSR_DMA_BITS (BIT(15) | BIT(13) | BIT(11))

void invalidate_tlb(void);

static volatile struct ta1618_boot_diag *const diag =
	(volatile struct ta1618_boot_diag *)(uintptr_t)DIAG_BASE;
static uint16_t *const fb = (uint16_t *)(uintptr_t)FRAMEBUFFER;
static struct fplinux_boot_screen boot_screen;

struct ta1618_boot_canvas {
	uint16_t *pixels;
	uint32_t width;
	uint32_t height;
	uint32_t stride;
};

static struct ta1618_boot_canvas boot_canvas;

enum ta1618_boot_stage {
	BOOT_STAGE_DISPLAY = 0,
	BOOT_STAGE_TIMER,
	BOOT_STAGE_KERNEL,
	BOOT_STAGE_DEVICE_TREE,
	BOOT_STAGE_PREPARE_LINUX,
	BOOT_STAGE_COUNT,
};

static const char *const boot_stage_labels[BOOT_STAGE_COUNT] = {
	"DISPLAY", "TIMER", "KERNEL", "DEVICE TREE", "PREPARE LINUX",
};

typedef char ta1618_diag_fits_reserved_area
	[sizeof(struct ta1618_boot_diag) <= TA1618_DIAG_BYTES ? 1 : -1];

static int boot_canvas_safe(const struct ta1618_boot_canvas *canvas)
{
	return canvas != NULL && canvas->pixels != NULL && canvas->width != 0 &&
	       canvas->height != 0 && canvas->stride >= canvas->width &&
	       canvas->stride <= FB_PIXELS &&
	       canvas->height <= FB_PIXELS / canvas->stride;
}

static int boot_canvas_configure(struct ta1618_boot_canvas *canvas,
				 uint16_t *pixels, uint32_t width,
				 uint32_t height)
{
	if (canvas == NULL)
		return -1;
	canvas->pixels = NULL;
	canvas->width = 0;
	canvas->height = 0;
	canvas->stride = 0;
	if (pixels == NULL || width == 0 || height == 0 || width > FB_PIXELS ||
	    height > FB_PIXELS / width)
		return -1;
	canvas->pixels = pixels;
	canvas->width = width;
	canvas->height = height;
	canvas->stride = width;
	return 0;
}

static void boot_screen_fill_rect(void *context, uint32_t x, uint32_t y,
				  uint32_t width, uint32_t height,
				  uint16_t colour)
{
	struct ta1618_boot_canvas *canvas = context;
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

static void update_checkpoint(const char *message,
			      enum fplinux_boot_screen_status status)
{
	(void)fplinux_boot_screen_set_checkpoint(&boot_screen, message, status);
}

static void diag_message(const char *message)
{
	unsigned i;

	for (i = 0; i + 1 < sizeof(diag->last_message) && message[i]; ++i)
		diag->last_message[i] = message[i];
	diag->last_message[i] = '\0';
}

static void record_stage(uint32_t stage, const char *message)
{
	fprintf(stderr, "TA1618_LINUX_BOOTSTRAP stage=%lu message=%s\n",
		(unsigned long)stage, message);
	diag->stage = stage;
	diag_message(message);
}

static __attribute__((noreturn)) void fail(uint32_t code, const char *message)
{
	diag->error = code;
	record_stage(0xee, message);
	fplinux_boot_screen_fail(&boot_screen, code, message);
	for (;;)
		;
}

static uint32_t reg_read(uint32_t address)
{
	return *(volatile uint32_t *)(uintptr_t)address;
}

static uint16_t reg_read16(uint32_t address)
{
	return *(volatile uint16_t *)(uintptr_t)address;
}

static uint8_t reg_read8(uint32_t address)
{
	return *(volatile uint8_t *)(uintptr_t)address;
}

static void reg_write(uint32_t address, uint32_t value)
{
	*(volatile uint32_t *)(uintptr_t)address = value;
}

/*
 * fpdoom's UMS9117 entry code identity-maps devices from 0x20000000 upward,
 * but leaves 0x00000000..0x1fffffff faulting.  The GIC lives at 0x12000000,
 * so map only that 1 MiB device section before taking the pre-Linux snapshot.
 */
static void map_gic_device_section(void)
{
	volatile uint32_t *table =
		(volatile uint32_t *)(uintptr_t)MMU_L1_TABLE_BASE;
	unsigned index = GICD_BASE >> 20;
	uint32_t descriptor = (GICD_BASE & MMU_SECTION_BASE_MASK) |
			      MMU_SECTION_AP_RW | MMU_SECTION_DESCRIPTOR;

	table[index] = descriptor;
	clean_dcache_range((void *)(uintptr_t)&table[index],
			   (void *)(uintptr_t)(&table[index] + 1));
	__asm__ volatile("dsb sy" : : : "memory");
	invalidate_tlb();
	__asm__ volatile("dsb sy\n\tisb" : : : "memory");
}

static void snapshot_gic(volatile struct ta1618_gic_snapshot *snapshot)
{
	uint32_t words;
	unsigned i;

	snapshot->ctlr = reg_read(GICD_BASE + GICD_CTLR);
	snapshot->typer = reg_read(GICD_BASE + GICD_TYPER);
	snapshot->iidr = reg_read(GICD_BASE + GICD_IIDR);
	words = (snapshot->typer & 0x1fu) + 1u;
	if (words > TA1618_GIC_WORDS)
		words = TA1618_GIC_WORDS;
	snapshot->words = words;

	for (i = 0; i < TA1618_GIC_WORDS; ++i) {
		if (i < words) {
			snapshot->group[i] =
				reg_read(GICD_BASE + GICD_IGROUPR + i * 4u);
			snapshot->enabled[i] =
				reg_read(GICD_BASE + GICD_ISENABLER + i * 4u);
			snapshot->pending[i] =
				reg_read(GICD_BASE + GICD_ISPENDR + i * 4u);
			snapshot->active[i] =
				reg_read(GICD_BASE + GICD_ISACTIVER + i * 4u);
		} else {
			snapshot->group[i] = 0;
			snapshot->enabled[i] = 0;
			snapshot->pending[i] = 0;
			snapshot->active[i] = 0;
		}
	}
}

static uint32_t
snapshot_usb_pending(const volatile struct ta1618_gic_snapshot *snapshot)
{
	unsigned word = TA1618_USB_INTID / 32u;
	unsigned bit = TA1618_USB_INTID % 32u;

	if (word >= snapshot->words)
		return 0;
	return (snapshot->pending[word] >> bit) & 1u;
}

static void snapshot_dma_channel(unsigned channel,
				 volatile unsigned int values[8])
{
	uint32_t base = MUSB_BASE + MUSB_DMA_CHANNEL(channel);
	unsigned i;

	for (i = 0; i < 8; ++i)
		values[i] = reg_read(base + i * 4u);
}

static void snapshot_musb(volatile struct ta1618_musb_snapshot *snapshot)
{
	snapshot->faddr = reg_read8(MUSB_BASE + MUSB_FADDR);
	snapshot->power = reg_read8(MUSB_BASE + MUSB_POWER);
	snapshot->intrtx = reg_read16(MUSB_BASE + MUSB_INTRTX);
	snapshot->intrrx = reg_read16(MUSB_BASE + MUSB_INTRRX);
	snapshot->intrtxe = reg_read16(MUSB_BASE + MUSB_INTRTXE);
	snapshot->intrrxe = reg_read16(MUSB_BASE + MUSB_INTRRXE);
	snapshot->intrusb = reg_read8(MUSB_BASE + MUSB_INTRUSB);
	snapshot->intrusbe = reg_read8(MUSB_BASE + MUSB_INTRUSBE);
	snapshot->frame = reg_read16(MUSB_BASE + MUSB_FRAME);
	snapshot->index = reg_read8(MUSB_BASE + MUSB_INDEX);
	snapshot->testmode = reg_read8(MUSB_BASE + MUSB_TESTMODE);
	snapshot->devctl = reg_read8(MUSB_BASE + MUSB_DEVCTL);
	snapshot->babble_ctl = reg_read8(MUSB_BASE + MUSB_BABBLE_CTL);
	snapshot->txfifosz = reg_read8(MUSB_BASE + MUSB_TXFIFOSZ);
	snapshot->rxfifosz = reg_read8(MUSB_BASE + MUSB_RXFIFOSZ);
	snapshot->txfifoadd = reg_read16(MUSB_BASE + MUSB_TXFIFOADD);
	snapshot->rxfifoadd = reg_read16(MUSB_BASE + MUSB_RXFIFOADD);
	snapshot->hwvers = reg_read16(MUSB_BASE + MUSB_HWVERS);
	snapshot->epinfo = reg_read8(MUSB_BASE + MUSB_EPINFO);
	snapshot->raminfo = reg_read8(MUSB_BASE + MUSB_RAMINFO);
	snapshot->linkinfo = reg_read8(MUSB_BASE + MUSB_LINKINFO);
	snapshot->vplen = reg_read8(MUSB_BASE + MUSB_VPLEN);
	snapshot->dma_raw_status = reg_read(MUSB_BASE + MUSB_DMA_RAW_STATUS);
	snapshot->dma_mask_status = reg_read(MUSB_BASE + MUSB_DMA_MASK_STATUS);
	snapshot->dma_req_status = reg_read(MUSB_BASE + MUSB_DMA_REQ_STATUS);
	snapshot->dma_enable_status = reg_read(MUSB_BASE + MUSB_DMA_EN_STATUS);
	snapshot->dma_debug_status = reg_read(MUSB_BASE + MUSB_DMA_DEBUG);
	snapshot_dma_channel(5, snapshot->dma_channel5);
	snapshot_dma_channel(21, snapshot->dma_channel21);
}

static void probe_and_quiesce_usb(void)
{
	update_checkpoint("GIC MAP", FPLINUX_BOOT_SCREEN_ACTIVE);
	map_gic_device_section();
	update_checkpoint("GIC MAP", FPLINUX_BOOT_SCREEN_DONE);
	update_checkpoint("IRQ SNAPSHOT", FPLINUX_BOOT_SCREEN_ACTIVE);
	snapshot_gic(&diag->gic_before_clear);
	snapshot_musb(&diag->musb_before_usb_tx);
	update_checkpoint("IRQ SNAPSHOT", FPLINUX_BOOT_SCREEN_DONE);
	diag->usb_pending_before =
		snapshot_usb_pending(&diag->gic_before_clear);

	/*
	 * IRQ and FIQ have remained masked since fpdoom entry.  Clear only the
	 * proven USB INTID, then cause one final, known USB TX.  Do not disturb
	 * any unrelated pending source before the Linux handoff.
	 */
	update_checkpoint("USB IRQ PROBE", FPLINUX_BOOT_SCREEN_ACTIVE);
	reg_write(GICD_BASE + GICD_ICPENDR + (TA1618_USB_INTID / 32u) * 4u,
		  BIT(TA1618_USB_INTID % 32u));
	__asm__ volatile("dsb sy\n\tisb" : : : "memory");
	snapshot_gic(&diag->gic_after_clear);
	diag->usb_pending_after_clear =
		snapshot_usb_pending(&diag->gic_after_clear);

	fprintf(stderr,
		"TA1618_USB_IRQ_PROBE spi=%lu intid=%lu phase=known-tx\n",
		(unsigned long)TA1618_USB_SPI, (unsigned long)TA1618_USB_INTID);
	snapshot_gic(&diag->gic_after_usb_tx);
	snapshot_musb(&diag->musb_after_usb_tx);
	diag->usb_pending_after_tx =
		snapshot_usb_pending(&diag->gic_after_usb_tx);
	diag->flags |= TA1618_DIAG_F_GIC_SNAPSHOTS | TA1618_DIAG_F_USB_TX_PROBE;
	update_checkpoint("USB IRQ PROBE", FPLINUX_BOOT_SCREEN_DONE);

	/*
	 * fpdoom leaves linked-list DMA channels 5 (bulk IN) and 21 (bulk OUT)
	 * armed.  Stop them before Linux may reuse the bootstrap's RAM, then
	 * clear the matching endpoint DMA modes and soft-disconnect.  Linux's
	 * PIO-only MUSB driver can subsequently enumerate from a clean state.
	 */
	update_checkpoint("DMA 5 QUIESCE", FPLINUX_BOOT_SCREEN_ACTIVE);
	diag->dma5_quiesce_status =
		ums9117_bootstrap_quiesce_usb_dma_channel(5);
	if ((diag->dma5_quiesce_status & UMS9117_BOOTSTRAP_DMA_OK) ==
	    UMS9117_BOOTSTRAP_DMA_OK) {
		diag->flags |= TA1618_DIAG_F_DMA5_QUIESCED;
		update_checkpoint("DMA 5 QUIESCE", FPLINUX_BOOT_SCREEN_DONE);
	} else {
		update_checkpoint("DMA 5 QUIESCE", FPLINUX_BOOT_SCREEN_FAILED);
	}
	update_checkpoint("DMA 21 QUIESCE", FPLINUX_BOOT_SCREEN_ACTIVE);
	diag->dma21_quiesce_status =
		ums9117_bootstrap_quiesce_usb_dma_channel(21);
	if ((diag->dma21_quiesce_status & UMS9117_BOOTSTRAP_DMA_OK) ==
	    UMS9117_BOOTSTRAP_DMA_OK) {
		diag->flags |= TA1618_DIAG_F_DMA21_QUIESCED;
		update_checkpoint("DMA 21 QUIESCE", FPLINUX_BOOT_SCREEN_DONE);
	} else {
		update_checkpoint("DMA 21 QUIESCE", FPLINUX_BOOT_SCREEN_FAILED);
	}
	if (!(diag->flags & TA1618_DIAG_F_DMA5_QUIESCED))
		fail(6, "USB DMA5 QUIESCE FAIL");
	if (!(diag->flags & TA1618_DIAG_F_DMA21_QUIESCED))
		fail(7, "USB DMA21 QUIESCE FAIL");

	ums9117_bootstrap_cleanup_usb_dma_and_disconnect();
	diag->flags |= TA1618_DIAG_F_SOFT_DISCONNECT;
	snapshot_musb(&diag->musb_after_quiesce);
	update_checkpoint("JUMP TO LINUX", FPLINUX_BOOT_SCREEN_ACTIVE);
}

static int enable_and_probe_sprd_timer(void)
{
	struct ums9117_bootstrap_timer_gates gates;
	struct ums9117_bootstrap_timer_result timer;
	int timer_ok;

	ums9117_bootstrap_enable_timer_gates(&gates);

	fprintf(stderr,
		"TA1618_TIMER_GATES eb0=0x%08lx->0x%08lx "
		"rtc=0x%08lx->0x%08lx clk=0x%08lx->0x%08lx\n",
		(unsigned long)gates.eb0_before, (unsigned long)gates.eb0_after,
		(unsigned long)gates.rtc_before, (unsigned long)gates.rtc_after,
		(unsigned long)gates.clk_before,
		(unsigned long)gates.clk_after);

	timer_ok = ums9117_bootstrap_probe_timer(&timer);

	fprintf(stderr,
		"TA1618_SPRD_TIMER syscnt=%lu->%lu polls=%lu "
		"ctl=0x%08lx->0x%08lx value=%lu shadow=%lu int=0x%08lx\n",
		(unsigned long)timer.syscnt_before,
		(unsigned long)timer.syscnt_after, timer.polls,
		(unsigned long)timer.ctl_before, (unsigned long)timer.ctl,
		(unsigned long)timer.value, (unsigned long)timer.shadow,
		(unsigned long)timer.int_status);

	return timer_ok;
}

int main(int argc, char **argv)
{
	uint32_t ram_bytes = *(volatile uint32_t *)(uintptr_t)0x80100000u;
	uint32_t display_width = sys_data.display.w1;
	uint32_t display_height = sys_data.display.h1;
	size_t zimage_bytes = ums9117_bootstrap_zimage_size();
	size_t dtb_bytes = ums9117_bootstrap_dtb_size();
	struct fplinux_boot_screen_canvas canvas;
	const struct fplinux_boot_screen_identity identity = {
		.brand = "FPLinux",
		.variant = "TA-1618",
		.model = "NOKIA 3210 4G",
		.mode = "VOLATILE RAM BOOT",
	};

	(void)argc;
	(void)argv;

	memset((void *)(uintptr_t)DIAG_BASE, 0, sizeof(*diag));
	diag->magic = TA1618_DIAG_MAGIC;
	diag->version = TA1618_DIAG_VERSION;
	diag->struct_bytes = sizeof(*diag);
	diag->usb_spi = TA1618_USB_SPI;
	diag->usb_intid = TA1618_USB_INTID;
	diag->ram_bytes = ram_bytes;
	diag->zimage_addr = ZIMAGE_STAGE;
	diag->zimage_bytes = (uint32_t)zimage_bytes;
	diag->dtb_addr = DTB_STAGE;
	diag->dtb_bytes = (uint32_t)dtb_bytes;
	diag->framebuffer_addr = FRAMEBUFFER;
	diag->framebuffer_bytes = FB_BYTES;

	fprintf(stderr,
		"TA1618_LINUX_BOOTSTRAP stage=0 message=ENTRY "
		"ram=0x%08lx zimage=%lu dtb=%lu\n",
		(unsigned long)ram_bytes, (unsigned long)zimage_bytes,
		(unsigned long)dtb_bytes);

	if (boot_canvas_configure(&boot_canvas, fb, display_width,
				  display_height) != 0)
		fail(1, "BAD DISPLAY SIZE");
	canvas.width = boot_canvas.width;
	canvas.height = boot_canvas.height;
	canvas.context = &boot_canvas;
	canvas.fill_rect = boot_screen_fill_rect;
	canvas.present = boot_screen_present;

	sys_framebuffer(boot_canvas.pixels);
	sys_start();
	if (fplinux_boot_screen_init(&boot_screen, &canvas, &identity,
				     boot_stage_labels, BOOT_STAGE_COUNT) != 0)
		fail(8, "BOOT SCREEN INIT FAIL");

	(void)fplinux_boot_screen_set_stage(&boot_screen, BOOT_STAGE_DISPLAY,
					    FPLINUX_BOOT_SCREEN_ACTIVE);
	if (display_width != FB_WIDTH || display_height != FB_HEIGHT)
		fail(1, "BAD DISPLAY SIZE");
	(void)fplinux_boot_screen_set_stage(&boot_screen, BOOT_STAGE_DISPLAY,
					    FPLINUX_BOOT_SCREEN_DONE);
	record_stage(1, "DISPLAY OK");

	(void)fplinux_boot_screen_set_stage(&boot_screen, BOOT_STAGE_TIMER,
					    FPLINUX_BOOT_SCREEN_ACTIVE);
	if (!enable_and_probe_sprd_timer())
		fail(5, "SPRD TIMER FAIL");
	(void)fplinux_boot_screen_set_stage(&boot_screen, BOOT_STAGE_TIMER,
					    FPLINUX_BOOT_SCREEN_DONE);
	record_stage(2, "SPRD TIMER OK");

	(void)fplinux_boot_screen_set_stage(&boot_screen, BOOT_STAGE_KERNEL,
					    FPLINUX_BOOT_SCREEN_ACTIVE);
	if (ram_bytes < RAM_REQUIRED)
		fail(2, "64MB RAM REQUIRED");
	if (!zimage_bytes || zimage_bytes > 0x01200000u)
		fail(3, "BAD KERNEL SIZE");
	record_stage(3, "COPY KERNEL");
	ums9117_bootstrap_copy_zimage(ZIMAGE_STAGE, zimage_bytes);
	(void)fplinux_boot_screen_set_stage(&boot_screen, BOOT_STAGE_KERNEL,
					    FPLINUX_BOOT_SCREEN_DONE);

	(void)fplinux_boot_screen_set_stage(&boot_screen,
					    BOOT_STAGE_DEVICE_TREE,
					    FPLINUX_BOOT_SCREEN_ACTIVE);
	if (!dtb_bytes || dtb_bytes > 0x00010000u)
		fail(4, "BAD DTB SIZE");
	record_stage(4, "COPY DTB");
	ums9117_bootstrap_copy_dtb(DTB_STAGE, dtb_bytes);
	clean_dcache_range((void *)(uintptr_t)DIAG_BASE,
			   (void *)(uintptr_t)(DIAG_BASE + sizeof(*diag)));
	clean_dcache_range((void *)(uintptr_t)FRAMEBUFFER,
			   (void *)(uintptr_t)(FRAMEBUFFER + FB_BYTES));
	(void)fplinux_boot_screen_set_stage(
		&boot_screen, BOOT_STAGE_DEVICE_TREE, FPLINUX_BOOT_SCREEN_DONE);

	(void)fplinux_boot_screen_set_stage(&boot_screen,
					    BOOT_STAGE_PREPARE_LINUX,
					    FPLINUX_BOOT_SCREEN_ACTIVE);
	record_stage(5, "PREPARE LINUX");
	diag->stage = 6;
	diag_message("JUMP ZIMAGE");
	probe_and_quiesce_usb();
	(void)fplinux_boot_screen_set_stage(&boot_screen,
					    BOOT_STAGE_PREPARE_LINUX,
					    FPLINUX_BOOT_SCREEN_DONE);
	clean_dcache_range((void *)(uintptr_t)DIAG_BASE,
			   (void *)(uintptr_t)(DIAG_BASE + sizeof(*diag)));
	clean_invalidate_dcache();
	invalidate_icache();

	ums9117_linux_handoff(ZIMAGE_STAGE, DTB_STAGE);
}
