/* SPDX-License-Identifier: GPL-2.0-only */
#include "clock-presentation.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct offset_case {
	int64_t utc;
	int offset_minutes;
	const char *name;
	const char *time;
	const char *date;
	const char *relative;
};

static void offset_calendar(void)
{
	const struct offset_case cases[] = {
		{ INT64_C(315532800), -720, "UTC-12:00", "12:00", "1979-12-31",
		  "Yesterday" },
		{ INT64_C(4102444799), 840, "UTC+14:00", "13:59", "2100-01-01",
		  "Tomorrow" },
		{ INT64_C(1704067200), 0, "UTC+00:00", "00:00", "2024-01-01",
		  "Today" },
		{ INT64_C(1704067200), 330, "UTC+05:30", "05:30", "2024-01-01",
		  "Today" },
		{ INT64_C(1704067200), -210, "UTC-03:30", "20:30", "2023-12-31",
		  "Yesterday" },
		{ INT64_C(1706744700), 345, "UTC+05:45", "05:30", "2024-02-01",
		  "Tomorrow" },
		{ INT64_C(1735689000), 345, "UTC+05:45", "05:35", "2025-01-01",
		  "Tomorrow" },
	};

	for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]);
	     ++index) {
		const struct offset_case *expected = &cases[index];
		char name[32], time[32], date[64], relative[32];

		assert(clock_presentation_offset_name(expected->offset_minutes,
						      name, sizeof(name)));
		assert(clock_presentation_offset(
			NULL, expected->offset_minutes, expected->utc, time,
			sizeof(time), date, sizeof(date), relative,
			sizeof(relative)));
		assert(!strcmp(name, expected->name));
		assert(!strcmp(time, expected->time));
		assert(!strcmp(date, expected->date));
		assert(!strcmp(relative, expected->relative));
	}
}

static void invalid_values_clear_outputs(void)
{
	const int invalid_offsets[] = { -735, 855, 1 };
	const int64_t invalid_times[] = { INT64_C(315532799),
					  INT64_C(4102444800) };
	char name[32], time[32], date[64], relative[32];

	for (size_t index = 0;
	     index < sizeof(invalid_offsets) / sizeof(invalid_offsets[0]);
	     ++index) {
		strcpy(name, "previous");
		strcpy(time, "previous");
		strcpy(date, "previous");
		strcpy(relative, "previous");
		assert(!clock_presentation_offset_name(invalid_offsets[index],
						       name, sizeof(name)));
		assert(!clock_presentation_offset(
			NULL, invalid_offsets[index], INT64_C(1704067200), time,
			sizeof(time), date, sizeof(date), relative,
			sizeof(relative)));
		assert(!name[0] && !time[0] && !date[0] && !relative[0]);
	}
	for (size_t index = 0;
	     index < sizeof(invalid_times) / sizeof(invalid_times[0]);
	     ++index) {
		strcpy(time, "previous");
		strcpy(date, "previous");
		strcpy(relative, "previous");
		assert(!clock_presentation_offset(
			NULL, 0, invalid_times[index], time, sizeof(time), date,
			sizeof(date), relative, sizeof(relative)));
		assert(!time[0] && !date[0] && !relative[0]);
	}
	assert(!clock_presentation_offset_name(345, name, 9));
	assert(!name[0]);
	assert(!clock_presentation_offset(NULL, 345, INT64_C(1704067200), time,
					  5, date, sizeof(date), relative,
					  sizeof(relative)));
	assert(!time[0] && !date[0] && !relative[0]);
}

int main(void)
{
	offset_calendar();
	invalid_values_clear_outputs();
	puts("clock-presentation: fixed offsets, calendar edges and invalid values passed");
	return 0;
}
