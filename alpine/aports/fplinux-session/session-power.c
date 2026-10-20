/* SPDX-License-Identifier: GPL-2.0-only */
#include "session-power.h"

#include <string.h>

enum key_state {
	KEY_STATE_RELEASED,
	KEY_STATE_DELIVERED,
	KEY_STATE_CONSUMED,
};

static uint64_t off_timeout_ms(const struct fplinux_session_power *power)
{
	if (power->locked)
		return FPLINUX_SESSION_LOCKED_OFF_MS;
	return (uint64_t)power->screen_off_seconds * 1000U;
}

/* Dimming starts min(7 s, timeout / 5) before the display turns off. */
static uint64_t dim_lead_ms(const struct fplinux_session_power *power)
{
	unsigned int seconds = power->screen_off_seconds / 5;

	return (uint64_t)(seconds < 7 ? seconds : 7) * 1000U;
}

static void clear_unlock(struct fplinux_session_power *power)
{
	power->unlock_armed = false;
	power->notice = FPLINUX_SESSION_NOTICE_NONE;
}

static void turn_off(struct fplinux_session_power *power, uint64_t time_ms)
{
	power->display = FPLINUX_UI_SESSION_DISPLAY_OFF;
	power->off_ms = time_ms;
	clear_unlock(power);
}

static void wake(struct fplinux_session_power *power, uint64_t now_ms)
{
	power->display = FPLINUX_UI_SESSION_DISPLAY_ON;
	power->activity_ms = now_ms;
}

/* Activity on a lit display restarts its timers and ends dimming. */
static void touch(struct fplinux_session_power *power, uint64_t now_ms)
{
	if (power->display == FPLINUX_UI_SESSION_DISPLAY_OFF)
		return;
	power->display = FPLINUX_UI_SESSION_DISPLAY_ON;
	power->activity_ms = now_ms;
}

static void lock(struct fplinux_session_power *power)
{
	power->locked = true;
	clear_unlock(power);
}

static bool has_auto_lock(const struct fplinux_session_power *power)
{
	return !power->locked &&
	       power->auto_lock_seconds != FPLINUX_SESSION_AUTO_LOCK_OFF;
}

static uint64_t auto_lock_ms(const struct fplinux_session_power *power)
{
	return power->off_ms + (uint64_t)power->auto_lock_seconds * 1000U;
}

void fplinux_session_power_init(struct fplinux_session_power *power,
				unsigned int screen_off_seconds,
				int auto_lock_seconds, uint64_t now_ms)
{
	memset(power, 0, sizeof(*power));
	power->screen_off_seconds = screen_off_seconds;
	power->auto_lock_seconds = auto_lock_seconds;
	power->display = FPLINUX_UI_SESSION_DISPLAY_ON;
	power->activity_ms = now_ms;
}

void fplinux_session_power_configure(struct fplinux_session_power *power,
				     unsigned int screen_off_seconds,
				     int auto_lock_seconds, uint64_t now_ms)
{
	fplinux_session_power_advance(power, now_ms);
	power->screen_off_seconds = screen_off_seconds;
	power->auto_lock_seconds = auto_lock_seconds;
	touch(power, now_ms);
}

void fplinux_session_power_advance(struct fplinux_session_power *power,
				   uint64_t now_ms)
{
	if (power->unlock_armed &&
	    now_ms - power->unlock_ms >= FPLINUX_SESSION_UNLOCK_WINDOW_MS)
		clear_unlock(power);
	if (power->notice == FPLINUX_SESSION_NOTICE_UNLOCK_HELP &&
	    now_ms >= power->notice_until_ms)
		power->notice = FPLINUX_SESSION_NOTICE_NONE;
	if (power->display != FPLINUX_UI_SESSION_DISPLAY_OFF &&
	    (power->locked || !power->keep_screen_on)) {
		uint64_t off_at = power->activity_ms + off_timeout_ms(power);

		if (now_ms >= off_at)
			turn_off(power, off_at);
		else if (!power->locked &&
			 power->display == FPLINUX_UI_SESSION_DISPLAY_ON &&
			 now_ms >= off_at - dim_lead_ms(power))
			power->display = FPLINUX_UI_SESSION_DISPLAY_DIM;
	}
	if (power->display == FPLINUX_UI_SESSION_DISPLAY_OFF &&
	    has_auto_lock(power) && now_ms >= auto_lock_ms(power))
		lock(power);
}

static void consider(uint64_t *earliest, uint64_t time_ms)
{
	if (!*earliest || time_ms < *earliest)
		*earliest = time_ms;
}

uint64_t
fplinux_session_power_deadline(const struct fplinux_session_power *power)
{
	uint64_t earliest = 0;

	if (power->unlock_armed)
		consider(&earliest,
			 power->unlock_ms + FPLINUX_SESSION_UNLOCK_WINDOW_MS);
	if (power->notice == FPLINUX_SESSION_NOTICE_UNLOCK_HELP)
		consider(&earliest, power->notice_until_ms);
	if (power->display != FPLINUX_UI_SESSION_DISPLAY_OFF &&
	    (power->locked || !power->keep_screen_on)) {
		uint64_t off_at = power->activity_ms + off_timeout_ms(power);

		if (!power->locked &&
		    power->display == FPLINUX_UI_SESSION_DISPLAY_ON)
			consider(&earliest, off_at - dim_lead_ms(power));
		else
			consider(&earliest, off_at);
	}
	if (power->display == FPLINUX_UI_SESSION_DISPLAY_OFF &&
	    has_auto_lock(power))
		consider(&earliest, auto_lock_ms(power));
	return earliest;
}

