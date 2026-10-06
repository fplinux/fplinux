/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_UMS9117_SC2720_CALIBRATION_H
#define FPLINUX_UMS9117_SC2720_CALIBRATION_H

struct device;
struct regmap;

/* The PCM owner serializes calibration with the other codec operations. */
int ums9117_sc2720_calibrate_headphones(struct device *dev,
					struct regmap *regmap);

#endif
