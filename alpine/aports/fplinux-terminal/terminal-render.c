/* SPDX-License-Identifier: GPL-2.0-only */
#include "terminal-render.h"

#include <limits.h>

static unsigned int glyph_width(const lv_font_t *font, uint32_t character)
{
	lv_font_glyph_dsc_t glyph;
	unsigned int width;

	if (!lv_font_get_glyph_dsc(font, &glyph, character, 0))
		return 0;
	if (glyph.is_placeholder) {
		lv_font_glyph_release_draw_data(&glyph);
		return 0;
	}
	width = glyph.adv_w;
	if (glyph.box_w > width)
		width = glyph.box_w;
	lv_font_glyph_release_draw_data(&glyph);
	return width;
}

bool fplinux_terminal_grid_init(struct fplinux_terminal_grid *grid,
				const lv_font_t *font)
{
	int32_t height;
	unsigned int width = 0;

	if (!font || (height = lv_font_get_line_height(font)) <= 0)
		return false;
	/* The shell and both installed keypad languages share these columns. */
	for (uint32_t code = 0x20; code <= 0x45f; ++code) {
		unsigned int candidate;

		if (code == 0x7f)
			code = 0x400;
		candidate = glyph_width(font, code);
		if (candidate > width)
			width = candidate;
	}
	if (!width || width > INT_MAX)
		return false;
	*grid = (struct fplinux_terminal_grid){
		.font = font,
		.cell_width = width,
		.cell_height = (unsigned int)height,
	};
	return true;
}

static void rectangle(lv_layer_t *layer, const lv_area_t *area,
		      lv_color_t color)
{
	lv_draw_rect_dsc_t fill;

	lv_draw_rect_dsc_init(&fill);
	fill.bg_color = color;
	fill.bg_opa = LV_OPA_COVER;
	lv_draw_rect(layer, &fill, area);
}

static void glyph(const struct fplinux_terminal_grid *grid, lv_layer_t *layer,
		  const lv_area_t *cell, uint32_t character, lv_color_t color)
{
	lv_draw_letter_dsc_t letter;
	lv_font_glyph_dsc_t descriptor;
	lv_point_t position = { .x = cell->x1, .y = cell->y1 };

	if (!character || character == ' ')
		return;
	if (lv_font_get_glyph_dsc(grid->font, &descriptor, character, 0)) {
		bool fits = !descriptor.is_placeholder &&
			    descriptor.box_w <= grid->cell_width;

		lv_font_glyph_release_draw_data(&descriptor);
		if (!fits)
			character = '?';
	} else {
		character = '?';
	}
	if (lv_font_get_glyph_dsc(grid->font, &descriptor, character, 0)) {
		position.x +=
			((int32_t)grid->cell_width - descriptor.box_w) / 2 -
			descriptor.ofs_x;
		lv_font_glyph_release_draw_data(&descriptor);
	}
	lv_draw_letter_dsc_init(&letter);
	letter.font = grid->font;
	letter.unicode = character;
	letter.color = color;
	letter.opa = LV_OPA_COVER;
	lv_draw_letter(layer, &letter, &position);
}

void fplinux_terminal_render(struct fplinux_terminal *terminal,
			     const struct fplinux_terminal_grid *grid,
			     lv_layer_t *layer, const lv_area_t *bounds)
{
	const struct tsm_screen_cell *cells =
		tsm_screen_draw2(terminal->screen);
	unsigned int columns = tsm_screen_get_width(terminal->screen);
	unsigned int rows = tsm_screen_get_height(terminal->screen);
	unsigned int cursor_x = tsm_screen_get_cursor_x(terminal->screen);
	unsigned int cursor_y = tsm_screen_get_cursor_y(terminal->screen);
	struct tsm_screen_attr defaults;
	lv_color_t foreground;
	uint32_t preedit = fplinux_terminal_preedit(terminal);

	if (!cells)
		return;
	tsm_vte_get_def_attr(terminal->vte, &defaults);
	foreground = lv_color_make(defaults.fr, defaults.fg, defaults.fb);
	rectangle(layer, bounds,
		  lv_color_make(defaults.br, defaults.bg, defaults.bb));
	for (unsigned int row = 0; row < rows; ++row)
		for (unsigned int column = 0; column < columns; ++column) {
			const struct tsm_screen_cell *cell =
				&cells[row * columns + column];
			lv_area_t area = {
				.x1 = bounds->x1 +
				      (int32_t)(column * grid->cell_width),
				.y1 = bounds->y1 +
				      (int32_t)(row * grid->cell_height),
			};
			lv_color_t text = lv_color_make(cell->fg.r, cell->fg.g,
							cell->fg.b);
			bool cursor = !terminal->history && row == cursor_y &&
				      column == cursor_x;

			area.x2 = area.x1 + (int32_t)grid->cell_width - 1;
			area.y2 = area.y1 + (int32_t)grid->cell_height - 1;
			if (area.x2 > bounds->x2 || area.y2 > bounds->y2)
				continue;
			rectangle(layer, &area,
				  cursor && preedit ?
					  lv_color_hex(0xadcfde) :
					  lv_color_make(cell->bg.r, cell->bg.g,
							cell->bg.b));
			glyph(grid, layer, &area,
			      cursor && preedit ? preedit : cell->ch,
			      cursor && preedit ? lv_color_hex(0x083442) :
						  text);
			if (cell->attr2.underline || (cursor && preedit) ||
			    (cursor && terminal->cursor_visible &&
			     !(tsm_screen_get_flags(terminal->screen) &
			       TSM_SCREEN_HIDE_CURSOR))) {
				area.y1 = area.y2 - 1;
				rectangle(layer, &area,
					  cursor ? foreground : text);
			}
		}
	terminal->dirty = false;
}
