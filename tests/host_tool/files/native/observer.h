/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_FILES_OBSERVER_H
#define FPLINUX_FILES_OBSERVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct files_observation {
	unsigned int frame;
	bool menu;
	bool editor;
	uint32_t cursor;
	unsigned int glyphs;
	unsigned int menu_glyphs;
	size_t focused;
	size_t first_visible;
	size_t count;
	char text[1024];
	char labels[16384];
	size_t menu_count;
	char menu_items[32][128];
	bool menu_separators[32];
};

#endif
