// SPDX-License-Identifier: GPL-2.0-only

#include "early-init.h"
#include "fplinux-panel-data.h"

static const struct ums9117_bootstrap_early_write uniwa_early_writes[] = {
	/* Composite board routes before peripheral initialization. */
	{ 0x402a0010U, 0x0 },
	{ 0x402a0014U, 0x0 },
	{ 0x402a001cU, 0x0 },
	{ 0x402a0020U, 0x0 },
	{ 0x402a0024U, 0x3b00 },
	{ 0x402a0004U, 0x0 },
	{ 0x402a0008U, 0x0 },
	{ 0x402a000cU, 0x5b0 },
	/* Radio and antenna routes remain available to their firmware consumer. */
	{ 0x402a0028U, 0x0 },
	{ 0x402a0428U, 0x8c004 },
	{ 0x402a002cU, 0x0 },
	{ 0x402a042cU, 0x8c044 },
	{ 0x402a0030U, 0x0 },
	{ 0x402a0430U, 0x8c001 },
	{ 0x402a0034U, 0x0 },
	{ 0x402a0434U, 0x8c001 },
	{ 0x402a0038U, 0x0 },
	{ 0x402a0438U, 0x8c001 },
	{ 0x402a003cU, 0x0 },
	{ 0x402a043cU, 0x8c001 },
	{ 0x402a0040U, 0x0 },
	{ 0x402a0440U, 0x8c001 },
	{ 0x402a0044U, 0x30 },
	{ 0x402a0444U, 0x82001 },
	{ 0x402a0048U, 0x30 },
	{ 0x402a0448U, 0x82001 },
	{ 0x402a004cU, 0x30 },
	{ 0x402a044cU, 0x82089 },
	{ 0x402a0050U, 0x30 },
	{ 0x402a0450U, 0x82001 },
	{ 0x402a0054U, 0x0 },
	{ 0x402a0454U, 0x82001 },
	{ 0x402a0058U, 0x0 },
	{ 0x402a0458U, 0x82001 },
	{ 0x402a005cU, 0x30 },
	{ 0x402a045cU, 0x82001 },
	{ 0x402a0060U, 0x30 },
	{ 0x402a0460U, 0x82001 },
	{ 0x402a0064U, 0x30 },
	{ 0x402a0464U, 0x82002 },
	{ 0x402a0068U, 0x30 },
	{ 0x402a0468U, 0x82004 },
	{ 0x402a0070U, 0x10 },
	{ 0x402a0470U, 0x8208a },
	{ 0x402a0074U, 0x10 },
	{ 0x402a0474U, 0x8208a },
	{ 0x402a006cU, 0x30 },
	{ 0x402a046cU, 0x82001 },
	{ 0x402a0078U, 0x10 },
	{ 0x402a0478U, 0x82001 },
	{ 0x402a007cU, 0x10 },
	{ 0x402a047cU, 0x8208a },
	/* Card-detect, ADI, clock, interrupt, reset and sleep pads. */
	{ 0x402a00dcU, 0x0 },
	{ 0x402a04dcU, 0x8208a },
	{ 0x402a00e4U, 0x0 },
	{ 0x402a04e4U, 0x80004 },
	{ 0x402a00ecU, 0x0 },
	{ 0x402a04ecU, 0x82001 },
	{ 0x402a00f4U, 0x0 },
	{ 0x402a04f4U, 0x82002 },
	{ 0x402a0100U, 0x0 },
	{ 0x402a0500U, 0x8e002 },
	{ 0x402a0110U, 0x0 },
	{ 0x402a0510U, 0x82002 },
	{ 0x402a0114U, 0x0 },
	{ 0x402a0514U, 0x80004 },
	{ 0x402a0118U, 0x0 },
	{ 0x402a0518U, 0x80004 },
	{ 0x402a011cU, 0x0 },
	{ 0x402a051cU, 0x82001 },
	/* Preserve the NAND ready/busy pad baseline across controller ownership. */
	{ 0x402a0558U, 0x18208a },
	/* Camera GPIO routes are restored after temporary NAND ownership. */
	{ 0x402a0170U, 0x0 },
	{ 0x402a0570U, 0x182000 },
	/* The NAND controller does not program the DATA6 pad configuration. */
	{ 0x402a0578U, 0x182000 },
	/* Radio and antenna routes remain available to their firmware consumer. */
	{ 0x402a0220U, 0x30 },
	{ 0x402a0620U, 0x82044 },
	{ 0x402a0224U, 0x30 },
	{ 0x402a0624U, 0x82001 },
	{ 0x402a0228U, 0x30 },
	{ 0x402a0628U, 0x82001 },
	{ 0x402a022cU, 0x30 },
	{ 0x402a062cU, 0x82044 },
	{ 0x402a0230U, 0x30 },
	{ 0x402a0630U, 0x82000 },
	{ 0x402a0234U, 0x30 },
	{ 0x402a0634U, 0x82044 },
	{ 0x402a0238U, 0x30 },
	{ 0x402a0638U, 0x82000 },
	{ 0x402a023cU, 0x30 },
	{ 0x402a063cU, 0x82000 },
	{ 0x402a0240U, 0x0 },
	{ 0x402a0640U, 0x8e004 },
	{ 0x402a0244U, 0x0 },
	{ 0x402a0644U, 0x8e004 },
	{ 0x402a0248U, 0x0 },
	{ 0x402a0648U, 0x8e004 },
	/* Analog clock requests, test-pad state and board reset routing. */
	{ 0x4060860cU, 0x142 },
	{ 0x40608610U, 0x111 },
	{ 0x40608614U, 0x142 },
	{ 0x40608618U, 0x142 },
	{ 0x40608600U, 0x140 },
};

