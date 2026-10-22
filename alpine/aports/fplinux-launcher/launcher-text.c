/* SPDX-License-Identifier: GPL-2.0-only */
#include "launcher-text.h"
#include "fplinux-settings-values.h"

#include <string.h>

/* Marks a source text for the catalogue; the caller translates it. */
#define N_(text) text

/* Every notice states what happened and the next useful step. */
static const struct {
	const char *code;
	const char *title;
	const char *body;
} notices[FPLINUX_LAUNCHER_NOTICE_COUNT] = {
	[FPLINUX_LAUNCHER_NOTICE_APP_CRASHED] = {
		"app-crashed",
		N_("App stopped"),
		N_("It closed unexpectedly. Open it again from the menu."),
	},
	[FPLINUX_LAUNCHER_NOTICE_APP_UNAVAILABLE] = {
		"app-unavailable",
		N_("App unavailable"),
		N_("It is missing or cannot run. Reinstall its package."),
	},
	[FPLINUX_LAUNCHER_NOTICE_APP_START_FAILED] = {
		"app-start-failed",
		N_("App did not start"),
		N_("Try again. If it fails again, reinstall it."),
	},
	[FPLINUX_LAUNCHER_NOTICE_TASKS_FULL] = {
		"tasks-full",
		N_("Too many open apps"),
		N_("Close an application in Tasks, then try again. Your open applications were kept."),
	},
	[FPLINUX_LAUNCHER_NOTICE_UNLOCK_PRESS_STAR] = {
		"unlock-press-star",
		"",
		N_("Now press *"),
	},
	[FPLINUX_LAUNCHER_NOTICE_UNLOCK_HELP] = {
		"unlock-help",
		"",
		N_("Press Unlock, then *"),
	},
	[FPLINUX_LAUNCHER_NOTICE_BATTERY_LOW] = {
		"battery-low",
		N_("Battery low"),
		N_("Connect the charger soon."),
	},
	[FPLINUX_LAUNCHER_NOTICE_BATTERY_CRITICAL] = {
		"battery-critical",
		N_("Battery almost empty"),
		N_("Connect the charger now."),
	},
	[FPLINUX_LAUNCHER_NOTICE_FAILED] = {
		NULL,
		N_("Action failed"),
		N_("Try again."),
	},
};

const char *fplinux_launcher_view_value(enum fplinux_launcher_view view)
{
	return view == FPLINUX_LAUNCHER_VIEW_HOME ? "home" : "catalogue";
}

const char *
fplinux_launcher_presentation_name(enum fplinux_launcher_view presentation)
{
	return presentation == FPLINUX_LAUNCHER_VIEW_LIST ? "list" : "grid";
}

bool fplinux_launcher_view_from_names(const char *value, const char *name,
				      enum fplinux_launcher_view *view,
				      enum fplinux_launcher_view *presentation)
{
	enum fplinux_launcher_view catalogue;

	if (!strcmp(name, "grid"))
		catalogue = FPLINUX_LAUNCHER_VIEW_GRID;
	else if (!strcmp(name, "list"))
		catalogue = FPLINUX_LAUNCHER_VIEW_LIST;
	else
		return false;
	if (!strcmp(value, "home"))
		*view = FPLINUX_LAUNCHER_VIEW_HOME;
	else if (!strcmp(value, "catalogue"))
		*view = catalogue;
	else
		return false;
	*presentation = catalogue;
	return true;
}

enum fplinux_launcher_notice fplinux_launcher_notice_from_code(const char *code)
{
	if (!code || !code[0])
		return FPLINUX_LAUNCHER_NOTICE_NONE;
	for (int i = 0; i < FPLINUX_LAUNCHER_NOTICE_COUNT; ++i)
		if (notices[i].code && !strcmp(notices[i].code, code))
			return i;
	return FPLINUX_LAUNCHER_NOTICE_FAILED;
}

const char *fplinux_launcher_notice_code(enum fplinux_launcher_notice notice)
{
	const char *code = notices[notice].code;

	return code ? code : "";
}

bool fplinux_launcher_notice_is_unlock_hint(enum fplinux_launcher_notice notice)
{
	return notice == FPLINUX_LAUNCHER_NOTICE_UNLOCK_PRESS_STAR ||
	       notice == FPLINUX_LAUNCHER_NOTICE_UNLOCK_HELP;
}

const char *fplinux_launcher_notice_title(enum fplinux_launcher_notice notice)
{
	const char *title = notices[notice].title;

	return title ? title : "";
}

const char *fplinux_launcher_notice_body(enum fplinux_launcher_notice notice)
{
	const char *body = notices[notice].body;

	return body ? body : "";
}

uint32_t fplinux_launcher_ms_to_next_minute(const struct timespec *now)
{
	int64_t second = now->tv_sec % 60;

	/* Dropping the sub-millisecond part rounds the delay up. */
	return (uint32_t)((60 - second) * 1000 - now->tv_nsec / 1000000);
}

bool fplinux_launcher_clock_is_ready(time_t now)
{
	const struct tm *date = localtime(&now);

	return date && date->tm_year >= FPLINUX_SETTINGS_YEAR_MIN - 1900 &&
	       date->tm_year <= FPLINUX_SETTINGS_YEAR_MAX - 1900;
}

uint32_t fplinux_launcher_clock_delay(const struct timespec *now)
{
	return fplinux_launcher_clock_is_ready(now->tv_sec) ?
		       fplinux_launcher_ms_to_next_minute(now) :
		       1000;
}

bool fplinux_launcher_split_date(const char *date, char *split, size_t capacity)
{
	const char *comma = strstr(date, ", ");
	size_t length = strlen(date);

	if (!comma || length >= capacity)
		return false;
	memcpy(split, date, length + 1);
	split[comma - date + 1] = '\n';
	return true;
}
