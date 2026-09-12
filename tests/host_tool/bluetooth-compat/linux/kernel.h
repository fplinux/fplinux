/* SPDX-License-Identifier: GPL-2.0-only */
#define ALIGN(value, alignment) \
	(((value) + (alignment) - 1) & ~((alignment) - 1))
#define min_t(type, left, right) \
	((type)(left) < (type)(right) ? (type)(left) : (type)(right))
