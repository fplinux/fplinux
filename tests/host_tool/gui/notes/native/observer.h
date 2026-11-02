/* SPDX-License-Identifier: 0BSD */
#ifndef FPLINUX_NOTES_OBSERVER_H
#define FPLINUX_NOTES_OBSERVER_H

#include <stdbool.h>
#include <stdint.h>

struct notes_observation {
	unsigned int frame;
	unsigned int editors;
	uint32_t cursor;
	bool editor;
	bool menu;
	char text[256];
	char labels[4096];
};

#endif
