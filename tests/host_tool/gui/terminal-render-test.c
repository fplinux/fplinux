/* SPDX-License-Identifier: GPL-2.0-only */
#include "terminal-render.h"

#include <assert.h>
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <string.h>

static const lv_font_t gui_font = { .line_height = 20 };
static lv_layer_t drawing_layer;
static struct {
	lv_draw_letter_dsc_t descriptor;
	lv_point_t position;
} letters[512];
static struct {
	lv_draw_rect_dsc_t descriptor;
	lv_area_t area;
} rectangles[1024];
static size_t letter_count, rectangle_count;

bool lv_font_get_glyph_dsc(const lv_font_t *font, lv_font_glyph_dsc_t *glyph,
			   uint32_t character, uint32_t next)
{
	assert(font == &gui_font);
	(void)next;
	*glyph = (lv_font_glyph_dsc_t){ .adv_w = 7, .box_w = 7 };
	if (character == 0x2603)
		return false;
	if (character == 'W')
		*glyph = (lv_font_glyph_dsc_t){ .adv_w = 11, .box_w = 11 };
	if (character == 0x428)
		*glyph = (lv_font_glyph_dsc_t){ .adv_w = 13,
						.box_w = 13,
						.ofs_x = -1 };
	return true;
}

int32_t lv_font_get_line_height(const lv_font_t *font)
{
	return font->line_height;
}

void lv_font_glyph_release_draw_data(lv_font_glyph_dsc_t *glyph)
{
	(void)glyph;
}

void lv_draw_rect_dsc_init(lv_draw_rect_dsc_t *descriptor)
{
	memset(descriptor, 0, sizeof(*descriptor));
}

void lv_draw_letter_dsc_init(lv_draw_letter_dsc_t *descriptor)
{
	memset(descriptor, 0, sizeof(*descriptor));
}

void lv_draw_rect(lv_layer_t *layer, const lv_draw_rect_dsc_t *descriptor,
		  const lv_area_t *area)
{
	assert(layer == &drawing_layer && rectangle_count < 1024);
	assert(descriptor->bg_opa == 255);
	rectangles[rectangle_count].descriptor = *descriptor;
	rectangles[rectangle_count++].area = *area;
}

void lv_draw_letter(lv_layer_t *layer, const lv_draw_letter_dsc_t *descriptor,
		    const lv_point_t *position)
{
	assert(layer == &drawing_layer && letter_count < 512);
	assert(descriptor->font == &gui_font && descriptor->opa == 255);
	letters[letter_count].descriptor = *descriptor;
	letters[letter_count++].position = *position;
}

lv_color_t lv_color_make(uint8_t red, uint8_t green, uint8_t blue)
{
	return (uint32_t)red << 16 | (uint32_t)green << 8 | blue;
}

lv_color_t lv_color_hex(uint32_t color)
{
	return color;
}

static void render(struct fplinux_terminal *terminal, const lv_area_t *bounds)
{
	struct fplinux_terminal_grid grid;

	letter_count = rectangle_count = 0;
	assert(fplinux_terminal_grid_init(&grid, &gui_font));
	assert(grid.font == &gui_font);
	assert(grid.cell_width == 13 && grid.cell_height == 20);
	fplinux_terminal_render(terminal, &grid, &drawing_layer, bounds);
}

static void feed(struct fplinux_terminal *terminal, const char *text)
{
	fplinux_terminal_feed(terminal, text, strlen(text));
}

static void glyphs_keep_font_columns_and_fallback(void)
{
	struct fplinux_terminal terminal;
	const lv_area_t bounds = { 3, 7, 54, 46 };

	assert(fplinux_terminal_init(&terminal, 4, 2, "render"));
	feed(&terminal, "WШ_☃");
	render(&terminal, &bounds);
	assert(letter_count == 4);
	assert(letters[0].descriptor.unicode == 'W');
	assert(letters[0].position.x == 4 && letters[0].position.y == 7);
	assert(letters[1].descriptor.unicode == 0x428);
	assert(letters[1].position.x == 17 && letters[1].position.y == 7);
	assert(letters[2].descriptor.unicode == '_');
	assert(letters[2].position.x == 32 && letters[2].position.y == 7);
	assert(letters[3].descriptor.unicode == '?');
	assert(letters[3].position.x == 45 && letters[3].position.y == 7);
	assert(letters[0].descriptor.color == 0xdedfde);
	assert(rectangles[0].descriptor.bg_color == 0x101010);
	assert(!terminal.dirty);
	fplinux_terminal_destroy(&terminal);
}

