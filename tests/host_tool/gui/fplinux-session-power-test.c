/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
/*
 * Host unit test of the display, keyguard and settings policy. The clock is a
 * plain number; expected times are literal policy examples.
 */
#include "session-power.h"
#include "session-settings.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define ON FPLINUX_UI_SESSION_DISPLAY_ON
#define DIM FPLINUX_UI_SESSION_DISPLAY_DIM
#define OFF FPLINUX_UI_SESSION_DISPLAY_OFF
#define PHONE FPLINUX_SESSION_KEY_PHONE
#define KEYBOARD FPLINUX_SESSION_KEY_KEYBOARD

static unsigned int failures;
/* Language files: en, ru and ast are system languages, kk is input only. */
static char languages[] = "/tmp/fplinux-session-languages-XXXXXX";

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

static void test_setting_values(void)
{
	static const char *const valid_brightness[] = { "1", "7", "10" };
	static const char *const invalid_brightness[] = {
		"0", "11", "01", "+5", " 5", "5 ", "", "five",
	};
	static const char *const invalid_screen_off[] = { "45", "030", "0",
							  "-30" };
	static const char *const invalid_auto_lock[] = { "OFF", "-1", "10",
							 "05" };
	/* Missing, input-only, misspelt or path-like codes. */
	static const char *const invalid_locale[] = {
		"de", "kk", "EN", "e", "enus", "", "../ru", "ru/", "r1",
	};
	static const char *const invalid_theme[] = {
		"Dark", "LIGHT", "", " dark", "dark ", "blue", "0", "1",
	};
	struct fplinux_session_settings settings;
	struct fplinux_language language;
	char value[32];
	unsigned int i;

	fplinux_session_settings_defaults(&settings, languages);
	check(!settings.has_brightness && settings.screen_off_seconds == 30 &&
		      settings.auto_lock_seconds == 5 &&
		      !strcmp(settings.locale, "ru") &&
		      !strcmp(settings.input_locale, "ru") &&
		      !strcmp(settings.lang, "ru_RU.UTF-8") &&
		      settings.theme == FPLINUX_UI_SESSION_THEME_LIGHT,
	      "defaults are 30 s, auto-lock 5 s, Russian and the light theme");
	check(fplinux_session_setting_find("screen_off_seconds") ==
			      FPLINUX_SESSION_SETTING_SCREEN_OFF &&
		      fplinux_session_setting_find("time") ==
			      FPLINUX_SESSION_SETTING_TIME &&
		      fplinux_session_setting_find("theme") ==
			      FPLINUX_SESSION_SETTING_THEME &&
		      fplinux_session_setting_find("input_locale") ==
			      FPLINUX_SESSION_SETTING_INPUT_LOCALE &&
		      fplinux_session_setting_find("bogus") ==
			      FPLINUX_SESSION_SETTING_UNKNOWN &&
		      fplinux_session_setting_find("") ==
			      FPLINUX_SESSION_SETTING_UNKNOWN,
	      "setting names are exact");
	for (i = 0; i < sizeof(valid_brightness) / sizeof(*valid_brightness);
	     ++i)
		check(fplinux_session_settings_parse(
			      &settings, languages,
			      FPLINUX_SESSION_SETTING_BRIGHTNESS,
			      valid_brightness[i]),
		      "brightness 1...10 is accepted");
	check(settings.has_brightness && settings.brightness == 10,
	      "brightness is stored");
	for (i = 0;
	     i < sizeof(invalid_brightness) / sizeof(*invalid_brightness); ++i)
		check(!fplinux_session_settings_parse(
			      &settings, languages,
			      FPLINUX_SESSION_SETTING_BRIGHTNESS,
			      invalid_brightness[i]),
		      "brightness outside 1...10 or non-canonical is rejected");
	check(fplinux_session_settings_parse(&settings, languages,
					     FPLINUX_SESSION_SETTING_SCREEN_OFF,
					     "120") &&
		      settings.screen_off_seconds == 120,
	      "screen timeout 120 is accepted");
	for (i = 0;
	     i < sizeof(invalid_screen_off) / sizeof(*invalid_screen_off); ++i)
		check(!fplinux_session_settings_parse(
			      &settings, languages,
			      FPLINUX_SESSION_SETTING_SCREEN_OFF,
			      invalid_screen_off[i]),
		      "screen timeout outside the list is rejected");
	check(fplinux_session_settings_parse(&settings, languages,
					     FPLINUX_SESSION_SETTING_AUTO_LOCK,
					     "off") &&
		      settings.auto_lock_seconds ==
			      FPLINUX_SESSION_AUTO_LOCK_OFF,
	      "auto-lock off is accepted");
	check(fplinux_session_settings_format(&settings,
					      FPLINUX_SESSION_SETTING_AUTO_LOCK,
					      value, sizeof(value)) &&
		      !strcmp(value, "off"),
	      "auto-lock off is reported as off");
	check(fplinux_session_settings_parse(&settings, languages,
					     FPLINUX_SESSION_SETTING_AUTO_LOCK,
					     "0") &&
		      settings.auto_lock_seconds == 0,
	      "auto-lock 0 is accepted");
	for (i = 0; i < sizeof(invalid_auto_lock) / sizeof(*invalid_auto_lock);
	     ++i)
		check(!fplinux_session_settings_parse(
			      &settings, languages,
			      FPLINUX_SESSION_SETTING_AUTO_LOCK,
			      invalid_auto_lock[i]),
		      "auto-lock outside the list is rejected");
	check(fplinux_session_settings_parse(&settings, languages,
					     FPLINUX_SESSION_SETTING_LOCALE,
					     "ast") &&
		      !strcmp(settings.locale, "ast") &&
		      !strcmp(settings.lang, "ast_ES.UTF-8"),
	      "an installed system language is a locale with its LANG");
	check(fplinux_session_settings_parse(&settings, languages,
					     FPLINUX_SESSION_SETTING_LOCALE,
					     "en") &&
		      !strcmp(settings.lang, "en_US.UTF-8"),
	      "English is a locale with its LANG");
	check(fplinux_language_load(languages, "kk", &language) &&
		      !language.system,
	      "the input-only language file is valid");
	for (i = 0; i < sizeof(invalid_locale) / sizeof(*invalid_locale); ++i)
		check(!fplinux_session_settings_parse(
			      &settings, languages,
			      FPLINUX_SESSION_SETTING_LOCALE,
			      invalid_locale[i]),
		      "a locale needs a system language file");
	check(!strcmp(settings.locale, "en") &&
		      !strcmp(settings.lang, "en_US.UTF-8"),
	      "a rejected locale keeps the current language");
	check(!fplinux_session_settings_parse(
		      &settings, NULL, FPLINUX_SESSION_SETTING_LOCALE, "ru"),
	      "no locale can be chosen without language files");
	check(!strcmp(settings.input_locale, "ru"),
	      "changing the UI language keeps the input language");
	check(fplinux_session_settings_parse(
		      &settings, languages,
		      FPLINUX_SESSION_SETTING_INPUT_LOCALE, "kk") &&
		      !strcmp(settings.input_locale, "kk") &&
		      !strcmp(settings.locale, "en") &&
		      !strcmp(settings.lang, "en_US.UTF-8"),
	      "an input-only language changes input without changing UI or LANG");
	check(fplinux_session_settings_format(
		      &settings, FPLINUX_SESSION_SETTING_INPUT_LOCALE, value,
		      sizeof(value)) &&
		      !strcmp(value, "kk"),
	      "input language is formatted as its code");
	{
		static const char *const invalid_input[] = {
			"ast", "de",	"EN",  "e",  "enus",
			"",    "../ru", "ru/", "r1",
		};
		for (i = 0; i < sizeof(invalid_input) / sizeof(*invalid_input);
		     ++i)
			check(!fplinux_session_settings_parse(
				      &settings, languages,
				      FPLINUX_SESSION_SETTING_INPUT_LOCALE,
				      invalid_input[i]),
			      "input language needs an installed input-enabled definition");
	}
	check(!strcmp(settings.input_locale, "kk"),
	      "a rejected input language keeps the selected one");
	check(fplinux_session_settings_parse(
		      &settings, NULL, FPLINUX_SESSION_SETTING_INPUT_LOCALE,
		      "en") &&
		      !strcmp(settings.input_locale, "en"),
	      "built-in English input is available without language files");
	check(!fplinux_session_settings_parse(&settings, languages,
					      FPLINUX_SESSION_SETTING_TIME,
					      "2026-10-01 12:00"),
	      "time is not a persistent setting");
	check(fplinux_session_settings_parse(&settings, languages,
					     FPLINUX_SESSION_SETTING_THEME,
					     "dark") &&
		      settings.theme == FPLINUX_UI_SESSION_THEME_DARK &&
		      fplinux_session_settings_format(
			      &settings, FPLINUX_SESSION_SETTING_THEME, value,
			      sizeof(value)) &&
		      !strcmp(value, "dark"),
	      "the dark theme is accepted and reported as dark");
	for (i = 0; i < sizeof(invalid_theme) / sizeof(*invalid_theme); ++i)
		check(!fplinux_session_settings_parse(
			      &settings, languages,
			      FPLINUX_SESSION_SETTING_THEME, invalid_theme[i]),
		      "a theme is exactly light or dark");
	check(settings.theme == FPLINUX_UI_SESSION_THEME_DARK,
	      "a rejected theme keeps the current one");
	check(fplinux_session_settings_parse(&settings, languages,
					     FPLINUX_SESSION_SETTING_THEME,
					     "light") &&
		      settings.theme == FPLINUX_UI_SESSION_THEME_LIGHT &&
		      fplinux_session_settings_format(
			      &settings, FPLINUX_SESSION_SETTING_THEME, value,
			      sizeof(value)) &&
		      !strcmp(value, "light"),
	      "the light theme is accepted and reported as light");
}

