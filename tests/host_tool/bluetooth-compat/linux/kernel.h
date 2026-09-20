/* SPDX-License-Identifier: GPL-2.0-only */
#include <stdbool.h>
#include <stdint.h>

#define U32_MAX UINT32_MAX
#define ALIGN(value, alignment) \
	(((value) + (alignment) - 1) & ~((alignment) - 1))
#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))
#define min(left, right) ((left) < (right) ? (left) : (right))
#define min_t(type, left, right) \
	((type)(left) < (type)(right) ? (type)(left) : (type)(right))

#define local_irq_save(flags) ((flags) = 0)
#define local_irq_restore(flags) ((void)(flags))

static inline bool irqs_disabled(void)
{
	return false;
}
