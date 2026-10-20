/* SPDX-License-Identifier: GPL-2.0-only */
#include "snake.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void assert_food_free(const struct fplinux_snake *snake)
{
	unsigned int segment;

	assert(snake->food < snake->columns * snake->rows);
	for (segment = 0; segment < snake->length; segment++)
		assert(snake->food != snake->body[segment]);
}

static void test_start_and_pause(void)
{
	struct fplinux_snake snake = { .best = 7 };

	fplinux_snake_start(&snake, 99, 8, 5);
	assert(snake.columns == 8 && snake.rows == 5);
	assert(snake.length == 3 && snake.score == 0 && snake.best == 7);
	assert(snake.body[0] == 20 && snake.body[1] == 19 &&
	       snake.body[2] == 18);
	assert(snake.paused && !snake.dead && !snake.won);
	assert(!fplinux_snake_tick(&snake));
	assert(snake.body[0] == 20);
	assert_food_free(&snake);
}

static void test_rectangular_movement(void)
{
	struct fplinux_snake snake = { 0 };

	fplinux_snake_start(&snake, 1, 8, 5);
	snake.food = 0;
	snake.paused = false;
	assert(fplinux_snake_tick(&snake));
	assert(snake.body[0] == 21 && snake.body[1] == 20);
	fplinux_snake_turn(&snake, FPLINUX_SNAKE_DOWN);
	assert(fplinux_snake_tick(&snake));
	assert(snake.body[0] == 29 && snake.body[1] == 21);
	assert(fplinux_snake_tick(&snake));
	assert(snake.body[0] == 37);
	assert(fplinux_snake_tick(&snake));
	assert(snake.dead && snake.paused && snake.body[0] == 37);
	assert(!fplinux_snake_tick(&snake));
}

static void test_walls(void)
{
	static const struct {
		enum fplinux_snake_direction direction;
		unsigned int last;
		unsigned int steps;
	} cases[] = {
		{ FPLINUX_SNAKE_RIGHT, 23, 3 },
		{ FPLINUX_SNAKE_UP, 4, 2 },
		{ FPLINUX_SNAKE_DOWN, 36, 2 },
	};
	size_t index;

	for (index = 0; index < sizeof(cases) / sizeof(cases[0]); index++) {
		struct fplinux_snake snake = { 0 };
		unsigned int step;

		fplinux_snake_start(&snake, 1, 8, 5);
		snake.food = 0;
		snake.paused = false;
		fplinux_snake_turn(&snake, cases[index].direction);
		for (step = 0; step < cases[index].steps; step++) {
			assert(fplinux_snake_tick(&snake));
			assert(!snake.dead);
		}
		assert(snake.body[0] == cases[index].last);
		assert(fplinux_snake_tick(&snake));
		assert(snake.dead && snake.paused);
	}
}

static void test_turns(void)
{
	struct fplinux_snake snake = { 0 };

	fplinux_snake_start(&snake, 1, 8, 5);
	snake.food = 0;
	snake.paused = false;
	fplinux_snake_turn(&snake, FPLINUX_SNAKE_LEFT);
	assert(fplinux_snake_tick(&snake));
	assert(snake.body[0] == 21);
	fplinux_snake_turn(&snake, FPLINUX_SNAKE_UP);
	fplinux_snake_turn(&snake, FPLINUX_SNAKE_LEFT);
	assert(fplinux_snake_tick(&snake));
	assert(snake.body[0] == 13);
	fplinux_snake_turn(&snake, FPLINUX_SNAKE_LEFT);
	assert(fplinux_snake_tick(&snake));
	assert(snake.body[0] == 12 && !snake.dead);
}

static void test_growth_and_restart(void)
{
	struct fplinux_snake snake = { 0 };
	uint32_t seed;

	for (seed = 0; seed < 100; seed++) {
		fplinux_snake_start(&snake, seed, 8, 5);
		snake.food = 21;
		snake.paused = false;
		assert(fplinux_snake_tick(&snake));
		assert(snake.body[0] == 21 && snake.body[3] == 18);
		assert(snake.length == 4 && snake.score == 1 &&
		       snake.best == 1);
		assert_food_free(&snake);
	}
	fplinux_snake_start(&snake, 1, 8, 5);
	assert(snake.best == 1 && snake.score == 0 && snake.length == 3);
	assert(snake.paused);
}

static void test_self_collision_and_vacating_tail(void)
{
	static const uint16_t hook[] = { 18, 17, 25, 26, 27 };
	static const uint16_t square[] = { 18, 17, 25, 26 };
	struct fplinux_snake snake = { 0 };

	fplinux_snake_start(&snake, 1, 8, 5);
	memcpy(snake.body, hook, sizeof(hook));
	snake.length = 5;
	snake.food = 0;
	snake.paused = false;
	fplinux_snake_turn(&snake, FPLINUX_SNAKE_DOWN);
	assert(fplinux_snake_tick(&snake));
	assert(snake.dead && snake.paused && snake.body[0] == 18);

	fplinux_snake_start(&snake, 1, 8, 5);
	memcpy(snake.body, square, sizeof(square));
	snake.length = 4;
	snake.food = 0;
	snake.paused = false;
	fplinux_snake_turn(&snake, FPLINUX_SNAKE_DOWN);
	assert(fplinux_snake_tick(&snake));
	assert(!snake.dead && snake.body[0] == 26 && snake.length == 4);
}

static void test_win(void)
{
	/* A continuous 29-cell body leaves only cell 0 free on a 6x5 board. */
	static const uint16_t body[] = {
		6, 12, 18, 24, 25, 19, 13, 7,  8,  14, 20, 26, 27, 21, 15,
		9, 10, 16, 22, 28, 29, 23, 17, 11, 5,  4,  3,  2,  1,
	};
	struct fplinux_snake snake = { 0 };

	fplinux_snake_start(&snake, 1, 6, 5);
	memcpy(snake.body, body, sizeof(body));
	snake.length = 29;
	snake.score = 26;
	snake.best = 26;
	snake.direction = FPLINUX_SNAKE_UP;
	snake.pending = FPLINUX_SNAKE_UP;
	snake.food = 0;
	snake.paused = false;
	assert(fplinux_snake_tick(&snake));
	assert(snake.body[0] == 0 && snake.body[29] == 1);
	assert(snake.won && snake.paused && !snake.dead);
	assert(snake.length == 30 && snake.score == 27 && snake.best == 27);
	assert(!fplinux_snake_tick(&snake));
}

static void test_size_limits(void)
{
	static const unsigned int sizes[][4] = {
		{ 0, 0, 5, 5 },	    { 3, 200, 5, 102 }, { 1000, 1000, 102, 5 },
		{ 20, 29, 20, 25 }, { 22, 24, 22, 23 }, { 32, 16, 32, 16 },
	};
	size_t index;

	for (index = 0; index < sizeof(sizes) / sizeof(sizes[0]); index++) {
		struct fplinux_snake snake = { 0 };

		fplinux_snake_start(&snake, 1, sizes[index][0],
				    sizes[index][1]);
		assert(snake.columns == sizes[index][2]);
		assert(snake.rows == sizes[index][3]);
		assert(snake.columns * snake.rows <= 512);
		assert_food_free(&snake);
	}
}

int main(void)
{
	test_start_and_pause();
	test_rectangular_movement();
	test_walls();
	test_turns();
	test_growth_and_restart();
	test_self_collision_and_vacating_tail();
	test_win();
	test_size_limits();
	puts("snake-model: all checks passed");
	return 0;
}
