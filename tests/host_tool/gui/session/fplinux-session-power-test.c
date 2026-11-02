/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
/*
 * Host unit test of the display and keyguard policy. The clock is a plain
 * number; expected times are literal policy examples.
 */
#include "session-power.h"

#include <stdio.h>

#define ON FPLINUX_UI_SESSION_DISPLAY_ON
#define DIM FPLINUX_UI_SESSION_DISPLAY_DIM
#define OFF FPLINUX_UI_SESSION_DISPLAY_OFF
#define PHONE FPLINUX_SESSION_KEY_PHONE
#define KEYBOARD FPLINUX_SESSION_KEY_KEYBOARD

static unsigned int failures;
/* Language files: en, ru and ast are system languages, kk is input only. */
static void check(bool condition, const char *message)
{
	if (!condition) {
		fprintf(stderr, "FAIL: %s\n", message);
		++failures;
	}
}

static void at(struct fplinux_session_power *power, uint64_t time_ms)
{
	fplinux_session_power_advance(power, time_ms);
}

static bool tap(struct fplinux_session_power *power,
		enum fplinux_session_key_source source, unsigned int code,
		uint64_t time_ms)
{
	bool pressed =
		fplinux_session_power_key(power, source, code, true, time_ms);
	bool released =
		fplinux_session_power_key(power, source, code, false, time_ms);

	return pressed && released;
}

static void test_idle_timers(void)
{
	struct fplinux_session_power power;

	fplinux_session_power_init(&power, 30, 5, 1000);
	check(fplinux_session_power_deadline(&power) == 25000,
	      "30 s timeout schedules dimming at 24 s");
	at(&power, 24999);
	check(power.display == ON, "display stays on before 24 s");
	at(&power, 25000);
	check(power.display == DIM, "display dims 6 s before 30 s");
	at(&power, 30999);
	check(power.display == DIM, "dimmed display stays lit until 30 s");
	at(&power, 31000);
	check(power.display == OFF && !power.locked,
	      "display turns off at 30 s");
	check(fplinux_session_power_deadline(&power) == 36000,
	      "auto-lock is scheduled 5 s after off");
	at(&power, 35999);
	check(!power.locked, "grace period keeps the phone unlocked");
	at(&power, 36000);
	check(power.locked && power.display == OFF,
	      "auto-lock engages after the grace period");

	fplinux_session_power_init(&power, 15, 5, 0);
	at(&power, 12000);
	check(power.display == DIM, "15 s timeout dims at 12 s");
	fplinux_session_power_init(&power, 60, 5, 0);
	at(&power, 52999);
	check(power.display == ON, "60 s timeout stays on before 53 s");
	at(&power, 53000);
	check(power.display == DIM, "dim lead is capped at 7 s");
	fplinux_session_power_init(&power, 300, 5, 0);
	at(&power, 293000);
	check(power.display == DIM, "300 s timeout dims at 293 s");

	fplinux_session_power_init(&power, 30, 0, 0);
	at(&power, 30000);
	check(power.locked, "auto-lock 0 locks when the display turns off");
	fplinux_session_power_init(&power, 30, FPLINUX_SESSION_AUTO_LOCK_OFF,
				   0);
	at(&power, 1000000);
	check(power.display == OFF && !power.locked &&
		      !fplinux_session_power_deadline(&power),
	      "auto-lock off never locks");
}

