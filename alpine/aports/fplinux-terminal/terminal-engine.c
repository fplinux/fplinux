/* SPDX-License-Identifier: GPL-2.0-only */
#include "fplinux-terminal.h"
#include "fplinux-ui-profile.h"
#include "fplinux-ui-session.h"

#include <string.h>

static void queue_write(struct tsm_vte *vte, const char *bytes, size_t size,
			void *data)
{
	struct fplinux_terminal *terminal = data;

	(void)vte;
	if (size > sizeof(terminal->output) - terminal->output_size) {
		terminal->failed = true;
		return;
	}
	memcpy(terminal->output + terminal->output_size, bytes, size);
	terminal->output_size += size;
}

static void palette_rgb(uint8_t color[3], uint32_t rgb)
{
	color[0] = (uint8_t)(rgb >> 16);
	color[1] = (uint8_t)(rgb >> 8);
	color[2] = (uint8_t)rgb;
}

bool fplinux_terminal_init(struct fplinux_terminal *terminal,
			   unsigned int columns, unsigned int rows)
{
	const struct fplinux_ui_theme_spec *dark =
		fplinux_ui_theme_spec(FPLINUX_UI_SESSION_THEME_DARK);
	/* Shell colours stay legible on the terminal's dark background. */
	uint8_t palette[TSM_COLOR_NUM][3] = {
		[TSM_COLOR_BLACK] = { 0x00, 0x00, 0x00 },
		[TSM_COLOR_RED] = { 0xf7, 0x59, 0x5a },
		[TSM_COLOR_GREEN] = { 0x39, 0xcb, 0x39 },
		[TSM_COLOR_YELLOW] = { 0xd6, 0xbe, 0x00 },
		[TSM_COLOR_BLUE] = { 0x5a, 0x86, 0xff },
		[TSM_COLOR_MAGENTA] = { 0xd6, 0x5d, 0xd6 },
		[TSM_COLOR_CYAN] = { 0x00, 0xc3, 0xc6 },
		[TSM_COLOR_LIGHT_GREY] = { 0xc6, 0xc7, 0xc6 },
		[TSM_COLOR_DARK_GREY] = { 0x84, 0x86, 0x84 },
		[TSM_COLOR_LIGHT_RED] = { 0xff, 0x82, 0x84 },
		[TSM_COLOR_LIGHT_GREEN] = { 0x73, 0xf3, 0x73 },
		[TSM_COLOR_LIGHT_YELLOW] = { 0xff, 0xf3, 0x63 },
		[TSM_COLOR_LIGHT_BLUE] = { 0x8c, 0xae, 0xff },
		[TSM_COLOR_LIGHT_MAGENTA] = { 0xff, 0x8e, 0xff },
		[TSM_COLOR_LIGHT_CYAN] = { 0x63, 0xff, 0xff },
		[TSM_COLOR_WHITE] = { 0xff, 0xff, 0xff },
	};

	palette_rgb(palette[TSM_COLOR_FOREGROUND],
		    dark->rgb[FPLINUX_UI_COLOUR_FOREGROUND]);
	palette_rgb(palette[TSM_COLOR_BACKGROUND],
		    dark->rgb[FPLINUX_UI_COLOUR_BACKGROUND]);
	memset(terminal, 0, sizeof(*terminal));
	fplinux_text_input_init(&terminal->input, NULL, NULL, false);
	if (!columns || !rows ||
	    tsm_screen_new(&terminal->screen, NULL, NULL) < 0)
		return false;
	if (tsm_vte_new(&terminal->vte, terminal->screen, queue_write, terminal,
			NULL, NULL) < 0)
		goto fail;
	if (tsm_vte_set_custom_palette(terminal->vte, palette) < 0 ||
	    tsm_vte_set_palette(terminal->vte, "custom") < 0)
		goto fail;
	/* Colour retained cells in both buffers before the first viewport shrink. */
	tsm_screen_set_flags(terminal->screen, TSM_SCREEN_ALTERNATE);
	tsm_screen_erase_screen(terminal->screen, false);
	tsm_screen_reset_flags(terminal->screen, TSM_SCREEN_ALTERNATE);
	if (tsm_screen_resize(terminal->screen, columns, rows) < 0)
		goto fail;
	tsm_screen_set_max_sb(terminal->screen,
			      FPLINUX_TERMINAL_SCROLLBACK_LINES);
	tsm_vte_set_backspace_sends_delete(terminal->vte, true);
	terminal->cursor_visible = true;
	terminal->dirty = true;
	return true;

fail:
	fplinux_terminal_destroy(terminal);
	return false;
}

void fplinux_terminal_destroy(struct fplinux_terminal *terminal)
{
	if (terminal->vte)
		tsm_vte_unref(terminal->vte);
	if (terminal->screen)
		tsm_screen_unref(terminal->screen);
	terminal->screen = NULL;
	terminal->vte = NULL;
}

void fplinux_terminal_feed(struct fplinux_terminal *terminal, const char *bytes,
			   size_t size)
{
	tsm_vte_input(terminal->vte, bytes, size);
	terminal->dirty = true;
}

void fplinux_terminal_consume(struct fplinux_terminal *terminal, size_t size)
{
	if (size > terminal->output_size)
		size = terminal->output_size;
	terminal->output_size -= size;
	memmove(terminal->output, terminal->output + size,
		terminal->output_size);
}

unsigned int
fplinux_terminal_history_offset(const struct fplinux_terminal *terminal)
{
	unsigned int count = tsm_screen_sb_get_line_count(terminal->screen);
	unsigned int position = tsm_screen_sb_get_line_pos(terminal->screen);

	return terminal->history && position <= count ? count - position : 0;
}
