/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Host harness for the session manager's battery module. Link it with
 * session-battery.c. It proves host behaviour only: no phone power supply.
 */
#define _GNU_SOURCE
#include "session-battery.h"

#include <errno.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int failures;

#define CHECK(condition)                                                       \
	do {                                                                   \
		if (!(condition)) {                                            \
			fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, \
				__LINE__, #condition);                         \
			++failures;                                            \
		}                                                              \
	} while (0)

#define STALE 120000U
#define ALERT_NONE FPLINUX_SESSION_BATTERY_ALERT_NONE
#define ALERT_LOW FPLINUX_SESSION_BATTERY_ALERT_LOW
#define ALERT_CRITICAL FPLINUX_SESSION_BATTERY_ALERT_CRITICAL

/* A fake power-supply class: one directory per supply, one file each. */
struct supply_root {
	char path[64];
};

static void supply_root_create(struct supply_root *root)
{
	snprintf(root->path, sizeof(root->path), "/tmp/session-battery-XXXXXX");
	if (!mkdtemp(root->path)) {
		perror("mkdtemp");
		exit(1);
	}
}

static int remove_entry(const char *path, const struct stat *status, int type,
			struct FTW *walk)
{
	(void)status;
	(void)type;
	(void)walk;
	return remove(path);
}

static void supply_root_remove(struct supply_root *root)
{
	nftw(root->path, remove_entry, 8, FTW_DEPTH | FTW_PHYS);
}

static void write_attribute(const struct supply_root *root, const char *supply,
			    const char *attribute, const char *value)
{
	char path[256];
	FILE *file;

	snprintf(path, sizeof(path), "%s/%s", root->path, supply);
	if (mkdir(path, 0700) && errno != EEXIST) {
		perror(path);
		exit(1);
	}
	snprintf(path, sizeof(path), "%s/%s/%s", root->path, supply, attribute);
	file = fopen(path, "w");
	if (!file) {
		perror(path);
		exit(1);
	}
	fprintf(file, "%s\n", value);
	fclose(file);
}

/* The phone's PMIC: a voltage-only battery and an "Unknown" charger. */
static void write_phone_supplies(const struct supply_root *root,
				 const char *voltage_uv, const char *online)
{
	write_attribute(root, "sc2720-battery", "type", "Battery");
	write_attribute(root, "sc2720-battery", "voltage_now", voltage_uv);
	write_attribute(root, "sc2720-battery", "current_now", "-150000");
	write_attribute(root, "sc2720-battery", "charge_counter", "-1200");
	write_attribute(root, "sc2720-charger", "type", "Unknown");
	write_attribute(root, "sc2720-charger", "online", online);
}

static void test_supply_read_phone_battery_without_charger(void)
{
	struct fplinux_session_supply_sample sample;
	struct supply_root root;

	supply_root_create(&root);
	write_phone_supplies(&root, "3805000", "0");
	CHECK(fplinux_session_supply_read(root.path, true, &sample));
	CHECK(sample.has_battery);
	CHECK(sample.has_voltage && sample.voltage_uv == 3805000);
	CHECK(sample.has_current && sample.current_ua == -150000);
	CHECK(!sample.has_capacity);
	CHECK(!sample.charging);
	supply_root_remove(&root);
}

static void test_supply_read_unknown_type_charger_online_is_charging(void)
{
	struct fplinux_session_supply_sample sample;
	struct supply_root root;

	supply_root_create(&root);
	write_phone_supplies(&root, "3805000", "1");
	CHECK(fplinux_session_supply_read(root.path, true, &sample));
	CHECK(sample.charging);
	CHECK(sample.has_battery);
	/* The frequent charger poll reads chargers only and reports no battery. */
	CHECK(fplinux_session_supply_read(root.path, false, &sample));
	CHECK(sample.charging);
	CHECK(!sample.has_battery);
	supply_root_remove(&root);
}

static void test_supply_read_capacity_is_clamped(void)
{
	struct fplinux_session_supply_sample sample;
	struct supply_root root;

	supply_root_create(&root);
	write_attribute(&root, "BAT0", "type", "Battery");
	write_attribute(&root, "BAT0", "capacity", "150");
	write_attribute(&root, "AC", "type", "Mains");
	write_attribute(&root, "AC", "online", "1");
	CHECK(fplinux_session_supply_read(root.path, true, &sample));
	CHECK(sample.has_battery && sample.has_capacity);
	CHECK(sample.capacity_percent == 100);
	CHECK(!sample.has_voltage);
	CHECK(sample.charging);
	supply_root_remove(&root);
}

