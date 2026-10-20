/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_LAUNCHER_POWER_H
#define FPLINUX_LAUNCHER_POWER_H

/*
 * The session manager owns the battery: it reads, smooths and raises the low
 * and critical alerts, and reports the level to the launcher in ACTIVE. The
 * launcher only draws it.
 */
#define FPLINUX_LAUNCHER_BATTERY_STEP_COUNT 5
#define FPLINUX_LAUNCHER_BATTERY_LOW_PERCENT 15

/* Number of filled bars, 0 to FPLINUX_LAUNCHER_BATTERY_STEP_COUNT. */
int fplinux_launcher_battery_steps(int percent);

#endif
