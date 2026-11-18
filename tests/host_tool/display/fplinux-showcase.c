// SPDX-License-Identifier: GPL-2.0-only
/* Host harness for the production ARMADA renderer and compiled timeline. */

#include "armada-scene.h"
#include "armada-renderer.h"
#include "armada-storyboard.h"

#include <stdint.h>
#include <stdio.h>
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
	EXPECT_RUMBLE_REMAINING = 1U << 4,
};

struct output_expectation {
	uint32_t frame;
	bool keypad;
	bool rumble;
	uint16_t cue_id;
	int lcd_level;
	unsigned int rumble_remaining_ms;
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
			      const struct output_expectation *expectations,
			      size_t count)
{
	size_t index;

	for (index = 0; index < count; ++index) {
		const struct output_expectation *expected =
			&expectations[index];
		struct armada_outputs actual;

		if (!armada_scene_render(scene, expected->frame, &actual) ||
		    ((expected->fields & EXPECT_KEYPAD) &&
		     actual.keypad != expected->keypad) ||
		    ((expected->fields & EXPECT_RUMBLE) &&
		     actual.rumble != expected->rumble) ||
		    ((expected->fields & EXPECT_CUE_ID) &&
		     actual.cue_id != expected->cue_id) ||
		    ((expected->fields & EXPECT_LCD_LEVEL) &&
		     actual.lcd_level != expected->lcd_level) ||
		    ((expected->fields & EXPECT_RUMBLE_REMAINING) &&
		     actual.rumble_remaining_ms !=
			     expected->rumble_remaining_ms)) {
			fprintf(stderr, "outputs at frame %u differ\n",
				(unsigned int)expected->frame);
			return 0;
		}
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
		  .rumble_remaining_ms = 100U,
		  .fields = EXPECT_KEYPAD | EXPECT_RUMBLE | EXPECT_CUE_ID |
			    EXPECT_LCD_LEVEL | EXPECT_RUMBLE_REMAINING },
		{ .frame = 721U,
		  .rumble_remaining_ms = 66U,
		  .fields = EXPECT_RUMBLE_REMAINING },
		{ .frame = 722U,
		  .rumble_remaining_ms = 33U,
		  .fields = EXPECT_RUMBLE_REMAINING },
		{ .frame = 723U,
		  .lcd_level = 6,
		  .fields = EXPECT_KEYPAD | EXPECT_RUMBLE | EXPECT_CUE_ID |
			    EXPECT_LCD_LEVEL | EXPECT_RUMBLE_REMAINING },
		{ .frame = 726U,
		  .keypad = true,
		  .rumble = true,
		  .cue_id = 2U,
		  .fields = EXPECT_KEYPAD | EXPECT_RUMBLE | EXPECT_CUE_ID },
		{ .frame = 740U,
		  .keypad = true,
		  .rumble = true,
		  .cue_id = 3U,
		  .rumble_remaining_ms = 33U,
		  .fields = EXPECT_KEYPAD | EXPECT_RUMBLE | EXPECT_CUE_ID |
			    EXPECT_RUMBLE_REMAINING },
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
	size_t count = (size_t)width * height;
	uint16_t *storage =
		malloc((count + 2U * GUARD_PIXELS) * sizeof(*storage));
	uint16_t *pixels;
	const uint16_t *scene_pixels;
	struct armada_scene *scene;
	struct armada_renderer *renderer;
	struct armada_storyboard storyboard = { 0 };
	char error[512];
	const char *check = "scene creation";
	uint32_t frame = 0U;
	size_t index;

	if (!storage) {
		fprintf(stderr, "showcase %ux%u: out of memory\n", width,
			height);
		return EXIT_FAILURE;
	}
	for (index = 0; index < count + 2U * GUARD_PIXELS; ++index)
		storage[index] = GUARD_VALUE;
	pixels = storage + GUARD_PIXELS;
	scene = armada_scene_create_with_renderer(
		width, height, ARMADA_RENDER_SOFTWARE, error, sizeof(error));
	if (!scene) {
		fprintf(stderr, "showcase %ux%u: %s failed\n", width, height,
			check);
		free(storage);
		return EXIT_FAILURE;
	}
	scene_pixels = armada_scene_pixels(scene);
	renderer = armada_renderer_create(width, height, pixels);
	if (!renderer) {
		fprintf(stderr,
			"showcase %ux%u: guarded renderer creation failed\n",
			width, height);
		armada_scene_destroy(scene);
		free(storage);
		return EXIT_FAILURE;
	}
	for (index = 0; index < sizeof(frames) / sizeof(frames[0]); ++index) {
		struct armada_outputs first_outputs;
		struct armada_outputs second_outputs;
		struct armada_outputs guarded_outputs;
		struct armada_frame prepared;
		uint64_t first_hash;
		uint64_t second_hash;
		struct armada_scene *fresh;
		bool rendered;

		frame = frames[index];
		memset(pixels, 0, count * sizeof(*pixels));
		check = "frame render";
		if (!armada_scene_render(scene, frame, &first_outputs))
			goto fail;
		first_hash = frame_hash(scene_pixels, count);
		armada_storyboard_update(&storyboard, frame, &prepared,
					 &guarded_outputs);
		armada_renderer_render(renderer, &prepared);
		check = "guarded detailed render";
		if (!guards_are_intact(storage, count) ||
		    !frame_has_detail(scene_pixels, count) ||
		    frame_hash(pixels, count) != first_hash)
			goto fail;
		/* The borrowed renderer surface's previous contents are not inputs. */
		memset(pixels, 0xa5, count * sizeof(*pixels));
		armada_renderer_render(renderer, &prepared);
		if (!guards_are_intact(storage, count) ||
		    frame_hash(pixels, count) != first_hash)
			goto fail;
		/* Playback, repeated frames and seeks must produce the same image/cues. */
		check = "fresh scene creation";
		fresh = armada_scene_create_with_renderer(
			width, height, ARMADA_RENDER_SOFTWARE, error,
			sizeof(error));
		if (!fresh)
			goto fail;
		rendered = armada_scene_render(fresh, frame, &second_outputs);
		second_hash = frame_hash(armada_scene_pixels(fresh), count);
		armada_scene_destroy(fresh);
		check = "repeatable render";
		if (!rendered || !guards_are_intact(storage, count) ||
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

		frame = 0U;
		check = "output cues";
		if (!output_cases_match(scene, output_expectations,
					ARRAY_SIZE(output_expectations)))
			goto fail;
		check = "first loop frame render";
		if (!armada_scene_render(scene, 0U, &first))
			goto fail;
		first_hash = frame_hash(scene_pixels, count);
		check = "wrapped loop frame render";
		if (!armada_scene_render(scene, 1140U, &wrapped))
			goto fail;
		wrapped_hash = frame_hash(scene_pixels, count);
		check = "loop wrap";
		if (first_hash != wrapped_hash ||
		    memcmp(&first, &wrapped, sizeof(first)) != 0)
			goto fail;
	}
	armada_scene_destroy(scene);
	armada_renderer_destroy(renderer);
	free(storage);
	return EXIT_SUCCESS;

fail:
	fprintf(stderr, "showcase %ux%u frame %u: %s failed\n", width, height,
		(unsigned int)frame, check);
	armada_scene_destroy(scene);
	armada_renderer_destroy(renderer);
	free(storage);
	return EXIT_FAILURE;
}

int main(int argc, char **argv)
{
	unsigned int width;
	unsigned int height;

	if (argc != 3)
		return EXIT_FAILURE;
	width = (unsigned int)strtoul(argv[1], NULL, 10);
	height = (unsigned int)strtoul(argv[2], NULL, 10);
	return render_case(width, height);
}
