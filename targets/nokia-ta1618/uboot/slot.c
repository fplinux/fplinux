// SPDX-License-Identifier: GPL-2.0-only
#include "ums9117-mmc.h"
#include "ta1618-sdio-board.h"

const struct ums9117_sdio_slot_board *ums9117_uboot_sdio_board(void)
{
	return &ta1618_sdio_board;
}
