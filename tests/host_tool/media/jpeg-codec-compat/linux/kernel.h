/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_TEST_JPEG_LINUX_KERNEL_H
#define FPLINUX_TEST_JPEG_LINUX_KERNEL_H

#include <stdint.h>

#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))
#define DIV_ROUND_CLOSEST(value, divisor) \
	(((value) + (divisor) / 2) / (divisor))
#define IS_ALIGNED(value, alignment) (!((value) & ((alignment) - 1)))
#define U16_MAX UINT16_MAX

#define clamp_t(type, value, lower, upper)               \
	((type)(value) < (type)(lower) ? (type)(lower) : \
	 (type)(value) > (type)(upper) ? (type)(upper) : \
					 (type)(value))

#endif
