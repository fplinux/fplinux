/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * fplinux-check: package-embedded
 *
 * The decisions of the phoneME Canvas port that need neither the VM nor the
 * runtime, so host tests can check them: MIDP key codes of runtime keys,
 * the Canvas drawing area of a display mode, and vibration patterns.
 */
#ifndef FPLINUX_PM_CANVAS_CORE_H
#define FPLINUX_PM_CANVAS_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* MIDP key codes without a character (Nokia and javacall values). */
enum {
	FPLINUX_PM_KEY_UP = -1,
	FPLINUX_PM_KEY_DOWN = -2,
	FPLINUX_PM_KEY_LEFT = -3,
	FPLINUX_PM_KEY_RIGHT = -4,
	FPLINUX_PM_KEY_SELECT = -5,
	FPLINUX_PM_KEY_SOFT1 = -6,
	FPLINUX_PM_KEY_SOFT2 = -7,
	FPLINUX_PM_KEY_CLEAR = -8,
	FPLINUX_PM_KEY_SEND = -10,
};

/*
 * The MIDP key code of a runtime key (Linux KEY_* code, keyboard or phone),
 * 0 when a Canvas gets nothing for it. Phone keys: soft keys -6/-7, OK -5,
 * the D-pad -1..-4, digits '0'..'9', '*', '#' and Dial -10 (SEND). Keyboard
 * keys: Menu -6, Escape -7, Enter -5, arrows, Backspace and Delete -8 (CLEAR),
 * otherwise the typed character (unicode, from the press) as MIDP does.
 */
int fplinux_pm_key_code(bool keyboard, unsigned int code, uint32_t unicode);

/* What decides the size of a Canvas's drawing area. */
struct fplinux_pm_canvas_geometry {
	/* The runtime is connected; without it the 240x320 profile is used. */
	bool runtime;
	/* Canvas.setFullScreenMode() of the Canvas shown or being shown. */
	bool full_screen;
	/* The runtime reports the mode asked for: the surface has its size. */
	bool surface_matches;
	/* The Canvas has a title band (never in full screen). */
	bool titled;
	int surface_width, surface_height;
	/* fplinux_ui_metrics(): the whole display and the bars around it. */
	int screen_width, screen_height;
	int status_height, softkey_height, header_height;
};

/*
 * The drawing area, from the surface once it has the size of the mode asked
 * for, otherwise predicted from the display and its bars, so that a Canvas
 * knows its full-screen size at once, before the runtime's resize arrives.
 */
void fplinux_pm_canvas_size(const struct fplinux_pm_canvas_geometry *geometry,
			    int *width, int *height);

/* Longest single pulse and the pause between the pulses of a long one. */
#define FPLINUX_PM_VIBRATION_PULSE_MAX_MS 3000u
#define FPLINUX_PM_VIBRATION_PULSE_MIN_MS 50u
#define FPLINUX_PM_VIBRATION_GAP_MS 100u

/*
 * Display.vibrate(duration) as a pattern for fplinux_ui_vibration_play():
 * {0, pulse, gap, pulse, ...}, pulses of at most 3000 ms, the last one at
 * least 50 ms. Returns the number of durations written (0 for duration 0),
 * at most capacity: a longer request is cut at the pattern's end.
 */
size_t fplinux_pm_vibration_pattern(unsigned int duration_ms,
				    unsigned int *pattern, size_t capacity);

#endif
