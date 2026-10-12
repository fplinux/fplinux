/* SPDX-License-Identifier: GPL-2.0-only */
#include <stdio.h>
#include "ums9117-drm.h"
#include "fplinux-demo-panel.h"
#include "fplinux-demo-panel-addresses.h"

static void print_commands(const struct ums9117_drm_command *commands,
			   size_t count)
{
	size_t index, byte;

	for (index = 0; index < count; index++) {
		printf("%02x/%u/%u/", commands[index].command,
		       commands[index].length, commands[index].delay_ms);
		for (byte = 0; byte < commands[index].length; byte++)
			printf("%02x", commands[index].data[byte]);
		putchar(' ');
	}
	putchar('\n');
}

#define COMMAND_COUNT(array) (sizeof(array) / sizeof((array)[0]))
int main(void)
{
	printf("%u %u\n", FPLINUX_PANEL_WIDTH, FPLINUX_PANEL_HEIGHT);
	printf("%x %x %x %x\n", FPLINUX_PANEL_CONTROL_MUX_PHYS,
	       FPLINUX_PANEL_CONTROL_CONFIG_PHYS, FPLINUX_PANEL_DATA_MUX_PHYS,
	       FPLINUX_PANEL_DATA_CONFIG_PHYS);
	print_commands(fplinux_panel_init, COMMAND_COUNT(fplinux_panel_init));
	print_commands(fplinux_panel_cold_finish,
		       COMMAND_COUNT(fplinux_panel_cold_finish));
	print_commands(fplinux_panel_wake_finish,
		       COMMAND_COUNT(fplinux_panel_wake_finish));
	return 0;
}