static void test_supply_read_absent_or_missing_battery_shows_nothing(void)
{
	struct fplinux_session_supply_sample sample;
	struct supply_root root;

	supply_root_create(&root);
	CHECK(fplinux_session_supply_read(root.path, true, &sample));
	CHECK(!sample.has_battery && !sample.charging);
	write_attribute(&root, "battery", "type", "Battery");
	write_attribute(&root, "battery", "present", "0");
	write_attribute(&root, "battery", "voltage_now", "3900000");
	CHECK(fplinux_session_supply_read(root.path, true, &sample));
	CHECK(!sample.has_battery);
	supply_root_remove(&root);
	CHECK(!fplinux_session_supply_read(root.path, true, &sample));
	CHECK(!sample.has_battery && !sample.charging);
}

static void test_supply_read_battery_online_does_not_mean_charging(void)
{
	struct fplinux_session_supply_sample sample;
	struct supply_root root;

	supply_root_create(&root);
	write_attribute(&root, "battery", "type", "Battery");
	write_attribute(&root, "battery", "voltage_now", "3900000");
	write_attribute(&root, "battery", "online", "1");
	CHECK(fplinux_session_supply_read(root.path, true, &sample));
	CHECK(sample.has_battery && !sample.charging);
	supply_root_remove(&root);
}

static void test_supply_read_picks_first_battery_by_name(void)
{
	struct fplinux_session_supply_sample sample;
	struct supply_root root;

	supply_root_create(&root);
	write_attribute(&root, "b-battery", "type", "Battery");
	write_attribute(&root, "b-battery", "capacity", "20");
	write_attribute(&root, "a-battery", "type", "Battery");
	write_attribute(&root, "a-battery", "capacity", "70");
	CHECK(fplinux_session_supply_read(root.path, true, &sample));
	CHECK(sample.has_capacity && sample.capacity_percent == 70);
	supply_root_remove(&root);
}

static void test_ocv_table_points_and_interpolation(void)
{
	/* Points of the generic curve: 4.2 V 100 %, 3.82 V 50 %, 3.4 V 0 %. */
	CHECK(fplinux_session_battery_permille_from_ocv(4200000) == 1000);
	CHECK(fplinux_session_battery_permille_from_ocv(4350000) == 1000);
	CHECK(fplinux_session_battery_permille_from_ocv(3820000) == 500);
	CHECK(fplinux_session_battery_permille_from_ocv(3680000) == 100);
	CHECK(fplinux_session_battery_permille_from_ocv(3400000) == 0);
	CHECK(fplinux_session_battery_permille_from_ocv(3100000) == 0);
	/* Halfway between 3.82 V (50 %) and 3.87 V (60 %). */
	CHECK(fplinux_session_battery_permille_from_ocv(3845000) == 550);
	/* Halfway between 3.4 V (0 %) and 3.6 V (5 %). */
	CHECK(fplinux_session_battery_permille_from_ocv(3500000) == 25);
}

static void test_ocv_compensates_internal_resistance(void)
{
	/* 0.1 ohm: 200 mA discharge hides 20 mV, 400 mA charge adds 40. */
	CHECK(fplinux_session_battery_ocv_uv(3700000, true, -200000) ==
	      3720000);
	CHECK(fplinux_session_battery_ocv_uv(4000000, true, 400000) == 3960000);
	CHECK(fplinux_session_battery_ocv_uv(3700000, false, -200000) ==
	      3700000);
}

static struct fplinux_session_supply_sample voltage_sample(int64_t voltage_uv)
{
	return (struct fplinux_session_supply_sample){
		.has_battery = true,
		.has_voltage = true,
		.voltage_uv = voltage_uv,
	};
}

static void test_battery_level_smooths_voltage_estimates(void)
{
	struct fplinux_session_battery battery = { 0 };
	struct fplinux_session_supply_sample sample = voltage_sample(3820000);

	CHECK(fplinux_session_battery_update(&battery, &sample, 1000));
	CHECK(fplinux_session_battery_percent(&battery) == 50);
	/* A 10-point rise to 60 % moves the level a quarter of the way. */
	sample.voltage_uv = 3870000;
	CHECK(fplinux_session_battery_update(&battery, &sample, 2000));
	CHECK(fplinux_session_battery_percent(&battery) == 53);
	CHECK(fplinux_session_battery_raw_percent(&battery) == 60);
	/* A 47-point jump to 100 % is shown at once. */
	sample.voltage_uv = 4200000;
	CHECK(fplinux_session_battery_update(&battery, &sample, 3000));
	CHECK(fplinux_session_battery_percent(&battery) == 100);
	/* A fuel-gauge capacity is shown as reported. */
	sample = (struct fplinux_session_supply_sample){
		.has_battery = true,
		.has_capacity = true,
		.capacity_percent = 42,
	};
	CHECK(fplinux_session_battery_update(&battery, &sample, 4000));
	CHECK(fplinux_session_battery_percent(&battery) == 42);
	CHECK(fplinux_session_battery_raw_percent(&battery) == 42);
	CHECK(battery.read_ms == 4000);
	sample = (struct fplinux_session_supply_sample){ 0 };
	CHECK(!fplinux_session_battery_update(&battery, &sample, 5000));
	CHECK(!battery.known);
}

