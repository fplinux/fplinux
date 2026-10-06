// SPDX-License-Identifier: GPL-2.0-only
#include "ums9117-nandc-feature.h"

u32 ums9117_nandc_b0_defined(u32 value, u8 defined_mask)
{
	/* Keep upper register bits so malformed feature reads still mismatch. */
	return value & ((u32)defined_mask | ~0xffU);
}

bool ums9117_nandc_b0_matches(u32 observed, u32 expected, u8 defined_mask)
{
	return ums9117_nandc_b0_defined(observed, defined_mask) ==
	       ums9117_nandc_b0_defined(expected, defined_mask);
}
