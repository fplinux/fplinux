/* SPDX-License-Identifier: GPL-2.0-only */
#define _POSIX_C_SOURCE 200809L
#include "settings-core.h"
#include "session-settings.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))

static void test_brightness(void)
{
	static const char *const invalid[] = {
		"0", "11", "", "-1", "5x", " 5", "+5", "999999999999999999999"
	};
	struct fplinux_session_settings settings = { 0 };
	unsigned int level = 4;
	char value[4];

	for (unsigned int offered = FPLINUX_SETTINGS_BRIGHTNESS_MIN;
	     offered <= FPLINUX_SETTINGS_BRIGHTNESS_MAX; offered++) {
		snprintf(value, sizeof(value), "%u", offered);
		assert(fplinux_session_settings_parse(
			&settings, NULL, FPLINUX_SESSION_SETTING_BRIGHTNESS,
			value));
		assert(settings.brightness == offered);
	}

	assert(fplinux_settings_parse_brightness("1", &level) && level == 1);
	assert(fplinux_settings_parse_brightness("10", &level) && level == 10);
	for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
		assert(!fplinux_settings_parse_brightness(invalid[i], &level));
		assert(level == 10);
		assert(!fplinux_session_settings_parse(
			&settings, NULL, FPLINUX_SESSION_SETTING_BRIGHTNESS,
			invalid[i]));
	}
	assert(!fplinux_session_settings_parse(
		&settings, NULL, FPLINUX_SESSION_SETTING_BRIGHTNESS, "01"));
}

static void test_offered_choices_reach_manager(void)
{
	static const char *const screen_off[] = { "15", "30", "60", "120",
						  "300" };
	static const char *const auto_lock[] = { "off", "0",  "5",
						 "30",	"60", "300" };
	static const char *const invalid_screen_off[] = { "off", "0",	"5",
							  "17",	 "015", "+15",
							  "15 ", " 15", "" };
	static const char *const invalid_auto_lock[] = {
		"OFF", "-1", "15", "7", "00", "05", "+5", "5 ", " 5", ""
	};
	struct fplinux_session_settings settings = { 0 };
	char value[16];

	assert(FPLINUX_SETTINGS_SCREEN_OFF_COUNT == ARRAY_SIZE(screen_off));
	for (size_t i = 0; i < ARRAY_SIZE(screen_off); i++) {
		assert(!strcmp(fplinux_settings_screen_off_values[i],
			       screen_off[i]));
		assert(fplinux_session_settings_parse(
			&settings, NULL, FPLINUX_SESSION_SETTING_SCREEN_OFF,
			fplinux_settings_screen_off_values[i]));
		assert(fplinux_session_settings_format(
			&settings, FPLINUX_SESSION_SETTING_SCREEN_OFF, value,
			sizeof(value)));
		assert(!strcmp(value, screen_off[i]));
	}
	assert(FPLINUX_SETTINGS_AUTO_LOCK_COUNT == ARRAY_SIZE(auto_lock));
	for (size_t i = 0; i < ARRAY_SIZE(auto_lock); i++) {
		assert(!strcmp(fplinux_settings_auto_lock_values[i],
			       auto_lock[i]));
		assert(fplinux_session_settings_parse(
			&settings, NULL, FPLINUX_SESSION_SETTING_AUTO_LOCK,
			fplinux_settings_auto_lock_values[i]));
		assert(fplinux_session_settings_format(
			&settings, FPLINUX_SESSION_SETTING_AUTO_LOCK, value,
			sizeof(value)));
		assert(!strcmp(value, auto_lock[i]));
	}
	for (size_t i = 0; i < ARRAY_SIZE(invalid_screen_off); i++) {
		assert(!fplinux_session_settings_parse(
			&settings, NULL, FPLINUX_SESSION_SETTING_SCREEN_OFF,
			invalid_screen_off[i]));
		assert(settings.screen_off_seconds == 300);
	}
	for (size_t i = 0; i < ARRAY_SIZE(invalid_auto_lock); i++) {
		assert(!fplinux_session_settings_parse(
			&settings, NULL, FPLINUX_SESSION_SETTING_AUTO_LOCK,
			invalid_auto_lock[i]));
		assert(settings.auto_lock_seconds == 300);
	}
}

static void test_time_format(void)
{
	struct tm date = { .tm_year = 126, .tm_mon = 7, .tm_mday = 30 };
	struct tm clock = { .tm_hour = 7, .tm_min = 5, .tm_sec = 9 };
	char value[20];

	assert(fplinux_settings_time_value(&date, &clock, value,
					   sizeof(value)));
	assert(!strcmp(value, "2026-08-30 07:05:09"));
	assert(!fplinux_settings_time_value(&date, &clock, value, 19));
	clock.tm_sec = 60;
	assert(fplinux_settings_time_value(&date, &clock, value,
					   sizeof(value)));
	assert(!strcmp(value, "2026-08-30 07:05:59"));
	clock.tm_sec = -1;
	assert(!fplinux_settings_time_value(&date, &clock, value,
					    sizeof(value)));
	clock.tm_sec = 0;
	clock.tm_hour = 24;
	assert(!fplinux_settings_time_value(&date, &clock, value,
					    sizeof(value)));
	clock.tm_hour = 0;
	date.tm_mon = 12;
	assert(!fplinux_settings_time_value(&date, &clock, value,
					    sizeof(value)));
}

