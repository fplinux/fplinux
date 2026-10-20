/* SPDX-License-Identifier: GPL-2.0-only */
#include "launcher-power.h"

static const int step_thresholds[FPLINUX_LAUNCHER_BATTERY_STEP_COUNT] = {
	15, 35, 55, 75, 95,
};

int fplinux_launcher_battery_steps(int percent)
{
	int steps = 0;

	while (steps < FPLINUX_LAUNCHER_BATTERY_STEP_COUNT &&
	       percent >= step_thresholds[steps])
		++steps;
	return steps;
}
