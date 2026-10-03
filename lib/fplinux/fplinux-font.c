/* SPDX-License-Identifier: GPL-2.0-only */
#include "fplinux-font.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FPLINUX_FONT_MAX_BYTES (1024U * 1024U)

struct fplinux_font_map_entry {
	uint32_t codepoint;
	unsigned int index;
};

static uint32_t read_u32(const unsigned char *bytes)
{
	return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 |
	       (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

static bool next_unicode(const unsigned char **cursor, const unsigned char *end,
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

static int compare_glyph(const void *first, const void *second)
{
	const struct fplinux_font_map_entry *left = first;
	const struct fplinux_font_map_entry *right = second;

	return (left->codepoint > right->codepoint) -
	       (left->codepoint < right->codepoint);
}

static bool read_font(struct fplinux_font *font, size_t size)
{
	const unsigned char *data = font->data;
	const unsigned char *end = data + size;
	const unsigned char *table;
	unsigned int header_bytes;
	unsigned int glyph_count;
	unsigned int index = 0;
	bool sequence = false;

	if (size < 32 || read_u32(data) != 0x864ab572 ||
	    read_u32(data + 4) != 0 || read_u32(data + 12) != 1)
		return false;
	header_bytes = read_u32(data + 8);
	glyph_count = read_u32(data + 16);
	font->glyph_bytes = read_u32(data + 20);
	font->height = read_u32(data + 24);
	font->width = read_u32(data + 28);
	font->row_bytes = (font->width + 7U) / 8U;
	if (header_bytes < 32 || header_bytes > size || !glyph_count ||
	    !font->width || font->width > 16 || !font->height ||
	    font->height > 32 ||
	    font->glyph_bytes != font->row_bytes * font->height ||
	    glyph_count > (size - header_bytes) / font->glyph_bytes)
		return false;
	font->bitmap = data + header_bytes;
	table = font->bitmap + (size_t)glyph_count * font->glyph_bytes;
	font->map = calloc((size_t)(end - table), sizeof(*font->map));
	if (!font->map)
		return false;
	while (table < end && index < glyph_count) {
		uint32_t codepoint;

		if (*table == 0xff) {
			++index;
			++table;
			sequence = false;
		} else if (*table == 0xfe) {
			sequence = true;
			++table;
		} else if (!next_unicode(&table, end, &codepoint)) {
			return false;
		} else if (!sequence) {
			font->map[font->map_count++] =
				(struct fplinux_font_map_entry){ codepoint,
								 index };
		}
	}
	if (index != glyph_count || table != end || !font->map_count)
		return false;
	qsort(font->map, font->map_count, sizeof(*font->map), compare_glyph);
	return true;
}

bool fplinux_font_open(struct fplinux_font *font, const char *path)
{
	FILE *file;
	long size;
	bool valid = false;

	memset(font, 0, sizeof(*font));
	file = fopen(path, "rb");
	if (!file)
		return false;
	if (fseek(file, 0, SEEK_END) || (size = ftell(file)) < 32 ||
	    (size_t)size > FPLINUX_FONT_MAX_BYTES || fseek(file, 0, SEEK_SET))
		goto done;
	font->data = malloc((size_t)size);
	if (!font->data ||
	    fread(font->data, 1, (size_t)size, file) != (size_t)size)
		goto done;
	valid = read_font(font, (size_t)size);
done:
	fclose(file);
	if (!valid)
		fplinux_font_close(font);
	return valid;
}

void fplinux_font_close(struct fplinux_font *font)
{
	free(font->data);
	free(font->map);
	memset(font, 0, sizeof(*font));
}

bool fplinux_font_open_default(struct fplinux_font *font,
			       unsigned int display_width, char *error,
			       size_t error_size)
{
	const char *path = "/usr/share/fplinux/fonts/default.psf";
	unsigned int width = display_width < 200 ? 6 : 8;
	unsigned int height = display_width < 200 ? 12 : 16;

	if (!fplinux_font_open(font, path)) {
		snprintf(error, error_size, "cannot load font %s", path);
		return false;
	}
	if (font->width != width || font->height != height) {
		snprintf(
			error, error_size,
			"default font is %ux%u; %u-pixel display requires %ux%u",
			font->width, font->height, display_width, width,
			height);
		fplinux_font_close(font);
		return false;
	}
	return true;
}

const unsigned char *fplinux_font_glyph(const struct fplinux_font *font,
					uint32_t codepoint)
{
	struct fplinux_font_map_entry key = { .codepoint = codepoint };
	const struct fplinux_font_map_entry *entry;

	entry = bsearch(&key, font->map, font->map_count, sizeof(*font->map),
			compare_glyph);
	if (!entry) {
		key.codepoint = 0xfffd;
		entry = bsearch(&key, font->map, font->map_count,
				sizeof(*font->map), compare_glyph);
	}
	if (!entry)
		return NULL;
	return font->bitmap + (size_t)entry->index * font->glyph_bytes;
}
