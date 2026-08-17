// SPDX-License-Identifier: GPL-2.0-only
#include "ums9117-bootstrap/boot-main.h"

int main(int argc, char **argv)
{
	static const struct ums9117_boot_board board = {
		.marker = "INOI240",
		.identity = {
			.brand = "FPLinux",
			.variant = "UMS9117",
			.model = "INOI 240 MODERN 4G",
			.mode = "VOLATILE RAM BOOT",
		},
		.display_width = 128,
		.display_height = 160,
		.hooks = NULL,
	};

	(void)argc;
	(void)argv;

	ums9117_boot_main(&board);
}
