// SPDX-License-Identifier: GPL-2.0-only
#include <linux/bitops.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/regmap.h>

#define SC2720_CHGR_STATUS 0xe14U
#define SC2720_CHGR_STATUS_CHARGER_ON BIT(3)
#define SC2720_CHGR_STATUS_CDP BIT(5)
#define SC2720_CHGR_STATUS_DCP BIT(6)
#define SC2720_CHGR_STATUS_SDP BIT(7)
#define SC2720_CHGR_STATUS_TYPE_MASK GENMASK(7, 5)
#define SC2720_CHGR_STATUS_DETECT_DONE BIT(11)

static enum power_supply_usb_type sc2720_charger_usb_type(unsigned int status)
{
	if (!(status & SC2720_CHGR_STATUS_CHARGER_ON) ||
	    !(status & SC2720_CHGR_STATUS_DETECT_DONE))
		return POWER_SUPPLY_USB_TYPE_UNKNOWN;

	switch (status & SC2720_CHGR_STATUS_TYPE_MASK) {
	case SC2720_CHGR_STATUS_SDP:
		return POWER_SUPPLY_USB_TYPE_SDP;
	case SC2720_CHGR_STATUS_CDP:
		return POWER_SUPPLY_USB_TYPE_CDP;
	case SC2720_CHGR_STATUS_DCP:
		return POWER_SUPPLY_USB_TYPE_DCP;
	default:
		return POWER_SUPPLY_USB_TYPE_UNKNOWN;
	}
}

static int sc2720_charger_get_property(struct power_supply *supply,
				       enum power_supply_property property,
				       union power_supply_propval *value)
{
	struct regmap *regmap = power_supply_get_drvdata(supply);
	unsigned int status;
	int ret;

	if (property != POWER_SUPPLY_PROP_ONLINE &&
	    property != POWER_SUPPLY_PROP_USB_TYPE)
		return -EINVAL;
	ret = regmap_read(regmap, SC2720_CHGR_STATUS, &status);
	if (ret)
		return ret;
	if (property == POWER_SUPPLY_PROP_ONLINE)
		value->intval = !!(status & SC2720_CHGR_STATUS_CHARGER_ON);
	else
		value->intval = sc2720_charger_usb_type(status);
	return 0;
}

static enum power_supply_property sc2720_charger_properties[] = {
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_USB_TYPE,
};

static const struct power_supply_desc sc2720_charger_description = {
	.name = "sc2720-charger",
	.type = POWER_SUPPLY_TYPE_UNKNOWN,
	.usb_types = BIT(POWER_SUPPLY_USB_TYPE_UNKNOWN) |
		     BIT(POWER_SUPPLY_USB_TYPE_SDP) |
		     BIT(POWER_SUPPLY_USB_TYPE_CDP) |
		     BIT(POWER_SUPPLY_USB_TYPE_DCP),
	.properties = sc2720_charger_properties,
	.num_properties = ARRAY_SIZE(sc2720_charger_properties),
	.get_property = sc2720_charger_get_property,
};

static int sc2720_charger_probe(struct platform_device *pdev)
{
	struct power_supply_config config = {};
	struct power_supply *supply;
	struct regmap *regmap = dev_get_regmap(pdev->dev.parent, NULL);
	unsigned int status;
	int ret;

	if (!regmap)
		return -EPROBE_DEFER;
	ret = regmap_read(regmap, SC2720_CHGR_STATUS, &status);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "charger status unavailable\n");

	config.fwnode = dev_fwnode(&pdev->dev);
	config.drv_data = regmap;
	supply = devm_power_supply_register(
		&pdev->dev, &sc2720_charger_description, &config);
	return PTR_ERR_OR_ZERO(supply);
}

static const struct of_device_id sc2720_charger_of_match[] = {
	{ .compatible = "sprd,ums9117-sc2720-charger" },
	{},
};
MODULE_DEVICE_TABLE(of, sc2720_charger_of_match);

static struct platform_driver sc2720_charger_driver = {
	.probe = sc2720_charger_probe,
	.driver = {
		.name = "sc2720-charger-ums9117",
		.of_match_table = sc2720_charger_of_match,
	},
};
module_platform_driver(sc2720_charger_driver);

MODULE_DESCRIPTION("Read-only SC2720 charger status on UMS9117");
MODULE_LICENSE("GPL");