static void test_wake_rule(void)
{
	struct fplinux_session_power power;
	uint64_t t;

	fplinux_session_power_init(&power, 30, 5, 0);
	check(fplinux_session_power_key(&power, PHONE, KEY_NUMERIC_5, true,
					1000),
	      "a key on a lit display is delivered");
	at(&power, 33000);
	check(power.display == OFF, "held key does not keep the display on");
	check(fplinux_session_power_key(&power, PHONE, KEY_NUMERIC_5, false,
					33000),
	      "release of a delivered press is delivered while off");
	check(power.display == OFF, "that release does not wake the display");
	check(!power.locked, "auto-lock is still in its grace period");

	check(!fplinux_session_power_key(&power, PHONE, KEY_NUMERIC_2, true,
					 34000),
	      "a press while off is consumed");
	check(power.display == ON && !power.locked,
	      "a key in the grace period wakes without locking");
	check(!fplinux_session_power_key(&power, PHONE, KEY_NUMERIC_2, true,
					 34100),
	      "a repeated press of the waking key is consumed");
	check(!fplinux_session_power_key(&power, PHONE, KEY_NUMERIC_2, false,
					 34200),
	      "the release of the waking key is consumed");
	check(tap(&power, PHONE, KEY_NUMERIC_2, 34300),
	      "the next gesture of that key is delivered");

	t = 34300;
	at(&power, t + 24000);
	check(power.display == DIM, "display dims again after activity");
	check(fplinux_session_power_key(&power, KEYBOARD, KEY_A, true,
					t + 24500),
	      "a keyboard key while dimmed is delivered");
	check(power.display == ON, "a key while dimmed undims");
	check(fplinux_session_power_key(&power, KEYBOARD, KEY_A, false,
					t + 24600),
	      "its release is delivered");
	at(&power, t + 24600 + 30000);
	check(power.display == OFF, "display turns off");
	check(!fplinux_session_power_key(&power, KEYBOARD, KEY_A, true,
					 t + 24600 + 30001) &&
		      !fplinux_session_power_key(&power, KEYBOARD, KEY_A, false,
						 t + 24600 + 30002),
	      "an external keyboard gesture while off only wakes");
	check(power.display == ON, "an external keyboard key wakes");
}

static void test_input_loss(void)
{
	static const enum fplinux_session_key_source sources[] = {
		PHONE,
		KEYBOARD,
	};
	struct fplinux_session_power power;

	for (size_t i = 0; i < 2; ++i) {
		enum fplinux_session_key_source source = sources[i];
		enum fplinux_session_key_source other = sources[1 - i];

		fplinux_session_power_init(&power, 30, 5, 0);
		at(&power, 30000);
		check(!fplinux_session_power_key(&power, source, KEY_A, true,
						 30001),
		      "the source has a held wake-only press");
		at(&power, 60001);
		check(!fplinux_session_power_key(&power, other, KEY_A, true,
						 60002),
		      "the other source has an independent wake-only press");
		fplinux_session_power_cancel_source(&power, source);
		check(power.activity_ms == 60002 && power.display == ON &&
			      !power.locked &&
			      fplinux_session_power_deadline(&power) == 84002,
		      "input loss leaves display, keyguard and idle deadline unchanged");
		check(tap(&power, source, KEY_A, 60003),
		      "the returned source delivers a new press without the lost release");
		check(!fplinux_session_power_key(&power, other, KEY_A, true,
						 60004) &&
			      !fplinux_session_power_key(&power, other, KEY_A,
							 false, 60005),
		      "cancelling one source preserves the other's consumed gesture");
		check(tap(&power, other, KEY_A, 60006),
		      "the other source delivers its next independent gesture");

		fplinux_session_power_init(&power, 30, 5, 0);
		check(fplinux_session_power_key(&power, other, KEY_A, true, 1),
		      "the other source has a delivered held press");
		fplinux_session_power_cancel_source(&power, source);
		at(&power, 30001);
		check(fplinux_session_power_key(&power, other, KEY_A, false,
						30002),
		      "the other source still releases its delivered press while off");
	}
}

static void lock_now(struct fplinux_session_power *power, uint64_t time_ms)
{
	check(fplinux_session_power_red_key(power, true, time_ms, true,
					    time_ms) ==
			      FPLINUX_SESSION_RED_NONE &&
		      fplinux_session_power_red_key(power, false, time_ms + 100,
						    true, time_ms + 100) ==
			      FPLINUX_SESSION_RED_NONE,
	      "red key on the home view is handled by the keyguard");
}

