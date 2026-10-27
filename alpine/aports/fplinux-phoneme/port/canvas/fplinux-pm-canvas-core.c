// SPDX-License-Identifier: GPL-2.0-only
/*
 * fplinux-check: package-embedded
 *
 * fplinux-phoneme Canvas decisions without VM or runtime state; see
 * fplinux-pm-canvas-core.h. Host tests compile this file on its own.
 */
#include "fplinux-pm-canvas-core.h"

#include <linux/input-event-codes.h>

/* The headless runner models the 240x320 profile: bars 20 + 32, band 28. */
enum {
	HEADLESS_WIDTH = 240,
	HEADLESS_HEIGHT = 320,
	HEADLESS_BARS = 20 + 32,
	HEADLESS_HEADER = 28,
};

static int phone_key_code(unsigned int code)
{
	switch (code) {
	case KEY_F13:
		return FPLINUX_PM_KEY_SOFT1;
	case KEY_F14:
		return FPLINUX_PM_KEY_SOFT2;
	case KEY_OK:
	case KEY_ENTER:
	case KEY_KPENTER:
		return FPLINUX_PM_KEY_SELECT;
	case KEY_UP:
		return FPLINUX_PM_KEY_UP;
	case KEY_DOWN:
		return FPLINUX_PM_KEY_DOWN;
	case KEY_LEFT:
		return FPLINUX_PM_KEY_LEFT;
	case KEY_RIGHT:
		return FPLINUX_PM_KEY_RIGHT;
	case KEY_NUMERIC_STAR:
		return '*';
	case KEY_NUMERIC_POUND:
		return '#';
	case KEY_PICKUP_PHONE:
		return FPLINUX_PM_KEY_SEND;
	default:
		if (code >= KEY_NUMERIC_0 && code <= KEY_NUMERIC_9)
			return '0' + (int)(code - KEY_NUMERIC_0);
		return 0;
	}
}

static int keyboard_key_code(unsigned int code, uint32_t unicode)
{
	switch (code) {
	case KEY_COMPOSE:
		return FPLINUX_PM_KEY_SOFT1;
	case KEY_ESC:
		return FPLINUX_PM_KEY_SOFT2;
	case KEY_ENTER:
	case KEY_KPENTER:
		return FPLINUX_PM_KEY_SELECT;
	case KEY_UP:
		return FPLINUX_PM_KEY_UP;
	case KEY_DOWN:
		return FPLINUX_PM_KEY_DOWN;
	case KEY_LEFT:
		return FPLINUX_PM_KEY_LEFT;
	case KEY_RIGHT:
		return FPLINUX_PM_KEY_RIGHT;
	case KEY_BACKSPACE:
	case KEY_DELETE:
		return FPLINUX_PM_KEY_CLEAR;
	default:
		/* Tab and printable BMP characters; MIDP codes stop below 0xFFFF. */
		if (unicode == '\t' ||
		    (unicode >= ' ' && unicode != 0x7f && unicode < 0xffff))
			return (int)unicode;
		return 0;
	}
}

int fplinux_pm_key_code(bool keyboard, unsigned int code, uint32_t unicode)
{
	return keyboard ? keyboard_key_code(code, unicode) :
			  phone_key_code(code);
}

void fplinux_pm_canvas_size(const struct fplinux_pm_canvas_geometry *geometry,
			    int *width, int *height)
{
	bool full = geometry->full_screen;
	int band = geometry->titled && !full ? geometry->header_height : 0;
	int w;
	int h;

	if (!geometry->runtime) {
		w = HEADLESS_WIDTH;
		h = HEADLESS_HEIGHT -
		    (full ? 0 :
			    HEADLESS_BARS +
				     (geometry->titled ? HEADLESS_HEADER : 0));
	} else if (geometry->surface_matches) {
		w = geometry->surface_width;
		h = geometry->surface_height - band;
	} else {
		w = geometry->screen_width;
		h = geometry->screen_height - band -
		    (full ? 0 :
			    geometry->status_height + geometry->softkey_height);
	}
	*width = w > 0 ? w : 1;
	*height = h > 0 ? h : 1;
}

size_t fplinux_pm_vibration_pattern(unsigned int duration_ms,
				    unsigned int *pattern, size_t capacity)
{
	/* {0, pulse, gap, pulse, ...}: k pulses take 2k durations. */
	size_t most = capacity / 2;
	size_t pulses;
	size_t count = 0;
	unsigned int share;
	unsigned int extra;

	if (!duration_ms || !most)
		return 0;
	if (duration_ms < FPLINUX_PM_VIBRATION_PULSE_MIN_MS)
		duration_ms = FPLINUX_PM_VIBRATION_PULSE_MIN_MS;
	pulses = (duration_ms + FPLINUX_PM_VIBRATION_PULSE_MAX_MS - 1) /
		 FPLINUX_PM_VIBRATION_PULSE_MAX_MS;
	if (pulses > most) {
		pulses = most;
		share = FPLINUX_PM_VIBRATION_PULSE_MAX_MS;
		extra = 0;
	} else {
		/* Equal pulses: none falls below the minimum. */
		share = duration_ms / (unsigned int)pulses;
		extra = duration_ms % (unsigned int)pulses;
	}
	pattern[count++] = 0;
	for (size_t i = 0; i < pulses; ++i) {
		if (i)
			pattern[count++] = FPLINUX_PM_VIBRATION_GAP_MS;
		pattern[count++] = share + (i < extra ? 1u : 0u);
	}
	return count;
}