static void test_time_values(void)
{
	static const char *const invalid[] = {
		"2026-02-29 10:00",	"1979-12-31 23:59",
		"2100-01-01 00:00",	"2026-10-01 24:00",
		"2026-10-01 12:60",	"2026-10-01T12:34",
		"2026-1-01 12:34",	"",
		"2026-00-10 10:00",	"2026-04-31 10:00",
		"2026-10-01 12:34:60",	"2026-10-01 12:34:5",
		"2026-10-01 12:34-05",	"2026-10-01 12:34:0x",
		"2026-10-01 12:34:000", "2026-04-31 10:00:00",
		"2026-10-01 12:34:",
	};
	char value[32];
	time_t seconds = 0;
	unsigned int i;

	setenv("TZ", "UTC0", 1);
	tzset();
	check(fplinux_session_time_parse("2026-10-01 12:34", &seconds) &&
		      seconds == 1790858040,
	      "local time without a zone is UTC");
	check(fplinux_session_time_parse("2024-02-29 10:00", &seconds) &&
		      seconds == 1709200800,
	      "a leap day is accepted");
	check(fplinux_session_time_parse("2026-10-01 12:34:56", &seconds) &&
		      seconds == 1790858040 + 56,
	      "seconds are accepted, so a date-only edit keeps them");
	check(fplinux_session_time_parse("2026-10-01 12:34:00", &seconds) &&
		      seconds == 1790858040,
	      "zero seconds equal the form without seconds");
	for (i = 0; i < sizeof(invalid) / sizeof(*invalid); ++i)
		check(!fplinux_session_time_parse(invalid[i], &seconds),
		      "malformed or out-of-range time is rejected");
	check(fplinux_session_time_format(1790858040, value, sizeof(value)) &&
		      !strcmp(value, "2026-10-01 12:34"),
	      "time is reported as YYYY-MM-DD HH:MM");
}

