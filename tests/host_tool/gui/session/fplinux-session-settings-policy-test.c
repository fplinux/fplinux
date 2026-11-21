/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
/*
 * Host unit test of settings, time, language and file policy.
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
		      !strcmp(settings.locale, "en") &&
		      !strcmp(settings.input_locale, "en") &&
		      !strcmp(settings.lang, "en_US.UTF-8") &&
		      settings.theme == FPLINUX_UI_SESSION_THEME_LIGHT,
	      "defaults are 30 s, auto-lock 5 s, English and the light theme");
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
	check(!strcmp(settings.input_locale, "en"),
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
	static const struct fplinux_ui_setting_time invalid[] = {
		{ 2026, 2, 29, 10, 0, 0 },  { 1979, 12, 31, 23, 59, 0 },
		{ 2100, 1, 1, 0, 0, 0 },    { 2026, 10, 1, 24, 0, 0 },
		{ 2026, 10, 1, 12, 60, 0 }, { 2026, 0, 10, 10, 0, 0 },
		{ 2026, 4, 31, 10, 0, 0 },  { 2026, 10, 1, 12, 34, 60 },
	};
	struct fplinux_ui_setting_time value = { 2026, 10, 1, 12, 34, 0 };
	time_t seconds = 0;
	unsigned int i;

	setenv("TZ", "UTC0", 1);
	tzset();
	check(fplinux_session_time_setpoint(&value, &seconds) &&
		      seconds == 1790858040,
	      "local time without a zone is UTC");
	value.second = 56;
	check(fplinux_session_time_setpoint(&value, &seconds) &&
		      seconds == 1790858040 + 56,
	      "seconds are accepted, so a date-only edit keeps them");
	value = (struct fplinux_ui_setting_time){ 2024, 2, 29, 10, 0, 0 };
	check(fplinux_session_time_setpoint(&value, &seconds) &&
		      seconds == 1709200800,
	      "a leap day is accepted");
	for (i = 0; i < sizeof(invalid) / sizeof(*invalid); ++i)
		check(!fplinux_session_time_setpoint(&invalid[i], &seconds),
		      "malformed or out-of-range time is rejected");
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
	      "the default uses the installed English definition");
	check(!fplinux_session_settings_parse(
		      &settings, english_only,
		      FPLINUX_SESSION_SETTING_INPUT_LOCALE, "en"),
	      "an installed English definition with input disabled is rejected");
	write_language(
		english_only, "ru",
		"name=Русский\nlang=ru_RU.UTF-8\nsystem=yes\ninput=no\n");
	fplinux_session_settings_defaults(&settings, english_only);
	check(!strcmp(settings.locale, "en") &&
		      !strcmp(settings.lang, "en_GB.UTF-8") &&
		      !strcmp(settings.input_locale, "en"),
	      "English precedes Russian when both are installed");
	snprintf(path, sizeof(path), "%s/en", english_only);
	unlink(path);
	fplinux_session_settings_defaults(&settings, english_only);
	check(!strcmp(settings.locale, "ru") &&
		      !strcmp(settings.input_locale, "en"),
	      "a UI language with input disabled defaults input to English");
	snprintf(path, sizeof(path), "%s/ru", english_only);
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
		      !strcmp(settings.locale, "en") &&
		      !strcmp(settings.input_locale, "en"),
	      "a missing file selects the defaults");
	write_file(path, "locale=ru\ninput_locale=ru\n");
	check(fplinux_session_settings_load(&settings, languages, path) &&
		      !strcmp(settings.locale, "ru") &&
		      !strcmp(settings.input_locale, "ru") &&
		      !strcmp(settings.lang, "ru_RU.UTF-8"),
	      "saved Russian selections override the English defaults");
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
		      !strcmp(settings.input_locale, "en"),
	      "an invalid saved input language keeps the default");
	write_file(path, "locale=de\n");
	check(fplinux_session_settings_load(&settings, languages, path) &&
		      !strcmp(settings.locale, "en") &&
		      !strcmp(settings.lang, "en_US.UTF-8"),
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
	check(fplinux_session_settings_save(&settings, path).state ==
		      FPLINUX_STORAGE_DURABLE,
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
	test_setting_values();
	test_time_values();
	test_default_language();
	test_settings_file();
	remove_languages();
	if (failures) {
		fprintf(stderr, "%u checks failed\n", failures);
		return 1;
	}
	puts("PASS: setting validation, theme, time parsing, default language, settings file");
	return 0;
}
