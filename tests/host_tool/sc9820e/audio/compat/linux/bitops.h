/* SPDX-License-Identifier: GPL-2.0-only */
#define BIT(n) (1UL << (n))
#define GENMASK(high, low) (((~0UL) >> (63 - (high))) & ((~0UL) << (low)))