static bool file_is(const char *path, const char *expected)
{
	char text[512];
	size_t bytes;
	FILE *file = fopen(path, "r");

	if (!file)
		return false;
	bytes = fread(text, 1, sizeof(text) - 1, file);
	fclose(file);
	text[bytes] = '\0';
	return !strcmp(text, expected);
}

static void write_file(const char *path, const char *text)
{
	FILE *file = fopen(path, "w");

	if (!file || fputs(text, file) < 0 || fclose(file)) {
		fprintf(stderr, "FAIL: cannot write %s\n", path);
		exit(1);
	}
}

/* head names the language; the formats every system language needs follow. */
static void write_language(const char *directory, const char *code,
			   const char *head)
{
	char path[PATH_MAX];
	FILE *file;
	int i;

	snprintf(path, sizeof(path), "%s/%s", directory, code);
	file = fopen(path, "w");
	if (!file || fputs(head, file) < 0) {
		fprintf(stderr, "FAIL: cannot write %s\n", path);
		exit(1);
	}
	fputs("decimal=,\ndate.long={weekday}, {day} {month.genitive}\n"
	      "date.short={dd}.{mm}.{yyyy}\n",
	      file);
	for (i = 0; i < 7; ++i)
		fprintf(file, "weekday.%d=day %d\n", i, i);
	for (i = 1; i <= 12; ++i)
		fprintf(file, "month.%d=month %d\nmonth.genitive.%d=of %d\n", i,
			i, i, i);
	if (fclose(file)) {
		fprintf(stderr, "FAIL: cannot write %s\n", path);
		exit(1);
	}
}

