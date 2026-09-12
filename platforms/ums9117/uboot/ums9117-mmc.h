/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_UMS9117_UBOOT_MMC_H
#define FPLINUX_UMS9117_UBOOT_MMC_H

#include "ums9117-sdio-slot.h"

const struct ums9117_sdio_slot_board *ums9117_uboot_sdio_board(void);

void ums9117_mmc_release(void);

#endif
