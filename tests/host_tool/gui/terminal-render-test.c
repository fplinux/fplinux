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
	fplinux_terminal_render(terminal, &grid, &drawing_layer, bounds,
				0xadcfde, 0x083442);
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

static void cursor_contrasts_with_resolved_backgrounds(void)
{
	static const struct {
		const char *text;
		lv_color_t marker;
	} backgrounds[] = {
		{ "\033[7m \033[0m\033[H", 0x101010 },
		{ "\033[107m \033[0m\033[H", 0x101010 },
		{ "\033[48;2;222;223;222m \033[0m\033[H", 0x101010 },
		{ "\033[42m \033[0m\033[H", 0x101010 },
		{ "\033[44m \033[0m\033[H", 0x101010 },
		{ "\033[40m \033[0m\033[H", 0xdedfde },
	};
	const lv_area_t bounds = { 3, 7, 3 + 4 * cell_width - 1,
				   7 + 2 * cell_height - 1 };
	const lv_area_t marker = { 3, 7 + cell_height - 2, 3 + cell_width - 1,
				   7 + cell_height - 1 };

	for (size_t index = 0;
	     index < sizeof(backgrounds) / sizeof(backgrounds[0]); ++index) {
		struct fplinux_terminal terminal;

		assert(fplinux_terminal_init(&terminal, 4, 2));
		feed(&terminal, backgrounds[index].text);
		render(&terminal, &bounds);
		expect_background(&marker, backgrounds[index].marker);
		terminal.cursor_visible = false;
		render(&terminal, &bounds);
		assert(background_at(3, 7 + cell_height - 1) !=
		       backgrounds[index].marker);
		fplinux_terminal_destroy(&terminal);
	}
}

static void ansi_and_explicit_rgb_keep_their_resolved_colors(void)
{
	static const lv_color_t colors[] = {
		0x000000, 0xf7595a, 0x39cb39, 0xd6be00, 0x5a86ff, 0xd65dd6,
		0x00c3c6, 0xc6c7c6, 0x848684, 0xff8284, 0x73f373, 0xfff363,
		0x8caeff, 0xff8eff, 0x63ffff, 0xffffff,
	};
	struct fplinux_terminal terminal;
	const lv_area_t bounds = { 3, 7, 3 + 16 * cell_width - 1,
				   7 + 2 * cell_height - 1 };
	char sgr[32];

	assert(fplinux_terminal_init(&terminal, 16, 2));
	for (unsigned int index = 0; index < 16; ++index) {
		unsigned int code = index < 8 ? 30 + index : 90 + index - 8;

		assert(snprintf(sgr, sizeof(sgr), "\033[%umW", code) <
		       (int)sizeof(sgr));
		feed(&terminal, sgr);
	}
	feed(&terminal,
	     "\033[0m\033[2;1H\033[38;2;17;34;51;48;2;68;85;102mW\033[?25l");
	render(&terminal, &bounds);
	assert(letter_count == 17);
	for (unsigned int index = 0; index < 16; ++index)
		expect_letter('W', 3 + (int32_t)index * cell_width, 7,
			      colors[index]);
	expect_letter('W', 3, 7 + cell_height, 0x112233);
	expect_background(&(lv_area_t){ 3, 7 + cell_height, 3 + cell_width - 1,
					bounds.y2 },
			  0x445566);
	fplinux_terminal_destroy(&terminal);
}

static void
deferred_wrap_keeps_cursor_preview_and_committed_edge(bool bottom,
						      bool alternate)
{
	struct fplinux_terminal terminal;
	const lv_area_t bounds = { 3, 7, 3 + 4 * cell_width - 1,
				   7 + 2 * cell_height - 1 };
	unsigned int source_row = bottom ? 1 : 0;
	const struct tsm_screen_cell *cells;
	lv_area_t cursor = {
		3 + 3 * cell_width,
		7 + (int32_t)source_row * cell_height + cell_height - 2,
		bounds.x2,
		7 + (int32_t)source_row * cell_height + cell_height - 1,
	};

	assert(fplinux_terminal_init(&terminal, 4, 2));
	if (alternate)
		feed(&terminal, "\033[?1049h");
	feed(&terminal, bottom ? "WWWW\r\nWШ_Z" : "WШ_Z");
	assert(tsm_screen_get_cursor_x(terminal.screen) == 4);
	assert(tsm_screen_get_cursor_y(terminal.screen) == source_row);
	render(&terminal, &bounds);
	expect_letter('Z', 3 + 3 * cell_width,
		      7 + (int32_t)source_row * cell_height, 0xdedfde);
	expect_background(&cursor, 0xdedfde);

	fplinux_terminal_phone(&terminal, KEY_NUMERIC_2, true, 100);
	fplinux_terminal_phone(&terminal, KEY_NUMERIC_2, false, 101);
	render(&terminal, &bounds);
	expect_letter('a', 3, 7 + cell_height, 0x083442);
	/* Previewing a bottom-row wrap must not cover the last committed glyph. */
	expect_letter('Z', 3 + 3 * cell_width, 7, 0xdedfde);
	assert(!terminal.output_size);
	assert(tsm_screen_get_cursor_x(terminal.screen) == 4);
	assert(tsm_screen_get_cursor_y(terminal.screen) == source_row);
	cells = tsm_screen_draw2(terminal.screen);
	assert(cells[source_row * 4].ch == 'W');
	assert(cells[source_row * 4 + 1].ch == 0x428);
	assert(cells[source_row * 4 + 2].ch == '_');
	assert(cells[source_row * 4 + 3].ch == 'Z');
	if (bottom)
		for (unsigned int index = 0; index < 4; ++index)
			assert(cells[index].ch == 'W');

	assert(fplinux_terminal_run(&terminal, FPLINUX_TERMINAL_COMMAND_CLEAR));
	render(&terminal, &bounds);
	assert(!fplinux_terminal_preedit(&terminal) && !terminal.output_size);
	expect_letter('Z', 3 + 3 * cell_width,
		      7 + (int32_t)source_row * cell_height, 0xdedfde);
	expect_background(&cursor, 0xdedfde);
	assert(letter_count == (bottom ? 8U : 4U));
	fplinux_terminal_destroy(&terminal);
}

