/* SPDX-License-Identifier: GPL-2.0-only */
#define _POSIX_C_SOURCE 200809L
#include <fplinux/fplinux-ui-locale.h>

#include <time.h>

/* Keep calendar projection real while replacing the UI's language context. */
bool fplinux_ui_format_date(const struct fplinux_ui *ui, time_t when,
			    enum fplinux_ui_date_style style, char *out,
			    size_t size)
{
	struct tm calendar;
	const char *format = style == FPLINUX_UI_DATE_CLOCK ? "%H:%M" :
							      "%Y-%m-%d";
	(void)ui;

	return gmtime_r(&when, &calendar) &&
	       strftime(out, size, format, &calendar) != 0;
}
