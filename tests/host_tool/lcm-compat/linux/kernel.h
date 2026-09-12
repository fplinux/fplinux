/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LCM_HOST_KERNEL_H
#define LCM_HOST_KERNEL_H

#include <errno.h>
#include <stdint.h>

#define BIT(bit) (1UL << (bit))
#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))
#define DIV_ROUND_UP(value, divisor) (((value) + (divisor) - 1) / (divisor))
#define min(left, right) ((left) < (right) ? (left) : (right))
#define max(left, right) ((left) > (right) ? (left) : (right))
#define IS_ERR(pointer) ((uintptr_t)(pointer) >= (uintptr_t)-4095)
#define PTR_ERR(pointer) ((long)(pointer))
#define WARN_ON_ONCE(condition) (!!(condition))

#endif
