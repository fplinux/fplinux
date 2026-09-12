// SPDX-License-Identifier: GPL-2.0-only
#include <linux/bitops.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/property.h>
#include <linux/slab.h>
#include <linux/soc/sprd/ums9117-adi.h>

#define SC2720_CHIP_ID_LOW 0xc00U
#define SC2720_CHIP_ID_HIGH 0xc04U
#define SC2720_CHGR_STATUS 0xe14U
#define SC2720_EXPECTED_ID_LOW 0xa003U
#define SC2720_EXPECTED_ID_HIGH 0x2720U
#define SC2720_CHGR_STATUS_CHARGER_ON BIT(3)

static int sc2720_charger_read_status(u16 *status)
{
	struct ums9117_adi_transaction transaction = {};
	u16 id_high;
	u16 id_low;
	int end_ret;
	int ret;

	ret = ums9117_adi_begin(&transaction);
	if (ret)
		return ret;
	ret = ums9117_adi_read(&transaction, SC2720_CHIP_ID_LOW, &id_low);
	if (!ret)
		ret = ums9117_adi_read(&transaction, SC2720_CHIP_ID_HIGH,
				       &id_high);
	if (!ret)
		ret = ums9117_adi_read(&transaction, SC2720_CHGR_STATUS,
				       status);
	end_ret = ums9117_adi_end(&transaction);
	if (!ret)
		ret = end_ret;
	if (ret)
		return ret;
	if (id_low != SC2720_EXPECTED_ID_LOW ||
	    id_high != SC2720_EXPECTED_ID_HIGH)
		return -ENODEV;
	return 0;
}

static int sc2720_charger_get_property(struct power_supply *supply,
				       enum power_supply_property property,
				       union power_supply_propval *value)
{
	u16 status;
	int ret;

	if (property != POWER_SUPPLY_PROP_ONLINE)
		return -EINVAL;
	ret = sc2720_charger_read_status(&status);
	if (ret)
		return ret;
	value->intval = !!(status & SC2720_CHGR_STATUS_CHARGER_ON);
	return 0;
}

static enum power_supply_property sc2720_charger_properties[] = {
	POWER_SUPPLY_PROP_ONLINE,
};

static const struct power_supply_desc sc2720_charger_description = {
	.type = POWER_SUPPLY_TYPE_UNKNOWN,
	.properties = sc2720_charger_properties,
	.num_properties = ARRAY_SIZE(sc2720_charger_properties),
	.get_property = sc2720_charger_get_property,
};

static int sc2720_charger_probe(struct platform_device *pdev)
{
	struct power_supply_config config = {};
	struct power_supply_desc *description;
	struct power_supply *supply;
	u16 status;
	int ret;

	description = devm_kmemdup(&pdev->dev, &sc2720_charger_description,
				   sizeof(*description), GFP_KERNEL);
	if (!description)
		return -ENOMEM;
	ret = device_property_read_string(&pdev->dev, "label",
					  &description->name);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "missing supply label\n");

	ret = sc2720_charger_read_status(&status);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "SC2720 charger status unavailable\n");

	config.fwnode = dev_fwnode(&pdev->dev);
	supply = devm_power_supply_register(&pdev->dev, description, &config);
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
