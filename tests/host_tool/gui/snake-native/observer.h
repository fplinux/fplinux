/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_SNAKE_OBSERVER_H
#define FPLINUX_SNAKE_OBSERVER_H

#include <stdbool.h>

struct snake_observation {
	unsigned int frame;
	unsigned int canvases;
	unsigned int starts;
	unsigned int head;
	unsigned int pending;
	unsigned int score;
	bool canvas;
	bool paused;
	bool dead;
	bool won;
	char labels[4096];
};

#endif
