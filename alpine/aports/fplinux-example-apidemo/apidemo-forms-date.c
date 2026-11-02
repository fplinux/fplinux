/* SPDX-License-Identifier: 0BSD */
#include "apidemo-forms.h"

#include <time.h>

/* Dates and times */

static size_t dates_status;
static size_t date_items[3];
static size_t duration_item;

/* 30 August 2026, 14:05 local time: the same value on every run. */
time_t apidemo_forms_sample_time(void)
{
	struct tm when = {
		.tm_year = 2026 - 1900,
		.tm_mon = 7,
		.tm_mday = 30,
		.tm_hour = 14,
		.tm_min = 5,
		.tm_isdst = -1,
	};

	return mktime(&when);
}

/* A date item reports when its edit is done with OK. */
static void dates_changed(struct fplinux_ui *ui, struct fplinux_ui_screen *form,
			  size_t index, void *context)
{
	char date[64];
	char clock[16];

	(void)context;
	if (index == duration_item) {
		apidemo_forms_show_status(
			form, dates_status, _("Duration: %u s"),
			fplinux_ui_form_get_duration(form, duration_item));
		return;
	}
	if (!fplinux_ui_format_date(ui, fplinux_ui_form_get_date(form, index),
				    FPLINUX_UI_DATE_LONG, date, sizeof(date)) ||
	    !fplinux_ui_format_date(ui, fplinux_ui_form_get_date(form, index),
				    FPLINUX_UI_DATE_CLOCK, clock,
				    sizeof(clock)))
		return;
	apidemo_forms_show_status(form, dates_status, _("Item %zu: %s, %s"),
				  index, date, clock);
}

static void dates_command(struct fplinux_ui *ui, struct fplinux_ui_screen *form,
			  int command_id, void *context)
{
	char text[256] = "";
	time_t now = time(NULL);

	switch (command_id) {
	case APIDEMO_COMMAND_BACK:
		fplinux_ui_go_back(ui);
		break;
	case COMMAND_NOW:
		for (size_t i = 0; i < APIDEMO_COUNT(date_items); ++i)
			fplinux_ui_form_set_date(form, date_items[i], now);
		break;
	case COMMAND_RESET:
		fplinux_ui_form_set_duration(form, duration_item, 90);
		break;
	case COMMAND_SHOW:
		/* Calendar values and elapsed seconds have separate APIs. */
		for (size_t i = 0; i < APIDEMO_COUNT(date_items); ++i)
			apidemo_forms_append_text(
				text, sizeof(text), "get_date(%zu) =\n%lld%s",
				date_items[i],
				(long long)fplinux_ui_form_get_date(
					form, date_items[i]),
				i + 1 < APIDEMO_COUNT(date_items) ? "\n" : "");
		apidemo_forms_append_text(
			text, sizeof(text), "\nDuration: %u s",
			fplinux_ui_form_get_duration(form, duration_item));
		apidemo_show(context, _("Values"), text);
		break;
	default:
		break;
	}
}

void apidemo_forms_open_dates(struct apidemo *app,
			      const struct apidemo_entry *entry)
{
	struct fplinux_ui_screen *form =
		fplinux_ui_form_new(app->ui, _(entry->title));
	time_t when = apidemo_forms_sample_time();

	if (!form) {
		apidemo_error(app, "fplinux_ui_form_new");
		return;
	}
	dates_status = apidemo_forms_append_status(form);
	/* Shown in the system language; OK edits the parts in that order. */
	date_items[0] = fplinux_ui_form_append_date(form, _("Date"),
						    FPLINUX_UI_DATE, when);
	date_items[1] = fplinux_ui_form_append_date(form, _("Time"),
						    FPLINUX_UI_TIME, when);
	date_items[2] = fplinux_ui_form_append_date(form, _("Date and time"),
						    FPLINUX_UI_DATE_TIME, when);
	duration_item =
		fplinux_ui_form_append_duration(form, _("Duration"), 90);
	apidemo_forms_add_command(form, COMMAND_RESET,
				  FPLINUX_UI_COMMAND_SCREEN,
				  _("Reset duration"), 3,
				  apidemo_glyph(app, "refresh"));
	apidemo_forms_add_command(form, COMMAND_NOW, FPLINUX_UI_COMMAND_SCREEN,
				  _("Now"), 1, apidemo_glyph(app, "date-time"));
	apidemo_forms_add_command(form, COMMAND_SHOW, FPLINUX_UI_COMMAND_SCREEN,
				  _("Show values"), 2,
				  apidemo_glyph(app, "view"));
	apidemo_add_back(form);
	fplinux_ui_screen_set_listener(form, dates_command, dates_changed, app);
	apidemo_push(app, form, "fplinux_ui_form_new");
}
