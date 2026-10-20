/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_LAUNCHER_TEXT_H
#define FPLINUX_LAUNCHER_TEXT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

enum fplinux_launcher_view {
	FPLINUX_LAUNCHER_VIEW_HOME,
	FPLINUX_LAUNCHER_VIEW_GRID,
	FPLINUX_LAUNCHER_VIEW_LIST,
};

/*
 * The manager's spelling of the launcher views, in SELECT and ACTIVE: value is
 * "home" or "catalogue" and name the catalogue presentation, "grid" or "list".
 */
const char *fplinux_launcher_view_value(enum fplinux_launcher_view view);
const char *
fplinux_launcher_presentation_name(enum fplinux_launcher_view presentation);
/*
 * Reads the view and presentation that ACTIVE asks a new launcher to open.
 * False, with both left unchanged, unless both names are valid.
 */
bool fplinux_launcher_view_from_names(const char *value, const char *name,
				      enum fplinux_launcher_view *view,
				      enum fplinux_launcher_view *presentation);

/*
 * Notice codes from the manager: ACTIVE.reason (application notices and
 * unlock hints) and ACTIVE.notice (battery notices).
 */
enum fplinux_launcher_notice {
	FPLINUX_LAUNCHER_NOTICE_NONE,
	FPLINUX_LAUNCHER_NOTICE_APP_CRASHED,
	FPLINUX_LAUNCHER_NOTICE_APP_UNAVAILABLE,
	FPLINUX_LAUNCHER_NOTICE_APP_START_FAILED,
	FPLINUX_LAUNCHER_NOTICE_TASKS_FULL,
	FPLINUX_LAUNCHER_NOTICE_UNLOCK_PRESS_STAR,
	FPLINUX_LAUNCHER_NOTICE_UNLOCK_HELP,
	FPLINUX_LAUNCHER_NOTICE_BATTERY_LOW,
	FPLINUX_LAUNCHER_NOTICE_BATTERY_CRITICAL,
	/* Any other non-empty manager code. */
	FPLINUX_LAUNCHER_NOTICE_FAILED,
	FPLINUX_LAUNCHER_NOTICE_COUNT,
};

enum fplinux_launcher_notice
fplinux_launcher_notice_from_code(const char *code);
/* The manager's code of a notice, "" for the launcher's own. */
const char *fplinux_launcher_notice_code(enum fplinux_launcher_notice notice);
bool fplinux_launcher_notice_is_unlock_hint(enum fplinux_launcher_notice notice);
/*
 * The English source texts; the caller translates them with gettext(). Unlock
 * hints have no title; their whole text is the body.
 */
const char *fplinux_launcher_notice_title(enum fplinux_launcher_notice notice);
const char *fplinux_launcher_notice_body(enum fplinux_launcher_notice notice);

/* Delay that ends just after the next wall-clock minute begins. */
uint32_t fplinux_launcher_ms_to_next_minute(const struct timespec *now);
/* Startup time is unknown outside the manager's supported years, 1980–2099. */
bool fplinux_launcher_clock_is_ready(time_t now);
/* Unknown startup time retries in one second; a set clock ticks each minute. */
uint32_t fplinux_launcher_clock_delay(const struct timespec *now);

enum fplinux_launcher_legend {
	FPLINUX_LAUNCHER_LEGEND_LEFT,
	FPLINUX_LAUNCHER_LEGEND_CENTRE,
	FPLINUX_LAUNCHER_LEGEND_RIGHT,
	FPLINUX_LAUNCHER_LEGEND_COUNT,
};

/*
 * Copies a long date with its first ", " replaced by ",\n", so that it
 * breaks after the weekday: "Воскресенье," / "27 сентября". False, with
 * split unchanged, when the date has no ", " or split is too small.
 */
bool fplinux_launcher_split_date(const char *date, char *split,
				 size_t capacity);

#endif
