// SPDX-License-Identifier: GPL-2.0-only

#include <stdint.h>
#include "fplinux-panel-data.h"

#define FPLINUX_LCD_PREPARE bq2830l_lcd_prepare

#define BQ2830L_DISPLAY_PADS_PHYS 0x402a0000U

static void bq2830l_lcd_reg_write(uint32_t address, uint32_t value)
{
	*(volatile uint32_t *)(uintptr_t)address = value;
}

static void bq2830l_lcd_prepare(void)
{
	unsigned int index;

	for (index = 0;
	     index < sizeof(fplinux_panel_pads) / sizeof(fplinux_panel_pads[0]);
	     index++)
		bq2830l_lcd_reg_write(BQ2830L_DISPLAY_PADS_PHYS +
					      fplinux_panel_pads[index].offset,
				      fplinux_panel_pads[index].value);
}
