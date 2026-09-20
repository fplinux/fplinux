// SPDX-License-Identifier: GPL-2.0-only
/* Host harness for the production ARMADA renderer and compiled timeline. */

#include "../../alpine/aports/fplinux-showcase/armada-scene.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define GUARD_PIXELS 16U
#define GUARD_VALUE 0xa55aU
#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))

enum expected_output_field {
	EXPECT_KEYPAD = 1U << 0,
	EXPECT_RUMBLE = 1U << 1,
	EXPECT_CUE_ID = 1U << 2,
	EXPECT_LCD_LEVEL = 1U << 3,
};

struct output_expectation {
	uint32_t frame;
	bool keypad;
	bool rumble;
	uint16_t cue_id;
	int lcd_level;
	unsigned int fields;
};

static uint64_t frame_hash(const uint16_t *pixels, size_t count)
{
	uint64_t hash = 1469598103934665603ULL;
	size_t index;

	for (index = 0; index < count; ++index) {
		hash ^= pixels[index];
		hash *= 1099511628211ULL;
	}
	return hash;
}

static int guards_are_intact(const uint16_t *storage, size_t pixels)
{
	unsigned int index;

	for (index = 0; index < GUARD_PIXELS; ++index)
		if (storage[index] != GUARD_VALUE ||
		    storage[GUARD_PIXELS + pixels + index] != GUARD_VALUE)
			return 0;
	return 1;
}

static int frame_has_detail(const uint16_t *pixels, size_t count)
{
	size_t index;
	size_t different = 0;

	for (index = 1; index < count; ++index)
		if (pixels[index] != pixels[0])
			++different;
	return different > count / 16U;
}

static int output_cases_match(struct armada_scene *scene,
			      const struct armada_metrics *metrics,
			      const struct output_expectation *expectations,
			      size_t count)
{
	size_t index;

	for (index = 0; index < count; ++index) {
		const struct output_expectation *expected =
			&expectations[index];
		struct armada_outputs actual;

		armada_scene_render(scene, expected->frame, metrics, &actual);
		if (((expected->fields & EXPECT_KEYPAD) &&
		     actual.keypad != expected->keypad) ||
		    ((expected->fields & EXPECT_RUMBLE) &&
		     actual.rumble != expected->rumble) ||
		    ((expected->fields & EXPECT_CUE_ID) &&
		     actual.cue_id != expected->cue_id) ||
		    ((expected->fields & EXPECT_LCD_LEVEL) &&
		     actual.lcd_level != expected->lcd_level))
			return 0;
	}
	return 1;
}

