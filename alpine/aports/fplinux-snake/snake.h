/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_SNAKE_H
#define FPLINUX_SNAKE_H

#include <stdbool.h>
#include <stdint.h>

/*
 * The application chooses the board from the screen; the model keeps it
 * between FPLINUX_SNAKE_MIN_SIDE cells a side and FPLINUX_SNAKE_MAX_CELLS
 * cells in all.
 */
#define FPLINUX_SNAKE_MAX_CELLS 512
#define FPLINUX_SNAKE_MIN_SIDE 5

enum fplinux_snake_direction {
	FPLINUX_SNAKE_UP,
	FPLINUX_SNAKE_RIGHT,
	FPLINUX_SNAKE_DOWN,
	FPLINUX_SNAKE_LEFT,
};

/* A cell is y * columns + x. */
struct fplinux_snake {
	uint16_t body[FPLINUX_SNAKE_MAX_CELLS];
	unsigned int columns;
	unsigned int rows;
	unsigned int length;
	unsigned int score;
	unsigned int best;
	unsigned int food;
	uint32_t random;
	enum fplinux_snake_direction direction;
	enum fplinux_snake_direction pending;
	bool paused;
	bool dead;
	bool won;
};

/*
 * A new paused game on a board of columns x rows, clamped to the limits
 * above (read the result back from the structure); keeps best.
 */
void fplinux_snake_start(struct fplinux_snake *snake, uint32_t seed,
			 unsigned int columns, unsigned int rows);
void fplinux_snake_turn(struct fplinux_snake *snake,
			enum fplinux_snake_direction direction);
bool fplinux_snake_tick(struct fplinux_snake *snake);

#endif
