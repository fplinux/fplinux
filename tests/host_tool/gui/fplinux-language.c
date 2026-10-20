/* SPDX-License-Identifier: GPL-2.0-only */
#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "fplinux-language.h"

/* A complete system language; cases below remove or change one line. */
static const char system_language[] =
	"name=Testish\n"
	"lang=tt_TT.UTF-8\n"
	"system=yes\n"
	"decimal=,\n"
	"date.long={weekday} {day} {month.genitive}\n"
	"date.short={dd}-{mm}-{yyyy}\n"
	"weekday.0=w0\nweekday.1=w1\nweekday.2=w2\n"
	"weekday.3=w3\nweekday.4=w4\nweekday.5=w5\n"
	"weekday.6=w6\n"
	"month.1=m1\nmonth.2=m2\nmonth.3=m3\n"
	"month.4=m4\nmonth.5=m5\nmonth.6=m6\n"
	"month.7=m7\nmonth.8=m8\nmonth.9=m9\n"
	"month.10=m10\nmonth.11=m11\nmonth.12=m12\n"
	"month.genitive.1=g1\nmonth.genitive.2=g2\n"
	"month.genitive.3=g3\nmonth.genitive.4=g4\n"
	"month.genitive.5=g5\nmonth.genitive.6=g6\n"
	"month.genitive.7=g7\nmonth.genitive.8=g8\n"
	"month.genitive.9=g9\nmonth.genitive.10=g10\n"
	"month.genitive.11=g11\n"
	"month.genitive.12=g12\n";

static void join_path(char *path, size_t size, const char *directory,
		      const char *name)
{
	int written = snprintf(path, size, "%s/%s", directory, name);

	assert(written > 0 && (size_t)written < size);
}

static void write_file(const char *directory, const char *name,
		       const char *text)
{
	char path[PATH_MAX];
	FILE *file;
	int result;

	join_path(path, sizeof(path), directory, name);
	file = fopen(path, "w");
	assert(file);
	result = fputs(text, file);
	assert(result >= 0);
	result = fclose(file);
	assert(!result);
}

static void expect_invalid(const char *directory, const char *text)
{
	struct fplinux_language language;

	write_file(directory, "zz", text);
	errno = 0;
	assert(!fplinux_language_load(directory, "zz", &language));
	assert(errno == EINVAL);
	assert(!language.code[0] && !language.name[0]);
}

/* The Russian file that the fplinux-language aport ships. */
static void test_shipped_russian(const char *shipped)
{
	struct fplinux_language ru;

	assert(fplinux_language_load(shipped, "ru", &ru));
	assert(!strcmp(ru.code, "ru"));
	assert(!strcmp(ru.name, "Русский"));
	assert(!strcmp(ru.lang, "ru_RU.UTF-8"));
	assert(ru.system && ru.input && ru.decimal == ',');
	assert(!strcmp(ru.date_long, "{weekday}, {day} {month.genitive}"));
	assert(!strcmp(ru.date_short, "{dd}.{mm}.{yyyy}"));
	assert(!strcmp(ru.weekday[0], "воскресенье"));
	assert(!strcmp(ru.weekday[2], "вторник"));
	assert(!strcmp(ru.weekday[6], "суббота"));
	assert(!strcmp(ru.month[0], "январь"));
	assert(!strcmp(ru.month[11], "декабрь"));
	assert(!strcmp(ru.month_genitive[2], "марта"));
	assert(!strcmp(ru.month_genitive[7], "августа"));
	assert(!strcmp(ru.indicator[FPLINUX_LANGUAGE_SENTENCE], "Абв"));
	assert(!strcmp(ru.indicator[FPLINUX_LANGUAGE_LOWER], "абв"));
	assert(!strcmp(ru.indicator[FPLINUX_LANGUAGE_UPPER], "АБВ"));
	assert(!strcmp(ru.multitap[0], " 0"));
	assert(!strcmp(ru.multitap[1], ".,?!1@'\"-()/:_;+&%*=$#^"));
	assert(!strcmp(ru.multitap[2], "абвг2"));
	assert(!strcmp(ru.multitap[3], "деёжз3"));
	assert(!strcmp(ru.multitap[8], "шщъы8"));
	assert(!strcmp(ru.multitap[9], "ьэюя9"));
	assert(!strcmp(ru.xkb, "ru"));
}