static void make_languages(void)
{
	if (!mkdtemp(languages)) {
		fprintf(stderr, "FAIL: cannot create a languages directory\n");
		exit(1);
	}
	write_language(
		languages, "en",
		"name=English\nlang=en_US.UTF-8\nsystem=yes\ninput=yes\n"
		"input.indicator.sentence=Abc\ninput.indicator.lower=abc\n"
		"input.indicator.upper=ABC\n");
	write_language(
		languages, "ru",
		"name=Русский\nlang=ru_RU.UTF-8\nsystem=yes\ninput=yes\n"
		"input.indicator.sentence=Абв\ninput.indicator.lower=абв\n"
		"input.indicator.upper=АБВ\n");
	write_language(languages, "ast",
		       "name=Asturianu\nlang=ast_ES.UTF-8\nsystem=yes\n");
	write_language(languages, "kk",
		       "name=Қазақ\nlang=kk_KZ.UTF-8\nsystem=no\ninput=yes\n"
		       "input.indicator.sentence=Абв\n"
		       "input.indicator.lower=абв\n"
		       "input.indicator.upper=АБВ\n");
}

static void remove_languages(void)
{
	static const char *const codes[] = { "en", "ru", "ast", "kk" };
	char path[PATH_MAX];
	unsigned int i;

	for (i = 0; i < sizeof(codes) / sizeof(*codes); ++i) {
		snprintf(path, sizeof(path), "%s/%s", languages, codes[i]);
		unlink(path);
	}
	rmdir(languages);
}

static void test_default_language(void)
{
	char english_only[] = "/tmp/fplinux-session-english-XXXXXX";
	struct fplinux_session_settings settings;
	char path[PATH_MAX];

	if (!mkdtemp(english_only)) {
		fprintf(stderr, "FAIL: cannot create a languages directory\n");
		exit(1);
	}
	/* Its LANG differs from the built-in English to tell the two apart. */
	write_language(english_only, "en",
		       "name=English\nlang=en_GB.UTF-8\nsystem=yes\n");
	fplinux_session_settings_defaults(&settings, english_only);
	check(!strcmp(settings.locale, "en") &&
		      !strcmp(settings.lang, "en_GB.UTF-8") &&
		      !strcmp(settings.input_locale, "en"),
	      "without Russian the default is the English file");
	check(!fplinux_session_settings_parse(
		      &settings, english_only,
		      FPLINUX_SESSION_SETTING_INPUT_LOCALE, "en"),
	      "an installed English definition with input disabled is rejected");
	write_language(
		english_only, "ru",
		"name=Русский\nlang=ru_RU.UTF-8\nsystem=yes\ninput=no\n");
	fplinux_session_settings_defaults(&settings, english_only);
	check(!strcmp(settings.locale, "ru") &&
		      !strcmp(settings.input_locale, "en"),
	      "a UI language with input disabled defaults input to English");
	snprintf(path, sizeof(path), "%s/ru", english_only);
	unlink(path);
	snprintf(path, sizeof(path), "%s/en", english_only);
	unlink(path);
	check(fplinux_session_settings_parse(
		      &settings, english_only,
		      FPLINUX_SESSION_SETTING_INPUT_LOCALE, "en") &&
		      !strcmp(settings.input_locale, "en"),
	      "missing English input uses the built-in definition");
	rmdir(english_only);
	fplinux_session_settings_defaults(&settings, NULL);
	check(!strcmp(settings.locale, "en") &&
		      !strcmp(settings.lang, "en_US.UTF-8") &&
		      !strcmp(settings.input_locale, "en"),
	      "without language files the default is the built-in English");
}