static void disabled_wrap_keeps_preedit_in_the_last_column(void)
{
	struct fplinux_terminal terminal;
	const lv_area_t bounds = { 3, 7, 3 + 4 * cell_width - 1,
				   7 + 2 * cell_height - 1 };
	unsigned int cursor_x, cursor_y;

	assert(fplinux_terminal_init(&terminal, 4, 2));
	feed(&terminal, "\033[?7lWШ_Z");
	assert(!(tsm_screen_get_flags(terminal.screen) & TSM_SCREEN_AUTO_WRAP));
	cursor_x = tsm_screen_get_cursor_x(terminal.screen);
	cursor_y = tsm_screen_get_cursor_y(terminal.screen);
	fplinux_terminal_phone(&terminal, KEY_NUMERIC_2, true, 100);
	fplinux_terminal_phone(&terminal, KEY_NUMERIC_2, false, 101);
	render(&terminal, &bounds);
	assert(letter_count == 4);
	expect_letter('a', 3 + 3 * cell_width, 7, 0x083442);
	assert(tsm_screen_get_cursor_x(terminal.screen) == cursor_x);
	assert(tsm_screen_get_cursor_y(terminal.screen) == cursor_y);
	assert(tsm_screen_draw2(terminal.screen)[3].ch == 'Z');
	assert(!terminal.output_size);
	assert(fplinux_terminal_run(&terminal, FPLINUX_TERMINAL_COMMAND_CLEAR));
	render(&terminal, &bounds);
	expect_letter('Z', 3 + 3 * cell_width, 7, 0xdedfde);
	fplinux_terminal_destroy(&terminal);
}

static void preedit_replaces_the_cursor_cell_without_sending_bytes(void)
{
	struct fplinux_terminal terminal;
	const lv_area_t bounds = { 3, 7, 3 + 4 * cell_width - 1,
				   7 + 2 * cell_height - 1 };

	assert(fplinux_terminal_init(&terminal, 4, 2));
	feed(&terminal, "\033[4mW \033[0m\033[1;2H");
	fplinux_terminal_phone(&terminal, KEY_NUMERIC_2, true, 100);
	render(&terminal, &bounds);
	assert(terminal.output_size == 0 && letter_count == 2);
	expect_letter('W', 3, 7, 0xdedfde);
	expect_letter('a', 3 + cell_width, 7, 0x083442);
	expect_background(&(lv_area_t){ 3 + cell_width, 7,
					3 + 2 * cell_width - 1,
					7 + cell_height - 1 },
			  0xadcfde);
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
		{ 128, 110, 21, 9, 121, 97 },
		{ 240, 246, 40, 20, 234, 231 },
		{ 320, 174, 53, 14, 313, 159 },
	}, larger[] = {
		{ 128, 110, 16, 6, 120, 87 },
		{ 240, 246, 30, 15, 232, 227 },
		{ 320, 174, 40, 10, 312, 151 },
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
	else if (!strcmp(argv[1], "cursor-contrast"))
		cursor_contrasts_with_resolved_backgrounds();
	else if (!strcmp(argv[1], "palette"))
		ansi_and_explicit_rgb_keep_their_resolved_colors();
	else if (!strcmp(argv[1], "right-edge") ||
		 !strcmp(argv[1], "bottom-edge") ||
		 !strcmp(argv[1], "bottom-edge-alternate"))
		deferred_wrap_keeps_cursor_preview_and_committed_edge(
			strcmp(argv[1], "right-edge"),
			!strcmp(argv[1], "bottom-edge-alternate"));
	else if (!strcmp(argv[1], "no-wrap"))
		disabled_wrap_keeps_preedit_in_the_last_column();
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
