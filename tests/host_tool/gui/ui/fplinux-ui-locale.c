/* SPDX-License-Identifier: GPL-2.0-only */
#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <errno.h>
#include <locale.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "fplinux-language.h"
#include "fplinux-ui-locale.h"

/* 2026-08-25 14:05:09 UTC, a Tuesday, and 2026-03-01 09:07 UTC, a Sunday. */
#define TUESDAY ((time_t)1787666709)
#define SUNDAY ((time_t)1772356020)

static const char *system_code = "tt";

/* The runtime keeps the manager's language code with the Wayland code. */
const char *fplinux_ui_locale(const struct fplinux_ui *ui)
{
	(void)ui;
	return system_code;
}

/*
 * Stand-ins for the language files: "tt" is a system language with
 * Russian-style names, "nn" an input-only language, others are missing.
 */
bool fplinux_language_load(const char *directory, const char *code,
			   struct fplinux_language *language)
{
	static const char *const weekdays[] = {
		"воскресенье", "понедельник", "вторник", "среда",
		"четверг",     "пятница",     "суббота",
	};

	assert(!strcmp(directory, FPLINUX_LANGUAGE_DIRECTORY));
	memset(language, 0, sizeof(*language));
	if (!strcmp(code, "nn")) {
		strcpy(language->name, "Input only");
		language->input = true;
		return true;
	}
	if (strcmp(code, "tt")) {
		errno = ENOENT;
		return false;
	}
	strcpy(language->code, "tt");
	strcpy(language->name, "Test");
	language->system = true;
	language->decimal = ',';
	strcpy(language->date_long, "{weekday}, {day} {month.genitive}");
	strcpy(language->date_short, "{dd}.{mm}.{yyyy}");
	for (size_t i = 0; i < 7; ++i)
		strcpy(language->weekday[i], weekdays[i]);
	strcpy(language->month[2], "март");
	strcpy(language->month_genitive[2], "марта");
	strcpy(language->month[7], "август");
	strcpy(language->month_genitive[7], "августа");
	return true;
}

void fplinux_language_english(struct fplinux_language *language)
{
	memset(language, 0, sizeof(*language));
	strcpy(language->code, "en");
	language->system = true;
	language->decimal = '.';
	strcpy(language->date_long, "{weekday}, {month} {day}");
	strcpy(language->date_short, "{mm}/{dd}/{yyyy}");
	strcpy(language->weekday[0], "Sunday");
	strcpy(language->weekday[2], "Tuesday");
	strcpy(language->month[2], "March");
	strcpy(language->month[7], "August");
}

static void expect_date(time_t when, enum fplinux_ui_date_style style,
			const char *expected)
{
	char text[64];
	size_t length = strlen(expected);

	assert(fplinux_ui_format_date(NULL, when, style, text, sizeof(text)));
	assert(!strcmp(text, expected));
	/* The terminator must fit as well. */
	assert(fplinux_ui_format_date(NULL, when, style, text, length + 1));
	errno = 0;
	assert(!fplinux_ui_format_date(NULL, when, style, text, length));
	assert(errno == ERANGE);
}

static void expect_number(double value, int decimals, const char *expected)
{
	char text[32];

	assert(fplinux_ui_format_number(NULL, value, decimals, text,
					sizeof(text)));
	assert(!strcmp(text, expected));
}

/* Names start a date with a capital, also outside ASCII. */
static void test_system_language(void)
{
	char text[8];

	system_code = "tt";
	expect_date(TUESDAY, FPLINUX_UI_DATE_LONG, "Вторник, 25 августа");
	expect_date(TUESDAY, FPLINUX_UI_DATE_SHORT, "25.08.2026");
	expect_date(TUESDAY, FPLINUX_UI_DATE_CLOCK, "14:05");
	expect_date(SUNDAY, FPLINUX_UI_DATE_LONG, "Воскресенье, 1 марта");
	expect_date(SUNDAY, FPLINUX_UI_DATE_SHORT, "01.03.2026");
	expect_date(SUNDAY, FPLINUX_UI_DATE_CLOCK, "09:07");
	assert(fplinux_ui_decimal_separator(NULL) == ',');
	expect_number(3.14159, 2, "3,14");
	expect_number(1234.5, 1, "1234,5");
	expect_number(-2.25, 3, "-2,250");
	expect_number(42.0, 0, "42");
	errno = 0;
	assert(!fplinux_ui_format_number(NULL, 1.0, 10, text, sizeof(text)));
	assert(errno == EINVAL);
	errno = 0;
	assert(!fplinux_ui_format_number(NULL, 1.0, -1, text, sizeof(text)));
	assert(errno == EINVAL);
	errno = 0;
	assert(!fplinux_ui_format_number(NULL, 12345.0, 3, text, sizeof(text)));
	assert(errno == ERANGE);
	assert(fplinux_ui_format_number(NULL, 1234.0, 2, text, sizeof(text)));
	assert(!strcmp(text, "1234,00"));
}

/* A missing file or a language that is not a system one formats in English. */
static void test_english_fallback(void)
{
	static const char *const codes[] = { "nn", "de" };

	for (size_t i = 0; i < sizeof(codes) / sizeof(*codes); ++i) {
		system_code = codes[i];
		expect_date(TUESDAY, FPLINUX_UI_DATE_LONG,
			    "Tuesday, August 25");
		expect_date(TUESDAY, FPLINUX_UI_DATE_SHORT, "08/25/2026");
		assert(fplinux_ui_decimal_separator(NULL) == '.');
		expect_number(3.14159, 2, "3.14");
	}
}

/* A language change affects subsequent dates and numbers. */
static void test_language_change(void)
{
	system_code = "tt";
	expect_date(TUESDAY, FPLINUX_UI_DATE_LONG, "Вторник, 25 августа");
	expect_number(3.14159, 2, "3,14");
	system_code = "de";
	expect_date(TUESDAY, FPLINUX_UI_DATE_LONG, "Tuesday, August 25");
	expect_number(3.14159, 2, "3.14");
	system_code = "tt";
	expect_date(TUESDAY, FPLINUX_UI_DATE_LONG, "Вторник, 25 августа");
	expect_number(3.14159, 2, "3,14");
}

int main(void)
{
	int result = setenv("TZ", "UTC0", 1);

	assert(!result);
	assert(setlocale(LC_CTYPE, "C.UTF-8"));
	tzset();
	test_system_language();
	test_english_fallback();
	test_language_change();
	return 0;
}
