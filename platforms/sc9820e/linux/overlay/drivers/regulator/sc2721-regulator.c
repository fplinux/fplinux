// SPDX-License-Identifier: GPL-2.0-only

#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/regulator/driver.h>

#include "sc27xx-regulator-core.h"

#define SC2721_PWR_WR_PROT 0xefc
#define SC2721_WR_UNLOCK_VALUE 0x6e7f

#define SC2721_LDO_SDCORE_PD 0xcf4
#define SC2721_LDO_SDCORE_VSEL 0xcf8
#define SC2721_LDO_SDIO_PD 0xd00
#define SC2721_LDO_SDIO_VSEL 0xd04
#define SC2721_LDO_WIFIPA_PD 0xd10
#define SC2721_LDO_WIFIPA_VSEL 0xd14
#define SC2721_LDO_CON_PD 0xd34
#define SC2721_LDO_CON_VSEL 0xd38

#define SC2721_DCDC_PD 0xc1c
#define SC2721_DCDC_CORE_PD BIT(5)
#define SC2721_DCDC_CORE_VSEL 0xc54
#define SC2721_DCDC_CORE_VSEL_MASK GENMASK(8, 0)
#define SC2721_DCDC_CORE_MIN_UV 400000
#define SC2721_DCDC_CORE_STEP_UV 3125

static int read_core_voltage(struct regmap *regmap)
{
	unsigned int value;
	int ret;

	ret = regmap_read(regmap, SC2721_DCDC_PD, &value);
	if (ret)
		return ret;
	if (value & SC2721_DCDC_CORE_PD)
		return -EIO;
	ret = regmap_read(regmap, SC2721_DCDC_CORE_VSEL, &value);
	if (ret)
		return ret;
	return SC2721_DCDC_CORE_MIN_UV +
	       (value & SC2721_DCDC_CORE_VSEL_MASK) * SC2721_DCDC_CORE_STEP_UV;
}

static int sc2721_core_get_voltage(struct regulator_dev *rdev)
{
	return read_core_voltage(rdev_get_regmap(rdev));
}

/* The core rail is shared; this provider has no voltage or power setters. */
static const struct regulator_ops sc2721_core_ops = {
	.get_voltage = sc2721_core_get_voltage,
	.is_enabled = regulator_is_enabled_regmap,
};

static const struct sc27xx_regulator_desc sc2721_regulators[] = {
	{
		.desc = {
			.name = "vddcore",
			.of_match = "dcdc@1",
			.of_match_full_name = true,
			.regulators_node = "regulators",
			.id = 1,
			.ops = &sc2721_core_ops,
			.type = REGULATOR_VOLTAGE,
			.owner = THIS_MODULE,
			.enable_reg = SC2721_DCDC_PD,
			.enable_mask = SC2721_DCDC_CORE_PD,
			.enable_is_inverted = true,
		},
	},
	{
		.desc = {
		.name = "vddsdcore",
		.of_match = "ldo@11",
		.of_match_full_name = true,
		.regulators_node = "regulators",
		.id = 11,
		.ops = &sc27xx_regu_linear_ops,
		.type = REGULATOR_VOLTAGE,
		.owner = THIS_MODULE,
		.n_voltages = 256,
		.min_uV = 1200000,
		.uV_step = 10000,
		.enable_reg = SC2721_LDO_SDCORE_PD,
		.enable_mask = BIT(0),
		.enable_is_inverted = true,
		.vsel_reg = SC2721_LDO_SDCORE_VSEL,
		.vsel_mask = GENMASK(7, 0),
		},
	},
	{
		.desc = {
		.name = "vddsdio",
		.of_match = "ldo@12",
		.of_match_full_name = true,
		.regulators_node = "regulators",
		.id = 12,
		.ops = &sc27xx_regu_linear_ops,
		.type = REGULATOR_VOLTAGE,
		.owner = THIS_MODULE,
		.n_voltages = 256,
		.min_uV = 1200000,
		.uV_step = 10000,
		.enable_reg = SC2721_LDO_SDIO_PD,
		.enable_mask = BIT(0),
		.enable_is_inverted = true,
		.vsel_reg = SC2721_LDO_SDIO_VSEL,
		.vsel_mask = GENMASK(7, 0),
		},
	},
	{
		.desc = {
			.name = "vddwifipa",
			.of_match = "ldo@14",
			.of_match_full_name = true,
			.regulators_node = "regulators",
			.id = 14,
			.ops = &sc27xx_regu_linear_ops,
			.type = REGULATOR_VOLTAGE,
			.owner = THIS_MODULE,
			.n_voltages = 256,
			.min_uV = 1200000,
			.uV_step = 10000,
			.enable_reg = SC2721_LDO_WIFIPA_PD,
			.enable_mask = BIT(0),
			.enable_is_inverted = true,
			.vsel_reg = SC2721_LDO_WIFIPA_VSEL,
			.vsel_mask = GENMASK(7, 0),
		},
	},
	{
		.desc = {
			.name = "vddcon",
			.of_match = "ldo@18",
			.of_match_full_name = true,
			.regulators_node = "regulators",
			.id = 18,
			.ops = &sc27xx_regu_linear_ops,
			.type = REGULATOR_VOLTAGE,
			.owner = THIS_MODULE,
			.n_voltages = 81,
			.min_uV = 1100000,
			.uV_step = 6250,
			.enable_reg = SC2721_LDO_CON_PD,
			.enable_mask = BIT(0),
			.enable_is_inverted = true,
			.vsel_reg = SC2721_LDO_CON_VSEL,
			.vsel_mask = GENMASK(6, 0),
		},
	},
};

static const struct sc27xx_regulator_data sc2721_regulator_data = {
	.regulators = sc2721_regulators,
	.num_regulators = ARRAY_SIZE(sc2721_regulators),
	.unlock_reg = SC2721_PWR_WR_PROT,
	.unlock_value = SC2721_WR_UNLOCK_VALUE,
};

static int sc2721_regulator_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct regmap *regmap = dev_get_regmap(dev->parent, NULL);
	int ret;

	if (!regmap)
		return dev_err_probe(dev, -ENODEV,
				     "failed to get parent regmap\n");
	/* Registration must not label a powered-down read-only rail as on. */
	ret = read_core_voltage(regmap);
	if (ret < 0)
		return dev_err_probe(dev, ret,
				     "cannot verify inherited core supply\n");
	return sc27xx_regulator_probe(pdev);
}

static const struct of_device_id sc2721_regulator_of_match[] = {
	{ .compatible = "sprd,sc2721-regulator",
	  .data = &sc2721_regulator_data },
	{}
};
MODULE_DEVICE_TABLE(of, sc2721_regulator_of_match);

static struct platform_driver sc2721_regulator_driver = {
	.probe = sc2721_regulator_probe,
	.driver = {
		.name = "sc2721-regulator",
		.of_match_table = sc2721_regulator_of_match,
	},
};
module_platform_driver(sc2721_regulator_driver);

MODULE_DESCRIPTION("SC2721 core, SD and connectivity voltage regulators");
MODULE_LICENSE("GPL");
