/* SPDX-License-Identifier: GPL-2.0-only */
#include "session-vibration.h"

#include <stdio.h>

#define COUNT(pattern) (sizeof(pattern) / sizeof(*(pattern)))

static unsigned int failures;

static void check(bool condition, const char *message)
{
	if (!condition) {
		fprintf(stderr, "FAIL: %s\n", message);
		++failures;
	}
}

static struct fplinux_session_vibration_state visible(uint64_t activity_ms)
{
	return (struct fplinux_session_vibration_state){
		.eligible = true,
		.shown = true,
		.lit = true,
		.activity_ms = activity_ms,
	};
}

static bool at(struct fplinux_session_vibration *vibration,
	       const struct fplinux_session_vibration_state *state,
	       uint64_t now_ms)
{
	fplinux_session_vibration_advance(vibration, state, now_ms);
	return vibration->vibrating;
}

static uint64_t deadline(const struct fplinux_session_vibration *vibration,
			 const struct fplinux_session_vibration_state *state,
			 uint64_t now_ms)
{
	return fplinux_session_vibration_deadline(vibration, state, now_ms);
}

/* Drive the public timer contract, measuring requested on-time. */
static uint64_t run(struct fplinux_session_vibration *vibration,
		    const struct fplinux_session_vibration_state *state,
		    uint64_t from_ms, uint64_t end_ms)
{
	uint64_t now_ms = from_ms;
	uint64_t on_ms = 0;

	at(vibration, state, now_ms);
	while (now_ms < end_ms) {
		uint64_t next_ms = deadline(vibration, state, now_ms);

		if (next_ms && next_ms <= now_ms) {
			check(false, "timer deadlines must advance time");
			break;
		}
		if (!next_ms || next_ms > end_ms)
			next_ms = end_ms;
		if (vibration->vibrating)
			on_ms += next_ms - now_ms;
		now_ms = next_ms;
		at(vibration, state, now_ms);
	}
	return on_ms;
}

static void test_pattern_timeline(void)
{
	const unsigned int pattern[] = { 0, 100, 100, 300 };
	struct fplinux_session_vibration vibration;
	struct fplinux_session_vibration_state state = visible(1000);

	fplinux_session_vibration_init(&vibration, 1000);
	check(!at(&vibration, &state, 1000) &&
		      !deadline(&vibration, &state, 1000),
	      "idle policy has no timer");
	fplinux_session_vibration_play(&vibration, pattern, COUNT(pattern),
				       false, 1000);
	check(at(&vibration, &state, 1000) &&
		      deadline(&vibration, &state, 1000) == 1100,
	      "initial zero wait starts the 100 ms pulse immediately");
	check(at(&vibration, &state, 1099), "first pulse lasts until 1100");
	check(!at(&vibration, &state, 1100) &&
		      deadline(&vibration, &state, 1100) == 1200,
	      "100 ms pause separates pulses");
	check(at(&vibration, &state, 1200), "second pulse begins at 1200");
	check(at(&vibration, &state, 1499), "second pulse lasts 300 ms");
	check(!at(&vibration, &state, 1500) && !vibration.playing &&
		      !deadline(&vibration, &state, 1500),
	      "one-shot completion releases the timer");
}

static void test_repeat_replace_stop(void)
{
	const unsigned int beat[] = { 0, 200, 300 };
	const unsigned int replacement[] = { 400, 100 };
	struct fplinux_session_vibration vibration;
	struct fplinux_session_vibration_state state = visible(0);

	fplinux_session_vibration_init(&vibration, 0);
	fplinux_session_vibration_play(&vibration, beat, COUNT(beat), true, 0);
	check(at(&vibration, &state, 0), "repeat begins immediately");
	check(!at(&vibration, &state, 200), "repeat includes its final pause");
	check(at(&vibration, &state, 500), "repeat begins its next cycle");
	check(run(&vibration, &state, 500, 10000) == 3800,
	      "nineteen whole cycles contain 3800 ms on-time");
	fplinux_session_vibration_play(&vibration, replacement,
				       COUNT(replacement), false, 10100);
	check(!at(&vibration, &state, 10100) &&
		      deadline(&vibration, &state, 10100) == 10500,
	      "replacement restarts its own initial wait");
	check(at(&vibration, &state, 10500), "replacement pulse starts");
	fplinux_session_vibration_stop(&vibration);
	check(!at(&vibration, &state, 10520) && !vibration.playing &&
		      !deadline(&vibration, &state, 10520),
	      "explicit stop cancels both output and timer");
}

static void test_visibility_and_masks(void)
{
	const unsigned int pulse[] = { 0, 1000 };
	struct fplinux_session_vibration vibration;
	struct fplinux_session_vibration_state state = visible(0);

	fplinux_session_vibration_init(&vibration, 0);
	fplinux_session_vibration_play(&vibration, pulse, COUNT(pulse), false,
				       0);
	check(at(&vibration, &state, 0), "foreground application can vibrate");
	state.eligible = false;
	check(fplinux_session_vibration_advance(&vibration, &state, 200) &&
		      !vibration.vibrating && !vibration.playing,
	      "hidden or leaving application loses its live pattern");
	state.eligible = true;
	check(!at(&vibration, &state, 400), "showing never restarts a pattern");
	state.shown = false;
	fplinux_session_vibration_play(&vibration, pulse, COUNT(pulse), false,
				       10000);
	check(!at(&vibration, &state, 10000) && vibration.playing &&
		      deadline(&vibration, &state, 10000) == 11000,
	      "surface presentation masks output without stopping its clock");
	state.shown = true;
	check(at(&vibration, &state, 10200),
	      "surface presentation permits output");
	state.lit = false;
	check(!at(&vibration, &state, 10400), "dark display masks output");
	state.lit = true;
	check(at(&vibration, &state, 10600),
	      "lighting resumes the remaining pulse");
	state.battery_low = true;
	check(!at(&vibration, &state, 10800), "low battery masks output");
	state.battery_low = false;
	check(at(&vibration, &state, 10900),
	      "charging permits remaining output");
	check(!at(&vibration, &state, 11000), "masked pattern retains its end");
}

