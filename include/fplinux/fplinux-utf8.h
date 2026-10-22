/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_UTF8_H
#define FPLINUX_UTF8_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Decode one UTF-8 scalar, including U+0000, in [*cursor, end), within one
 * byte array. Empty or invalid input returns false and leaves cursor and
 * value unchanged.
 */
bool fplinux_utf8_next(const unsigned char **cursor, const unsigned char *end,
		       uint32_t *value);

#endif
