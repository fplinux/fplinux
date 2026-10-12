// SPDX-License-Identifier: GPL-2.0-only

#include "fplinux-panel-data.h"

#define FPLINUX_LCD_SEND nokia110_lcd_send

static void spi_send(unsigned index, unsigned data);

static void nokia110_lcd_send(unsigned index, unsigned data)
{
	uint32_t control = MEM4(0x70b00008);

	/* Each command byte selects CS0 with an eight-bit channel. */
	MEM4(0x70b00008) = (control & ~0x817cU) | (8U << 2);
	spi_send(index, data);
	/* Memory Write keeps CS0 selected for the bootstrap pixel transfer. */
	if (index || data != 0x2cU)
		MEM4(0x70b00008) |= 0x100U;
}
