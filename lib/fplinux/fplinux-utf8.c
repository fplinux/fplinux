/* SPDX-License-Identifier: GPL-2.0-only */
#include "fplinux-utf8.h"

#include <stddef.h>

bool fplinux_utf8_next(const unsigned char **cursor, const unsigned char *end,
		       uint32_t *value)
{
	const unsigned char *text = *cursor;
	uint32_t codepoint;
	unsigned int count;
	unsigned int index;

	if (text == end)
		return false;
	if (*text < 0x80) {
		*value = *text;
		*cursor = text + 1;
		return true;
	}
	if (*text >= 0xc2 && *text <= 0xdf) {
		codepoint = *text & 0x1f;
		count = 2;
	} else if (*text >= 0xe0 && *text <= 0xef) {
		codepoint = *text & 0x0f;
		count = 3;
	} else if (*text >= 0xf0 && *text <= 0xf4) {
		codepoint = *text & 7;
		count = 4;
	} else {
		return false;
	}
	if ((size_t)(end - text) < count)
		return false;
	for (index = 1; index < count; ++index) {
		if ((text[index] & 0xc0) != 0x80)
			return false;
		codepoint = (codepoint << 6) | (text[index] & 0x3f);
	}
	if ((count == 3 && codepoint < 0x800) ||
	    (count == 4 && codepoint < 0x10000) || codepoint > 0x10ffff ||
	    (codepoint >= 0xd800 && codepoint <= 0xdfff))
		return false;
	*value = codepoint;
	*cursor = text + count;
	return true;
}
