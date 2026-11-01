/* SPDX-License-Identifier: GPL-2.0-only */
#include "brightness-ui.h"
#include "fplinux-keypad.h"

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>

int main(void)
{
	static const struct {
		unsigned int width, height, bar_y;
	} displays[] = {
		{ 128, 128, 50 }, { 128, 160, 47 }, { 176, 220, 59 },
		{ 240, 320, 66 }, { 320, 240, 62 },
	};
	unsigned int requested = 99;

	assert(brightness_ui_key(0, FPLINUX_KEY_DOWN, &requested) ==
	       BRIGHTNESS_UI_NONE);
	assert(requested == 99);
	assert(brightness_ui_key(0, FPLINUX_KEY_UP, &requested) ==
	       BRIGHTNESS_UI_SET);
	assert(requested == 1);
	assert(brightness_ui_key(10, FPLINUX_KEY_UP, &requested) ==
	       BRIGHTNESS_UI_NONE);
	assert(brightness_ui_key(10, FPLINUX_KEY_DOWN, &requested) ==
	       BRIGHTNESS_UI_SET);
	assert(requested == 9);
	assert(brightness_ui_key(5, FPLINUX_KEY_SOFT_RIGHT, &requested) ==
	       BRIGHTNESS_UI_EXIT);
	for (size_t i = 0; i < sizeof(displays) / sizeof(displays[0]); ++i) {
		unsigned int width = displays[i].width,
			     height = displays[i].height;
		size_t stride = width + 4;
		uint16_t *pixels =
			malloc((stride * height + 8) * sizeof(*pixels));
		struct brightness_ui_surface surface = {
			.pixels = pixels,
			.width = width,
			.height = height,
			.stride_bytes = stride * sizeof(*pixels),
		};

		assert(pixels);
		for (size_t j = 0; j < stride * height + 8; ++j)
			pixels[j] = 0xa55a;
		for (unsigned int level = 0; level <= 10; level += 5) {
			unsigned int runs = 0;
			bool previous = false;

			assert(brightness_ui_render(&surface, level,
						    level == 10));
			for (unsigned int x = 0; x < width; ++x) {
				bool filled =
					pixels[displays[i].bar_y * stride + x] ==
					0xae7b;

				if (filled && !previous)
					++runs;
				previous = filled;
			}
			assert(runs == level);
		}
		assert(!brightness_ui_render(&surface, 11, false));
		for (unsigned int y = 0; y < height; ++y)
			for (unsigned int x = width; x < stride; ++x)
				assert(pixels[y * stride + x] == 0xa55a);
		for (size_t j = stride * height; j < stride * height + 8; ++j)
			assert(pixels[j] == 0xa55a);
		free(pixels);
	}
	return 0;
}
