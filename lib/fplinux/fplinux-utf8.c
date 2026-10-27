/* SPDX-License-Identifier: GPL-2.0-only */
#include "fplinux-utf8.h"

#include <stddef.h>
#include <string.h>

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

static uint32_t folded_next(const unsigned char **cursor,
			    const unsigned char *end)
{
	uint32_t value;

	if (!fplinux_utf8_next(cursor, end, &value)) {
		value = 0x110000 + **cursor;
		(*cursor)++;
	}
	if (value >= 'A' && value <= 'Z')
		return value + ('a' - 'A');
	if (value >= 0x410 && value <= 0x42f)
		return value + 0x20;
	return value == 0x401 ? 0x451 : value;
}

bool fplinux_utf8_contains_casefold(const char *text, const char *query)
{
	const unsigned char *start = (const unsigned char *)text;
	const unsigned char *needle = (const unsigned char *)query;
	const unsigned char *end;
	const unsigned char *needle_end;

	if (!text || !query)
		return false;
	end = start + strlen(text);
	needle_end = needle + strlen(query);
	for (;;) {
		const unsigned char *candidate = start;
		const unsigned char *part = needle;
		bool matches = true;

		while (part != needle_end && candidate != end) {
			if (folded_next(&candidate, end) !=
			    folded_next(&part, needle_end)) {
				matches = false;
				break;
			}
		}
		if (matches && part == needle_end)
			return true;
		if (start == end)
			return false;
		folded_next(&start, end);
	}
}
