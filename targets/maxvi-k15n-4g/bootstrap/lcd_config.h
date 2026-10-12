// SPDX-License-Identifier: GPL-2.0-only

#include "fplinux-panel-data.h"
#define FPLINUX_LCD_PREPARE maxvi_lcd_prepare

static void maxvi_lcd_prepare(void)
{
	unsigned int index;

	for (index = 0;
	     index < sizeof(fplinux_panel_pads) / sizeof(fplinux_panel_pads[0]);
	     index++)
		MEM4(0x402a0000U + fplinux_panel_pads[index].offset) =
			fplinux_panel_pads[index].value;
}
