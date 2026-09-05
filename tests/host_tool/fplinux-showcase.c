// SPDX-License-Identifier: GPL-2.0-only
/* Host harness for the production ARMADA renderer and compiled timeline. */

#include "../../alpine/aports/fplinux-showcase/armada-scene.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define GUARD_PIXELS 16U
#define GUARD_VALUE 0xa55aU

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

static int render_case(unsigned int width, unsigned int height)
{
	static const uint32_t frames[] = {
		0U,   42U,   43U,   44U,   45U,	  45U,	 58U,	42U,
		120U, 210U,  390U,  570U,  660U,  720U,	 720U,	726U,
		930U, 1020U, 1051U, 1031U, 1032U, 1033U, 1080U, 1139U,
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
		struct armada_outputs quiet;
		struct armada_outputs pulse;
		struct armada_outputs gap;
		struct armada_outputs second_dot;
		struct armada_outputs dash_end;
		struct armada_outputs letter_start;
		struct armada_outputs final_signal;
		struct armada_outputs after_morse;
		struct armada_outputs first_noon;
		struct armada_outputs sunset;
		struct armada_outputs midnight;
		struct armada_outputs sunrise;
		struct armada_outputs next_noon;
		struct armada_outputs first;
		struct armada_outputs wrapped;
		uint64_t first_hash;
		uint64_t wrapped_hash;

		armada_scene_render(scene, 600U, &metrics, &quiet);
		armada_scene_render(scene, 720U, &metrics, &pulse);
		armada_scene_render(scene, 723U, &metrics, &gap);
		armada_scene_render(scene, 726U, &metrics, &second_dot);
		armada_scene_render(scene, 740U, &metrics, &dash_end);
		armada_scene_render(scene, 756U, &metrics, &letter_start);
		armada_scene_render(scene, 938U, &metrics, &final_signal);
		armada_scene_render(scene, 939U, &metrics, &after_morse);
		armada_scene_render(scene, 0U, &metrics, &first_noon);
		armada_scene_render(scene, 285U, &metrics, &sunset);
		armada_scene_render(scene, 570U, &metrics, &midnight);
		armada_scene_render(scene, 855U, &metrics, &sunrise);
		armada_scene_render(scene, 1139U, &metrics, &next_noon);
		if (quiet.keypad || quiet.rumble || quiet.lcd_level != 6 ||
		    !pulse.keypad || !pulse.rumble || pulse.cue_id != 1U ||
		    pulse.lcd_level != 6 || gap.keypad || gap.rumble ||
		    gap.cue_id != 0U || gap.lcd_level != 6 ||
		    !second_dot.keypad || !second_dot.rumble ||
		    second_dot.cue_id != 2U || !dash_end.keypad ||
		    !dash_end.rumble || dash_end.cue_id != 3U ||
		    !letter_start.keypad || !letter_start.rumble ||
		    letter_start.cue_id != 5U || !final_signal.keypad ||
		    !final_signal.rumble || final_signal.cue_id != 23U ||
		    after_morse.keypad || after_morse.rumble ||
		    after_morse.cue_id != 0U || first_noon.lcd_level != 10 ||
		    sunset.lcd_level != 6 || midnight.lcd_level != 6 ||
		    sunrise.lcd_level != 6 || next_noon.lcd_level != 10)
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
