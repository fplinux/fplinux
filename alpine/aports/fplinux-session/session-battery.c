/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "session-battery.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * A generic open-circuit voltage curve of a 4.2 V Li-ion cell. The level is
 * an estimate for the bar display, not a calibration of a fitted battery.
 */
static const struct {
	int32_t ocv_uv;
	int16_t percent;
} ocv_table[] = {
	{ 4200000, 100 }, { 4060000, 90 }, { 3980000, 80 }, { 3920000, 70 },
	{ 3870000, 60 },  { 3820000, 50 }, { 3790000, 40 }, { 3770000, 30 },
	{ 3740000, 20 },  { 3680000, 10 }, { 3600000, 5 },  { 3400000, 0 },
};

#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))
/* Assumed cell and contact resistance, 0.1 ohm. */
#define INTERNAL_RESISTANCE_UOHM 100000
/*
 * New voltage estimates move the shown level by a quarter of the change, which
 * hides load-dependent noise. A change of 20 points or more between two reads
 * is not discharge, for example a replaced battery,
 * and is shown at once. So is a reading after a gap longer than
 * FPLINUX_SESSION_BATTERY_STALE_MS, such as a long display-off period.
 */
#define SMOOTHING_DIVISOR 4
#define SMOOTHING_LIMIT_PERMILLE 200
#define ALERT_REARM_PERCENT 5

static bool read_attribute(int directory_fd, const char *supply,
			   const char *attribute, char *text, size_t capacity)
{
	char path[NAME_MAX + 32];
	size_t length = 0;
	int fd;

	if (snprintf(path, sizeof(path), "%s/%s", supply, attribute) >=
	    (int)sizeof(path))
		return false;
	fd = openat(directory_fd, path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return false;
	while (length < capacity - 1) {
		ssize_t count = read(fd, text + length, capacity - 1 - length);

		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0)
			break;
		length += (size_t)count;
	}
	close(fd);
	text[length] = '\0';
	while (length && isspace((unsigned char)text[length - 1]))
		text[--length] = '\0';
	return length > 0;
}

static bool read_integer(int directory_fd, const char *supply,
			 const char *attribute, int64_t *value)
{
	char text[32];
	char *end;
	long long number;

	if (!read_attribute(directory_fd, supply, attribute, text,
			    sizeof(text)))
		return false;
	errno = 0;
	number = strtoll(text, &end, 10);
	if (errno || end == text || *end)
		return false;
	*value = number;
	return true;
}

static void read_battery(int directory_fd, const char *supply,
			 struct fplinux_session_supply_sample *sample)
{
	int64_t value;

	if (read_integer(directory_fd, supply, "present", &value) && !value)
		return;
	sample->has_capacity =
		read_integer(directory_fd, supply, "capacity", &value);
	if (sample->has_capacity)
		sample->capacity_percent = value < 0   ? 0 :
					   value > 100 ? 100 :
							 (int)value;
	sample->has_voltage = read_integer(directory_fd, supply, "voltage_now",
					   &sample->voltage_uv) &&
			      sample->voltage_uv > 0;
	sample->has_current = read_integer(directory_fd, supply, "current_now",
					   &sample->current_ua);
	sample->has_battery = sample->has_capacity || sample->has_voltage;
}

bool fplinux_session_supply_read(const char *directory, bool include_battery,
				 struct fplinux_session_supply_sample *sample)
{
	struct fplinux_session_supply_sample battery = { 0 };
	char battery_name[NAME_MAX + 1] = { 0 };
	struct dirent *entry;
	DIR *supplies;
	int directory_fd;

	memset(sample, 0, sizeof(*sample));
	supplies = opendir(directory);
	if (!supplies)
		return false;
	directory_fd = dirfd(supplies);
	if (directory_fd < 0) {
		closedir(supplies);
		return false;
	}
	while ((entry = readdir(supplies))) {
		struct fplinux_session_supply_sample candidate = { 0 };
		char type[32];
		int64_t online;

		if (entry->d_name[0] == '.' ||
		    !read_attribute(directory_fd, entry->d_name, "type", type,
				    sizeof(type)))
			continue;
		if (strcmp(type, "Battery")) {
			if (read_integer(directory_fd, entry->d_name, "online",
					 &online) &&
			    online == 1)
				sample->charging = true;
			continue;
		}
		/* Use the first battery by name so that the choice is stable. */
		if (!include_battery ||
		    (battery_name[0] &&
		     strcmp(entry->d_name, battery_name) > 0))
			continue;
		read_battery(directory_fd, entry->d_name, &candidate);
		if (!candidate.has_battery)
			continue;
		battery = candidate;
		strcpy(battery_name, entry->d_name);
	}
	closedir(supplies);
	battery.charging = sample->charging;
	*sample = battery;
	return true;
}

