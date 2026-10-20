/* SPDX-License-Identifier: MIT */
#ifndef FPLINUX_WESTON_PRESENTATION_H
#define FPLINUX_WESTON_PRESENTATION_H

#include <stdint.h>

struct weston_output;
struct wl_listener;

/* The notification and its payload are borrowed for the listener call. */
struct fplinux_weston_presentation {
	struct weston_output *output;
	uint64_t serial;
};

/*
 * Successful presentation of a submitted repaint, with a valid timestamp.
 * Remove the listener before the output is disabled or the owner is freed.
 */
void weston_output_add_presentation_listener(struct weston_output *output,
					     struct wl_listener *listener);
/* Read from post_latch_signal to identify the repaint being submitted. */
uint64_t weston_output_get_submission_serial(struct weston_output *output);

#endif
