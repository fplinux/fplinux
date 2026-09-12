/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_TEST_SDBOOT_MMC_H
#define FPLINUX_TEST_SDBOOT_MMC_H

struct mmc {
	int initialized;
};
struct mmc *find_mmc_device(int device);
int mmc_init(struct mmc *mmc);

#endif
