/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_TEST_JPEG_LINUX_BITOPS_H
#define FPLINUX_TEST_JPEG_LINUX_BITOPS_H

static inline int fls(unsigned int value)
{
	int position = 0;

	while (value) {
		++position;
		value >>= 1;
	}
	return position;
}

#endif
