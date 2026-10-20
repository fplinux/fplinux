/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_SC27XX_REGULATOR_CORE_H
#define FPLINUX_SC27XX_REGULATOR_CORE_H

#include <linux/regulator/driver.h>
#include <linux/types.h>

struct platform_device;

struct sc27xx_regulator_desc {
	struct regulator_desc desc;
	u32 fixed_vsel;
};

struct sc27xx_regulator_data {
	const struct sc27xx_regulator_desc *regulators;
	unsigned int num_regulators;
	u32 unlock_reg;
	u32 unlock_value;
};

extern const struct regulator_ops sc27xx_regu_linear_ops;

int sc27xx_regulator_probe(struct platform_device *pdev);

#endif
