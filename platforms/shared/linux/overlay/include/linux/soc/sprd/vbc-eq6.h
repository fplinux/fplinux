/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LINUX_SOC_SPRD_VBC_EQ6_H
#define LINUX_SOC_SPRD_VBC_EQ6_H

#include <linux/types.h>

#define SPRD_VBC_EQ6_SECTION_COUNT 6U
#define SPRD_VBC_EQ6_SECTION_COEFFICIENTS 7U
#define SPRD_VBC_EQ6_FILTER_WORDS 6U
#define SPRD_VBC_EQ6_OUTPUT_SCALE 42U
#define SPRD_VBC_EQ6_A0 16384

/* Q12 input scale and Q14 coefficients; the denominator is stored negated. */
struct sprd_vbc_eq6_section {
	s16 scale;
	s16 b0;
	s16 b1;
	s16 minus_a1;
	s16 b2;
	s16 minus_a2;
};

static inline bool
sprd_vbc_eq6_section_valid(const struct sprd_vbc_eq6_section *section)
{
	int a1 = section->minus_a1;
	int a2 = section->minus_a2;
	int bound = SPRD_VBC_EQ6_A0 - a2;

	return section->scale > 0 && a2 > -SPRD_VBC_EQ6_A0 &&
	       a2 < SPRD_VBC_EQ6_A0 && a1 > -bound && a1 < bound;
}

static inline void
sprd_vbc_eq6_filter_words(const struct sprd_vbc_eq6_section *section,
			  s16 words[SPRD_VBC_EQ6_FILTER_WORDS])
{
	words[0] = section->b0;
	words[1] = SPRD_VBC_EQ6_A0;
	words[2] = section->b1;
	words[3] = section->minus_a1;
	words[4] = section->b2;
	words[5] = section->minus_a2;
}

#endif
