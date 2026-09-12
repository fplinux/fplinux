// SPDX-License-Identifier: GPL-2.0-only
#include "ums9117-bootstrap/sd-stage0.h"

int main(int argc, char **argv)
{
	static const struct ums9117_sd_stage0_board board = {
		.display_width = 240,
		.display_height = 320,
	};

	(void)argc;
	(void)argv;
	ums9117_sd_stage0_main(&board);
}
