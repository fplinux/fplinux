// SPDX-License-Identifier: GPL-2.0-only
#include "armada-storyboard.h"
#include "armada-math.h"

#define ARMADA_WORD_STEP 1.02f
#define ARMADA_LETTER_SCALE 1.06f
#define ARMADA_WORD_Y 0.28f
#define ARMADA_HARDWARE_FRAME (24U * ARMADA_FRAMES_PER_SECOND)
#define ARMADA_LETTER_STAGGER_FRAMES 5U
#define ARMADA_RISE_DURATION_FRAMES 36U
#define ARMADA_SINK_DURATION_FRAMES 45U
#define ARMADA_MOTION_STEPS_PER_FRAME 4U
#define ARMADA_SUBMERGED_FRAME (36U * ARMADA_FRAMES_PER_SECOND)
#define ARMADA_SINK_FRAME                                       \
	(ARMADA_SUBMERGED_FRAME - ARMADA_SINK_DURATION_FRAMES - \
	 (ARMADA_LETTERS - 1U) * ARMADA_LETTER_STAGGER_FRAMES)
#define ARMADA_RISE_RIPPLE_DURATION 84U
#define ARMADA_MORSE_START_FRAME ARMADA_HARDWARE_FRAME
#define ARMADA_MORSE_UNIT_FRAMES 3U

struct armada_morse_symbol {
	unsigned int letter;
	uint16_t cue_id;
	unsigned int elapsed;
	unsigned int duration;
};

static const char *const morse_patterns[ARMADA_LETTERS] = {
	"..-.", ".--.", ".-..", "..", "-.", "..-", "-..-",
};

static bool morse_symbol_for_frame(uint32_t frame,
				   struct armada_morse_symbol *symbol)
{
	uint32_t elapsed;
	uint16_t cue_id = 0;
	unsigned int letter;

	*symbol = (struct armada_morse_symbol){ .letter = ARMADA_LETTERS };
	if (frame < ARMADA_MORSE_START_FRAME)
		return false;
	elapsed = frame - ARMADA_MORSE_START_FRAME;
	for (letter = 0; letter < ARMADA_LETTERS; ++letter) {
		const char *pattern = morse_patterns[letter];
		unsigned int index;

		for (index = 0; pattern[index]; ++index) {
			unsigned int duration =
				(pattern[index] == '-' ? 3U : 1U) *
				ARMADA_MORSE_UNIT_FRAMES;

			if (elapsed < duration) {
				symbol->letter = letter;
				symbol->cue_id = (uint16_t)(cue_id + 1U);
				symbol->elapsed = elapsed;
				symbol->duration = duration;
				return true;
			}
			elapsed -= duration;
			++cue_id;
			if (pattern[index + 1U]) {
				if (elapsed < ARMADA_MORSE_UNIT_FRAMES)
					return false;
				elapsed -= ARMADA_MORSE_UNIT_FRAMES;
			}
		}
		if (letter + 1U < ARMADA_LETTERS) {
			unsigned int gap = 3U * ARMADA_MORSE_UNIT_FRAMES;

			if (elapsed < gap)
				return false;
			elapsed -= gap;
		}
	}
	return false;
}

static float cycle_solar_phase(uint32_t frame)
{
	return 16.0f - (float)frame * 64.0f / (float)ARMADA_DURATION_FRAMES;
}

static float cycle_daylight(float solar_altitude)
{
	return smoothstep(
		f_clamp((solar_altitude + 0.10f) / 0.72f, 0.0f, 1.0f));
}

static int cycle_lcd_level(uint32_t frame)
{
	float daylight =
		cycle_daylight(wave_sample_f(cycle_solar_phase(frame)));

	return 6 + (int)(daylight * 4.0f + 0.5f);
}