static void test_settings_file(void)
{
	char directory[] = "/tmp/fplinux-session-settings-XXXXXX";
	char path[PATH_MAX];
	char long_line[400];
	struct fplinux_session_settings settings;
	struct fplinux_session_settings loaded;
	struct stat status;
	struct dirent *entry;
	unsigned int entries = 0;
	DIR *listing;

	if (!mkdtemp(directory)) {
		fprintf(stderr, "FAIL: cannot create a test directory\n");
		exit(1);
	}
	snprintf(path, sizeof(path), "%s/session.conf", directory);
	check(fplinux_session_settings_load(&settings, languages, path) &&
		      settings.screen_off_seconds == 30 &&
		      !strcmp(settings.locale, "ru"),
	      "a missing file selects the defaults");
	write_file(path, "locale=ast\n");
	check(fplinux_session_settings_load(&settings, languages, path) &&
		      !strcmp(settings.locale, "ast") &&
		      !strcmp(settings.lang, "ast_ES.UTF-8"),
	      "a saved system language is restored with its LANG");
	write_file(path, "locale=ast\ninput_locale=kk\n");
	check(fplinux_session_settings_load(&settings, languages, path) &&
		      !strcmp(settings.locale, "ast") &&
		      !strcmp(settings.input_locale, "kk"),
	      "saved UI and input languages are loaded independently");
	write_file(path, "input_locale=ast\n");
	check(fplinux_session_settings_load(&settings, languages, path) &&
		      !strcmp(settings.input_locale, "ru"),
	      "an invalid saved input language keeps the default");
	write_file(path, "locale=de\n");
	check(fplinux_session_settings_load(&settings, languages, path) &&
		      !strcmp(settings.locale, "ru") &&
		      !strcmp(settings.lang, "ru_RU.UTF-8"),
	      "a saved language that is no longer installed is the default");
	write_file(path, "theme=dark\n");
	check(fplinux_session_settings_load(&settings, languages, path) &&
		      settings.theme == FPLINUX_UI_SESSION_THEME_DARK,
	      "a saved dark theme is restored");
	write_file(path, "theme=blue\n");
	check(fplinux_session_settings_load(&settings, languages, path) &&
		      settings.theme == FPLINUX_UI_SESSION_THEME_LIGHT,
	      "an invalid saved theme is the default light theme");
	memset(long_line, 'x', sizeof(long_line) - 2);
	long_line[sizeof(long_line) - 2] = '\n';
	long_line[sizeof(long_line) - 1] = '\0';
	write_file(path, "screen_off_seconds=60\n"
			 "auto_lock_seconds=7\n"
			 "unknown=1\n"
			 "locale=en\n"
			 "brightness=0\n");
	{
		FILE *file = fopen(path, "a");

		if (!file || fputs(long_line, file) < 0 ||
		    fputs("locale=ru", file) < 0 || fclose(file)) {
			fprintf(stderr, "FAIL: cannot extend %s\n", path);
			exit(1);
		}
	}
	check(fplinux_session_settings_load(&settings, languages, path),
	      "a file with invalid lines is readable");
	check(settings.screen_off_seconds == 60 &&
		      settings.auto_lock_seconds == 5 &&
		      !settings.has_brightness,
	      "invalid values fall back to their defaults");
	check(!strcmp(settings.locale, "ru"),
	      "a final line without newline is read");

	fplinux_session_settings_defaults(&settings, languages);
	settings.has_brightness = true;
	settings.brightness = 4;
	settings.screen_off_seconds = 120;
	settings.auto_lock_seconds = FPLINUX_SESSION_AUTO_LOCK_OFF;
	strcpy(settings.locale, "en");
	strcpy(settings.input_locale, "kk");
	settings.theme = FPLINUX_UI_SESSION_THEME_DARK;
	umask(0077);
	check(fplinux_session_settings_save(&settings, path),
	      "settings are saved");
	check(file_is(path, "screen_off_seconds=120\n"
			    "auto_lock_seconds=off\n"
			    "locale=en\n"
			    "input_locale=kk\n"
			    "theme=dark\n"
			    "brightness=4\n"),
	      "the file holds key=value lines");
	check(stat(path, &status) == 0 && (status.st_mode & 0777) == 0644,
	      "the file is readable 0644 regardless of umask");
	check(fplinux_session_settings_load(&loaded, languages, path) &&
		      loaded.has_brightness && loaded.brightness == 4 &&
		      loaded.screen_off_seconds == 120 &&
		      loaded.auto_lock_seconds ==
			      FPLINUX_SESSION_AUTO_LOCK_OFF &&
		      !strcmp(loaded.locale, "en") &&
		      !strcmp(loaded.input_locale, "kk") &&
		      !strcmp(loaded.lang, "en_US.UTF-8") &&
		      loaded.theme == FPLINUX_UI_SESSION_THEME_DARK,
	      "saved settings load unchanged");
	listing = opendir(directory);
	while (listing && (entry = readdir(listing)))
		if (entry->d_name[0] != '.')
			++entries;
	if (listing)
		closedir(listing);
	check(entries == 1, "no temporary file remains");
	unlink(path);
	rmdir(directory);
}

int main(void)
{
	make_languages();
	test_idle_timers();
	test_wake_rule();
	test_input_loss();
	test_keyguard();
	test_red_key();
	test_keep_screen_on();
	test_activity();
	test_setting_values();
	test_time_values();
	test_default_language();
	test_settings_file();
	remove_languages();
	if (failures) {
		fprintf(stderr, "%u checks failed\n", failures);
		return 1;
	}
	puts("PASS: idle timers, wake rule, keyguard, red key, keep-screen-on, activity, setting validation, theme, time parsing, default language, settings file");
	return 0;
}
