/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_SESSION_POWER_H
#define FPLINUX_SESSION_POWER_H

#include <linux/input-event-codes.h>
#include <stdbool.h>
#include <stdint.h>

#include "fplinux-ui-session.h"

#define FPLINUX_SESSION_AUTO_LOCK_OFF -1
#define FPLINUX_SESSION_LOCKED_OFF_MS 10000U
#define FPLINUX_SESSION_UNLOCK_WINDOW_MS 1500U
#define FPLINUX_SESSION_UNLOCK_HELP_MS 2000U
#define FPLINUX_SESSION_RED_HOLD_MS 5000U

enum fplinux_session_key_source {
	FPLINUX_SESSION_KEY_PHONE,
	FPLINUX_SESSION_KEY_KEYBOARD,
	FPLINUX_SESSION_KEY_SOURCE_COUNT,
};

enum fplinux_session_notice {
	FPLINUX_SESSION_NOTICE_NONE,
	FPLINUX_SESSION_NOTICE_UNLOCK_PRESS_STAR,
	FPLINUX_SESSION_NOTICE_UNLOCK_HELP,
};

enum fplinux_session_red_action {
	FPLINUX_SESSION_RED_NONE,
	FPLINUX_SESSION_RED_HOME,
};

/*
 * Display, keyguard and idle policy without I/O. Every call takes the current
 * CLOCK_MONOTONIC time in milliseconds and first applies the timers that are
 * due, so callers observe the same result whether or not their timer fired.
 * The caller owns the backlight, keypad light, visibility messages and key
 * delivery; it reads the public state after each call.
 */
struct fplinux_session_power {
	unsigned int screen_off_seconds;
	int auto_lock_seconds;
	enum fplinux_ui_session_display display;
	bool locked;
	bool keep_screen_on;
	enum fplinux_session_notice notice;
	uint64_t activity_ms;
	uint64_t off_ms;
	uint64_t unlock_ms;
	uint64_t notice_until_ms;
	bool unlock_armed;
	bool red_pressed;
	uint64_t red_pressed_ms;
	/* Per key: whether its press was delivered or consumed. */
	unsigned char keys[FPLINUX_SESSION_KEY_SOURCE_COUNT][KEY_CNT];
};

void fplinux_session_power_init(struct fplinux_session_power *power,
				unsigned int screen_off_seconds,
				int auto_lock_seconds, uint64_t now_ms);
/* New timeouts count as activity. */
void fplinux_session_power_configure(struct fplinux_session_power *power,
				     unsigned int screen_off_seconds,
				     int auto_lock_seconds, uint64_t now_ms);
void fplinux_session_power_advance(struct fplinux_session_power *power,
				   uint64_t now_ms);
/* Next time at which advance changes the state, or 0 when none is pending. */
uint64_t
fplinux_session_power_deadline(const struct fplinux_session_power *power);
/*
 * A key other than the red key. Returns true when the event reaches the
 * application: a gesture started while the display is off only wakes it, and
 * nothing pressed while locked is delivered.
 */
bool fplinux_session_power_key(struct fplinux_session_power *power,
			       enum fplinux_session_key_source source,
			       unsigned int code, bool pressed,
			       uint64_t now_ms);
/*
 * The phone red key, decided on a release shorter than the kernel's power-off
 * hold. event_ms is the input event time of the press or release. home_view
 * means that no application is in the foreground and the launcher shows its
 * home view. Returns HOME when the caller must perform the Home action.
 */
enum fplinux_session_red_action
fplinux_session_power_red_key(struct fplinux_session_power *power, bool pressed,
			      uint64_t event_ms, bool home_view,
			      uint64_t now_ms);
/*
 * Input ownership was lost: forget this source's held gestures without
 * generating releases or activity. Phone input also owns the red key.
 */
void fplinux_session_power_cancel_source(struct fplinux_session_power *power,
					 enum fplinux_session_key_source source);
/*
 * A complete system-shortcut gesture such as Ctrl+Escape. Returns true when
 * it may act: the display was on and the keyguard is not engaged.
 */
bool fplinux_session_power_system_key(struct fplinux_session_power *power,
				      uint64_t now_ms);
/* Resets the idle timers; wake also lights an off display. */
void fplinux_session_power_activity(struct fplinux_session_power *power,
				    bool wake, uint64_t now_ms);
/* The caller passes whether a visible, unlocked application keeps it on. */
void fplinux_session_power_set_keep_screen_on(
	struct fplinux_session_power *power, bool enabled, uint64_t now_ms);
/* Applications can be seen: the display is on or dimmed and not locked. */
bool fplinux_session_power_is_visible(const struct fplinux_session_power *power);

#endif
