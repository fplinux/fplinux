/* SPDX-License-Identifier: GPL-2.0-only */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include "ums9117-bootstrap/boot-main.h"

void ums9117_boot_main(const struct ums9117_boot_board *board)
{
	printf("%" PRIu32 " %" PRIu32 "\n", board->display_width,
	       board->display_height);
	exit(0);
}
