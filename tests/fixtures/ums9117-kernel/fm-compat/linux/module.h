/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_FM_HOST_MODULE_H
#define FPLINUX_FM_HOST_MODULE_H

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <linux/device.h>
#include <linux/types.h>

#define THIS_MODULE NULL
#define ERR_PTR(error) ((void *)(intptr_t)(error))
#define container_of(ptr, type, member) \
	((type *)((char *)(ptr) - offsetof(type, member)))
#define clamp_t(type, value, low, high)  \
	((type)(value) < (low) ? (low) : \
				 ((type)(value) > (high) ? (high) : (value)))
#define DIV_ROUND_CLOSEST(value, divisor) \
	(((value) + (divisor) / 2) / (divisor))

static inline void strscpy(char *dest, const char *src, size_t bytes)
{
	strncpy(dest, src, bytes - 1);
	dest[bytes - 1] = 0;
}

#endif
