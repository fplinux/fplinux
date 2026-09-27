/* SPDX-License-Identifier: GPL-2.0-only */
#include "fplinux-terminal.h"

#include <stdio.h>
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

static void shell_marker(struct tsm_vte *vte, const char *text, size_t size,
			 void *data)
{
	struct fplinux_terminal *terminal = data;
	size_t prefix_size = strlen(terminal->shell_marker);

	(void)vte;
	/* Only this shell's prompt marker can enable the Readline action. */
	if (size != prefix_size + 1 ||
	    memcmp(text, terminal->shell_marker, prefix_size))
		return;
	if (text[prefix_size] == 'B')
		terminal->editing = true;
	else if (text[prefix_size] == 'C')
		terminal->editing = false;
}

bool fplinux_terminal_init(struct fplinux_terminal *terminal,
			   unsigned int columns, unsigned int rows,
			   const char *shell_token)
{
	uint8_t palette[TSM_COLOR_NUM][3] = {
		{ 0x00, 0x00, 0x00 },
		{ 0xcd, 0x00, 0x00 },
		{ 0x00, 0xcd, 0x00 },
		{ 0xcd, 0xcd, 0x00 },
		{ 0x00, 0x00, 0xee },
		{ 0xcd, 0x00, 0xcd },
		{ 0x00, 0xcd, 0xcd },
		{ 0xe5, 0xe5, 0xe5 },
		{ 0x7f, 0x7f, 0x7f },
		{ 0xff, 0x00, 0x00 },
		{ 0x00, 0xff, 0x00 },
		{ 0xff, 0xff, 0x00 },
		{ 0x5c, 0x5c, 0xff },
		{ 0xff, 0x00, 0xff },
		{ 0x00, 0xff, 0xff },
		{ 0xff, 0xff, 0xff },
		[TSM_COLOR_FOREGROUND] = { 0xd8, 0xd8, 0xd8 },
		[TSM_COLOR_BACKGROUND] = { 0x11, 0x11, 0x11 },
	};

	memset(terminal, 0, sizeof(*terminal));
	fplinux_multitap_init(&terminal->compose);
	if (!shell_token || !*shell_token || strlen(shell_token) > 32)
		return false;
	snprintf(terminal->shell_marker, sizeof(terminal->shell_marker),
		 "777;%s;", shell_token);
	if (!columns || !rows ||
	    tsm_screen_new(&terminal->screen, NULL, NULL) < 0)
		return false;
	if (tsm_screen_resize(terminal->screen, columns, rows) < 0 ||
	    tsm_vte_new(&terminal->vte, terminal->screen, queue_write, terminal,
			NULL, NULL) < 0)
		goto fail;
	if (tsm_vte_set_custom_palette(terminal->vte, palette) < 0 ||
	    tsm_vte_set_palette(terminal->vte, "custom") < 0)
		goto fail;
	tsm_screen_set_max_sb(terminal->screen,
			      FPLINUX_TERMINAL_SCROLLBACK_LINES);
	tsm_vte_set_backspace_sends_delete(terminal->vte, true);
	tsm_vte_set_osc_cb(terminal->vte, shell_marker, terminal);
	terminal->active = true;
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
