/* SPDX-License-Identifier: GPL-2.0-only */
#define _POSIX_C_SOURCE 200809L
#include "clock-presentation.h"

#include "clock-model.h"

#include <fplinux/fplinux-ui-locale.h>
#include <libintl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define _(text) gettext(text)
#define SECONDS_PER_DAY INT64_C(86400)

static bool clear_text(char *out, size_t capacity)
{
	if (!out || !capacity)
		return false;
	out[0] = '\0';
	return true;
}

static bool copy_text(char *out, size_t capacity, const char *text)
{
	size_t length = strlen(text);

	if (length >= capacity)
		return false;
	memcpy(out, text, length + 1);
	return true;
}

static bool format_date(struct fplinux_ui *ui, int64_t utc,
			enum fplinux_ui_date_style style, char *out,
			size_t capacity)
{
	struct clock_local_time calendar;

	if (clock_model_utc_time(utc, &calendar) != CLOCK_OK)
		return false;
	return fplinux_ui_format_date(ui, (time_t)utc, style, out, capacity);
}

bool clock_presentation_time(struct fplinux_ui *ui, int64_t utc, bool seconds,
			     char *out, size_t capacity)
{
	struct clock_local_time calendar;
	size_t length;
	int written;

	if (!clear_text(out, capacity))
		return false;
	if (clock_model_utc_time(utc, &calendar) != CLOCK_OK ||
	    !fplinux_ui_format_date(ui, (time_t)utc, FPLINUX_UI_DATE_CLOCK, out,
				    capacity))
		goto failed;
	if (!seconds)
		return true;
	length = strlen(out);
	written = snprintf(out + length, capacity - length, ":%02u",
			   calendar.second);
	if (written < 0 || (size_t)written >= capacity - length)
		goto failed;
	return true;

failed:
	out[0] = '\0';
	return false;
}

static const char *relative_day_label(int64_t utc, int64_t now_utc)
{
	int64_t day_difference =
		utc / SECONDS_PER_DAY - now_utc / SECONDS_PER_DAY;

	switch (day_difference) {
	case -1:
		return _("Yesterday");
	case 0:
		return _("Today");
	case 1:
		return _("Tomorrow");
	default:
		return NULL;
	}
}

static bool format_when(struct fplinux_ui *ui, int64_t utc, int64_t now_utc,
			bool full_date, char *out, size_t capacity)
{
	struct clock_local_time calendar;
	const char *label;

	if (clock_model_utc_time(utc, &calendar) != CLOCK_OK ||
	    clock_model_utc_time(now_utc, &calendar) != CLOCK_OK)
		return false;
	label = relative_day_label(utc, now_utc);
	if (label)
		return copy_text(out, capacity, label);
	return format_date(ui, utc,
			   full_date ? FPLINUX_UI_DATE_LONG :
				       FPLINUX_UI_DATE_SHORT,
			   out, capacity);
}

void clock_presentation_when(struct fplinux_ui *ui, int64_t utc,
			     int64_t now_utc, bool full_date, char *out,
			     size_t capacity)
{
	if (!clear_text(out, capacity))
		return;
	if (!format_when(ui, utc, now_utc, full_date, out, capacity)) {
		out[0] = '\0';
		copy_text(out, capacity, _("Time unavailable"));
	}
}

bool clock_presentation_offset_name(int offset_minutes, char *out,
				    size_t capacity)
{
	int magnitude, written;

	if (!clear_text(out, capacity) ||
	    !clock_model_world_offset_valid(offset_minutes))
		return false;
	magnitude = offset_minutes < 0 ? -offset_minutes : offset_minutes;
	written = snprintf(out, capacity, "UTC%c%02d:%02d",
			   offset_minutes < 0 ? '-' : '+', magnitude / 60,
			   magnitude % 60);
	if (written < 0 || (size_t)written >= capacity) {
		out[0] = '\0';
		return false;
	}
	return true;
}

bool clock_presentation_offset(struct fplinux_ui *ui, int offset_minutes,
			       int64_t utc, char *time, size_t time_capacity,
			       char *date, size_t date_capacity, char *relative,
			       size_t relative_capacity)
{
	struct clock_local_time calendar;
	struct tm shifted_calendar;
	int64_t offset_seconds, projected_utc;
	time_t projected_time;
	const char *relative_label;
	bool outputs_valid;

	outputs_valid = clear_text(time, time_capacity);
	outputs_valid = clear_text(date, date_capacity) && outputs_valid;
	outputs_valid = clear_text(relative, relative_capacity) &&
			outputs_valid;
	if (!outputs_valid || !clock_model_world_offset_valid(offset_minutes) ||
	    clock_model_utc_time(utc, &calendar) != CLOCK_OK)
		return false;
	offset_seconds = (int64_t)offset_minutes * 60;
	if ((offset_seconds > 0 && utc > INT64_MAX - offset_seconds) ||
	    (offset_seconds < 0 && utc < INT64_MIN - offset_seconds))
		return false;
	projected_utc = utc + offset_seconds;
	projected_time = (time_t)projected_utc;
	if ((int64_t)projected_time != projected_utc ||
	    !gmtime_r(&projected_time, &shifted_calendar))
		return false;
	relative_label = relative_day_label(projected_utc, utc);
	if (!relative_label ||
	    !fplinux_ui_format_date(ui, projected_time, FPLINUX_UI_DATE_CLOCK,
				    time, time_capacity) ||
	    !fplinux_ui_format_date(ui, projected_time, FPLINUX_UI_DATE_LONG,
				    date, date_capacity) ||
	    !copy_text(relative, relative_capacity, relative_label)) {
		time[0] = '\0';
		date[0] = '\0';
		relative[0] = '\0';
		return false;
	}
	return true;
}