static void test_save_uses_live_clock(void)
{
	/* Form opened at 2026-10-02 00:13:56 UTC; Save is 210 seconds later. */
	const time_t now = 1790900246;
	const time_t next_day = 1790986436;
	const time_t seven = 1790924733;
	char value[20];

	assert(fplinux_settings_save_time(now, NULL, NULL, value,
					  sizeof(value)));
	assert(!strcmp(value, "2026-10-02 00:17:26"));
	assert(fplinux_settings_save_time(now, &next_day, NULL, value,
					  sizeof(value)));
	assert(!strcmp(value, "2026-10-03 00:17:26"));
	assert(fplinux_settings_save_time(now, NULL, &seven, value,
					  sizeof(value)));
	assert(!strcmp(value, "2026-10-02 07:05:00"));
	assert(fplinux_settings_save_time(now, &next_day, &seven, value,
					  sizeof(value)));
	assert(!strcmp(value, "2026-10-03 07:05:00"));
	assert(!fplinux_settings_save_time(now, NULL, NULL, value, 19));
	assert(fplinux_settings_time_in_range("1980-01-01 00:00:00"));
	assert(fplinux_settings_time_in_range("2099-12-31 23:59:59"));
	assert(!fplinux_settings_time_in_range("1979-12-31 23:59:59"));
	assert(!fplinux_settings_time_in_range("2100-01-01 00:00:00"));
	assert(!fplinux_settings_time_in_range("19x0-01-01 00:00:00"));
	assert(!fplinux_settings_time_in_range(""));
}

static void test_offered_years_reach_manager(void)
{
	static const char *const invalid[] = {
		"1979-12-31 23:59:59", "2100-01-01 00:00:00",
		"2026-04-31 12:00:00", "2025-02-29 12:00:00",
		"2026-01-01 24:00:00", "2026-01-01 12:60:00",
		"2026-01-01 12:00:60", "2026-1-01 12:00:00",
	};
	struct tm date = { .tm_mon = 0, .tm_mday = 1 };
	struct tm clock = { .tm_hour = 12 };
	time_t seconds;
	char value[20];

	for (int year = FPLINUX_SETTINGS_YEAR_MIN;
	     year <= FPLINUX_SETTINGS_YEAR_MAX; year++) {
		date.tm_year = year - 1900;
		assert(fplinux_settings_time_value(&date, &clock, value,
						   sizeof(value)));
		assert(fplinux_settings_time_in_range(value));
		assert(fplinux_session_time_parse(value, &seconds));
	}
	assert(fplinux_session_time_parse("1980-01-01 00:00:00", &seconds));
	assert(seconds == 315532800);
	assert(fplinux_session_time_parse("2099-12-31 23:59:59", &seconds));
	assert(seconds == 4102444799LL);
	for (size_t i = 0; i < ARRAY_SIZE(invalid); i++)
		assert(!fplinux_session_time_parse(invalid[i], &seconds));
}

static void test_detail_and_choices(void)
{
	static const char *const choices[] = { "first", "second" };
	char detail[48];

	assert(fplinux_settings_choice_find(choices, 2, "second") == 1);
	assert(fplinux_settings_choice_find(choices, 2, "missing") == 2);
	assert(fplinux_settings_date_time_detail("03.10.2026", "14:05", detail,
						 17));
	assert(!strcmp(detail, "03.10.2026 14:05"));
	assert(!fplinux_settings_date_time_detail("03.10.2026", "14:05", detail,
						  16));
	assert(fplinux_settings_date_time_detail("", "14:05", detail,
						 sizeof(detail)));
	assert(!strcmp(detail, "14:05"));
	assert(fplinux_settings_date_time_detail("03.10.2026", "", detail,
						 sizeof(detail)));
	assert(!strcmp(detail, "03.10.2026"));
}

static void test_os_release(const char *path)
{
	char value[64];

	assert(fplinux_settings_read_os_name(path, value, sizeof(value)));
	assert(!strcmp(value, "FPLinux 1"));
	assert(fplinux_settings_read_os_field(path, "BUILD_ID", value,
					      sizeof(value)));
	assert(!strcmp(value, "012345abcdef"));
	assert(!fplinux_settings_read_os_field(path, "BUILD_ID", value, 12));
	assert(!fplinux_settings_read_os_field(path, "UNKNOWN", value,
					       sizeof(value)));
}

int main(int argc, char **argv)
{
	assert(argc == 2);
	assert(!setenv("TZ", "UTC0", 1));
	tzset();
	test_brightness();
	test_offered_choices_reach_manager();
	test_time_format();
	test_save_uses_live_clock();
	test_offered_years_reach_manager();
	test_detail_and_choices();
	test_os_release(argv[1]);
	puts("settings-core: all checks passed");
	return 0;
}
