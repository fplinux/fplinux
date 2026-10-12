/* SPDX-License-Identifier: GPL-2.0-only */
#include <stdint.h>
#include <stdio.h>

/* The adapter supplies the fixed table ABI; this exercises generated data. */
typedef struct {
	uint32_t id;
	uint32_t id_mask;
	uint16_t width;
	uint16_t height;
	uint16_t memory_access;
	uint8_t timing_ns[6];
	struct {
		uint32_t hz;
	} spi;
	const uint8_t *commands;
} lcd_config_t;

#define LCM_CMD(command, length) (0x80 | (length)), (command)
#define LCM_DELAY(ms) (0x40 | (((ms) >> 8) & 0x1f)), ((ms) & 0xff)
#define LCM_END 0
#include "fplinux-panel-data.h"

int main(void)
{
	const lcd_config_t *panel = &lcd_config_t117[0];
	size_t index;

	printf("%u %u %u %u %u\n", panel->id, panel->id_mask, panel->width,
	       panel->height, panel->memory_access);
	for (index = 0; index < sizeof(fplinux_panel_init); index++)
		printf("%02x", fplinux_panel_init[index]);
	putchar('\n');
	for (index = 0;
	     index < sizeof(fplinux_panel_pads) / sizeof(fplinux_panel_pads[0]);
	     index++)
		printf("%x:%x ", fplinux_panel_pads[index].offset,
		       fplinux_panel_pads[index].value);
	putchar('\n');
	return 0;
}