static void test_fresh_reading_still_smooths(void)
{
	struct fplinux_session_battery battery = { 0 };
	struct fplinux_session_supply_sample sample = voltage_sample(3820000);

	CHECK(fplinux_session_battery_update(&battery, &sample, 0));
	sample.voltage_uv = 3870000;
	CHECK(fplinux_session_battery_update(&battery, &sample, STALE - 1));
	CHECK(fplinux_session_battery_percent(&battery) == 53);
	CHECK(fplinux_session_battery_raw_percent(&battery) == 60);
}

static void test_stale_reading_shows_raw(void)
{
	struct fplinux_session_battery battery = { 0 };
	struct fplinux_session_supply_sample sample = voltage_sample(3740000);

	CHECK(fplinux_session_battery_update(&battery, &sample, 0));
	CHECK(fplinux_session_battery_percent(&battery) == 20);
	/* A 10-point drop just after the stale limit is shown in full. */
	sample.voltage_uv = 3680000;
	CHECK(fplinux_session_battery_update(&battery, &sample, STALE));
	CHECK(fplinux_session_battery_percent(&battery) == 10);
	/* A 10-point rise one poll later is smoothed. */
	sample.voltage_uv = 3740000;
	CHECK(fplinux_session_battery_update(&battery, &sample,
					     STALE + 60000U));
	CHECK(fplinux_session_battery_percent(&battery) == 13);
	CHECK(fplinux_session_battery_raw_percent(&battery) == 20);
}

static void test_stale_drop_22_to_3_is_shown_at_once(void)
{
	struct fplinux_session_battery battery = { 0 };
	struct fplinux_session_supply_sample sample = voltage_sample(3746000);

	CHECK(fplinux_session_battery_update(&battery, &sample, 0));
	CHECK(fplinux_session_battery_percent(&battery) == 22);
	/* After a long display-off gap the drop to 3 % is not smoothed. */
	sample.voltage_uv = 3520000;
	CHECK(fplinux_session_battery_update(&battery, &sample, STALE));
	CHECK(fplinux_session_battery_percent(&battery) == 3);
	CHECK(fplinux_session_battery_raw_percent(&battery) == 3);
}

static void test_battery_alerts_fire_once_per_threshold(void)
{
	struct fplinux_session_battery_alerts alerts = { 0 };

	CHECK(fplinux_session_battery_alert(&alerts, 50, 50, false) ==
	      ALERT_NONE);
	CHECK(fplinux_session_battery_alert(&alerts, 14, 14, false) ==
	      ALERT_LOW);
	CHECK(fplinux_session_battery_alert(&alerts, 13, 13, false) ==
	      ALERT_NONE);
	CHECK(fplinux_session_battery_alert(&alerts, 4, 4, false) ==
	      ALERT_CRITICAL);
	CHECK(fplinux_session_battery_alert(&alerts, 3, 3, false) ==
	      ALERT_NONE);
	/* Recovering to 9 % re-arms nothing; 10 % re-arms critical only. */
	CHECK(fplinux_session_battery_alert(&alerts, 9, 9, false) ==
	      ALERT_NONE);
	CHECK(fplinux_session_battery_alert(&alerts, 12, 12, false) ==
	      ALERT_NONE);
	CHECK(fplinux_session_battery_alert(&alerts, 4, 4, false) ==
	      ALERT_CRITICAL);
	/* Low re-arms only at 20 %. */
	CHECK(fplinux_session_battery_alert(&alerts, 19, 19, false) ==
	      ALERT_NONE);
	CHECK(fplinux_session_battery_alert(&alerts, 14, 14, false) ==
	      ALERT_NONE);
	CHECK(fplinux_session_battery_alert(&alerts, 20, 20, false) ==
	      ALERT_NONE);
	CHECK(fplinux_session_battery_alert(&alerts, 14, 14, false) ==
	      ALERT_LOW);
}

static void test_battery_alerts_wait_while_charging(void)
{
	struct fplinux_session_battery_alerts alerts = { 0 };

	CHECK(fplinux_session_battery_alert(&alerts, 3, 3, true) == ALERT_NONE);
	CHECK(fplinux_session_battery_alert(&alerts, 3, 3, false) ==
	      ALERT_CRITICAL);
	CHECK(fplinux_session_battery_alert(&alerts, 3, 3, false) ==
	      ALERT_NONE);
}

