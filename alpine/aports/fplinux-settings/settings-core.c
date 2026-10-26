/* SPDX-License-Identifier: GPL-2.0-only */
#define _POSIX_C_SOURCE 200809L
#include "settings-core.h"
#include "fplinux-ui-profile.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

size_t fplinux_settings_choice_find(const char *const *values, size_t count,
				    const char *value)
{
	size_t index;

	for (index = 0; index < count; index++)
		if (!strcmp(values[index], value))
			break;
	return index;
}

bool fplinux_settings_parse_brightness(const char *value, unsigned int *level)
{
	unsigned long number;
	char *end;

	if (value[0] < '0' || value[0] > '9')
		return false;
	errno = 0;
	number = strtoul(value, &end, 10);
	if (*end || errno || number < FPLINUX_SETTINGS_BRIGHTNESS_MIN ||
	    number > FPLINUX_SETTINGS_BRIGHTNESS_MAX)
		return false;
	*level = (unsigned int)number;
	return true;
}

bool fplinux_settings_time_value(const struct tm *date, const struct tm *clock,
				 char *value, size_t size)
{
	int year = date->tm_year + 1900;
	/* A leap second keeps the minute. */
	int second = clock->tm_sec == 60 ? 59 : clock->tm_sec;
	int length;

	if (year < 1000 || year > 9999 || date->tm_mon < 0 ||
	    date->tm_mon > 11 || date->tm_mday < 1 || date->tm_mday > 31 ||
	    clock->tm_hour < 0 || clock->tm_hour > 23 || clock->tm_min < 0 ||
	    clock->tm_min > 59 || second < 0 || second > 59)
		return false;
	length = snprintf(value, size, "%04d-%02d-%02d %02d:%02d:%02d", year,
			  date->tm_mon + 1, date->tm_mday, clock->tm_hour,
			  clock->tm_min, second);
	return length >= 0 && (size_t)length < size;
}

bool fplinux_settings_save_time(time_t now, const time_t *date,
				const time_t *clock, char *value, size_t size)
{
	struct tm date_parts;
	struct tm clock_parts;

	if (!localtime_r(date ? date : &now, &date_parts) ||
	    !localtime_r(clock ? clock : &now, &clock_parts))
		return false;
	if (clock)
		clock_parts.tm_sec = 0;
	return fplinux_settings_time_value(&date_parts, &clock_parts, value,
					   size);
}

bool fplinux_settings_time_in_range(const char *value)
{
	int year = 0;

	for (int i = 0; i < 4; i++) {
		if (value[i] < '0' || value[i] > '9')
			return false;
		year = year * 10 + (value[i] - '0');
	}
	return value[4] == '-' && year >= FPLINUX_SETTINGS_YEAR_MIN &&
	       year <= FPLINUX_SETTINGS_YEAR_MAX;
}

bool fplinux_settings_date_time_detail(const char *date, const char *clock,
				       char *detail, size_t size)
{
	const char *separator = date[0] && clock[0] ? " " : "";
	int length = snprintf(detail, size, "%s%s%s", date, separator, clock);

	return length >= 0 && (size_t)length < size;
}

/* The system release file uses plain or quoted field values. */
static bool copy_field(const char *line, const char *key, char *value,
		       size_t size)
{
	size_t key_length = strlen(key);
	const char *text;
	size_t length;

	if (strncmp(line, key, key_length))
		return false;
	text = line + key_length;
	length = strlen(text);
	if (length >= 2 && (text[0] == '"' || text[0] == '\'') &&
	    text[length - 1] == text[0]) {
		text++;
		length -= 2;
	}
	if (length >= size)
		return false;
	memcpy(value, text, length);
	value[length] = '\0';
	return true;
}

bool fplinux_settings_read_os_field(const char *path, const char *field,
				    char *value, size_t size)
{
	char key[64];
	char line[256];
	int length = snprintf(key, sizeof(key), "%s=", field);
	FILE *file;
	bool found = false;

	if (length < 0 || (size_t)length >= sizeof(key))
		return false;
	file = fopen(path, "re");
	if (!file)
		return false;
	while (fgets(line, sizeof(line), file)) {
		line[strcspn(line, "\n")] = '\0';
		if (copy_field(line, key, value, size)) {
			found = value[0] != '\0';
			break;
		}
	}
	fclose(file);
	return found;
}

bool fplinux_settings_read_os_name(const char *path, char *name, size_t size)
{
	char line[256];
	char os_name[96] = "";
	char version[64] = "";
	char version_id[64] = "";
	const char *suffix;
	FILE *file = fopen(path, "re");
	int length;

	if (!file)
		return false;
	while (fgets(line, sizeof(line), file)) {
		line[strcspn(line, "\n")] = '\0';
		if (!copy_field(line, "NAME=", os_name, sizeof(os_name)) &&
		    !copy_field(line, "VERSION=", version, sizeof(version)))
			copy_field(line, "VERSION_ID=", version_id,
				   sizeof(version_id));
	}
	fclose(file);
	if (!os_name[0])
		return false;
	suffix = version[0] ? version : version_id;
	length = snprintf(name, size, "%s%s%s", os_name, suffix[0] ? " " : "",
			  suffix);
	return length >= 0 && (size_t)length < size;
}

bool fplinux_settings_read_memory(const char *path,
				  unsigned long *available_kib,
				  unsigned long *total_kib)
{
	bool has_available = false;
	bool has_total = false;
	char line[128];
	FILE *file = fopen(path, "re");

	if (!file)
		return false;
	while (fgets(line, sizeof(line), file)) {
		if (sscanf(line, "MemAvailable: %lu kB", available_kib) == 1)
			has_available = true;
		else if (sscanf(line, "MemTotal: %lu kB", total_kib) == 1)
			has_total = true;
	}
	fclose(file);
	return has_available && has_total;
}

bool fplinux_settings_about_logo(int profile, bool dark, char *path,
				 size_t size)
{
	const struct fplinux_ui_profile_spec *spec =
		fplinux_ui_profile_spec(profile);
	int length;

	if (!spec)
		return false;
	length = snprintf(path, size, "/usr/share/fplinux/brand/%s-%s-%d.png",
			  spec->about_lockup ? "lockup" : "mark",
			  dark ? "dark" : "light", spec->about_logo_width);
	return length >= 0 && (size_t)length < size;
}

bool fplinux_settings_read_line(const char *path, char *line, size_t size)
{
	FILE *file;
	bool found;

	if (!size || size > INT_MAX)
		return false;
	file = fopen(path, "re");
	if (!file)
		return false;
	found = fgets(line, (int)size, file) != NULL;
	fclose(file);
	if (!found)
		return false;
	line[strcspn(line, "\n")] = '\0';
	return line[0] != '\0';
}
