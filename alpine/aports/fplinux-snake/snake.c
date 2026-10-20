/* SPDX-License-Identifier: GPL-2.0-only */
#include "snake.h"

#include <string.h>

static unsigned int clamp(unsigned int value, unsigned int low,
			  unsigned int high)
{
	return value < low ? low : value > high ? high : value;
}

static void place_food(struct fplinux_snake *snake)
{
	unsigned int cells = snake->columns * snake->rows;
	unsigned int available = cells - snake->length;
	unsigned int chosen;
	unsigned int cell;

	if (!available) {
		snake->won = true;
		snake->paused = true;
		return;
	}
	snake->random ^= snake->random << 13;
	snake->random ^= snake->random >> 17;
	snake->random ^= snake->random << 5;
	chosen = snake->random % available;
	for (cell = 0; cell < cells; cell++) {
		unsigned int segment;

		for (segment = 0; segment < snake->length; segment++)
			if (snake->body[segment] == cell)
				break;
		if (segment != snake->length)
			continue;
		if (!chosen--) {
			snake->food = cell;
			return;
		}
	}
}

void fplinux_snake_start(struct fplinux_snake *snake, uint32_t seed,
			 unsigned int columns, unsigned int rows)
{
	unsigned int best = snake->best;
	unsigned int center;

	columns = clamp(columns, FPLINUX_SNAKE_MIN_SIDE,
			FPLINUX_SNAKE_MAX_CELLS / FPLINUX_SNAKE_MIN_SIDE);
	rows = clamp(rows, FPLINUX_SNAKE_MIN_SIDE,
		     FPLINUX_SNAKE_MAX_CELLS / columns);
	/* The head at the centre, the body to its left in the same row. */
	center = columns * (rows / 2) + columns / 2;
	memset(snake, 0, sizeof(*snake));
	snake->columns = columns;
	snake->rows = rows;
	snake->best = best;
	snake->random = seed ? seed : 1;
	snake->length = 3;
	snake->body[0] = center;
	snake->body[1] = center - 1;
	snake->body[2] = center - 2;
	snake->direction = FPLINUX_SNAKE_RIGHT;
	snake->pending = FPLINUX_SNAKE_RIGHT;
	snake->paused = true;
	place_food(snake);
}

void fplinux_snake_turn(struct fplinux_snake *snake,
			enum fplinux_snake_direction direction)
{
	/* At most one turn per tick; two rapid presses cannot reverse the body. */
	if (snake->pending == snake->direction &&
	    ((unsigned int)direction + 2) % 4 != snake->direction)
		snake->pending = direction;
}

bool fplinux_snake_tick(struct fplinux_snake *snake)
{
	int columns = (int)snake->columns;
	int x = snake->body[0] % columns;
	int y = snake->body[0] / columns;
	unsigned int next;
	unsigned int segment;
	unsigned int occupied;
	bool eating;

	if (snake->paused || snake->dead || snake->won)
		return false;
	snake->direction = snake->pending;
	if (snake->direction == FPLINUX_SNAKE_UP)
		y--;
	else if (snake->direction == FPLINUX_SNAKE_DOWN)
		y++;
	else if (snake->direction == FPLINUX_SNAKE_LEFT)
		x--;
	else
		x++;
	if (x < 0 || x >= columns || y < 0 || y >= (int)snake->rows)
		goto collision;
	next = (unsigned int)(y * columns + x);
	eating = next == snake->food;
	/* The tail vacates its cell unless the snake is growing. */
	occupied = snake->length - (eating ? 0 : 1);
	for (segment = 0; segment < occupied; segment++)
		if (snake->body[segment] == next)
			goto collision;
	if (eating) {
		snake->length++;
		snake->score++;
		if (snake->score > snake->best)
			snake->best = snake->score;
	}
	memmove(snake->body + 1, snake->body,
		(snake->length - 1) * sizeof(snake->body[0]));
	snake->body[0] = next;
	if (eating)
		place_food(snake);
	return true;

collision:
	snake->dead = true;
	snake->paused = true;
	return true;
}
