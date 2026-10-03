/* SPDX-License-Identifier: GPL-2.0-only */
#include "terminal-render.h"
#include "terminal-help.h"

#include <stdio.h>
#include <string.h>

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

static uint16_t rgb565(unsigned int red, unsigned int green, unsigned int blue)
{
	return (uint16_t)((red >> 3) << 11 | (green >> 2) << 5 | blue >> 3);
}

static void rectangle(const struct fplinux_terminal_surface *surface,
		      unsigned int x, unsigned int y, unsigned int width,
		      unsigned int height, uint16_t color)
{
	unsigned int row;
	unsigned int column;

	if (x >= surface->width || y >= surface->height)
		return;
	if (width > surface->width - x)
		width = surface->width - x;
	if (height > surface->height - y)
		height = surface->height - y;
	for (row = y; row < y + height; ++row) {
		uint16_t *pixels =
			(uint16_t *)((unsigned char *)surface->pixels +
				     row * surface->stride_bytes);

		for (column = x; column < x + width; ++column)
			pixels[column] = color;
	}
}

static void glyph(const struct fplinux_terminal_surface *surface,
		  const struct fplinux_font *font, unsigned int x,
		  unsigned int y, uint32_t codepoint, uint16_t foreground,
		  uint16_t background, bool underline)
{
	const unsigned char *bitmap;
	unsigned int row;
	unsigned int column;

	rectangle(surface, x, y, font->width, font->height, background);
	if (!codepoint || codepoint == ' ')
		goto decoration;
	bitmap = fplinux_font_glyph(font, codepoint);
	if (!bitmap)
		return;
	for (row = 0; row < font->height && y + row < surface->height; ++row) {
		uint16_t *pixels =
			(uint16_t *)((unsigned char *)surface->pixels +
				     (y + row) * surface->stride_bytes);

		for (column = 0;
		     column < font->width && x + column < surface->width;
		     ++column)
			if (bitmap[row * font->row_bytes + column / 8U] &
			    (0x80U >> (column % 8U)))
				pixels[x + column] = foreground;
	}
decoration:
	if (underline)
		rectangle(surface, x, y + font->height - 2, font->width, 1,
			  foreground);
}

static void text_span(const struct fplinux_terminal_surface *surface,
		      const struct fplinux_font *font, unsigned int x,
		      unsigned int y, const char *text, size_t size,
		      uint16_t foreground, uint16_t background)
{
	const unsigned char *cursor = (const unsigned char *)text;
	const unsigned char *end = cursor + size;
	uint32_t codepoint;

	while (x + font->width <= surface->width &&
	       next_unicode(&cursor, end, &codepoint)) {
		glyph(surface, font, x, y, codepoint, foreground, background,
		      false);
		x += font->width;
	}
}

static void text_line(const struct fplinux_terminal_surface *surface,
		      const struct fplinux_font *font, unsigned int x,
		      unsigned int y, const char *text, uint16_t foreground,
		      uint16_t background)
{
	text_span(surface, font, x, y, text, strlen(text), foreground,
		  background);
}

static void render_modifiers(struct fplinux_terminal *terminal,
			     const struct fplinux_font *font,
			     const struct fplinux_terminal_surface *surface,
			     unsigned int y)
{
	static const char *const labels[] = { "Ctrl", "Alt", "Shift" };
	static const unsigned int masks[] = {
		TSM_CONTROL_MASK,
		TSM_ALT_MASK,
		TSM_SHIFT_MASK,
	};
	unsigned int selected = terminal->modifier_panel ?
					terminal->modifier_selection :
					terminal->modifier;
	unsigned int index;

	for (index = 0; index < 3; ++index) {
		unsigned int x = index * surface->width / 3;
		unsigned int end = (index + 1) * surface->width / 3;
		uint16_t background =
			terminal->modifier_panel &&
					index == terminal->modifier_index ?
				rgb565(80, 80, 80) :
				rgb565(32, 32, 32);
		char text[8];

		snprintf(text, sizeof(text),
			 selected & masks[index] ? "[%s]" : " %s ",
			 labels[index]);
		rectangle(surface, x, y, end - x, font->height, background);
		text_line(surface, font, x, y, text, rgb565(216, 216, 216),
			  background);
	}
}

static void render_help(const struct fplinux_terminal *terminal,
			const struct fplinux_font *font,
			const struct fplinux_terminal_surface *surface,
			unsigned int visible_rows)
{
	struct fplinux_terminal_help help;
	uint16_t background = rgb565(17, 17, 17);
	uint16_t foreground = rgb565(216, 216, 216);
	uint16_t title_background = rgb565(48, 48, 48);
	unsigned int row = 1;
	const char *text;
	size_t size;
	size_t title_size;
	size_t counter_size;
	char counter[32];

	if (!visible_rows)
		return;
	fplinux_terminal_help_open(terminal, &help);
	snprintf(counter, sizeof(counter), "%u/%u", help.section + 1,
		 help.section_count);
	counter_size = strlen(counter);
	title_size = strlen(help.title);
	if (title_size + counter_size + 1 > help.columns)
		title_size = help.columns - counter_size - 1;
	rectangle(surface, 0, 0, surface->width, font->height,
		  title_background);
	text_span(surface, font, 0, 0, help.title, title_size, foreground,
		  title_background);
	text_line(surface, font, (help.columns - counter_size) * font->width, 0,
		  counter, foreground, title_background);
	while (row < visible_rows &&
	       (size = fplinux_terminal_help_next(&help, &text))) {
		text_span(surface, font, 0, row * font->height, text, size,
			  foreground, background);
		++row;
	}
}

