/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_SESSION_VIBRATOR_H
#define FPLINUX_SESSION_VIBRATOR_H

#include <stdbool.h>
#include <stdint.h>

/* One descriptor owns one bounded FF_RUMBLE effect. */
struct fplinux_session_vibrator {
	int fd;
	int effect_id;
	bool on;
};

void fplinux_session_vibrator_init(struct fplinux_session_vibrator *vibrator);
/*
 * Selects a unique character device with phys fplinux/vibrator0 and
 * FF_RUMBLE, then uploads an effect with equal strong/weak magnitude.
 * Duration must be nonzero. Failure leaves the backend closed; EINVAL means
 * a null directory or zero duration, ENODEV no match, ENOTUNIQ ambiguity.
 */
bool fplinux_session_vibrator_open(struct fplinux_session_vibrator *vibrator,
				   const char *input_dir, uint16_t duration_ms,
				   uint16_t magnitude);
bool fplinux_session_vibrator_available(
	const struct fplinux_session_vibrator *vibrator);
/*
 * Writes only a state change. An execution error closes the device and
 * preserves its errno; callers must report the error before cleanup.
 */
bool fplinux_session_vibrator_set(struct fplinux_session_vibrator *vibrator,
				  bool on);
/* Stops, erases and closes even on failure; preserves the first error. */
bool fplinux_session_vibrator_close(struct fplinux_session_vibrator *vibrator);

#endif
