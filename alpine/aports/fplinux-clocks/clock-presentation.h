/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_CLOCK_PRESENTATION_H
#define FPLINUX_CLOCK_PRESENTATION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct fplinux_ui;

/* Time uses the system UTC policy. Failed formats are empty. */
bool clock_presentation_time(struct fplinux_ui *ui, int64_t utc, bool seconds,
			     char *out, size_t capacity);
/* Relative labels use the UTC calendar day; other dates use full_date.
 * Invalid dates use the translated unavailable label when it fits.
 */
void clock_presentation_when(struct fplinux_ui *ui, int64_t utc,
			     int64_t now_utc, bool full_date, char *out,
			     size_t capacity);
/* Fixed-offset civil days are compared with the system UTC day at utc.
 * The input UTC time uses the model range; shifted dates may cross its edges.
 * Invalid offsets or dates leave all outputs empty.
 */
bool clock_presentation_offset(struct fplinux_ui *ui, int offset_minutes,
			       int64_t utc, char *time, size_t time_capacity,
			       char *date, size_t date_capacity, char *relative,
			       size_t relative_capacity);
/* Canonical UTC+HH:MM or UTC-HH:MM. Invalid offsets leave the output empty. */
bool clock_presentation_offset_name(int offset_minutes, char *out,
				    size_t capacity);

#endif
