// SPDX-License-Identifier: GPL-2.0-only
#include <linux/bitops.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/soc/sprd/ums9117-adi.h>

#define SC2720_CHIP_ID_LOW 0xc00U
#define SC2720_CHIP_ID_HIGH 0xc04U
#define SC2720_CHGR_STATUS 0xe14U
#define SC2720_EXPECTED_ID_LOW 0xa003U
#define SC2720_EXPECTED_ID_HIGH 0x2720U
#define SC2720_CHGR_STATUS_CHARGER_ON BIT(3)

static int ta1618_charger_read_status(u16 *status)
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

static int ta1618_charger_get_property(struct power_supply *supply,
				       enum power_supply_property property,
				       union power_supply_propval *value)
{
	u16 status;
	int ret;

	if (property != POWER_SUPPLY_PROP_ONLINE)
		return -EINVAL;
	ret = ta1618_charger_read_status(&status);
	if (ret)
		return ret;
	value->intval = !!(status & SC2720_CHGR_STATUS_CHARGER_ON);
	return 0;
}

static enum power_supply_property ta1618_charger_properties[] = {
	POWER_SUPPLY_PROP_ONLINE,
};

static const struct power_supply_desc ta1618_charger_description = {
	.name = "ta1618-charger",
	.type = POWER_SUPPLY_TYPE_UNKNOWN,
	.properties = ta1618_charger_properties,
	.num_properties = ARRAY_SIZE(ta1618_charger_properties),
	.get_property = ta1618_charger_get_property,
};

static int ta1618_charger_probe(struct platform_device *pdev)
{
	struct power_supply_config config = {};
	struct power_supply *supply;
	u16 status;
	int ret;

	ret = ta1618_charger_read_status(&status);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "SC2720 charger status unavailable\n");

	config.fwnode = dev_fwnode(&pdev->dev);
	supply = devm_power_supply_register(
		&pdev->dev, &ta1618_charger_description, &config);
	return PTR_ERR_OR_ZERO(supply);
}

static const struct of_device_id ta1618_charger_of_match[] = {
	{ .compatible = "fplinux,ta1618-sc2720-charger" },
	{},
};
MODULE_DEVICE_TABLE(of, ta1618_charger_of_match);

static struct platform_driver ta1618_charger_driver = {
	.probe = ta1618_charger_probe,
	.driver = {
		.name = "ta1618-sc2720-charger",
		.of_match_table = ta1618_charger_of_match,
	},
};
module_platform_driver(ta1618_charger_driver);

MODULE_DESCRIPTION("Nokia TA-1618 read-only SC2720 charger status");
MODULE_LICENSE("GPL");
