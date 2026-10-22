/* SPDX-License-Identifier: GPL-2.0-only */
#include "terminal-render.h"

#include <assert.h>
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <string.h>

static const char *font_path;
static int cell_width, cell_height;
static lv_layer_t drawing_layer;
static struct {
	lv_draw_label_dsc_t descriptor;
	lv_point_t position;
	uint32_t character;
} letters[512];
static struct {
	lv_draw_rect_dsc_t descriptor;
	lv_area_t area;
} rectangles[4096];
static size_t letter_count, rectangle_count;

bool lv_font_get_glyph_dsc(const lv_font_t *font, lv_font_glyph_dsc_t *glyph,
			   uint32_t character, uint32_t next)
{
	bool found = font->get_glyph_dsc(font, glyph, character, next);

	if (found)
		glyph->resolved_font = font;
	return found;
}

int32_t lv_font_get_line_height(const lv_font_t *font)
{
	return font->line_height;
}

void lv_font_glyph_release_draw_data(lv_font_glyph_dsc_t *glyph)
{
	(void)glyph;
}

void lv_draw_buf_flush_cache(lv_draw_buf_t *draw_buffer, const lv_area_t *area)
{
	assert(draw_buffer && !area);
}

void lv_draw_rect_dsc_init(lv_draw_rect_dsc_t *descriptor)
{
	memset(descriptor, 0, sizeof(*descriptor));
}

void lv_draw_label_dsc_init(lv_draw_label_dsc_t *descriptor)
{
	memset(descriptor, 0, sizeof(*descriptor));
}

void lv_draw_rect(lv_layer_t *layer, const lv_draw_rect_dsc_t *descriptor,
		  const lv_area_t *area)
{
	assert(layer == &drawing_layer && rectangle_count < 4096);
	assert(descriptor->bg_opa == 255);
	rectangles[rectangle_count].descriptor = *descriptor;
	rectangles[rectangle_count++].area = *area;
}

void lv_draw_character(lv_layer_t *layer, lv_draw_label_dsc_t *descriptor,
		       const lv_point_t *position, uint32_t character)
{
	assert(layer == &drawing_layer && letter_count < 512);
	assert(descriptor->font && descriptor->opa == 255);
	letters[letter_count].descriptor = *descriptor;
	letters[letter_count].character = character;
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
	assert(fplinux_terminal_grid_open(&grid, font_path));
	assert(grid.cell_width == (unsigned int)cell_width &&
	       grid.cell_height == (unsigned int)cell_height);
	fplinux_terminal_render(terminal, &grid, &drawing_layer, bounds);
	fplinux_terminal_grid_close(&grid);
}

static void feed(struct fplinux_terminal *terminal, const char *text)
{
	fplinux_terminal_feed(terminal, text, strlen(text));
}

static void expect_letter(uint32_t character, int32_t x, int32_t y,
			  lv_color_t color)
{
	for (size_t index = 0; index < letter_count; ++index)
		if (letters[index].character == character &&
		    letters[index].position.x == x &&
		    letters[index].position.y == y &&
		    letters[index].descriptor.color == color)
			return;
	assert(false);
}

static lv_color_t background_at(int32_t x, int32_t y)
{
	for (size_t count = rectangle_count; count; --count) {
		lv_area_t area = rectangles[count - 1].area;

		if (x >= area.x1 && x <= area.x2 && y >= area.y1 &&
		    y <= area.y2)
			return rectangles[count - 1].descriptor.bg_color;
	}
	assert(false);
	return 0;
}

static void expect_background(const lv_area_t *area, lv_color_t color)
{
	for (int32_t y = area->y1; y <= area->y2; ++y)
		for (int32_t x = area->x1; x <= area->x2; ++x)
			assert(background_at(x, y) == color);
}

