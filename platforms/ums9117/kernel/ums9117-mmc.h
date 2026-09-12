/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_UMS9117_MMC_H
#define FPLINUX_UMS9117_MMC_H

struct platform_device;
struct ums9117_sdio_slot_board;

int ums9117_mmc_probe(struct platform_device *pdev,
		      const struct ums9117_sdio_slot_board *board);
void ums9117_mmc_remove(struct platform_device *pdev);
void ums9117_mmc_shutdown(struct platform_device *pdev);

#endif
