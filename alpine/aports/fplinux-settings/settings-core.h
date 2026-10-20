/* SPDX-License-Identifier: GPL-2.0-only */
/* Display-independent parts of the Settings application. */

#ifndef FPLINUX_SETTINGS_CORE_H
#define FPLINUX_SETTINGS_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#include "fplinux-settings-values.h"

/* The SETTING time value "YYYY-MM-DD HH:MM:SS" and its terminator. */
#define FPLINUX_SETTINGS_TIME_BYTES 20U

/* Index of value in values, or count when it is not one of them. */
size_t fplinux_settings_choice_find(const char *const *values, size_t count,
				    const char *value);
bool fplinux_settings_parse_brightness(const char *value, unsigned int *level);

/*
 * Writes the SETTING time value "YYYY-MM-DD HH:MM:SS" from the day of date
 * and the time of day of clock; false when a field is out of range or size
 * is below FPLINUX_SETTINGS_TIME_BYTES.
 */
bool fplinux_settings_time_value(const struct tm *date, const struct tm *clock,
				 char *value, size_t size);
/*
 * The time value that Save sends at now. The day is the edited date's, or
 * today's when date is NULL; the time is the edited clock's hour and minute
 * with 0 seconds, or the current time with its seconds when clock is NULL.
 * So an item the user did not edit never sets the clock back to the moment
 * the form opened.
 */
bool fplinux_settings_save_time(time_t now, const time_t *date,
				const time_t *clock, char *value, size_t size);
/* Whether the year of a time value is one the manager accepts. */
bool fplinux_settings_time_in_range(const char *value);
/*
 * The Date and time row's detail from the short date and the clock that the
 * runtime formats: "30.09.2026 23:58", or the one that is not empty; false
 * when size is too small.
 */
bool fplinux_settings_date_time_detail(const char *date, const char *clock,
				       char *detail, size_t size);

/* Read-only system facts; false when the source cannot supply the value. */
bool fplinux_settings_read_os_name(const char *path, char *name, size_t size);
bool fplinux_settings_read_line(const char *path, char *line, size_t size);
bool fplinux_settings_read_memory(const char *path,
				  unsigned long *available_kib,
				  unsigned long *total_kib);
/* The installed brand image selected by the common display profile. */
bool fplinux_settings_about_logo(int profile, bool dark, char *path,
				 size_t size);

#endif