static int render_case(unsigned int width, unsigned int height)
{
	static const uint32_t frames[] = {
		0U,   42U,   43U,   44U,   45U,	  45U,	 58U,	42U,
		120U, 210U,  390U,  570U,  660U,  720U,	 720U,	726U,
		930U, 1020U, 1051U, 1031U, 1032U, 1033U, 1080U, 1139U,
	};
	static const struct output_expectation output_expectations[] = {
		{ .frame = 600U,
		  .lcd_level = 6,
		  .fields = EXPECT_KEYPAD | EXPECT_RUMBLE | EXPECT_LCD_LEVEL },
		{ .frame = 720U,
		  .keypad = true,
		  .rumble = true,
		  .cue_id = 1U,
		  .lcd_level = 6,
		  .fields = EXPECT_KEYPAD | EXPECT_RUMBLE | EXPECT_CUE_ID |
			    EXPECT_LCD_LEVEL },
		{ .frame = 723U,
		  .lcd_level = 6,
		  .fields = EXPECT_KEYPAD | EXPECT_RUMBLE | EXPECT_CUE_ID |
			    EXPECT_LCD_LEVEL },
		{ .frame = 726U,
		  .keypad = true,
		  .rumble = true,
		  .cue_id = 2U,
		  .fields = EXPECT_KEYPAD | EXPECT_RUMBLE | EXPECT_CUE_ID },
		{ .frame = 740U,
		  .keypad = true,
		  .rumble = true,
		  .cue_id = 3U,
		  .fields = EXPECT_KEYPAD | EXPECT_RUMBLE | EXPECT_CUE_ID },
		{ .frame = 756U,
		  .keypad = true,
		  .rumble = true,
		  .cue_id = 5U,
		  .fields = EXPECT_KEYPAD | EXPECT_RUMBLE | EXPECT_CUE_ID },
		{ .frame = 938U,
		  .keypad = true,
		  .rumble = true,
		  .cue_id = 23U,
		  .fields = EXPECT_KEYPAD | EXPECT_RUMBLE | EXPECT_CUE_ID },
		{ .frame = 939U,
		  .fields = EXPECT_KEYPAD | EXPECT_RUMBLE | EXPECT_CUE_ID },
		{ .frame = 0U, .lcd_level = 10, .fields = EXPECT_LCD_LEVEL },
		{ .frame = 285U, .lcd_level = 6, .fields = EXPECT_LCD_LEVEL },
		{ .frame = 570U, .lcd_level = 6, .fields = EXPECT_LCD_LEVEL },
		{ .frame = 855U, .lcd_level = 6, .fields = EXPECT_LCD_LEVEL },
		{ .frame = 1139U, .lcd_level = 10, .fields = EXPECT_LCD_LEVEL },
	};
	struct armada_metrics metrics = {
		.average_fps_tenths = 300,
		.minimum_fps_tenths = 280,
		.maximum_frame_us = 35714,
		.valid = true,
	};
	size_t count = (size_t)width * height;
	uint16_t *storage =
		malloc((count + 2U * GUARD_PIXELS) * sizeof(*storage));
	uint16_t *pixels;
	struct armada_scene *scene;
	size_t index;

	if (!storage)
		return EXIT_FAILURE;
	for (index = 0; index < count + 2U * GUARD_PIXELS; ++index)
		storage[index] = GUARD_VALUE;
	pixels = storage + GUARD_PIXELS;
	scene = armada_scene_create(width, height, pixels);
	if (!scene) {
		free(storage);
		return EXIT_FAILURE;
	}
	for (index = 0; index < sizeof(frames) / sizeof(frames[0]); ++index) {
		struct armada_outputs first_outputs;
		struct armada_outputs second_outputs;
		uint64_t first_hash;
		uint64_t second_hash;
		struct armada_scene *fresh;

		memset(pixels, 0, count * sizeof(*pixels));
		armada_scene_render(scene, frames[index], &metrics,
				    &first_outputs);
		first_hash = frame_hash(pixels, count);
		if (!guards_are_intact(storage, count) ||
		    !frame_has_detail(pixels, count))
			goto fail;
		/* Playback, repeated frames and seeks must produce the same image/cues. */
		fresh = armada_scene_create(width, height, pixels);
		if (!fresh)
			goto fail;
		/* A fresh destination's previous contents are not a scene input. */
		memset(pixels, 0xa5, count * sizeof(*pixels));
		armada_scene_render(fresh, frames[index], &metrics,
				    &second_outputs);
		armada_scene_destroy(fresh);
		second_hash = frame_hash(pixels, count);
		if (!guards_are_intact(storage, count) ||
		    first_hash != second_hash ||
		    memcmp(&first_outputs, &second_outputs,
			   sizeof(first_outputs)) != 0)
			goto fail;
	}
	{
		struct armada_outputs first;
		struct armada_outputs wrapped;
		uint64_t first_hash;
		uint64_t wrapped_hash;

		if (!output_cases_match(scene, &metrics, output_expectations,
					ARRAY_SIZE(output_expectations)))
			goto fail;
		memset(pixels, 0, count * sizeof(*pixels));
		armada_scene_render(scene, 0U, &metrics, &first);
		first_hash = frame_hash(pixels, count);
		memset(pixels, 0, count * sizeof(*pixels));
		armada_scene_render(scene, ARMADA_DURATION_FRAMES, &metrics,
				    &wrapped);
		wrapped_hash = frame_hash(pixels, count);
		if (first_hash != wrapped_hash ||
		    memcmp(&first, &wrapped, sizeof(first)) != 0)
			goto fail;
	}
	armada_scene_destroy(scene);
	free(storage);
	return EXIT_SUCCESS;

fail:
	armada_scene_destroy(scene);
	free(storage);
	return EXIT_FAILURE;
}

int main(void)
{
	return render_case(240U, 320U) == EXIT_SUCCESS &&
			       render_case(128U, 160U) == EXIT_SUCCESS ?
		       EXIT_SUCCESS :
		       EXIT_FAILURE;
}
