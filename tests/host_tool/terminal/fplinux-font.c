/* SPDX-License-Identifier: GPL-2.0-only */
#include "fplinux-font.h"

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void verify_empty(const struct fplinux_font *font)
{
	assert(!font->data && !font->bitmap && !font->map);
	assert(!font->map_count && !font->width && !font->height);
	assert(!font->glyph_bytes && !font->row_bytes);
}

static const char *default_fixture;

FILE *__real_fopen(const char *path, const char *mode);

/* Replace only the installed-file boundary with a case-owned PSF2 file. */
FILE *__wrap_fopen(const char *path, const char *mode)
{
	if (default_fixture) {
		assert(!strcmp(path, "/usr/share/fplinux/fonts/default.psf"));
		path = default_fixture;
	}
	return __real_fopen(path, mode);
}

static int inspect_default(const char *width, const char *path)
{
	struct fplinux_font font = { 0 };
	char error[160];
	char *end;
	unsigned long display_width = strtoul(width, &end, 10);

	assert(*width && !*end && display_width <= UINT_MAX);
	default_fixture = path;
	if (fplinux_font_open_default(&font, (unsigned int)display_width, error,
				      sizeof(error)))
		printf("%u %u\n", font.width, font.height);
	else {
		puts(error);
		verify_empty(&font);
	}
	fplinux_font_close(&font);
	verify_empty(&font);
	return 0;
}

static void print_glyph(const struct fplinux_font *font, const char *argument)
{
	char *end;
	unsigned long codepoint = strtoul(argument, &end, 16);
	const unsigned char *bitmap;
	unsigned int index;

	assert(*argument && !*end && codepoint <= 0x10ffff);
	bitmap = fplinux_font_glyph(font, (uint32_t)codepoint);
	if (!bitmap) {
		puts("missing");
		return;
	}
	for (index = 0; index < font->glyph_bytes; ++index)
		printf("%02x", bitmap[index]);
	putchar('\n');
}

int main(int argc, char **argv)
{
	struct fplinux_font font = { 0 };
	int index;

	assert(argc >= 2);
	if (!strcmp(argv[1], "--default")) {
		assert(argc == 4);
		return inspect_default(argv[2], argv[3]);
	}
	fplinux_font_close(&font);
	verify_empty(&font);
	if (!fplinux_font_open(&font, argv[1])) {
		puts("rejected");
		verify_empty(&font);
		fplinux_font_close(&font);
		return 0;
	}
	printf("%u %u %u %u\n", font.width, font.height, font.row_bytes,
	       font.glyph_bytes);
	for (index = 2; index < argc; ++index)
		print_glyph(&font, argv[index]);
	fplinux_font_close(&font);
	verify_empty(&font);
	fplinux_font_close(&font);
	assert(fplinux_font_open(&font, argv[1]));
	fplinux_font_close(&font);
	verify_empty(&font);
	return 0;
}
