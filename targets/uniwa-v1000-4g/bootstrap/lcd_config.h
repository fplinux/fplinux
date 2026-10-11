// SPDX-License-Identifier: GPL-2.0-only
#define FPLINUX_LCD_SPI 1
#define FPLINUX_LCD_INIT_RESET_MS 16
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

static const uint8_t uniwa_panel_init[] = {
	LCM_DELAY(10),
	LCM_CMD(0xc8, 3),
	0xff,
	0x93,
	0x42,
	LCM_CMD(0x36, 1),
	0x08,
	LCM_CMD(0x35, 1),
	0x00,
	LCM_CMD(0x3a, 1),
	0x55,
	LCM_CMD(0xc0, 2),
	0x13,
	0x0e,
	LCM_CMD(0xc1, 1),
	0x02,
	LCM_CMD(0xc5, 1),
	0xd3,
	LCM_CMD(0xb1, 2),
	0x00,
	0x1b,
	LCM_CMD(0xb4, 1),
	0x02,
	LCM_CMD(0xb6, 2),
	0x0a,
	0xe0,
	LCM_CMD(0xe0, 15),
	0x00,
	0x03,
	0x08,
	0x06,
	0x13,
	0x09,
	0x39,
	0x39,
	0x48,
	0x02,
	0x0a,
	0x08,
	0x17,
	0x17,
	0x0f,
	LCM_CMD(0xe1, 15),
	0x00,
	0x28,
	0x29,
	0x01,
	0x0d,
	0x03,
	0x3f,
	0x33,
	0x52,
	0x04,
	0x0f,
	0x0e,
	0x37,
	0x38,
	0x0f,
	LCM_CMD(0x11, 0),
	LCM_DELAY(120),
	LCM_CMD(0x29, 0),
	LCM_DELAY(20),
	LCM_CMD(0x2c, 0),
	LCM_END,
};

static const lcd_config_t lcd_config_t117[] = {
	{ 0x9342, 0xffff, 320, 240, 0x08, { 0 }, { 48000000 }, uniwa_panel_init },
};
