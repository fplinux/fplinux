/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_SESSION_CLOCK_H
#define FPLINUX_SESSION_CLOCK_H

#include "clock-model.h"
#include "clock-wire.h"

struct fplinux_ui_prefs;
struct fplinux_ui_session_clock_presentation;

/* The session is the sole writer and retains data while the app is closed. */
struct fplinux_session_clock {
	struct clock_model model;
	/* Retains publication state until the owner's cleanup. */
	struct fplinux_ui_prefs *preferences;
	struct fplinux_storage_result storage;
	uint64_t retry_at_ms;
	unsigned int retry_delay_ms;
};

struct clock_result
fplinux_session_clock_init(struct fplinux_session_clock *clock,
			   int64_t wall_utc, uint64_t mono_ms,
			   const char *preferences_directory);
/* A new init requires cleanup of any previous successful init. */
void fplinux_session_clock_cleanup(struct fplinux_session_clock *clock);
struct clock_result
fplinux_session_clock_tick(struct fplinux_session_clock *clock,
			   int64_t wall_utc, uint64_t mono_ms);
struct clock_result
fplinux_session_clock_apply(struct fplinux_session_clock *clock,
			    const struct clock_request *request,
			    int64_t wall_utc, uint64_t mono_ms);
/* Explicit save retries the latest accepted configuration immediately. */
struct clock_result
fplinux_session_clock_retry(struct fplinux_session_clock *clock,
			    uint64_t mono_ms);
/* Monotonic deadline, or zero for idle. A running stopwatch needs no wakeup. */
uint64_t
fplinux_session_clock_deadline(const struct fplinux_session_clock *clock,
			       uint64_t mono_ms);
/* Absolute UTC deadline; the session arms its separate realtime timerfd. */
int64_t
fplinux_session_clock_alarm_deadline(const struct fplinux_session_clock *clock);
void fplinux_session_clock_presentation(
	const struct fplinux_session_clock *clock,
	struct fplinux_ui_session_clock_presentation *presentation);
bool fplinux_session_clock_ringing(const struct fplinux_session_clock *clock);

#endif
