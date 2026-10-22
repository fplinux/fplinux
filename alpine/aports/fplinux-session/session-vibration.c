/* SPDX-License-Identifier: GPL-2.0-only */
#include "session-vibration.h"

#include <string.h>

#define BUDGET_FULL (2 * (uint64_t)FPLINUX_SESSION_VIBRATION_BUDGET_MS)
#define BUDGET_RESUME (2 * (uint64_t)FPLINUX_SESSION_VIBRATION_RESUME_MS)

void fplinux_session_vibration_init(struct fplinux_session_vibration *vibration,
				    uint64_t now_ms)
{
	memset(vibration, 0, sizeof(*vibration));
	vibration->budget = BUDGET_FULL;
	vibration->budget_ms = now_ms;
}

void fplinux_session_vibration_play(struct fplinux_session_vibration *vibration,
				    const unsigned int *pattern,
				    unsigned int count, bool repeat,
				    uint64_t now_ms)
{
	unsigned int i;

	if (!count || count > FPLINUX_UI_VIBRATION_PATTERN_MAX)
		return;
	memcpy(vibration->pattern, pattern, count * sizeof(*pattern));
	vibration->length_ms = 0;
	for (i = 0; i < count; ++i)
		vibration->length_ms += pattern[i];
	vibration->playing = true;
	vibration->repeat = repeat && vibration->length_ms;
	vibration->count = count;
	vibration->start_ms = now_ms;
}

void fplinux_session_vibration_stop(struct fplinux_session_vibration *vibration)
{
	vibration->playing = false;
}

static bool current_step(const struct fplinux_session_vibration *vibration,
			 uint64_t now_ms, bool *on, uint64_t *end_ms)
{
	uint64_t at = vibration->start_ms;
	uint64_t elapsed = now_ms > at ? now_ms - at : 0;
	unsigned int i;

	if (!vibration->playing)
		return false;
	if (vibration->repeat) {
		uint64_t loops = elapsed / vibration->length_ms;

		at += loops * vibration->length_ms;
		elapsed -= loops * vibration->length_ms;
	}
	for (i = 0; i < vibration->count; ++i) {
		uint64_t duration = vibration->pattern[i];

		if (elapsed < duration) {
			*on = i % 2;
			*end_ms = at + duration;
			return true;
		}
		elapsed -= duration;
		at += duration;
	}
	return false;
}

static bool is_present(const struct fplinux_session_vibration_state *state,
		       uint64_t now_ms)
{
	return now_ms < state->activity_ms ||
	       now_ms - state->activity_ms <
		       FPLINUX_SESSION_VIBRATION_PRESENCE_MS;
}

/* On-time spends one half-ms per ms; off-time refills the same amount. */
static void spend_budget(struct fplinux_session_vibration *vibration,
			 uint64_t now_ms)
{
	uint64_t elapsed = now_ms > vibration->budget_ms ?
				   now_ms - vibration->budget_ms :
				   0;

	if (vibration->vibrating)
		vibration->budget = vibration->budget > elapsed ?
					    vibration->budget - elapsed :
					    0;
	else if (BUDGET_FULL - vibration->budget > elapsed)
		vibration->budget += elapsed;
	else
		vibration->budget = BUDGET_FULL;
	vibration->budget_ms = now_ms;
	if (!vibration->budget)
		vibration->budget_empty = true;
	else if (vibration->budget >= BUDGET_RESUME)
		vibration->budget_empty = false;
}

static bool gap_pending(const struct fplinux_session_vibration *vibration,
			uint64_t now_ms)
{
	return !vibration->vibrating && vibration->vibrated &&
	       now_ms - vibration->gap_ms < FPLINUX_SESSION_VIBRATION_GAP_MS;
}

/* Replacing a pattern cannot extend a continuous pulse past its limit. */
bool fplinux_session_vibration_advance(
	struct fplinux_session_vibration *vibration,
	const struct fplinux_session_vibration_state *state, uint64_t now_ms)
{
	bool requested = false;
	bool dropped = false;
	uint64_t end_ms;
	bool on;

	spend_budget(vibration, now_ms);
	if (!current_step(vibration, now_ms, &requested, &end_ms))
		vibration->playing = false;
	if (vibration->playing && !state->eligible) {
		vibration->playing = false;
		dropped = true;
	}
	requested = vibration->playing && requested && state->shown &&
		    state->lit && !state->battery_low &&
		    is_present(state, now_ms);
	if (!requested)
		vibration->pulse_cut = false;
	on = requested && !vibration->pulse_cut && !vibration->budget_empty &&
	     !gap_pending(vibration, now_ms);
	if (on && vibration->vibrating &&
	    now_ms - vibration->pulse_ms >=
		    FPLINUX_SESSION_VIBRATION_PULSE_MS) {
		on = false;
		vibration->pulse_cut = true;
	}
	if (on && !vibration->vibrating)
		vibration->pulse_ms = now_ms;
	if (!on && vibration->vibrating) {
		vibration->vibrated = true;
		vibration->gap_ms = now_ms;
	}
	vibration->vibrating = on;
	return dropped;
}

static void consider(uint64_t *earliest, uint64_t time_ms)
{
	if (!*earliest || time_ms < *earliest)
		*earliest = time_ms;
}

uint64_t fplinux_session_vibration_deadline(
	const struct fplinux_session_vibration *vibration,
	const struct fplinux_session_vibration_state *state, uint64_t now_ms)
{
	uint64_t earliest = 0;
	uint64_t end_ms;
	bool on;

	if (!current_step(vibration, now_ms, &on, &end_ms))
		return 0;
	consider(&earliest, end_ms);
	if (vibration->vibrating) {
		consider(&earliest, vibration->pulse_ms +
					    FPLINUX_SESSION_VIBRATION_PULSE_MS);
		consider(&earliest, now_ms + vibration->budget);
	}
	if (on && vibration->budget_empty)
		consider(&earliest, now_ms + BUDGET_RESUME - vibration->budget);
	else if (on && !vibration->pulse_cut && gap_pending(vibration, now_ms))
		consider(&earliest,
			 vibration->gap_ms + FPLINUX_SESSION_VIBRATION_GAP_MS);
	if (is_present(state, now_ms))
		consider(&earliest,
			 state->activity_ms +
				 FPLINUX_SESSION_VIBRATION_PRESENCE_MS);
	return earliest;
}