static void update_environment(struct armada_frame *frame)
{
	float solar_phase = cycle_solar_phase(frame->number);
	float solar_altitude = wave_sample_f(solar_phase);
	float lunar_altitude = -solar_altitude - 0.18f;
	float solar_horizontal = wave_sample_f(solar_phase + 16.0f);
	float sun_weight = smoothstep(
		f_clamp((solar_altitude + 0.05f) / 0.25f, 0.0f, 1.0f));
	float moon_weight = smoothstep(
		f_clamp((lunar_altitude + 0.05f) / 0.25f, 0.0f, 1.0f));
	float moon_mix = smoothstep(
		f_clamp((0.05f - solar_altitude) / 0.28f, 0.0f, 1.0f));

	frame->environment.sun_direction = vec_normalize(
		vec3(solar_horizontal * 0.58f, solar_altitude * 0.82f, 1.0f));
	frame->environment.moon_direction = vec_normalize(
		vec3(-solar_horizontal * 0.58f, lunar_altitude * 0.82f, 1.0f));
	frame->environment.sun_visibility = sun_weight;
	frame->environment.moon_visibility = moon_weight;
	frame->environment.daylight = cycle_daylight(solar_altitude);
	frame->environment.night = smoothstep(
		f_clamp((lunar_altitude + 0.08f) / 0.64f, 0.0f, 1.0f));
	frame->environment.celestial_direction = vec_normalize(
		color_mix(frame->environment.sun_direction,
			  frame->environment.moon_direction, moon_mix));
	frame->environment.light_color = color_mix(
		vec3(0.72f, 0.88f, 1.0f), vec3(0.46f, 0.68f, 1.0f), moon_mix);
	frame->environment.exposure =
		0.78f + frame->environment.daylight * 0.22f;
}

static float motion_progress(float time, float first, float last)
{
	float value = f_clamp((time - first) / (last - first), 0.0f, 1.0f);

	return value * value * value * (value * (value * 6.0f - 15.0f) + 10.0f);
}

static void letter_targets(float frame,
			   struct armada_letter_state letters[ARMADA_LETTERS])
{
	float time = (float)frame / (float)ARMADA_FRAMES_PER_SECOND;
	float curl = motion_progress(time, 5.5f, 13.0f) *
		     (1.0f - motion_progress(time, 18.0f, 24.0f));
	float turn = 128.0f * motion_progress(time, 6.0f, 23.0f);
	float curvature = curl * 64.0f / (float)ARMADA_LETTERS;
	float spacing = ARMADA_WORD_STEP + (2.15f - ARMADA_WORD_STEP) * curl;
	float plane_yaw = wave_sample_f(turn * 0.5f + 4.0f) * curl * 2.8f;
	float plane_sine = wave_sample_f(plane_yaw);
	float plane_cosine = wave_sample_f(plane_yaw + 16.0f);
	float center_y = ARMADA_WORD_Y + 0.42f + curl * 2.0f +
			 wave_sample_f(curl * 32.0f) * 0.25f;
	float center_z = 4.98f + curl * 0.32f;
	float individuality = 1.0f - curl * 0.35f;
	float path_x[ARMADA_LETTERS] = { 0 };
	float path_y[ARMADA_LETTERS] = { 0 };
	float mean_x = 0.0f;
	float mean_y = 0.0f;
	float projected_sum = 0.0f;
	float inverse_depth_sum = 0.0f;
	float center_x;
	struct armada_vec3 camera = camera_position();
	unsigned int letter;