static void decorations_stay_in_their_cell(void)
{
	struct fplinux_terminal terminal;
	const lv_area_t bounds = { 3, 7, 54, 46 };
	bool underline = false, cursor = false;

	assert(fplinux_terminal_init(&terminal, 4, 2, "render"));
	feed(&terminal, "\033[4;31m_\033[0m");
	render(&terminal, &bounds);
	assert(letter_count == 1 && letters[0].descriptor.color == 0xf7595a);
	for (size_t i = 0; i < rectangle_count; ++i) {
		lv_area_t area = rectangles[i].area;

		if (area.x1 == 3 && area.x2 == 15 && area.y1 == 25 &&
		    area.y2 == 26) {
			assert(rectangles[i].descriptor.bg_color == 0xf7595a);
			underline = true;
		}
		if (area.x1 == 16 && area.x2 == 28 && area.y1 == 25 &&
		    area.y2 == 26) {
			assert(rectangles[i].descriptor.bg_color == 0xdedfde);
			cursor = true;
		}
	}
	assert(underline && cursor);
	feed(&terminal, "\033[?25l");
	render(&terminal, &bounds);
	for (size_t i = 0; i < rectangle_count; ++i) {
		lv_area_t area = rectangles[i].area;

		assert(!(area.x1 == 16 && area.x2 == 28 && area.y1 == 25 &&
			 area.y2 == 26));
	}
	fplinux_terminal_destroy(&terminal);
}

static void preedit_replaces_the_cursor_cell_without_sending_bytes(void)
{
	struct fplinux_terminal terminal;
	const lv_area_t bounds = { 3, 7, 54, 46 };

	assert(fplinux_terminal_init(&terminal, 4, 2, "render"));
	feed(&terminal, "W");
	fplinux_terminal_phone(&terminal, KEY_NUMERIC_2, true, false, 100,
			       true);
	render(&terminal, &bounds);
	assert(terminal.output_size == 0 && letter_count == 2);
	assert(letters[1].descriptor.unicode == 'a');
	assert(letters[1].descriptor.color == 0x083442);
	assert(letters[1].position.x == 19 && letters[1].position.y == 7);
	assert(rectangles[2].descriptor.bg_color == 0xadcfde);
	assert(rectangles[2].area.x1 == 16 && rectangles[2].area.x2 == 28);
	fplinux_terminal_destroy(&terminal);
}

static void layouts_clip_partial_cells(void)
{
	const struct {
		unsigned int width, height, columns, rows;
		int32_t last_x, last_y;
	} sizes[] = {
		{ 128, 160, 9, 8, 107, 140 },
		{ 240, 320, 18, 16, 224, 300 },
		{ 320, 240, 24, 12, 302, 220 },
	};

	for (size_t i = 0; i < 3; ++i) {
		struct fplinux_terminal terminal;
		lv_area_t bounds = { 0, 0, (int32_t)sizes[i].width - 1,
				     (int32_t)sizes[i].height - 1 };
		char edge_text[64];

		assert(fplinux_terminal_init(&terminal, sizes[i].columns + 1,
					     sizes[i].rows + 1, "render"));
		feed(&terminal, "WШ_");
		snprintf(edge_text, sizeof(edge_text),
			 "\033[%u;%uHZ\033[%u;%uHQ", sizes[i].rows,
			 sizes[i].columns, sizes[i].rows + 1,
			 sizes[i].columns + 1);
		feed(&terminal, edge_text);
		render(&terminal, &bounds);
		for (size_t j = 0; j < rectangle_count; ++j) {
			lv_area_t area = rectangles[j].area;

			assert(area.x1 >= 0 && area.y1 >= 0);
			assert(area.x2 <= bounds.x2 && area.y2 <= bounds.y2);
		}
		assert(letter_count == 4 &&
		       letters[3].descriptor.unicode == 'Z');
		assert(letters[3].position.x == sizes[i].last_x);
		assert(letters[3].position.y == sizes[i].last_y);
		fplinux_terminal_destroy(&terminal);
	}
}

int main(int argc, char **argv)
{
	assert(argc == 2);
	if (!strcmp(argv[1], "glyphs"))
		glyphs_keep_font_columns_and_fallback();
	else if (!strcmp(argv[1], "decorations"))
		decorations_stay_in_their_cell();
	else if (!strcmp(argv[1], "preedit"))
		preedit_replaces_the_cursor_cell_without_sending_bytes();
	else {
		assert(!strcmp(argv[1], "layouts"));
		layouts_clip_partial_cells();
	}
	printf("terminal-render %s: OK\n", argv[1]);
	return 0;
}