static void test_keyguard(void)
{
	struct fplinux_session_power power;
	uint64_t t = 1000;

	fplinux_session_power_init(&power, 30, 5, 0);
	lock_now(&power, t);
	check(power.locked && power.display == OFF,
	      "red key on the home view locks and turns the display off");
	check(!fplinux_session_power_is_visible(&power),
	      "applications are hidden under the lock");
	t = 5000;
	check(!tap(&power, PHONE, KEY_OK, t), "the waking key is consumed");
	check(power.display == ON && power.locked,
	      "a key wakes the lock screen");
	check(power.notice == FPLINUX_SESSION_NOTICE_NONE,
	      "the waking key is not an unlock step");
	check(!fplinux_session_power_is_visible(&power),
	      "a lit lock screen still hides applications");
	check(fplinux_session_power_deadline(&power) == t + 10000,
	      "the lock screen turns off after 10 s");
	at(&power, t + 9999);
	check(power.display == ON, "lock screen has no dim phase");
	at(&power, t + 10000);
	check(power.display == OFF, "lock screen turns off after 10 s");

	t = 20000;
	tap(&power, PHONE, KEY_NUMERIC_1, t);
	check(!tap(&power, PHONE, KEY_NUMERIC_5, t + 100),
	      "keys never reach the application while locked");
	check(power.notice == FPLINUX_SESSION_NOTICE_UNLOCK_HELP,
	      "another key shows the unlock help");
	at(&power, t + 100 + 1999);
	check(power.notice == FPLINUX_SESSION_NOTICE_UNLOCK_HELP,
	      "unlock help stays for 2 s");
	at(&power, t + 100 + 2000);
	check(power.notice == FPLINUX_SESSION_NOTICE_NONE,
	      "unlock help ends after 2 s");

	t = 23000;
	check(!tap(&power, PHONE, KEY_F13, t), "left soft key is consumed");
	check(power.notice == FPLINUX_SESSION_NOTICE_UNLOCK_PRESS_STAR,
	      "left soft key asks for star");
	at(&power, t + 1500);
	check(power.locked && power.notice == FPLINUX_SESSION_NOTICE_NONE,
	      "the star prompt ends after 1500 ms");
	check(!tap(&power, PHONE, KEY_NUMERIC_STAR, t + 1500),
	      "a late star is consumed");
	check(power.locked &&
		      power.notice == FPLINUX_SESSION_NOTICE_UNLOCK_HELP,
	      "a late star does not unlock");

	t = 26000;
	tap(&power, PHONE, KEY_F13, t);
	check(!tap(&power, KEYBOARD, KEY_KPASTERISK, t + 100) && power.locked,
	      "an external keyboard star does not unlock");
	tap(&power, PHONE, KEY_OK, t + 200);
	check(!fplinux_session_power_key(&power, PHONE, KEY_NUMERIC_STAR, true,
					 t + 1699),
	      "the unlocking star is consumed");
	check(!power.locked && power.notice == FPLINUX_SESSION_NOTICE_NONE &&
		      power.display == ON,
	      "OK then star within 1500 ms unlocks");
	check(!fplinux_session_power_key(&power, PHONE, KEY_NUMERIC_STAR, false,
					 t + 1750),
	      "the release of the unlocking star is consumed");
	check(fplinux_session_power_is_visible(&power),
	      "applications are visible after unlocking");
	check(tap(&power, PHONE, KEY_NUMERIC_STAR, t + 1800),
	      "keys reach the application after unlocking");
	at(&power, t + 1800 + 24000);
	check(power.display == DIM, "normal timers resume after unlocking");
}

