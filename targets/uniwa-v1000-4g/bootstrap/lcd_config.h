// SPDX-License-Identifier: GPL-2.0-only

#include "fplinux-panel-data.h"
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