static bool same_language(const struct fplinux_language *a,
			  const struct fplinux_language *b)
{
	bool same = !strcmp(a->code, b->code) && !strcmp(a->name, b->name) &&
		    !strcmp(a->lang, b->lang) && a->system == b->system &&
		    a->input == b->input && a->decimal == b->decimal &&
		    !strcmp(a->date_long, b->date_long) &&
		    !strcmp(a->date_short, b->date_short) &&
		    !strcmp(a->xkb, b->xkb);

	for (size_t i = 0; i < 7; ++i)
		same = same && !strcmp(a->weekday[i], b->weekday[i]);
	for (size_t i = 0; i < 12; ++i)
		same = same && !strcmp(a->month[i], b->month[i]) &&
		       !strcmp(a->month_genitive[i], b->month_genitive[i]);
	for (size_t i = 0; i < 3; ++i)
		same = same && !strcmp(a->indicator[i], b->indicator[i]);
	for (size_t i = 0; i < FPLINUX_LANGUAGE_KEYS; ++i)
		same = same && !strcmp(a->multitap[i], b->multitap[i]);
	return same;
}

/* Without a file the built-in English is what the shipped file says. */
static void test_english(const char *shipped)
{
	struct fplinux_language builtin;
	struct fplinux_language file;

	fplinux_language_english(&builtin);
	assert(!strcmp(builtin.code, "en"));
	assert(!strcmp(builtin.name, "English"));
	assert(!strcmp(builtin.lang, "en_US.UTF-8"));
	assert(builtin.system && builtin.input && builtin.decimal == '.');
	assert(!strcmp(builtin.date_long, "{weekday}, {month} {day}"));
	assert(!strcmp(builtin.date_short, "{mm}/{dd}/{yyyy}"));
	assert(!strcmp(builtin.weekday[2], "Tuesday"));
	assert(!strcmp(builtin.month[7], "August"));
	assert(!strcmp(builtin.month_genitive[7], "August"));
	assert(!strcmp(builtin.indicator[FPLINUX_LANGUAGE_SENTENCE], "Abc"));
	assert(!strcmp(builtin.multitap[1], ".,?!1@'\"-()/:_;+&%*=$#^"));
	assert(!strcmp(builtin.multitap[7], "pqrs7"));
	assert(!strcmp(builtin.xkb, "us"));
	assert(fplinux_language_load(shipped, "en", &file));
	assert(same_language(&builtin, &file));
}

/* Comments, unknown keys, escapes and untrimmed values. */
static void test_format(const char *scratch)
{
	struct fplinux_language language;

	write_file(scratch, "xx",
		   "# A comment line\n"
		   "\n"
		   "name=Back\\\\slash=x\n"
		   "future.key=ignored\n"
		   "input=yes\n"
		   "input.indicator.sentence=Ab\n"
		   "input.indicator.lower=ab\n"
		   "input.indicator.upper=AB\n"
		   "multitap.0= 0\n"
		   "multitap.2=ß€😀2\n"
		   "#multitap.3=commented out\n"
		   "weekday.7=out of range\n"
		   "month.01=leading zero\n");
	assert(fplinux_language_load(scratch, "xx", &language));
	assert(!strcmp(language.code, "xx"));
	assert(!strcmp(language.name, "Back\\slash=x"));
	assert(!language.system && language.input);
	assert(language.decimal == '.');
	assert(!language.lang[0] && !language.weekday[0][0]);
	assert(!strcmp(language.multitap[0], " 0"));
	assert(!strcmp(language.multitap[2], "ß€😀2"));
	assert(!language.multitap[3][0] && !language.xkb[0]);
	assert(!language.month[0][0]);
	write_file(scratch, "yy", system_language);
	assert(fplinux_language_load(scratch, "yy", &language));
	assert(language.system && !language.input && language.decimal == ',');
	assert(!strcmp(language.month_genitive[11], "g12"));
}

/* The complete system language with one line replaced is invalid. */
static void expect_variant_invalid(const char *scratch, const char *line,
				   const char *replacement)
{
	char text[sizeof(system_language) + 64];
	const char *at = strstr(system_language, line);
	size_t before;
	int written;

	assert(at);
	before = (size_t)(at - system_language);
	written = snprintf(text, sizeof(text), "%.*s%s%s", (int)before,
			   system_language, replacement, at + strlen(line));
	assert(written > 0 && (size_t)written < sizeof(text));
	expect_invalid(scratch, text);
}

