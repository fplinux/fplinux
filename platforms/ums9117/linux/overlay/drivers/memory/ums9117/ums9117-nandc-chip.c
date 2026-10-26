// SPDX-License-Identifier: GPL-2.0-only
#include <linux/kernel.h>

#include "ums9117-nandc-chip.h"

/* Complete physical layouts, including the OOB bytes of each page. */
static const struct ums9117_nandc_chip ums9117_nandc_chips[] = {
	{
		.name = "FM25LG01B",
		.manufacturer_id = 0xa1,
		.device_id = 0xb1,
		.page_main_bytes = 2048,
		.oob_bytes = 128,
		.pages_per_block = 64,
		.block_count = 1024,
		.plane_count = 1,
		.feature_b0_defined_mask = 0xff,
	},
	{
		.name = "FM25LS01",
		.manufacturer_id = 0xa1,
		.device_id = 0xa5,
		.page_main_bytes = 2048,
		.oob_bytes = 128,
		.pages_per_block = 64,
		.block_count = 1024,
		.plane_count = 1,
		.feature_b0_defined_mask = 0xf0,
	},
	{
		.name = "DS35M1GA",
		.manufacturer_id = 0xe5,
		.device_id = 0x21,
		.page_main_bytes = 2048,
		.oob_bytes = 64,
		.pages_per_block = 64,
		.block_count = 1024,
		.plane_count = 1,
		.feature_b0_defined_mask = 0xff,
	},
	{
		.name = "GD5F1GQ4RExxG",
		.manufacturer_id = 0xc8,
		.device_id = 0xc1,
		.page_main_bytes = 2048,
		.oob_bytes = 128,
		.pages_per_block = 64,
		.block_count = 1024,
		.plane_count = 1,
		.feature_b0_defined_mask = 0xff,
	},
	{
		.name = "GD5F1GQ5RExxG",
		.manufacturer_id = 0xc8,
		.device_id = 0x41,
		.page_main_bytes = 2048,
		.oob_bytes = 128,
		.pages_per_block = 64,
		.block_count = 1024,
		.plane_count = 1,
		.feature_b0_defined_mask = 0xff,
	},
	{
		.name = "GD5F1GM7RExxG",
		.manufacturer_id = 0xc8,
		.device_id = 0x81,
		.page_main_bytes = 2048,
		.oob_bytes = 128,
		.pages_per_block = 64,
		.block_count = 1024,
		.plane_count = 1,
		.feature_b0_defined_mask = 0xff,
	},
	{
		.name = "GD5F1GQ5RExxH",
		.manufacturer_id = 0xc8,
		.device_id = 0x21,
		.page_main_bytes = 2048,
		.oob_bytes = 64,
		.pages_per_block = 64,
		.block_count = 1024,
		.plane_count = 1,
		.feature_b0_defined_mask = 0xff,
	},
	{
		.name = "F50D1G41LB",
		.manufacturer_id = 0xc8,
		.device_id = 0x11,
		.page_main_bytes = 2048,
		.oob_bytes = 64,
		.pages_per_block = 64,
		.block_count = 1024,
		.plane_count = 1,
		.feature_b0_defined_mask = 0xff,
	},
	{
		.name = "MX35UF1G14AC",
		.manufacturer_id = 0xc2,
		.device_id = 0x90,
		.page_main_bytes = 2048,
		.oob_bytes = 64,
		.pages_per_block = 64,
		.block_count = 1024,
		.plane_count = 1,
		.feature_b0_defined_mask = 0xff,
	},
	{
		.name = "F35UQA001G",
		.manufacturer_id = 0xcd,
		.device_id = 0x61,
		.page_main_bytes = 2048,
		.oob_bytes = 64,
		.pages_per_block = 64,
		.block_count = 1024,
		.plane_count = 1,
		.feature_b0_defined_mask = 0xd7,
	},
};

static bool ums9117_nandc_chip_supported(const struct ums9117_nandc_chip *chip)
{
	return chip->page_main_bytes == UMS9117_NANDC_PAGE_MAIN_BYTES &&
	       chip->oob_bytes <= UMS9117_NANDC_OOB_MAX_BYTES &&
	       chip->pages_per_block == UMS9117_NANDC_PAGES_PER_BLOCK &&
	       chip->plane_count == 1 &&
	       (u32)chip->block_count * chip->pages_per_block <=
		       UMS9117_NANDC_PAGE_COUNT_MAX;
}

const struct ums9117_nandc_chip *ums9117_nandc_find_chip(u32 id_raw)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(ums9117_nandc_chips); i++) {
		const struct ums9117_nandc_chip *chip = &ums9117_nandc_chips[i];
		/* READ_ID places the manufacturer in the low byte. */
		u32 chip_id = (u32)chip->device_id << 8 | chip->manufacturer_id;

		if (id_raw == chip_id)
			return ums9117_nandc_chip_supported(chip) ? chip : NULL;
	}
	return NULL;
}
