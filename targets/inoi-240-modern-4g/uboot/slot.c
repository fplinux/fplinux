// SPDX-License-Identifier: GPL-2.0-only
#include "ums9117-mmc.h"
#include "inoi240-sdio-board.h"

const struct ums9117_sdio_slot_board *ums9117_uboot_sdio_board(void)
{
	return &inoi240_sdio_board;
}