	/* Equal-length links curl the row into a ring without exchanging letters. */
	for (letter = 1; letter < ARMADA_LETTERS; ++letter) {
		float tangent = turn + ((float)letter - 3.5f) * curvature;

		path_x[letter] = path_x[letter - 1U] +
				 spacing * wave_sample_f(tangent + 16.0f);
		path_y[letter] =
			path_y[letter - 1U] + spacing * wave_sample_f(tangent);
		mean_x += path_x[letter];
		mean_y += path_y[letter];
	}
	mean_x /= (float)ARMADA_LETTERS;
	mean_y /= (float)ARMADA_LETTERS;
	for (letter = 0; letter < ARMADA_LETTERS; ++letter) {
		struct armada_letter_state *state = &letters[letter];
		uint32_t delay = letter * ARMADA_LETTER_STAGGER_FRAMES;
		uint32_t sink_start = ARMADA_SINK_FRAME + delay;
		float rise = motion_progress(
			(float)frame, (float)delay,
			(float)(delay + ARMADA_RISE_DURATION_FRAMES));
		float sink = motion_progress(
			(float)frame, (float)sink_start,
			(float)(sink_start + ARMADA_SINK_DURATION_FRAMES));
		float afloat = rise * (1.0f - sink);
		float floating = afloat * individuality;
		float hover_phase = time * (6.4f + (float)letter * 0.31f) +
				    (float)letter * 10.7f;
		float depth_phase = time * (4.6f + (float)letter * 0.23f) +
				    (float)letter * 17.3f;
		float swell = wave_sample_f(hover_phase + 13.0f);
		float hover_y = wave_sample_f(hover_phase) *
				(0.090f + (float)(letter % 3U) * 0.012f);
		float hover_x = wave_sample_f(depth_phase + 16.0f) * 0.018f;
		float hover_z = wave_sample_f(depth_phase) *
				(0.065f + (float)(letter % 2U) * 0.012f);
		float tangent = turn + ((float)letter - 3.0f) * curvature;
		float x = path_x[letter] - mean_x;
		float y = path_y[letter] - mean_y;
		float z = y * 0.20f * curl;
		float nominal_x = x * plane_cosine + z * plane_sine;
		float nominal_z = center_z - x * plane_sine + z * plane_cosine;
		float inverse_depth = 1.0f / (nominal_z - camera.z);

		*state = (struct armada_letter_state){ 0 };
		state->position.x = nominal_x + hover_x * floating;
		state->position.y =
			-1.72f * (1.0f - afloat) +
			(center_y + y * 0.83f + hover_y * individuality) *
				afloat;
		state->position.z =
			5.28f * (1.0f - afloat) +
			(nominal_z + hover_z * individuality) * afloat;
		state->scale = ARMADA_LETTER_SCALE;
		state->scale_x =
			1.06f - afloat * 0.06f - swell * floating * 0.015f;
		state->scale_y =
			0.76f + afloat * 0.24f + swell * floating * 0.028f;
		state->roll = tangent +
			      wave_sample_f(hover_phase + 21.0f) *
				      (0.55f + (float)(letter % 2U) * 0.07f) *
				      floating;
		state->yaw =
			plane_yaw * wave_sample_f(tangent + 16.0f) +
			wave_sample_f(depth_phase + 7.0f) * 0.90f * floating;
		state->pitch =
			-plane_yaw * wave_sample_f(tangent) + curl * 1.80f +
			wave_sample_f(hover_phase + 37.0f) * 0.65f * floating;
		state->visible = frame < (float)ARMADA_SUBMERGED_FRAME;
		projected_sum += nominal_x * inverse_depth;
		inverse_depth_sum += inverse_depth;
	}
	/* Center the formation, without coupling the independent hover offsets. */
	center_x = -projected_sum / inverse_depth_sum;
	for (letter = 0; letter < ARMADA_LETTERS; ++letter)
		letters[letter].position.x += center_x;
}

static void spring_step(float *position, float *velocity, float target,
			float frequency, float damping)
{
	const float step = 1.0f / (float)(ARMADA_FRAMES_PER_SECOND *
					  ARMADA_MOTION_STEPS_PER_FRAME);
	float acceleration = frequency * frequency * (target - *position) -
			     2.0f * damping * frequency * *velocity;

	*velocity += acceleration * step;
	*position += *velocity * step;
}

