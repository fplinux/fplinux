/* SPDX-License-Identifier: GPL-2.0-only */
#include "terminal-render.h"

#include <math.h>
#include <string.h>

static bool glyph_descriptor(const lv_font_t *font,
			     lv_font_glyph_dsc_t *descriptor,
			     uint32_t character, uint32_t next)
{
	const struct fplinux_font *psf = font->dsc;
	const unsigned char *bitmap = fplinux_font_glyph(psf, character);

	(void)next;
	if (!bitmap)
		return false;
	*descriptor = (lv_font_glyph_dsc_t){
		.gid.index =
			(uint32_t)((bitmap - psf->bitmap) / psf->glyph_bytes),
		.format = LV_FONT_GLYPH_FORMAT_A1,
		.adv_w = psf->width,
		.box_w = psf->width,
		.box_h = psf->height,
		.stride = psf->row_bytes,
	};
	return true;
}

static const void *glyph_bitmap(lv_font_glyph_dsc_t *descriptor,
				lv_draw_buf_t *draw_buffer)
{
	const struct fplinux_font *psf = descriptor->resolved_font->dsc;
	const unsigned char *bitmap =
		psf->bitmap + (size_t)descriptor->gid.index * psf->glyph_bytes;

	if (descriptor->req_raw_bitmap)
		return bitmap;
	if (!draw_buffer)
		return NULL;
	/* LVGL supplies an A8 destination even for one-bit source glyphs. */
	for (unsigned int row = 0; row < psf->height; ++row) {
		unsigned char *pixels =
			draw_buffer->data + row * draw_buffer->header.stride;

		for (unsigned int column = 0; column < psf->width; ++column) {
			unsigned char bits =
				bitmap[row * psf->row_bytes + column / 8U];

			pixels[column] = bits & (0x80U >> (column % 8U)) ?
						 LV_OPA_COVER :
						 0;
		}
	}
	lv_draw_buf_flush_cache(draw_buffer, NULL);
	return draw_buffer;
}

bool fplinux_terminal_grid_open(struct fplinux_terminal_grid *grid,
				const char *path)
{
	memset(grid, 0, sizeof(*grid));
	if (!fplinux_font_open(&grid->psf, path))
		return false;
	if (!((grid->psf.width == 6 && grid->psf.height == 12) ||
	      (grid->psf.width == 8 && grid->psf.height == 16))) {
		fplinux_terminal_grid_close(grid);
		return false;
	}
	grid->font = (lv_font_t){
		.dsc = &grid->psf,
		.get_glyph_dsc = glyph_descriptor,
		.get_glyph_bitmap = glyph_bitmap,
		.line_height = grid->psf.height,
		.kerning = LV_FONT_KERNING_NONE,
		.static_bitmap = 1,
	};
	grid->cell_width = grid->psf.width;
	grid->cell_height = grid->psf.height;
	return true;
}

void fplinux_terminal_grid_close(struct fplinux_terminal_grid *grid)
{
	fplinux_font_close(&grid->psf);
	memset(grid, 0, sizeof(*grid));
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
	lv_draw_label_dsc_t letter;
	lv_point_t position = { .x = cell->x1, .y = cell->y1 };

	if (!character || character == ' ')
		return;
	lv_draw_label_dsc_init(&letter);
	letter.font = &grid->font;
	letter.color = color;
	letter.opa = LV_OPA_COVER;
	lv_draw_character(layer, &letter, &position, character);
}

static double linear_channel(uint8_t channel)
{
	double value = channel / 255.0;

	return value <= 0.04045 ? value / 12.92 :
				  pow((value + 0.055) / 1.055, 2.4);
}

static double luminance(uint8_t red, uint8_t green, uint8_t blue)
{
	return 0.2126 * linear_channel(red) + 0.7152 * linear_channel(green) +
	       0.0722 * linear_channel(blue);
}

static double contrast(double first, double second)
{
	return first > second ? (first + 0.05) / (second + 0.05) :
				(second + 0.05) / (first + 0.05);
}

