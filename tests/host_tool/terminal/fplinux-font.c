/* SPDX-License-Identifier: GPL-2.0-only */
#define _POSIX_C_SOURCE 200809L
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
static const char *installed_path;
static const char *installed_directory;

FILE *__real_fopen(const char *path, const char *mode);

/* Replace only the installed-file boundary with a case-owned PSF2 file. */
FILE *__wrap_fopen(const char *path, const char *mode)
{
	char fixture_path[PATH_MAX];
	int length;

	if (default_fixture) {
		assert(!strcmp(path, installed_path));
		path = default_fixture;
	} else if (installed_directory) {
		assert(!strncmp(path, "/usr/share/fplinux/fonts/", 25));
		length = snprintf(fixture_path, sizeof(fixture_path), "%s/%s",
				  installed_directory, path + 25);
		assert(length > 0 && (size_t)length < sizeof(fixture_path));
		path = fixture_path;
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
	if (!strcmp(argv[1], "--installed")) {
		assert(argc == 4);
		installed_directory = argv[3];
		return inspect_default(argv[2], NULL);
	}
	if (!strcmp(argv[1], "--default")) {
		assert(argc == 5);
		installed_path = argv[4];
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
