/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef FPLINUX_UI_LOCALE_H
#define FPLINUX_UI_LOCALE_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

struct fplinux_ui;

/*
 * Translations use GNU gettext through musl: an application marks strings
 * with gettext() and ngettext() from <libintl.h>, usually through a
 * `#define _(text) gettext(text)` macro, and installs one catalogue per
 * language as /usr/share/locale/<language>/LC_MESSAGES/<application-id>.mo.
 * English is the source text and needs no catalogue. fplinux_ui_open() sets
 * the locale from the session and binds the text domain named after the
 * application ID, so gettext() works as soon as it returns.
 *
 * musl formats dates and numbers only in the C locale, so the runtime
 * provides the language-dependent formats below. System languages are
 * defined by /usr/share/fplinux/languages/<language> files.
 */

enum fplinux_ui_date_style {
	/* "Tuesday, August 30" / "Вторник, 30 августа" */
	FPLINUX_UI_DATE_LONG,
	/* "08/30/2026" / "30.08.2026" */
	FPLINUX_UI_DATE_SHORT,
	/* "14:05", 24-hour */
	FPLINUX_UI_DATE_CLOCK,
};

/* Formats local time; false with errno ERANGE when out is too small. */
bool fplinux_ui_format_date(const struct fplinux_ui *ui, time_t when,
			    enum fplinux_ui_date_style style, char *out,
			    size_t size);

/*
 * Formats a number with the given count of fraction digits (0 to 9) and the
 * language's decimal separator; no digit grouping.
 */
bool fplinux_ui_format_number(const struct fplinux_ui *ui, double value,
			      int decimals, char *out, size_t size);
/* The decimal separator of the system language, '.' or ','. */
char fplinux_ui_decimal_separator(const struct fplinux_ui *ui);

#endif
