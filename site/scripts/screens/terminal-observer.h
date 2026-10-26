/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_TERMINAL_OBSERVER_H
#define FPLINUX_TERMINAL_OBSERVER_H

#include <stdbool.h>
#include <stddef.h>

struct terminal_label_observation {
	char text[1024];
	int left;
	int top;
	int width;
	int height;
	int text_width;
	int text_height;
	bool scrolls;
	bool ellipsizes;
};

struct terminal_frame_observation {
	int content_left;
	int content_top;
	int content_width;
	int content_height;
	size_t list_count;
	size_t list_focused;
	bool list_selected[2];
	bool labels_truncated;
	bool shell_resources_closed;
	bool pty_watched;
	bool final_output_visible;
	unsigned int terminal_columns;
	unsigned int terminal_rows;
	unsigned int terminal_cursor_x;
	unsigned int terminal_cursor_y;
	unsigned int terminal_flags;
	unsigned int terminal_preedit;
	unsigned int terminal_first_character;
	unsigned int terminal_first_background;
	unsigned int terminal_edge_character;
	bool terminal_cursor_visible;
	size_t label_count;
	struct terminal_label_observation labels[32];
};

#endif
