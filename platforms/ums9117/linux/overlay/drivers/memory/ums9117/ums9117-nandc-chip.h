/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_UMS9117_NANDC_CHIP_H
#define FPLINUX_UMS9117_NANDC_CHIP_H

#include <linux/types.h>

/* The fixed reader transfers two 1 KiB main sectors and one OOB area. */
#define UMS9117_NANDC_PAGE_MAIN_BYTES 2048U
#define UMS9117_NANDC_PAGES_PER_BLOCK 64U
#define UMS9117_NANDC_OOB_MAX_BYTES 128U
#define UMS9117_NANDC_PAGE_COUNT_MAX 65536U

struct ums9117_nandc_chip {
	const char *name;
	u8 manufacturer_id;
	u8 device_id;
	u16 page_main_bytes;
	u16 oob_bytes;
	u16 pages_per_block;
	u16 block_count;
	u8 plane_count;
	u8 feature_b0_defined_mask;
};

const struct ums9117_nandc_chip *ums9117_nandc_find_chip(u32 id_raw);

#endif
