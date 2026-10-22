/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_SESSION_VIBRATION_H
#define FPLINUX_SESSION_VIBRATION_H

#include <stdbool.h>
#include <stdint.h>

#include "fplinux-ui-vibration.h"

#define FPLINUX_SESSION_VIBRATION_PRESENCE_MS 300000U
#define FPLINUX_SESSION_VIBRATION_PULSE_MS 3000U
#define FPLINUX_SESSION_VIBRATION_GAP_MS 100U
#define FPLINUX_SESSION_VIBRATION_BUDGET_MS 3000U
#define FPLINUX_SESSION_VIBRATION_RESUME_MS 1000U

/* Pure policy; every time is CLOCK_MONOTONIC in milliseconds. */
struct fplinux_session_vibration {
	bool playing;
	bool repeat;
	unsigned int count;
	unsigned int pattern[FPLINUX_UI_VIBRATION_PATTERN_MAX];
	uint64_t start_ms;
	uint64_t length_ms;
	/* The last requested output and the beginning of its pulse or gap. */
	bool vibrating;
	bool vibrated;
	uint64_t pulse_ms;
	uint64_t gap_ms;
	bool pulse_cut;
	/* Half milliseconds of on-time, updated at budget_ms. */
	uint64_t budget;
	uint64_t budget_ms;
	bool budget_empty;
};

struct fplinux_session_vibration_state {
	/* Connected foreground application, visible and not closing/leaving. */
	bool eligible;
	bool shown;
	/* Display on or dimmed, with no pending wake presentation. */
	bool lit;
	/* Below the low-battery threshold without a charger. */
	bool battery_low;
	uint64_t activity_ms;
};

void fplinux_session_vibration_init(struct fplinux_session_vibration *vibration,
				    uint64_t now_ms);
/* The caller validates the shared vibration pattern before replacing it. */
void fplinux_session_vibration_play(struct fplinux_session_vibration *vibration,
				    const unsigned int *pattern,
				    unsigned int count, bool repeat,
				    uint64_t now_ms);
void fplinux_session_vibration_stop(struct fplinux_session_vibration *vibration);
/* Updates vibrating; returns true if ineligibility discarded a live pattern. */
bool fplinux_session_vibration_advance(
	struct fplinux_session_vibration *vibration,
	const struct fplinux_session_vibration_state *state, uint64_t now_ms);
/* Call after advance; returns the next policy deadline, or zero. */
uint64_t fplinux_session_vibration_deadline(
	const struct fplinux_session_vibration *vibration,
	const struct fplinux_session_vibration_state *state, uint64_t now_ms);

#endif