static void step_letter(struct armada_letter_motion *motion,
			const struct armada_letter_state *target,
			unsigned int letter, bool signal, uint32_t tick)
{
	struct armada_letter_state *pose = &motion->pose;
	struct armada_vec3 previous = pose->position;
	struct armada_water_crossing *crossing = NULL;
	float frequency = 6.0f + (float)letter * 0.10f;
	float stretch;

	spring_step(&pose->position.x, &motion->velocity.x, target->position.x,
		    frequency, 0.48f);
	spring_step(&pose->position.y, &motion->velocity.y, target->position.y,
		    frequency, 0.48f);
	spring_step(&pose->position.z, &motion->velocity.z, target->position.z,
		    frequency, 0.48f);
	/* Lean into the actual movement; angular momentum outlives each turn. */
	spring_step(&pose->yaw, &motion->angular_velocity.x,
		    target->yaw + motion->velocity.z * 1.5f, 8.0f, 0.56f);
	spring_step(&pose->pitch, &motion->angular_velocity.y,
		    target->pitch - motion->velocity.y * 1.4f, 8.0f, 0.56f);
	/* Angles stay unwrapped here: two complete turns are 128, not zero. */
	spring_step(&pose->roll, &motion->angular_velocity.z,
		    target->roll - motion->velocity.x * 0.8f, 8.0f, 0.56f);
	stretch = f_clamp(motion->velocity.y * 0.025f, -0.10f, 0.10f);
	spring_step(&pose->scale_x, &motion->stretch_velocity_x,
		    target->scale_x - stretch * 0.5f, 12.0f, 0.60f);
	spring_step(&pose->scale_y, &motion->stretch_velocity_y,
		    target->scale_y + stretch, 12.0f, 0.60f);
	spring_step(&motion->recoil, &motion->recoil_velocity,
		    signal ? 1.0f : 0.0f, 24.0f, 0.60f);
	/* End the invisible recoil tail before it reaches subnormal floats. */
	if (!signal && f_abs(motion->recoil) < 0.00001f &&
	    f_abs(motion->recoil_velocity) < 0.0001f) {
		motion->recoil = 0.0f;
		motion->recoil_velocity = 0.0f;
	}
	pose->visible = target->visible;

	if (!motion->rise.tick && previous.y < ARMADA_WATER_Y &&
	    pose->position.y >= ARMADA_WATER_Y)
		crossing = &motion->rise;
	else if (!motion->sink.tick &&
		 tick >= ARMADA_SINK_FRAME * ARMADA_MOTION_STEPS_PER_FRAME &&
		 previous.y > ARMADA_WATER_Y &&
		 pose->position.y <= ARMADA_WATER_Y)
		crossing = &motion->sink;
	if (crossing) {
		float fraction = (ARMADA_WATER_Y - previous.y) /
				 (pose->position.y - previous.y);

		crossing->position = vec_add(
			previous,
			vec_scale(vec_sub(pose->position, previous), fraction));
		crossing->tick = tick;
	}
}

static void advance_letters(struct armada_storyboard *storyboard,
			    struct armada_frame *frame)
{
	uint32_t target_tick = frame->number * ARMADA_MOTION_STEPS_PER_FRAME;
	struct armada_letter_state targets[ARMADA_LETTERS];
	unsigned int letter;

	/* Fixed steps make seeks, skipped frames and normal playback identical. */
	if (!storyboard->motion_initialized ||
	    target_tick < storyboard->motion_tick) {
		memset(storyboard->motion, 0, sizeof(storyboard->motion));
		letter_targets(0.0f, targets);
		for (letter = 0; letter < ARMADA_LETTERS; ++letter)
			storyboard->motion[letter].pose = targets[letter];
		storyboard->motion_tick = 0U;
		storyboard->motion_initialized = true;
	}
	while (storyboard->motion_tick < target_tick) {
		struct armada_morse_symbol symbol;
		bool signal;

		++storyboard->motion_tick;
		letter_targets((float)storyboard->motion_tick /
				       (float)ARMADA_MOTION_STEPS_PER_FRAME,
			       targets);
		signal = morse_symbol_for_frame(
			storyboard->motion_tick / ARMADA_MOTION_STEPS_PER_FRAME,
			&symbol);
		for (letter = 0; letter < ARMADA_LETTERS; ++letter)
			step_letter(&storyboard->motion[letter],
				    &targets[letter], letter,
				    signal && symbol.letter == letter,
				    storyboard->motion_tick);
	}
	for (letter = 0; letter < ARMADA_LETTERS; ++letter)
		frame->letters[letter] = storyboard->motion[letter].pose;
}

static void prepare_ripples(const struct armada_storyboard *storyboard,
			    struct armada_frame *frame)
{
	bool sinking = frame->number >= ARMADA_SINK_FRAME;
	unsigned int letter;