static void render_menu(const struct fplinux_terminal *terminal,
			const struct fplinux_font *font,
			const struct fplinux_terminal_surface *surface,
			unsigned int visible_rows)
{
	unsigned int count = fplinux_terminal_menu_count(terminal);
	unsigned int first = 0;
	unsigned int row = 0;
	uint16_t background = rgb565(17, 17, 17);
	uint16_t foreground = rgb565(216, 216, 216);
	uint16_t heading_color = rgb565(144, 144, 144);
	const char *title = NULL;

	if (visible_rows < 2)
		return;
	if (terminal->menu == FPLINUX_TERMINAL_MENU_SYMBOLS)
		title = "Symbols";
	else if (terminal->menu == FPLINUX_TERMINAL_MENU_SPECIAL)
		title = "Special keys";
	else if (terminal->menu == FPLINUX_TERMINAL_MENU_LANGUAGE)
		title = "Language";
	if (title) {
		text_line(surface, font, 0, 0, title, heading_color,
			  background);
		row = 1;
	}
	if (terminal->menu_index >= visible_rows - row)
		first = terminal->menu_index - (visible_rows - row) + 1;
	for (; row < visible_rows && first < count; ++row, ++first) {
		uint16_t shade = first == terminal->menu_index ?
					 rgb565(48, 48, 48) :
					 background;
		rectangle(surface, 0, row * font->height, surface->width,
			  font->height, shade);
		text_line(surface, font, 0, row * font->height,
			  fplinux_terminal_menu_label(terminal, first),
			  foreground, shade);
	}
}

void fplinux_terminal_render(struct fplinux_terminal *terminal,
			     const struct fplinux_font *font,
			     const struct fplinux_terminal_surface *surface)
{
	const struct tsm_screen_cell *cells =
		tsm_screen_draw2(terminal->screen);
	unsigned int columns = tsm_screen_get_width(terminal->screen);
	unsigned int rows = tsm_screen_get_height(terminal->screen);
	unsigned int cursor_y = tsm_screen_get_cursor_y(terminal->screen);
	bool modifiers = terminal->modifier_panel || terminal->modifier;
	unsigned int toolbar_rows = modifiers ? 2 : 1;
	unsigned int visible_rows = surface->height / font->height;
	unsigned int first_row = 0;
	unsigned int x;
	unsigned int y;
	char status[80];
	uint16_t background = rgb565(17, 17, 17);
	uint16_t status_background = rgb565(48, 48, 48);

	if (!cells || !terminal->active)
		return;
	visible_rows =
		visible_rows > toolbar_rows ? visible_rows - toolbar_rows : 0;
	if (visible_rows > rows)
		visible_rows = rows;
	/* Keep the input row visible without changing the PTY's dimensions. */
	if (visible_rows && cursor_y >= visible_rows && !terminal->history)
		first_row = cursor_y - visible_rows + 1;
	rectangle(surface, 0, 0, surface->width, surface->height, background);
	for (y = 0; y < visible_rows; ++y)
		for (x = 0; x < columns; ++x) {
			const struct tsm_screen_cell *cell =
				&cells[(first_row + y) * columns + x];

			glyph(surface, font, x * font->width, y * font->height,
			      cell->ch,
			      rgb565(cell->fg.r, cell->fg.g, cell->fg.b),
			      rgb565(cell->bg.r, cell->bg.g, cell->bg.b),
			      cell->attr2.underline);
		}
	x = tsm_screen_get_cursor_x(terminal->screen);
	y = cursor_y - first_row;
	if (x >= columns)
		x = columns - 1;
	if (!terminal->history &&
	    terminal->menu == FPLINUX_TERMINAL_MENU_CLOSED &&
	    y < visible_rows) {
		uint32_t preedit = fplinux_terminal_preedit(terminal);

		if (preedit)
			glyph(surface, font, x * font->width, y * font->height,
			      preedit, rgb565(216, 216, 216),
			      rgb565(48, 48, 48), true);
		else if (terminal->cursor_visible &&
			 !(tsm_screen_get_flags(terminal->screen) &
			   TSM_SCREEN_HIDE_CURSOR))
			rectangle(surface, x * font->width,
				  (y + 1) * font->height - 2, font->width, 2,
				  rgb565(128, 128, 128));
	}
	if (terminal->menu != FPLINUX_TERMINAL_MENU_CLOSED) {
		rectangle(surface, 0, 0, surface->width,
			  visible_rows * font->height, background);
		if (terminal->menu == FPLINUX_TERMINAL_MENU_HELP)
			render_help(terminal, font, surface, visible_rows);
		else
			render_menu(terminal, font, surface, visible_rows);
	}
	if (modifiers && 2 * font->height <= surface->height)
		render_modifiers(terminal, font, surface,
				 surface->height - 2 * font->height);
	fplinux_terminal_status(terminal, status, sizeof(status));
	if (font->height <= surface->height) {
		unsigned int status_y = surface->height - font->height;

		rectangle(surface, 0, status_y, surface->width, font->height,
			  status_background);
		text_line(surface, font, 0, status_y, status,
			  rgb565(216, 216, 216), status_background);
	}
	terminal->dirty = false;
}