static void glyphs_keep_font_columns_and_fallback(void)
{
	struct fplinux_terminal terminal;
	const lv_area_t bounds = { 3, 7, 3 + 4 * cell_width - 1,
				   7 + 2 * cell_height - 1 };

	assert(fplinux_terminal_init(&terminal, 4, 2));
	feed(&terminal, "WШ_☃");
	render(&terminal, &bounds);
	assert(letter_count == 4);
	expect_letter('W', 3, 7, 0xdedfde);
	expect_letter(0x428, 3 + cell_width, 7, 0xdedfde);
	expect_letter('_', 3 + 2 * cell_width, 7, 0xdedfde);
	expect_letter(0x2603, 3 + 3 * cell_width, 7, 0xdedfde);
	expect_background(&(lv_area_t){ 3, 7, bounds.x2, 7 + cell_height - 3 },
			  0x101010);
	assert(!terminal.dirty);
	fplinux_terminal_destroy(&terminal);
}

static void decorations_stay_in_their_cell(void)
{
	struct fplinux_terminal terminal;
	const lv_area_t bounds = { 3, 7, 3 + 4 * cell_width - 1,
				   7 + 2 * cell_height - 1 };

	assert(fplinux_terminal_init(&terminal, 4, 2));
	feed(&terminal, "\033[4;31m_\033[0m");
	render(&terminal, &bounds);
	assert(letter_count == 1);
	expect_letter('_', 3, 7, 0xf7595a);
	expect_background(&(lv_area_t){ 3, 7 + cell_height - 2,
					3 + cell_width - 1,
					7 + cell_height - 1 },
			  0xf7595a);
	expect_background(&(lv_area_t){ 3 + cell_width, 7 + cell_height - 2,
					3 + 2 * cell_width - 1,
					7 + cell_height - 1 },
			  0xdedfde);
	feed(&terminal, "\033[?25l");
	render(&terminal, &bounds);
	expect_background(&(lv_area_t){ 3 + cell_width, 7,
					3 + 2 * cell_width - 1,
					7 + cell_height - 1 },
			  0x101010);
	fplinux_terminal_destroy(&terminal);
}

static void preedit_replaces_the_cursor_cell_without_sending_bytes(void)
{
	struct fplinux_terminal terminal;
	const lv_area_t bounds = { 3, 7, 3 + 4 * cell_width - 1,
				   7 + 2 * cell_height - 1 };

	assert(fplinux_terminal_init(&terminal, 4, 2));
	feed(&terminal, "W");
	fplinux_terminal_phone(&terminal, KEY_NUMERIC_2, true, 100);
	render(&terminal, &bounds);
	assert(terminal.output_size == 0 && letter_count == 2);
	expect_letter('W', 3, 7, 0xdedfde);
	expect_letter('a', 3 + cell_width, 7, 0x083442);
	expect_background(&(lv_area_t){ 3 + cell_width, 7,
					3 + 2 * cell_width - 1,
					7 + cell_height - 3 },
			  0xadcfde);
	expect_background(&(lv_area_t){ 3 + cell_width, 7 + cell_height - 2,
					3 + 2 * cell_width - 1,
					7 + cell_height - 1 },
			  0xdedfde);
	assert(background_at(3 + cell_width - 1, 7) == 0x101010);
	assert(background_at(3 + 2 * cell_width, 7) == 0x101010);
	fplinux_terminal_destroy(&terminal);
}

static void underline_keeps_its_color_when_cursor_is_hidden(bool blink)
{
	struct fplinux_terminal terminal;
	const lv_area_t bounds = { 3, 7, 3 + 4 * cell_width - 1,
				   7 + 2 * cell_height - 1 };
	const lv_area_t underline = { 3, 7 + cell_height - 2,
				      3 + cell_width - 1, 7 + cell_height - 1 };

	assert(fplinux_terminal_init(&terminal, 4, 2));
	feed(&terminal, "\033[4;31m_\033[0m\033[H");
	if (blink)
		terminal.cursor_visible = false;
	else
		feed(&terminal, "\033[?25l");
	render(&terminal, &bounds);
	expect_letter('_', 3, 7, 0xf7595a);
	expect_background(&underline, 0xf7595a);
	if (!blink)
		feed(&terminal, "\033[?25h");
	terminal.cursor_visible = true;
	render(&terminal, &bounds);
	expect_background(&underline, 0xdedfde);
	fplinux_terminal_destroy(&terminal);
}