/* Left soft key or OK arms the keyguard; `*` within the window unlocks. */
static void unlock_key(struct fplinux_session_power *power,
		       enum fplinux_session_key_source source,
		       unsigned int code, uint64_t now_ms)
{
	bool phone = source == FPLINUX_SESSION_KEY_PHONE;

	if (phone && (code == KEY_F13 || code == KEY_OK)) {
		power->unlock_armed = true;
		power->unlock_ms = now_ms;
		power->notice = FPLINUX_SESSION_NOTICE_UNLOCK_PRESS_STAR;
	} else if (phone && code == KEY_NUMERIC_STAR && power->unlock_armed) {
		power->locked = false;
		clear_unlock(power);
	} else {
		power->unlock_armed = false;
		power->notice = FPLINUX_SESSION_NOTICE_UNLOCK_HELP;
		power->notice_until_ms =
			now_ms + FPLINUX_SESSION_UNLOCK_HELP_MS;
	}
}

bool fplinux_session_power_key(struct fplinux_session_power *power,
			       enum fplinux_session_key_source source,
			       unsigned int code, bool pressed, uint64_t now_ms)
{
	unsigned char unknown = KEY_STATE_RELEASED;
	unsigned char *state = &unknown;
	unsigned char previous;

	fplinux_session_power_advance(power, now_ms);
	if ((unsigned int)source < FPLINUX_SESSION_KEY_SOURCE_COUNT &&
	    code < KEY_CNT)
		state = &power->keys[source][code];
	if (pressed) {
		if (power->display == FPLINUX_UI_SESSION_DISPLAY_OFF) {
			wake(power, now_ms);
			*state = KEY_STATE_CONSUMED;
			return false;
		}
		touch(power, now_ms);
		/* A repeat belongs to the gesture that its first press began. */
		if (*state == KEY_STATE_CONSUMED)
			return false;
		if (power->locked) {
			unlock_key(power, source, code, now_ms);
			*state = KEY_STATE_CONSUMED;
			return false;
		}
		*state = KEY_STATE_DELIVERED;
		return true;
	}
	previous = *state;
	*state = KEY_STATE_RELEASED;
	touch(power, now_ms);
	if (previous == KEY_STATE_CONSUMED)
		return false;
	if (previous == KEY_STATE_DELIVERED)
		return true;
	return fplinux_session_power_is_visible(power);
}

enum fplinux_session_red_action
fplinux_session_power_red_key(struct fplinux_session_power *power, bool pressed,
			      uint64_t event_ms, bool home_view,
			      uint64_t now_ms)
{
	fplinux_session_power_advance(power, now_ms);
	if (pressed) {
		power->red_pressed = true;
		power->red_pressed_ms = event_ms;
		touch(power, now_ms);
		return FPLINUX_SESSION_RED_NONE;
	}
	if (!power->red_pressed)
		return FPLINUX_SESSION_RED_NONE;
	power->red_pressed = false;
	if (event_ms < power->red_pressed_ms ||
	    event_ms - power->red_pressed_ms >= FPLINUX_SESSION_RED_HOLD_MS)
		return FPLINUX_SESSION_RED_NONE;
	if (power->display == FPLINUX_UI_SESSION_DISPLAY_OFF) {
		wake(power, now_ms);
		return FPLINUX_SESSION_RED_NONE;
	}
	touch(power, now_ms);
	if (power->locked) {
		turn_off(power, now_ms);
		return FPLINUX_SESSION_RED_NONE;
	}
	if (home_view) {
		lock(power);
		turn_off(power, now_ms);
		return FPLINUX_SESSION_RED_NONE;
	}
	return FPLINUX_SESSION_RED_HOME;
}

void fplinux_session_power_cancel_source(struct fplinux_session_power *power,
					 enum fplinux_session_key_source source)
{
	if ((unsigned int)source >= FPLINUX_SESSION_KEY_SOURCE_COUNT)
		return;
	memset(power->keys[source], KEY_STATE_RELEASED,
	       sizeof(power->keys[source]));
	if (source == FPLINUX_SESSION_KEY_PHONE)
		power->red_pressed = false;
}

bool fplinux_session_power_system_key(struct fplinux_session_power *power,
				      uint64_t now_ms)
{
	fplinux_session_power_advance(power, now_ms);
	if (power->display == FPLINUX_UI_SESSION_DISPLAY_OFF) {
		wake(power, now_ms);
		return false;
	}
	touch(power, now_ms);
	if (power->locked) {
		unlock_key(power, FPLINUX_SESSION_KEY_KEYBOARD, KEY_ESC,
			   now_ms);
		return false;
	}
	return true;
}

void fplinux_session_power_activity(struct fplinux_session_power *power,
				    bool wake_display, uint64_t now_ms)
{
	fplinux_session_power_advance(power, now_ms);
	if (power->display == FPLINUX_UI_SESSION_DISPLAY_OFF) {
		if (wake_display)
			wake(power, now_ms);
		return;
	}
	touch(power, now_ms);
}

void fplinux_session_power_set_keep_screen_on(
	struct fplinux_session_power *power, bool enabled, uint64_t now_ms)
{
	fplinux_session_power_advance(power, now_ms);
	if (power->keep_screen_on == enabled)
		return;
	power->keep_screen_on = enabled;
	/* Releasing restarts the full timeout instead of blanking at once. */
	touch(power, now_ms);
}

bool fplinux_session_power_is_visible(const struct fplinux_session_power *power)
{
	return power->display != FPLINUX_UI_SESSION_DISPLAY_OFF &&
	       !power->locked;
}
