// SPDX-License-Identifier: GPL-2.0-only
#include <stdbool.h>
#include <stdio.h>

#include "ums9117-nandc-feature.h"

static int test_feature_values(void)
{
	static const struct {
		u32 observed;
		u8 defined_mask;
		u32 expected;
	} cases[] = {
		{ 0x38, 0xd7, 0x10 },	{ 0x28, 0xd7, 0x00 },
		{ 0x3f, 0xd7, 0x17 },	{ 0x2f, 0xd7, 0x07 },
		{ 0xbf, 0xd7, 0x97 },	{ 0xaf, 0xd7, 0x87 },
		{ 0x40, 0xd7, 0x40 },	{ 0x38, 0xff, 0x38 },
		{ 0x28, 0xff, 0x28 },	{ 0xff, 0xff, 0xff },
		{ 0x138, 0xd7, 0x110 }, { 0x138, 0xff, 0x138 },
	};
	unsigned int i;
	int failed = 0;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		u32 actual = ums9117_nandc_b0_defined(cases[i].observed,
						      cases[i].defined_mask);

		if (actual == cases[i].expected)
			continue;
		fprintf(stderr,
			"B0 value 0x%x mask 0x%02x: got 0x%x, expected 0x%x\n",
			cases[i].observed, cases[i].defined_mask, actual,
			cases[i].expected);
		failed = 1;
	}
	return failed;
}

static int test_readback_matching(void)
{
	static const struct {
		const char *scenario;
		u32 observed;
		u32 expected;
		u8 defined_mask;
		bool matches;
	} cases[] = {
		{ "raw reserved bits differ", 0x00, 0x28, 0xd7, true },
		{ "restored reserved bits differ", 0x10, 0x38, 0xd7, true },
		{ "raw ECC remains on", 0x10, 0x28, 0xd7, false },
		{ "restored ECC remains off", 0x00, 0x38, 0xd7, false },
		{ "QE changed", 0x11, 0x38, 0xd7, false },
		{ "drive bit 1 changed", 0x12, 0x38, 0xd7, false },
		{ "drive bit 2 changed", 0x14, 0x38, 0xd7, false },
		{ "OTP enable changed", 0x50, 0x38, 0xd7, false },
		{ "OTP lock changed", 0x90, 0x38, 0xd7, false },
		{ "defined non-ECC state preserved", 0x97, 0xbf, 0xd7, true },
		{ "upper register bit set", 0x110, 0x38, 0xd7, false },
		{ "full byte raw match", 0x28, 0x28, 0xff, true },
		{ "full byte restored match", 0x38, 0x38, 0xff, true },
		{ "full byte reserved bit 3 differs", 0x30, 0x38, 0xff, false },
		{ "full byte reserved bit 5 differs", 0x18, 0x38, 0xff, false },
		{ "full byte upper register bit set", 0x138, 0x38, 0xff,
		  false },
	};
	unsigned int i;
	int failed = 0;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		bool matches = ums9117_nandc_b0_matches(cases[i].observed,
							cases[i].expected,
							cases[i].defined_mask);

		if (matches == cases[i].matches)
			continue;
		fprintf(stderr, "B0 readback: %s\n", cases[i].scenario);
		failed = 1;
	}
	return failed;
}

int main(void)
{
	return test_feature_values() | test_readback_matching();
}
