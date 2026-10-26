/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LINUX_SOC_SPRD_VBC_EQ6_IO_H
#define LINUX_SOC_SPRD_VBC_EQ6_IO_H

#include <linux/errno.h>
#include <linux/io.h>
#include <linux/soc/sprd/vbc-eq6.h>

#define SPRD_VBC_EQ6_COEFFICIENT_HIGH(n) (0x100 + 8 * (n))
#define SPRD_VBC_EQ6_COEFFICIENT_LOW(n) (0x104 + 8 * (n))

struct sprd_vbc_eq6_mismatch {
	unsigned int coefficient;
	u16 expected;
	u16 observed;
};

/* Scales and module lifetime remain with the caller that silences the route. */
static inline int sprd_vbc_eq6_write_filters(
	void __iomem *vbc,
	const struct sprd_vbc_eq6_section sections[SPRD_VBC_EQ6_SECTION_COUNT],
	struct sprd_vbc_eq6_mismatch *mismatch)
{
	s16 words[SPRD_VBC_EQ6_FILTER_WORDS];
	unsigned int section;
	unsigned int word;
	unsigned int coefficient;
	u16 observed;

	for (section = 0; section < SPRD_VBC_EQ6_SECTION_COUNT; section++) {
		sprd_vbc_eq6_filter_words(&sections[section], words);
		for (word = 0; word < SPRD_VBC_EQ6_FILTER_WORDS; word++) {
			coefficient =
				section * SPRD_VBC_EQ6_SECTION_COEFFICIENTS +
				1 + word;
			writel((u16)words[word],
			       vbc + SPRD_VBC_EQ6_COEFFICIENT_HIGH(
					     coefficient));
			writel(0,
			       vbc + SPRD_VBC_EQ6_COEFFICIENT_LOW(coefficient));
		}
	}
	for (section = 0; section < SPRD_VBC_EQ6_SECTION_COUNT; section++) {
		sprd_vbc_eq6_filter_words(&sections[section], words);
		for (word = 0; word < SPRD_VBC_EQ6_FILTER_WORDS; word++) {
			coefficient =
				section * SPRD_VBC_EQ6_SECTION_COEFFICIENTS +
				1 + word;
			observed = readl(vbc + SPRD_VBC_EQ6_COEFFICIENT_HIGH(
						       coefficient)) &
				   0xffff;
			if (observed == (u16)words[word])
				continue;
			*mismatch = (struct sprd_vbc_eq6_mismatch){
				.coefficient = coefficient,
				.expected = (u16)words[word],
				.observed = observed,
			};
			return -EIO;
		}
	}
	return 0;
}

#endif