int64_t fplinux_session_battery_ocv_uv(int64_t voltage_uv, bool has_current,
				       int64_t current_ua)
{
	if (!has_current)
		return voltage_uv;
	/* Charging current raises the terminal voltage, discharge lowers it. */
	return voltage_uv - current_ua * INTERNAL_RESISTANCE_UOHM / 1000000;
}

int fplinux_session_battery_permille_from_ocv(int64_t ocv_uv)
{
	size_t last = ARRAY_SIZE(ocv_table) - 1;

	if (ocv_uv >= ocv_table[0].ocv_uv)
		return ocv_table[0].percent * 10;
	if (ocv_uv <= ocv_table[last].ocv_uv)
		return ocv_table[last].percent * 10;
	for (size_t i = 1; i <= last; ++i) {
		int64_t high_uv = ocv_table[i - 1].ocv_uv;
		int64_t low_uv = ocv_table[i].ocv_uv;
		int64_t high = ocv_table[i - 1].percent * 10;
		int64_t low = ocv_table[i].percent * 10;

		if (ocv_uv >= low_uv)
			return (int)(low + (ocv_uv - low_uv) * (high - low) /
						   (high_uv - low_uv));
	}
	return 0;
}

bool fplinux_session_battery_update(
	struct fplinux_session_battery *battery,
	const struct fplinux_session_supply_sample *sample, uint64_t now_ms)
{
	bool fresh = battery->known && now_ms >= battery->read_ms &&
		     now_ms - battery->read_ms <
			     FPLINUX_SESSION_BATTERY_STALE_MS;
	int raw;

	if (!sample->has_battery) {
		battery->known = false;
		return false;
	}
	if (sample->has_capacity) {
		raw = sample->capacity_percent * 10;
		battery->permille = raw;
	} else {
		raw = fplinux_session_battery_permille_from_ocv(
			fplinux_session_battery_ocv_uv(sample->voltage_uv,
						       sample->has_current,
						       sample->current_ua));
		if (fresh &&
		    abs(raw - battery->permille) < SMOOTHING_LIMIT_PERMILLE)
			battery->permille +=
				(raw - battery->permille) / SMOOTHING_DIVISOR;
		else
			battery->permille = raw;
	}
	battery->raw_permille = raw;
	battery->read_ms = now_ms;
	battery->known = true;
	return true;
}

int fplinux_session_battery_percent(
	const struct fplinux_session_battery *battery)
{
	return (battery->permille + 5) / 10;
}

int fplinux_session_battery_raw_percent(
	const struct fplinux_session_battery *battery)
{
	return (battery->raw_permille + 5) / 10;
}

enum fplinux_session_battery_alert
fplinux_session_battery_alert(struct fplinux_session_battery_alerts *alerts,
			      int percent, int raw_percent, bool charging)
{
	if (charging) {
		/* A charger ends the discharge cycle. */
		alerts->low_sent = false;
		alerts->critical_sent = false;
		return FPLINUX_SESSION_BATTERY_ALERT_NONE;
	}
	if (percent >=
	    FPLINUX_SESSION_BATTERY_LOW_PERCENT + ALERT_REARM_PERCENT)
		alerts->low_sent = false;
	/*
	 * Critical fires on the raw level, so both levels must recover: a
	 * smoothed level alone would re-arm it on every poll while the raw
	 * level stays low, bringing a closed notice straight back.
	 */
	if (percent >= FPLINUX_SESSION_BATTERY_CRITICAL_PERCENT +
			       ALERT_REARM_PERCENT &&
	    raw_percent >= FPLINUX_SESSION_BATTERY_CRITICAL_PERCENT +
				   ALERT_REARM_PERCENT)
		alerts->critical_sent = false;
	/* Critical follows the raw reading so that smoothing cannot delay it. */
	if (raw_percent < FPLINUX_SESSION_BATTERY_CRITICAL_PERCENT &&
	    !alerts->critical_sent) {
		alerts->critical_sent = true;
		alerts->low_sent = true;
		return FPLINUX_SESSION_BATTERY_ALERT_CRITICAL;
	}
	if (percent < FPLINUX_SESSION_BATTERY_LOW_PERCENT &&
	    !alerts->low_sent) {
		alerts->low_sent = true;
		return FPLINUX_SESSION_BATTERY_ALERT_LOW;
	}
	return FPLINUX_SESSION_BATTERY_ALERT_NONE;
}
