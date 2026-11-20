/* SPDX-License-Identifier: GPL-2.0-only */
#include <errno.h>
#define ALIGN(value, alignment) \
	(((value) + (alignment) - 1) & ~((alignment) - 1))
#define min(a, b) ((a) < (b) ? (a) : (b))
#define min_t(type, a, b) min((type)(a), (type)(b))
