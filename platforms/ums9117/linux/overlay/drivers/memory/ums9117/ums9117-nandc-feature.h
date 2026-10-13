/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_UMS9117_NANDC_FEATURE_H
#define FPLINUX_UMS9117_NANDC_FEATURE_H

#include <linux/types.h>

u32 ums9117_nandc_b0_defined(u32 value, u8 defined_mask);
bool ums9117_nandc_b0_matches(u32 observed, u32 expected, u8 defined_mask);

#endif
