/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef FPLINUX_UI_VIBRATION_H
#define FPLINUX_UI_VIBRATION_H

#include <stdbool.h>
#include <stddef.h>

struct fplinux_ui;

/*
 * The session plays a pattern only for the visible foreground application.
 * Hiding or leaving stops it; showing the application does not restart it.
 * Vibration never wakes the display or delays dimming.
 *
 * Durations in milliseconds alternate between a pause and a pulse, starting
 * with a pause: {0, 200} requests a 200 ms pulse immediately. Pulses last
 * 50...3000 ms, pauses between pulses 100...60000 ms, and the initial wait
 * and final pause 0...60000 ms. The system may pause sustained repetition.
 */
#define FPLINUX_UI_VIBRATION_PATTERN_MAX 64

/* Capability and effect upload succeeded at process configuration time. */
bool fplinux_ui_vibration_supported(const struct fplinux_ui *ui);
/*
 * Replaces the pattern with 2...PATTERN_MAX durations. A repeat joins the
 * final pause and initial wait: their sum is at least 100 ms, or zero when
 * joining the last and first pulse keeps their sum at most 3000 ms.
 * Returns false with EINVAL for an invalid pattern, ENOTSUP without a
 * vibrator, EAGAIN while hidden or leaving, or a session connection error.
 * True means the request was sent; an execution error arrives as a SESSION
 * ERROR named "vibration". Neither response measures physical vibration.
 */
bool fplinux_ui_vibration_play(struct fplinux_ui *ui,
			       const unsigned int *pattern_ms, size_t count,
			       bool repeat);
/* Requests an immediate stop; its result has the same connection contract. */
bool fplinux_ui_vibration_stop(struct fplinux_ui *ui);

#endif