	frame->ripple_count = 0U;
	for (letter = 0; letter < ARMADA_LETTERS; ++letter) {
		const struct armada_water_crossing *event =
			sinking ? &storyboard->motion[letter].sink :
				  &storyboard->motion[letter].rise;
		float emitted_frame = (float)event->tick /
				      (float)ARMADA_MOTION_STEPS_PER_FRAME;
		float age = (float)frame->number - emitted_frame;
		float lifetime = sinking ? (float)ARMADA_DURATION_FRAMES -
						   emitted_frame :
					   (float)ARMADA_RISE_RIPPLE_DURATION;
		struct armada_ripple *ripple;
		float attack;
		float decay;

		if (!event->tick || age < 0.0f || age >= lifetime)
			continue;
		ripple = &frame->ripples[frame->ripple_count++];
		attack = smoothstep(f_clamp(age / 6.0f, 0.0f, 1.0f));
		decay = smoothstep(
			f_clamp((lifetime - age) / (sinking ? 86.0f : 64.0f),
				0.0f, 1.0f));
		ripple->origin = event->position;
		ripple->radius = (sinking ? 0.026f : 0.023f) * age;
		ripple->amplitude =
			(sinking ? 0.050f : 0.032f) * attack * decay;
	}
}

static void apply_impact(const struct armada_storyboard *storyboard,
			 struct armada_frame *frame, unsigned int letter)
{
	struct armada_morse_symbol symbol;
	float attack;
	float release;
	float envelope;
	float recoil = storyboard->motion[letter].recoil;

	/* Apply to the rendered copy, never to the integrated physical pose. */
	frame->letters[letter].position.z -= recoil * 0.42f;
	frame->letters[letter].scale *= 1.0f + recoil * 0.13f;
	frame->letters[letter].pitch += recoil * 0.28f;

	if (!morse_symbol_for_frame(frame->number, &symbol) ||
	    symbol.letter != letter)
		return;
	if (symbol.duration == ARMADA_MORSE_UNIT_FRAMES) {
		attack = smoothstep((float)(symbol.elapsed + 1U) / 1.5f);
		release = smoothstep((float)(symbol.duration - symbol.elapsed) /
				     1.5f);
	} else {
		attack = smoothstep((float)(symbol.elapsed + 1U) / 2.0f);
		release = smoothstep((float)(symbol.duration - symbol.elapsed) /
				     2.0f);
	}
	envelope = attack * release;
	frame->letters[letter].emissive =
		f_max(frame->letters[letter].emissive, envelope);
}

static void prepare_rotations(struct armada_frame *frame)
{
	unsigned int index;

	for (index = 0; index < ARMADA_LETTERS; ++index) {
		struct armada_letter_state *letter = &frame->letters[index];

		letter->yaw_sine = wave_sample_f(letter->yaw);
		letter->yaw_cosine = wave_sample_f(letter->yaw + 16.0f);
		letter->pitch_sine = wave_sample_f(letter->pitch);
		letter->pitch_cosine = wave_sample_f(letter->pitch + 16.0f);
		letter->roll_sine = wave_sample_f(letter->roll);
		letter->roll_cosine = wave_sample_f(letter->roll + 16.0f);
	}
}

static void compute_outputs(uint32_t frame, struct armada_outputs *outputs)
{
	struct armada_morse_symbol symbol;

	outputs->keypad = false;
	outputs->rumble = false;
	outputs->cue_id = 0;
	outputs->lcd_level = cycle_lcd_level(frame);
	if (!morse_symbol_for_frame(frame, &symbol))
		return;
	outputs->keypad = true;
	outputs->rumble = true;
	outputs->cue_id = symbol.cue_id;
}

void armada_storyboard_update(struct armada_storyboard *storyboard,
			      uint32_t number, struct armada_frame *frame,
			      struct armada_outputs *outputs)
{
	unsigned int letter;

	frame->number = number % ARMADA_DURATION_FRAMES;
	compute_outputs(frame->number, outputs);
	advance_letters(storyboard, frame);
	prepare_ripples(storyboard, frame);
	for (letter = 0; letter < ARMADA_LETTERS; ++letter)
		apply_impact(storyboard, frame, letter);
	update_environment(frame);
	prepare_rotations(frame);
}
