/* SPDX-License-Identifier: GPL-2.0-only */
#include "fplinux-utf8.h"

#include <assert.h>
#include <locale.h>
#include <stddef.h>

static void verify_sequence(void)
{
	static const unsigned char text[] = {
		0x41, 0xc3, 0xa9, 0xe2, 0x82, 0xac, 0xf0, 0x9f, 0x99, 0x82,
	};
	static const struct {
		uint32_t codepoint;
		size_t offset;
	} decoded[] = {
		{ 0x41, 1 },
		{ 0xe9, 3 },
		{ 0x20ac, 6 },
		{ 0x1f642, 10 },
	};
	const unsigned char *cursor = text;
	uint32_t value = 0;

	for (size_t index = 0; index < sizeof(decoded) / sizeof(decoded[0]);
	     ++index) {
		assert(fplinux_utf8_next(&cursor, text + sizeof(text), &value));
		assert(value == decoded[index].codepoint);
		assert(cursor == text + decoded[index].offset);
	}
	assert(!fplinux_utf8_next(&cursor, text + sizeof(text), &value));
	assert(cursor == text + sizeof(text));
	assert(value == 0x1f642);
}

static void verify_scalar_boundaries(void)
{
	static const struct {
		unsigned char bytes[4];
		size_t size;
		uint32_t codepoint;
	} accepted[] = {
		{ { 0x00 }, 1, 0x00 },
		{ { 0x7f }, 1, 0x7f },
		{ { 0xc2, 0x80 }, 2, 0x80 },
		{ { 0xdf, 0xbf }, 2, 0x7ff },
		{ { 0xe0, 0xa0, 0x80 }, 3, 0x800 },
		{ { 0xed, 0x9f, 0xbf }, 3, 0xd7ff },
		{ { 0xee, 0x80, 0x80 }, 3, 0xe000 },
		{ { 0xef, 0xbf, 0xbf }, 3, 0xffff },
		{ { 0xf0, 0x90, 0x80, 0x80 }, 4, 0x10000 },
		{ { 0xf4, 0x8f, 0xbf, 0xbf }, 4, 0x10ffff },
	};

	for (size_t index = 0; index < sizeof(accepted) / sizeof(accepted[0]);
	     ++index) {
		const unsigned char *cursor = accepted[index].bytes;
		const unsigned char *end = cursor + accepted[index].size;
		uint32_t value = 0x1234;

		assert(fplinux_utf8_next(&cursor, end, &value));
		assert(cursor == end);
		assert(value == accepted[index].codepoint);
	}
}

static void verify_rejected_input(void)
{
	static const struct {
		unsigned char bytes[4];
		size_t size;
	} rejected[] = {
		{ { 0 }, 0 },
		{ { 0x80 }, 1 },
		{ { 0xc0, 0x80 }, 2 },
		{ { 0xe0, 0x80, 0x80 }, 3 },
		{ { 0xf0, 0x80, 0x80, 0x80 }, 4 },
		{ { 0xed, 0xa0, 0x80 }, 3 },
		{ { 0xed, 0xbf, 0xbf }, 3 },
		{ { 0xf4, 0x90, 0x80, 0x80 }, 4 },
		{ { 0xf5, 0x80, 0x80, 0x80 }, 4 },
		{ { 0xff }, 1 },
		{ { 0xc2, 0x80 }, 1 },
		{ { 0xe2, 0x82, 0xac }, 1 },
		{ { 0xe2, 0x82, 0xac }, 2 },
		{ { 0xf0, 0x90, 0x80, 0x80 }, 1 },
		{ { 0xf0, 0x90, 0x80, 0x80 }, 2 },
		{ { 0xf0, 0x90, 0x80, 0x80 }, 3 },
		{ { 0xc2, 0x41 }, 2 },
		{ { 0xe2, 0x41, 0xac }, 3 },
		{ { 0xe2, 0x82, 0x41 }, 3 },
		{ { 0xf0, 0x41, 0x80, 0x80 }, 4 },
		{ { 0xf0, 0x90, 0x41, 0x80 }, 4 },
		{ { 0xf0, 0x90, 0x80, 0x41 }, 4 },
	};

	for (size_t index = 0; index < sizeof(rejected) / sizeof(rejected[0]);
	     ++index) {
		const unsigned char *cursor = rejected[index].bytes;
		uint32_t value = 0x1234;

		assert(!fplinux_utf8_next(
			&cursor, cursor + rejected[index].size, &value));
		assert(cursor == rejected[index].bytes);
		assert(value == 0x1234);
	}
}

int main(void)
{
	assert(setlocale(LC_ALL, ""));
	verify_sequence();
	verify_scalar_boundaries();
	verify_rejected_input();
	return 0;
}
