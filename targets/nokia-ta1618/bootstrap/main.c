// SPDX-License-Identifier: GPL-2.0-only
#include "ums9117-bootstrap/boot-main.h"

int main(int argc, char **argv)
{
	static const struct ums9117_boot_board board = {
		.display_width = 240,
		.display_height = 320,
	};

	(void)argc;
	(void)argv;

	ums9117_boot_main(&board);
}
