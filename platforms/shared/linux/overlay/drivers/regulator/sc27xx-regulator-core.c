// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2017 Spreadtrum Communications Inc.
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/regmap.h>

#include "sc27xx-regulator-core.h"

const struct regulator_ops sc27xx_regu_linear_ops = {
	.enable = regulator_enable_regmap,
	.disable = regulator_disable_regmap,
	.is_enabled = regulator_is_enabled_regmap,
	.list_voltage = regulator_list_voltage_linear,
	.map_voltage = regulator_map_voltage_linear,
	.get_voltage_sel = regulator_get_voltage_sel_regmap,
	.set_voltage_sel = regulator_set_voltage_sel_regmap,
};
EXPORT_SYMBOL_GPL(sc27xx_regu_linear_ops);

int sc27xx_regulator_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	const struct sc27xx_regulator_data *data = device_get_match_data(dev);
	struct regulator_config config = {};
	const struct regulator_desc *desc;
	struct regulator_dev *rdev;
	struct regmap *regmap;
	unsigned int i;
	int ret;

	if (!data)
		return -EINVAL;

	regmap = dev_get_regmap(dev->parent, NULL);
	if (!regmap)
		return dev_err_probe(dev, -ENODEV,
				     "failed to get parent regmap\n");

	if (data->unlock_reg) {
		ret = regmap_write(regmap, data->unlock_reg,
				   data->unlock_value);
		if (ret)
			return dev_err_probe(
				dev, ret, "failed to release regulator lock\n");
	}

	config.dev = dev;
	config.regmap = regmap;

	for (i = 0; i < data->num_regulators; i++) {
		desc = &data->regulators[i].desc;
		rdev = devm_regulator_register(dev, desc, &config);
		if (IS_ERR(rdev))
			return dev_err_probe(
				dev, PTR_ERR(rdev),
				"failed to register regulator %s\n",
				desc->name);
	}

	return 0;
}
EXPORT_SYMBOL_GPL(sc27xx_regulator_probe);

MODULE_DESCRIPTION("SC27xx regulator registration");
MODULE_LICENSE("GPL");
