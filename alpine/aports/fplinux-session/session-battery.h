/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_SESSION_BATTERY_H
#define FPLINUX_SESSION_BATTERY_H

#include <stdbool.h>
#include <stdint.h>

#define FPLINUX_SESSION_BATTERY_LOW_PERCENT 15
#define FPLINUX_SESSION_BATTERY_CRITICAL_PERCENT 5
#define FPLINUX_SESSION_BATTERY_POLL_MS 60000U
/*
 * A reading older than two polls, e.g. after the display was off, is not
 * smoothed.
 */
#define FPLINUX_SESSION_BATTERY_STALE_MS (2 * FPLINUX_SESSION_BATTERY_POLL_MS)

/* One reading of the Linux power-supply class. */
struct fplinux_session_supply_sample {
	/* A present supply of type Battery with a capacity or a voltage. */
	bool has_battery;
	bool has_capacity;
	bool has_voltage;
	bool has_current;
	/* A supply of any other type reports online = 1. */
	bool charging;
	int capacity_percent;
	int64_t voltage_uv;
	/* Signed: positive while charging, negative while discharging. */
	int64_t current_ua;
};

/*
 * Reads every supply below directory. Without include_battery only the
 * external supplies are read, for the frequent charger poll. Returns false
 * when the directory cannot be listed; the sample then reports nothing.
 */
bool fplinux_session_supply_read(const char *directory, bool include_battery,
				 struct fplinux_session_supply_sample *sample);

/* Open-circuit voltage after removing the internal-resistance drop. */
int64_t fplinux_session_battery_ocv_uv(int64_t voltage_uv, bool has_current,
				       int64_t current_ua);
/* State of charge in permille from the open-circuit voltage table. */
int fplinux_session_battery_permille_from_ocv(int64_t ocv_uv);

struct fplinux_session_battery {
	bool known;
	/* Shown permille: voltage estimates smoothed, capacity as reported. */
	int permille;
	/* The latest reading alone, without smoothing. */
	int raw_permille;
	/* CLOCK_MONOTONIC milliseconds of the latest reading. */
	uint64_t read_ms;
};

/*
 * Folds one sample taken at now_ms into the level; returns whether a level is
 * known. A reading that is stale, or far from the shown level, replaces it.
 */
bool fplinux_session_battery_update(
	struct fplinux_session_battery *battery,
	const struct fplinux_session_supply_sample *sample, uint64_t now_ms);
/* Shown level, rounded. */
int fplinux_session_battery_percent(
	const struct fplinux_session_battery *battery);
/* Latest reading without smoothing, rounded. */
int fplinux_session_battery_raw_percent(
	const struct fplinux_session_battery *battery);

enum fplinux_session_battery_alert {
	FPLINUX_SESSION_BATTERY_ALERT_NONE,
	FPLINUX_SESSION_BATTERY_ALERT_LOW,
	FPLINUX_SESSION_BATTERY_ALERT_CRITICAL,
};

/* Already alerted in this discharge cycle. */
struct fplinux_session_battery_alerts {
	bool low_sent;
	bool critical_sent;
};

/*
 * Each alert is raised once per discharge cycle. Low follows the shown
 * percent, critical the raw percent. A charger ends the cycle; without one,
 * low is armed again after the shown level recovers by 5 points and critical
 * after both the shown and the raw level do.
 */
enum fplinux_session_battery_alert
fplinux_session_battery_alert(struct fplinux_session_battery_alerts *alerts,
			      int percent, int raw_percent, bool charging);

#endif