static lv_color_t cursor_color(const struct tsm_screen_attr *defaults,
			       const struct tsm_screen_cell *cell)
{
	double foreground = luminance(defaults->fr, defaults->fg, defaults->fb);
	double background = luminance(defaults->br, defaults->bg, defaults->bb);
	double actual = cell ? luminance(cell->bg.r, cell->bg.g, cell->bg.b) :
			       background;

	if (contrast(actual, foreground) >= contrast(actual, background))
		return lv_color_make(defaults->fr, defaults->fg, defaults->fb);
	return lv_color_make(defaults->br, defaults->bg, defaults->bb);
}

void fplinux_terminal_render(struct fplinux_terminal *terminal,
			     const struct fplinux_terminal_grid *grid,
			     lv_layer_t *layer, const lv_area_t *bounds,
			     lv_color_t accent, lv_color_t accent_text)
{
	const struct tsm_screen_cell *cells =
		tsm_screen_draw2(terminal->screen);
	unsigned int columns = tsm_screen_get_width(terminal->screen);
	unsigned int rows = tsm_screen_get_height(terminal->screen);
	unsigned int cursor_x = tsm_screen_get_cursor_x(terminal->screen);
	unsigned int cursor_y = tsm_screen_get_cursor_y(terminal->screen);
	unsigned int flags = tsm_screen_get_flags(terminal->screen);
	unsigned int source_offset = 0;
	struct tsm_screen_attr defaults;
	lv_color_t foreground, background;
	uint32_t preedit =
		terminal->history ? 0 : fplinux_terminal_preedit(terminal);

	if (!cells)
		return;
	if (cursor_x >= columns) {
		cursor_x = columns - 1;
		if (preedit && (flags & TSM_SCREEN_AUTO_WRAP)) {
			cursor_x = 0;
			if (cursor_y + 1 < rows)
				++cursor_y;
			else
				/* Project one wrap without changing the stored screen. */
				source_offset = 1;
		}
	}
	tsm_vte_get_def_attr(terminal->vte, &defaults);
	foreground = lv_color_make(defaults.fr, defaults.fg, defaults.fb);
	background = lv_color_make(defaults.br, defaults.bg, defaults.bb);
	rectangle(layer, bounds, background);
	for (unsigned int row = 0; row < rows; ++row)
		for (unsigned int column = 0; column < columns; ++column) {
			const struct tsm_screen_cell *cell =
				row + source_offset < rows ?
					&cells[(row + source_offset) * columns +
					       column] :
					NULL;
			lv_area_t area = {
				.x1 = bounds->x1 +
				      (int32_t)(column * grid->cell_width),
				.y1 = bounds->y1 +
				      (int32_t)(row * grid->cell_height),
			};
			lv_color_t text = foreground, fill = background;
			uint32_t character = 0;
			bool cursor = !terminal->history && row == cursor_y &&
				      column == cursor_x;
			bool pending = cursor && preedit;
			bool cursor_underline =
				cursor && !pending &&
				terminal->cursor_visible &&
				!(flags & TSM_SCREEN_HIDE_CURSOR);

			if (cell) {
				text = lv_color_make(cell->fg.r, cell->fg.g,
						     cell->fg.b);
				fill = lv_color_make(cell->bg.r, cell->bg.g,
						     cell->bg.b);
				character = cell->ch;
			}
			if (pending) {
				text = accent_text;
				fill = accent;
				character = preedit;
			}
			area.x2 = area.x1 + (int32_t)grid->cell_width - 1;
			area.y2 = area.y1 + (int32_t)grid->cell_height - 1;
			if (area.x2 > bounds->x2 || area.y2 > bounds->y2)
				continue;
			rectangle(layer, &area, fill);
			glyph(grid, layer, &area, character, text);
			if (!pending && (cursor_underline ||
					 (cell && cell->attr2.underline))) {
				area.y1 = area.y2 - 1;
				rectangle(layer, &area,
					  cursor_underline ?
						  cursor_color(&defaults,
							       cell) :
						  text);
			}
		}
	terminal->dirty = false;
}