static void test_invalid(const char *scratch)
{
	expect_invalid(scratch, "input=no\n");
	expect_invalid(scratch, "name=\n");
	expect_invalid(scratch, "name=a\\t\n");
	expect_invalid(scratch, "name=a\\\n");
	expect_invalid(scratch, "name=a\nname=b\n");
	expect_invalid(scratch, "name=a\nmonth.1=x\nmonth.1=y\n");
	expect_invalid(scratch, "name=a\nno separator\n");
	expect_invalid(scratch, "name=\xff\n");
	expect_invalid(scratch, "name=\xd0\n");
	expect_invalid(scratch, "name=\xc2\x85\n");
	expect_invalid(scratch, "name=a\tb\n");
	expect_invalid(scratch, "name=a\r\n");
	expect_invalid(scratch, "name=a\\nb\n");
	expect_invalid(scratch, "na me=a\n");
	expect_invalid(scratch, "name=a\nsystem=maybe\n");
	expect_invalid(scratch, "name=a\ninput=yes\n");
	expect_invalid(scratch, "name=a\nxkb=us,ru\n");
	expect_invalid(scratch, "name=a\nxkb=0123456789abcdef\n");
	expect_invalid(scratch, "name=a\ndecimal=;\n");
	expect_invalid(scratch, "name=0123456789012345678901234567890123456789"
				"012345678901234567890123\n");
	expect_variant_invalid(scratch, "month.12=m12\n", "");
	expect_variant_invalid(scratch, "weekday.3=w3\n", "weekday.3=\n");
	expect_variant_invalid(scratch, "decimal=,\n", "");
	expect_variant_invalid(scratch, "lang=tt_TT.UTF-8\n", "");
	expect_variant_invalid(scratch, "lang=tt_TT.UTF-8\n", "lang=tt TT\n");
	expect_variant_invalid(scratch,
			       "date.long={weekday} {day} {month.genitive}\n",
			       "date.long={year}\n");
	expect_variant_invalid(scratch, "date.short={dd}-{mm}-{yyyy}\n",
			       "date.short={dd}}\n");
	expect_variant_invalid(scratch, "date.short={dd}-{mm}-{yyyy}\n",
			       "date.short={dd\n");
}

static void test_codes(const char *scratch)
{
	static const char *const invalid[] = { "RU",   "russ", "r",
					       "../x", "",     "r1" };
	struct fplinux_language language;

	for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); ++i) {
		errno = 0;
		assert(!fplinux_language_load(scratch, invalid[i], &language));
		assert(errno == EINVAL);
	}
	errno = 0;
	assert(!fplinux_language_load(scratch, "qq", &language));
	assert(errno == ENOENT);
}

/* Files that load, by code in byte order; the count includes all of them. */
static void test_list(const char *scratch)
{
	char codes[3][FPLINUX_LANGUAGE_CODE_BYTES];
	char directory[PATH_MAX];
	char path[PATH_MAX];
	int result;

	join_path(directory, sizeof(directory), scratch, "list");
	result = mkdir(directory, 0700);
	assert(!result);
	write_file(directory, "ru", "name=Ru\n");
	write_file(directory, "en", "name=En\n");
	write_file(directory, "kab", "name=Kab\n");
	write_file(directory, "zz", "input=yes\n");
	write_file(directory, "toolong", "name=Long\n");
	write_file(directory, "EN", "name=Upper\n");
	join_path(path, sizeof(path), directory, "de");
	result = mkdir(path, 0700);
	assert(!result);
	assert(fplinux_language_list(directory, codes, 3) == 3);
	assert(!strcmp(codes[0], "en"));
	assert(!strcmp(codes[1], "kab"));
	assert(!strcmp(codes[2], "ru"));
	memset(codes, 0, sizeof(codes));
	assert(fplinux_language_list(directory, codes, 2) == 3);
	assert(!strcmp(codes[0], "en"));
	assert(!strcmp(codes[1], "kab"));
	assert(!codes[2][0]);
	assert(fplinux_language_list(directory, NULL, 0) == 3);
	join_path(path, sizeof(path), scratch, "missing");
	assert(fplinux_language_list(path, codes, 3) == 0);
}

int main(int argc, char **argv)
{
	assert(argc == 3);
	test_shipped_russian(argv[1]);
	test_english(argv[1]);
	test_format(argv[2]);
	test_invalid(argv[2]);
	test_codes(argv[2]);
	test_list(argv[2]);
	puts("fplinux-language: OK");
	return 0;
}