static void
growing_grid_preserves_text_and_fills_new_cells_with_default_background(
	bool alternate)
{
	struct fplinux_terminal terminal;
	const lv_area_t bounds = { 3, 7, 3 + 6 * cell_width - 1,
				   7 + 4 * cell_height - 1 };

	assert(fplinux_terminal_init(&terminal, 4, 2));
	if (alternate)
		feed(&terminal, "\033[?1049h");
	feed(&terminal, "\033[2J\033[H\033[31;44mW\033[0m\033[?25l");
	assert(tsm_screen_resize(terminal.screen, 6, 4) == 0);
	render(&terminal, &bounds);
	assert(letter_count == 1);
	expect_letter('W', 3, 7, 0xf7595a);
	expect_background(&(lv_area_t){ 3, 7, 3 + cell_width - 1,
					7 + cell_height - 1 },
			  0x5a86ff);
	expect_background(&(lv_area_t){ 3 + 4 * cell_width, 7, bounds.x2,
					7 + 2 * cell_height - 1 },
			  0x101010);
	expect_background(&(lv_area_t){ 3, 7 + 2 * cell_height, bounds.x2,
					bounds.y2 },
			  0x101010);
	fplinux_terminal_destroy(&terminal);
}

static void layouts_clip_partial_cells(void)
{
	const struct layout {
		unsigned int width, height, columns, rows;
		int32_t last_x, last_y;
	} compact[] = {
		{ 128, 110, 21, 9, 120, 96 },
		{ 240, 246, 40, 20, 234, 228 },
		{ 320, 174, 53, 14, 312, 156 },
	}, larger[] = {
		{ 128, 110, 16, 6, 120, 80 },
		{ 240, 246, 30, 15, 232, 224 },
		{ 320, 174, 40, 10, 312, 144 },
	};
	const struct layout *sizes = cell_width == 6 ? compact : larger;

	for (size_t i = 0; i < 3; ++i) {
		struct fplinux_terminal terminal;
		lv_area_t bounds = { 0, 0, (int32_t)sizes[i].width - 1,
				     (int32_t)sizes[i].height - 1 };
		char edge_text[64];

		assert(fplinux_terminal_init(&terminal, sizes[i].columns + 1,
					     sizes[i].rows + 1));
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
		assert(letter_count == 4);
		expect_letter('Z', sizes[i].last_x, sizes[i].last_y, 0xdedfde);
		fplinux_terminal_destroy(&terminal);
	}
}

