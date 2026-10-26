// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stddef.h>
#include <string.h>

#include "ums9117-nandc-chip.h"
#include "ums9117-nandc-feature.h"

static void check_chip_layouts(void)
{
	static const struct {
		u32 id;
		const char *name;
		u8 manufacturer;
		u8 device;
		u16 oob_bytes;
		u8 defined_mask;
		u32 raw_bytes;
	} cases[] = {
		{ 0x21e5, "DS35M1GA", 0xe5, 0x21, 64, 0xff, 138412032 },
		{ 0xa5a1, "FM25LS01", 0xa1, 0xa5, 128, 0xf0, 142606336 },
		{ 0xb1a1, "FM25LG01B", 0xa1, 0xb1, 128, 0xff, 142606336 },
	};
	size_t i;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		const struct ums9117_nandc_chip *chip =
			ums9117_nandc_find_chip(cases[i].id);

		assert(chip);
		assert(strcmp(chip->name, cases[i].name) == 0);
		assert(chip->manufacturer_id == cases[i].manufacturer);
		assert(chip->device_id == cases[i].device);
		assert(chip->page_main_bytes == 2048);
		assert(chip->oob_bytes == cases[i].oob_bytes);
		assert(chip->pages_per_block == 64);
		assert(chip->block_count == 1024);
		assert(chip->plane_count == 1);
		assert(chip->feature_b0_defined_mask == cases[i].defined_mask);
		assert((u32)(chip->page_main_bytes + chip->oob_bytes) *
			       chip->pages_per_block * chip->block_count ==
		       cases[i].raw_bytes);
	}
}

static void check_unknown_id(void)
{
	assert(ums9117_nandc_find_chip(0) == NULL);
	assert(ums9117_nandc_find_chip(0xa4a1) == NULL);
	assert(ums9117_nandc_find_chip(0xa1a5) == NULL);
	assert(ums9117_nandc_find_chip(0x0100a5a1) == NULL);
}

static void check_fm25ls01_feature_bits(void)
{
	const struct ums9117_nandc_chip *chip = ums9117_nandc_find_chip(0xa5a1);
	u8 mask;

	assert(chip);
	mask = chip->feature_b0_defined_mask;
	assert(ums9117_nandc_b0_defined(0x1f, mask) == 0x10);
	assert(ums9117_nandc_b0_defined(0xaf, mask) == 0xa0);
	assert(ums9117_nandc_b0_matches(0x0f, 0x00, mask));
	assert(ums9117_nandc_b0_matches(0x1f, 0x10, mask));
	assert(!ums9117_nandc_b0_matches(0x10, 0x00, mask));
	assert(!ums9117_nandc_b0_matches(0x00, 0x10, mask));
	assert(!ums9117_nandc_b0_matches(0x30, 0x10, mask));
	assert(!ums9117_nandc_b0_matches(0x50, 0x10, mask));
	assert(!ums9117_nandc_b0_matches(0x90, 0x10, mask));
	assert(!ums9117_nandc_b0_matches(0x110, 0x10, mask));
}

int main(void)
{
	check_chip_layouts();
	check_unknown_id();
	check_fm25ls01_feature_bits();
	return 0;
}