static void test_charging_clears_both_flags(void)
{
	struct fplinux_session_battery_alerts alerts = { 0 };

	CHECK(fplinux_session_battery_alert(&alerts, 14, 14, false) ==
	      ALERT_LOW);
	CHECK(alerts.low_sent && !alerts.critical_sent);
	CHECK(fplinux_session_battery_alert(&alerts, 14, 14, true) ==
	      ALERT_NONE);
	CHECK(!alerts.low_sent && !alerts.critical_sent);
	/* Unplugged at 14 %: a new discharge cycle raises low again. */
	CHECK(fplinux_session_battery_alert(&alerts, 14, 14, false) ==
	      ALERT_LOW);
	CHECK(fplinux_session_battery_alert(&alerts, 4, 4, false) ==
	      ALERT_CRITICAL);
	CHECK(alerts.low_sent && alerts.critical_sent);
	CHECK(fplinux_session_battery_alert(&alerts, 4, 4, true) == ALERT_NONE);
	CHECK(!alerts.low_sent && !alerts.critical_sent);
	CHECK(fplinux_session_battery_alert(&alerts, 4, 4, false) ==
	      ALERT_CRITICAL);
}

static void test_critical_follows_raw_level(void)
{
	struct fplinux_session_battery_alerts alerts = { 0 };

	/* Smoothing still shows 8 % while the reading is 3 %. */
	CHECK(fplinux_session_battery_alert(&alerts, 8, 3, false) ==
	      ALERT_CRITICAL);
	CHECK(fplinux_session_battery_alert(&alerts, 8, 3, false) ==
	      ALERT_NONE);
	/* A raw dip alone does not raise low when the shown level is fine. */
	memset(&alerts, 0, sizeof(alerts));
	CHECK(fplinux_session_battery_alert(&alerts, 20, 14, false) ==
	      ALERT_NONE);
}

/*
 * A smoothed level above the re-arm point with a raw level below the critical
 * one, as after a fast drop, raises critical once, not on every poll.
 */
static void test_critical_fires_once_while_smoothing_lags(void)
{
	struct fplinux_session_battery_alerts alerts = { 0 };

	CHECK(fplinux_session_battery_alert(&alerts, 14, 4, false) ==
	      ALERT_CRITICAL);
	for (int i = 0; i < 5; ++i)
		CHECK(fplinux_session_battery_alert(&alerts, 14, 4, false) ==
		      ALERT_NONE);
}

static void test_alerts_rearm_on_shown_level(void)
{
	struct fplinux_session_battery_alerts alerts = { 0 };

	CHECK(fplinux_session_battery_alert(&alerts, 8, 3, false) ==
	      ALERT_CRITICAL);
	/* A raw 10 % with the shown level still at 8 % re-arms nothing. */
	CHECK(fplinux_session_battery_alert(&alerts, 8, 10, false) ==
	      ALERT_NONE);
	CHECK(alerts.critical_sent);
	/* Shown 10 % with the raw level still at 3 % re-arms nothing either. */
	CHECK(fplinux_session_battery_alert(&alerts, 10, 3, false) ==
	      ALERT_NONE);
	CHECK(alerts.critical_sent);
	/* Both at 10 %: critical re-arms and fires again on the next drop. */
	CHECK(fplinux_session_battery_alert(&alerts, 10, 10, false) ==
	      ALERT_NONE);
	CHECK(fplinux_session_battery_alert(&alerts, 10, 3, false) ==
	      ALERT_CRITICAL);
	/* Shown 19 %: low stays sent; shown 20 %: low re-arms. */
	CHECK(fplinux_session_battery_alert(&alerts, 19, 19, false) ==
	      ALERT_NONE);
	CHECK(alerts.low_sent);
	CHECK(fplinux_session_battery_alert(&alerts, 20, 20, false) ==
	      ALERT_NONE);
	CHECK(!alerts.low_sent && !alerts.critical_sent);
}

int main(void)
{
	test_supply_read_phone_battery_without_charger();
	test_supply_read_unknown_type_charger_online_is_charging();
	test_supply_read_capacity_is_clamped();
	test_supply_read_absent_or_missing_battery_shows_nothing();
	test_supply_read_battery_online_does_not_mean_charging();
	test_supply_read_picks_first_battery_by_name();
	test_ocv_table_points_and_interpolation();
	test_ocv_compensates_internal_resistance();
	test_battery_level_smooths_voltage_estimates();
	test_fresh_reading_still_smooths();
	test_stale_reading_shows_raw();
	test_stale_drop_22_to_3_is_shown_at_once();
	test_battery_alerts_fire_once_per_threshold();
	test_battery_alerts_wait_while_charging();
	test_charging_clears_both_flags();
	test_critical_follows_raw_level();
	test_alerts_rearm_on_shown_level();
	test_critical_fires_once_while_smoothing_lags();
	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	puts("session-battery-test: PASS");
	return 0;
}