static void test_presence(void)
{
	const unsigned int pulse[] = { 0, 1000, 1000 };
	struct fplinux_session_vibration vibration;
	struct fplinux_session_vibration_state state = visible(0);

	fplinux_session_vibration_init(&vibration, 299500);
	fplinux_session_vibration_play(&vibration, pulse, COUNT(pulse), true,
				       299500);
	check(at(&vibration, &state, 299500) &&
		      deadline(&vibration, &state, 299500) == 300000,
	      "presence expiry is due before the pulse edge");
	check(!at(&vibration, &state, 300000) && vibration.playing,
	      "five minutes without activity masks output");
	state.activity_ms = 300100;
	check(at(&vibration, &state, 300100),
	      "activity resumes the remaining step");
}

static void test_duty_budget(void)
{
	const unsigned int duty[] = { 0, 1500, 500 };
	struct fplinux_session_vibration vibration;
	struct fplinux_session_vibration_state state = visible(0);

	fplinux_session_vibration_init(&vibration, 0);
	fplinux_session_vibration_play(&vibration, duty, COUNT(duty), true, 0);
	check(run(&vibration, &state, 0, 10999) == 8499 &&
		      vibration.vibrating &&
		      deadline(&vibration, &state, 10999) == 11000,
	      "full duty budget allows five pulses and 999 ms of the sixth");
	check(!at(&vibration, &state, 11000) && vibration.budget_empty,
	      "exhaustion turns output off");
	at(&vibration, &state, 11500);
	check(!at(&vibration, &state, 12000) &&
		      deadline(&vibration, &state, 12000) == 13000,
	      "resumption waits for 1000 ms of on-time to refill");
	check(!at(&vibration, &state, 12999), "output stays off before refill");
	check(at(&vibration, &state, 13000), "refilled budget resumes output");
	check(run(&vibration, &state, 13000, 289000) == 138000,
	      "sustained output uses half the long-run elapsed time");
}

static void test_gap_and_continuous_pulse_limit(void)
{
	const unsigned int short_pulse[] = { 0, 200 };
	const unsigned int next_pulse[] = { 0, 300 };
	const unsigned int long_pulse[] = { 0, 2900, 1000 };
	const unsigned int joined[] = { 0, 1000, 200, 1500 };
	struct fplinux_session_vibration vibration;
	struct fplinux_session_vibration_state state = visible(0);

	fplinux_session_vibration_init(&vibration, 0);
	fplinux_session_vibration_play(&vibration, short_pulse,
				       COUNT(short_pulse), false, 50);
	check(at(&vibration, &state, 50), "first pulse needs no preceding gap");
	check(!at(&vibration, &state, 250), "first pulse ends");
	fplinux_session_vibration_play(&vibration, next_pulse,
				       COUNT(next_pulse), false, 300);
	check(!at(&vibration, &state, 300) &&
		      deadline(&vibration, &state, 300) == 350,
	      "replacement within the 100 ms gap waits");
	check(at(&vibration, &state, 350),
	      "pending pulse starts after the gap");
	check(!at(&vibration, &state, 600),
	      "delayed pulse retains its planned end");
	fplinux_session_vibration_play(&vibration, long_pulse,
				       COUNT(long_pulse), false, 10000);
	check(at(&vibration, &state, 10000), "long pulse starts");
	fplinux_session_vibration_play(&vibration, long_pulse,
				       COUNT(long_pulse), false, 12800);
	check(at(&vibration, &state, 12800) &&
		      deadline(&vibration, &state, 12800) == 13000,
	      "replacing an on-step keeps its original pulse limit");
	check(!at(&vibration, &state, 13000),
	      "continuous pulse stops after 3000 ms");
	check(!at(&vibration, &state, 13200),
	      "pulse limit persists within on-step");
	check(!at(&vibration, &state, 15700), "off-step clears pulse limit");
	fplinux_session_vibration_play(&vibration, short_pulse,
				       COUNT(short_pulse), false, 16000);
	check(at(&vibration, &state, 16000), "subsequent pulse may run again");
	fplinux_session_vibration_init(&vibration, 0);
	fplinux_session_vibration_play(&vibration, joined, COUNT(joined), true,
				       0);
	check(run(&vibration, &state, 0, 2700) == 2500 && vibration.vibrating,
	      "zero restart wait joins the last and first pulses");
	check(at(&vibration, &state, 3699),
	      "joined pulse remains within limit");
	check(!at(&vibration, &state, 3700),
	      "joined pulse ends at the next pause");
}

int main(void)
{
	test_pattern_timeline();
	test_repeat_replace_stop();
	test_visibility_and_masks();
	test_presence();
	test_duty_budget();
	test_gap_and_continuous_pulse_limit();
	return failures ? 1 : 0;
}
