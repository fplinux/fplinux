/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef FPLINUX_LANGUAGE_H
#define FPLINUX_LANGUAGE_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Language definitions from /usr/share/fplinux/languages/<code>: the system
 * language name and LANG, number and date formats, and the multi-tap input
 * map. Files use key=value lines with the preferences escapes. The language
 * definition is shared by the UI runtime and the session manager.
 */
#define FPLINUX_LANGUAGE_DIRECTORY "/usr/share/fplinux/languages"
#define FPLINUX_LANGUAGE_CODE_BYTES 4
#define FPLINUX_LANGUAGE_TEXT_BYTES 64
#define FPLINUX_LANGUAGE_KEYS 10

enum fplinux_language_case {
	FPLINUX_LANGUAGE_SENTENCE,
	FPLINUX_LANGUAGE_LOWER,
	FPLINUX_LANGUAGE_UPPER,
};

struct fplinux_language {
	char code[FPLINUX_LANGUAGE_CODE_BYTES];
	char name[FPLINUX_LANGUAGE_TEXT_BYTES];
	char lang[FPLINUX_LANGUAGE_TEXT_BYTES];
	bool system;
	bool input;
	char decimal;
	char date_long[FPLINUX_LANGUAGE_TEXT_BYTES];
	char date_short[FPLINUX_LANGUAGE_TEXT_BYTES];
	char weekday[7][FPLINUX_LANGUAGE_TEXT_BYTES];
	char month[12][FPLINUX_LANGUAGE_TEXT_BYTES];
	char month_genitive[12][FPLINUX_LANGUAGE_TEXT_BYTES];
	char indicator[3][FPLINUX_LANGUAGE_TEXT_BYTES];
	/* UTF-8 characters cycled by keys 0-9, in order; empty if none. */
	char multitap[FPLINUX_LANGUAGE_KEYS][FPLINUX_LANGUAGE_TEXT_BYTES];
	char xkb[FPLINUX_LANGUAGE_CODE_BYTES * 4];
};

/*
 * Loads directory/code; false with errno for a missing or invalid file. A
 * NULL directory or a code that is not [a-z]{2,3} fails with EINVAL before
 * any path is built, so a code from a message is safe to pass.
 */
bool fplinux_language_load(const char *directory, const char *code,
			   struct fplinux_language *language);
/* The built-in English definition, used when no file can be read. */
void fplinux_language_english(struct fplinux_language *language);
/* Writes up to capacity valid codes in byte order; returns how many exist. */
size_t fplinux_language_list(const char *directory,
			     char codes[][FPLINUX_LANGUAGE_CODE_BYTES],
			     size_t capacity);

#endif