#define FPLINUX_BOARD_INIT()                                      \
	ums9117_bootstrap_early_init(uniwa_early_writes,          \
				     sizeof(uniwa_early_writes) / \
					     sizeof(uniwa_early_writes[0]))
#define FPLINUX_LCD_SEND uniwa_lcd_send
#define FPLINUX_LCD_BACKLIGHT_START uniwa_backlight_start

#define UNIWA_SPI_BASE_PHYS 0x70b00000U
#define UNIWA_WLED_CONTROL_PHYS 0x40608180U
#define UNIWA_WLED_CURRENT_PHYS 0x406081c4U
#define UNIWA_WLED_DUTY_PHYS 0x406081ccU
#define UNIWA_SPI_TIMEOUT_MS 250U

static void uniwa_spi_wait(uint32_t offset, uint32_t mask, uint32_t value)
{
	uint32_t started = sys_timer_ms();

	while ((MEM4(UNIWA_SPI_BASE_PHYS + offset) & mask) != value) {
		if ((uint32_t)(sys_timer_ms() - started) >=
		    UNIWA_SPI_TIMEOUT_MS)
			ERR_EXIT("LCD SPI transfer timed out\n");
	}
}

static void uniwa_backlight_start(void)
{
	uint32_t current = adi_read(UNIWA_WLED_CURRENT_PHYS);
	uint32_t control = adi_read(UNIWA_WLED_CONTROL_PHYS);

	adi_write(UNIWA_WLED_CURRENT_PHYS, (current & ~0x3fU) | 0x22U);
	adi_write(UNIWA_WLED_DUTY_PHYS, 0x7fffU);
	adi_write(UNIWA_WLED_CONTROL_PHYS, control | 0x3000U);
}

static void uniwa_lcd_send(unsigned index, unsigned data)
{
	uintptr_t base = UNIWA_SPI_BASE_PHYS;
	uint32_t control;

	control = (MEM4(base + 0x008) & ~0xf7cU) | (8U << 2);
	MEM4(base + 0x008) = control;
	control = MEM4(base + 0x054) & ~0x83ffU;
	if (index)
		control |= 0x8000U;
	MEM4(base + 0x054) = control;
	MEM4(base + 0x058) = (MEM4(base + 0x058) & ~0xffffU) | 1U;
	MEM4(base + 0x024) = 0x100U;
	MEM4(base + 0x064) |= 2U;
	MEM4(base + 0x000) = data;
	uniwa_spi_wait(0x028, 0x100, 0x100);
	MEM4(base + 0x024) = 0x100U;
	uniwa_spi_wait(0x034, 0x180, 0x080);
	MEM4(base + 0x054) |= 0x8000U;
	/* Memory Write leaves CS selected for the bootstrap pixel transfer. */
	if (index || data != 0x2cU)
		MEM4(base + 0x008) |= 0x100U;
}
