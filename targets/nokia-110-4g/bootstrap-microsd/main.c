// SPDX-License-Identifier: GPL-2.0-only
#include "generated/fplinux-bootstrap-display.h"
#include "ums9117-bootstrap/sd-stage0.h"

int main(int argc, char **argv)
{
	static const struct ums9117_sd_stage0_board board = {
		.display_width = FPLINUX_BOOTSTRAP_DISPLAY_WIDTH,
		.display_height = FPLINUX_BOOTSTRAP_DISPLAY_HEIGHT,
	};

	(void)argc;
	(void)argv;
	ums9117_sd_stage0_main(&board);
}