static void psf_adapter_maps_unicode_and_expands_rows_with_stride(void)
{
	const unsigned char compact_expected[] = {
		255,  0,    0,	 0,    0,    255, 0xa5, 0xa5, 0,    255,  0,
		0,    255,  0,	 0xa5, 0xa5, 0,	  0,	255,  255,  0,	  0,
		0xa5, 0xa5, 255, 255,  255,  255, 255,	255,  0xa5, 0xa5,
	};
	const unsigned char larger_expected[] = {
		255, 0,	  0,   0,   0,	 0,   0,   255, 0xa5, 0xa5,
		0,   255, 0,   0,   0,	 0,   255, 0,	0xa5, 0xa5,
		0,   0,	  255, 0,   0,	 255, 0,   0,	0xa5, 0xa5,
		255, 255, 255, 255, 255, 255, 255, 255, 0xa5, 0xa5,
	};
	const unsigned char compact_russian[] = {
		0x84, 0x48, 0x30, 0xfc, 0, 0, 0, 0, 0, 0, 0, 0
	};
	const unsigned char larger_russian[] = { 0x81, 0x42, 0x24, 0xff, 0, 0,
						 0,    0,    0,	   0,	 0, 0,
						 0,    0,    0,	   0 };
	const unsigned char compact_fallback[] = { 0xfc, 0x84, 0xb4, 0xa4,
						   0xa4, 0x94, 0x84, 0x94,
						   0x84, 0xfc, 0,    0 };
	const unsigned char larger_fallback[] = {
		0xff, 0x81, 0xbd, 0xa5, 0xa5, 0xa5, 0x89, 0x91,
		0x81, 0x91, 0x81, 0xff, 0,    0,    0,	  0,
	};
	const unsigned char *expected = cell_width == 6 ? compact_expected :
							  larger_expected;
	const unsigned char *russian = cell_width == 6 ? compact_russian :
							 larger_russian;
	const unsigned char *fallback = cell_width == 6 ? compact_fallback :
							  larger_fallback;
	size_t expected_size = cell_width == 6 ? sizeof(compact_expected) :
						 sizeof(larger_expected);
	struct fplinux_terminal_grid grid = { 0 };
	lv_font_glyph_dsc_t descriptor;
	unsigned char pixels[160];
	lv_draw_buf_t buffer = { .data = pixels,
				 .header.stride = cell_width + 2 };

	assert(fplinux_terminal_grid_open(&grid, font_path));
	assert(grid.font.line_height == cell_height);
	assert(lv_font_get_glyph_dsc(&grid.font, &descriptor, 0x428, 0));
	assert(descriptor.adv_w == (unsigned int)cell_width &&
	       descriptor.box_w == (unsigned int)cell_width &&
	       descriptor.box_h == (unsigned int)cell_height &&
	       descriptor.stride == 1 &&
	       descriptor.format == LV_FONT_GLYPH_FORMAT_A1);
	descriptor.req_raw_bitmap = true;
	assert(!memcmp(grid.font.get_glyph_bitmap(&descriptor, NULL), russian,
		       (size_t)cell_height));
	descriptor.req_raw_bitmap = false;
	memset(pixels, 0xa5, sizeof(pixels));
	assert(grid.font.get_glyph_bitmap(&descriptor, &buffer) == &buffer);
	assert(!memcmp(pixels, expected, expected_size));
	for (int row = 4; row < cell_height; ++row) {
		const unsigned char *pixels_row =
			pixels + row * (cell_width + 2);

		assert(!memcmp(pixels_row, "\0\0\0\0\0\0\0\0",
			       (size_t)cell_width));
		assert(pixels_row[cell_width] == 0xa5 &&
		       pixels_row[cell_width + 1] == 0xa5);
	}
	assert(lv_font_get_glyph_dsc(&grid.font, &descriptor, 0x2603, 0));
	descriptor.req_raw_bitmap = true;
	assert(!memcmp(grid.font.get_glyph_bitmap(&descriptor, NULL), fallback,
		       (size_t)cell_height));
	fplinux_terminal_grid_close(&grid);
	assert(!grid.psf.data && !grid.cell_width && !grid.cell_height);
	fplinux_terminal_grid_close(&grid);
}

int main(int argc, char **argv)
{
	assert(argc == 4);
	font_path = argv[2];
	assert(!strcmp(argv[3], "6x12") || !strcmp(argv[3], "8x16"));
	cell_width = !strcmp(argv[3], "6x12") ? 6 : 8;
	cell_height = !strcmp(argv[3], "6x12") ? 12 : 16;
	if (!strcmp(argv[1], "reject-font")) {
		struct fplinux_terminal_grid grid = { 0 };

		assert(!fplinux_terminal_grid_open(&grid, font_path));
		assert(!grid.psf.data && !grid.cell_width && !grid.cell_height);
		fplinux_terminal_grid_close(&grid);
	} else if (!strcmp(argv[1], "font"))
		psf_adapter_maps_unicode_and_expands_rows_with_stride();
	else if (!strcmp(argv[1], "glyphs"))
		glyphs_keep_font_columns_and_fallback();
	else if (!strcmp(argv[1], "decorations"))
		decorations_stay_in_their_cell();
	else if (!strcmp(argv[1], "preedit"))
		preedit_replaces_the_cursor_cell_without_sending_bytes();
	else if (!strcmp(argv[1], "hidden-cursor-underline") ||
		 !strcmp(argv[1], "blinking-cursor-underline"))
		underline_keeps_its_color_when_cursor_is_hidden(
			!strcmp(argv[1], "blinking-cursor-underline"));
	else if (!strcmp(argv[1], "resize") ||
		 !strcmp(argv[1], "resize-alternate"))
		growing_grid_preserves_text_and_fills_new_cells_with_default_background(
			!strcmp(argv[1], "resize-alternate"));
	else {
		assert(!strcmp(argv[1], "layouts"));
		layouts_clip_partial_cells();
	}
	printf("terminal-render %s: OK\n", argv[1]);
	return 0;
}