static void test_red_key(void)
{
	struct fplinux_session_power power;

	fplinux_session_power_init(&power, 30, 5, 0);
	check(fplinux_session_power_red_key(&power, true, 100, false, 100) ==
			      FPLINUX_SESSION_RED_NONE &&
		      fplinux_session_power_red_key(&power, false, 4999 + 100,
						    false, 5099) ==
			      FPLINUX_SESSION_RED_HOME,
	      "short red key in an application is Home");
	check(!power.locked && power.display == ON,
	      "red key Home neither locks nor blanks");
	fplinux_session_power_red_key(&power, true, 6000, false, 6000);
	check(fplinux_session_power_red_key(&power, false, 11000, false,
					    11000) ==
			      FPLINUX_SESSION_RED_NONE &&
		      power.display == ON,
	      "a 5 s hold is left to the kernel power-off");
	check(fplinux_session_power_red_key(&power, false, 11100, false,
					    11100) == FPLINUX_SESSION_RED_NONE,
	      "a release without a press does nothing");
	fplinux_session_power_red_key(&power, true, 12000, true, 12000);
	fplinux_session_power_cancel_source(&power, PHONE);
	check(fplinux_session_power_red_key(&power, false, 12100, true,
					    12100) ==
			      FPLINUX_SESSION_RED_NONE &&
		      !power.locked,
	      "a cancelled press does not act on release");
	fplinux_session_power_red_key(&power, true, 12200, false, 12200);
	fplinux_session_power_cancel_source(&power, KEYBOARD);
	check(fplinux_session_power_red_key(&power, false, 12300, false,
					    12300) == FPLINUX_SESSION_RED_HOME,
	      "losing an external keyboard preserves the phone red-key gesture");

	lock_now(&power, 13000);
	check(power.locked && power.display == OFF, "red key locks on home");
	lock_now(&power, 14000);
	check(power.locked && power.display == ON,
	      "red key while off wakes the lock screen");
	lock_now(&power, 15000);
	check(power.locked && power.display == OFF,
	      "red key on the lit lock screen turns it off");

	fplinux_session_power_init(&power, 30, FPLINUX_SESSION_AUTO_LOCK_OFF,
				   0);
	at(&power, 30000);
	lock_now(&power, 31000);
	check(power.display == ON && !power.locked,
	      "red key while off wakes an unlocked phone");
}

static void test_keep_screen_on(void)
{
	struct fplinux_session_power power;

	fplinux_session_power_init(&power, 30, 5, 0);
	at(&power, 25000);
	check(power.display == DIM, "display dims");
	fplinux_session_power_set_keep_screen_on(&power, true, 26000);
	check(power.display == ON, "keep-screen-on undims");
	check(!fplinux_session_power_deadline(&power),
	      "keep-screen-on cancels the idle timers");
	at(&power, 1000000);
	check(power.display == ON && !power.locked,
	      "keep-screen-on prevents dim, off and auto-lock");
	fplinux_session_power_set_keep_screen_on(&power, false, 1000000);
	at(&power, 1000000 + 23999);
	check(power.display == ON, "releasing restarts the full timeout");
	at(&power, 1000000 + 30000);
	check(power.display == OFF, "the display turns off after release");

	fplinux_session_power_init(&power, 30, 5, 0);
	fplinux_session_power_set_keep_screen_on(&power, true, 0);
	lock_now(&power, 1000);
	check(power.locked && power.display == OFF,
	      "a manual red-key lock works despite keep-screen-on");
	fplinux_session_power_set_keep_screen_on(&power, false, 1100);
	tap(&power, PHONE, KEY_NUMERIC_1, 2000);
	at(&power, 12000);
	check(power.display == OFF, "the lock screen timeout still applies");
}

static void test_activity(void)
{
	struct fplinux_session_power power;

	fplinux_session_power_init(&power, 30, 5, 0);
	fplinux_session_power_activity(&power, false, 20000);
	at(&power, 20000 + 23999);
	check(power.display == ON, "application activity restarts the timers");
	at(&power, 20000 + 30000);
	fplinux_session_power_activity(&power, false, 50001);
	check(power.display == OFF, "application activity does not wake");
	fplinux_session_power_activity(&power, true, 50002);
	check(power.display == ON && !power.locked,
	      "charger activity wakes the display");
	check(fplinux_session_power_system_key(&power, 50003),
	      "Ctrl+Escape acts on a lit unlocked display");
	at(&power, 50003 + 30000);
	check(!fplinux_session_power_system_key(&power, 50003 + 30001) &&
		      power.display == ON,
	      "Ctrl+Escape while off only wakes");
	fplinux_session_power_configure(&power, 15, 5, 90000);
	at(&power, 90000 + 12000);
	check(power.display == DIM, "a new timeout applies from the change");
}

int main(void)
{
	test_idle_timers();
	test_wake_rule();
	test_input_loss();
	test_keyguard();
	test_red_key();
	test_keep_screen_on();
	test_activity();
	if (failures) {
		fprintf(stderr, "%u checks failed\n", failures);
		return 1;
	}
	puts("PASS: idle timers, wake rule, keyguard, red key, keep-screen-on, activity");
	return 0;
}
