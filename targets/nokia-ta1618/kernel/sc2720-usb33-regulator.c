// SPDX-License-Identifier: GPL-2.0-only
#include <linux/bitops.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regulator/driver.h>

#include <linux/soc/sprd/ums9117-adi.h>

#define SC2720_LDO_PD_CTRL 0x0d30U
#define SC2720_LDO_USB_PD BIT(0)
#define SC2720_LDO_USB_VSEL 0x0d34U
#define SC2720_LDO_USB_VSEL_MASK GENMASK(6, 0)
#define SC2720_LDO_USB_VSEL_3300_MV 0x60U
#define SC2720_LDO_USB_UV 3300000

static int sc2720_usb33_update_power_down(u16 value)
{
	struct ums9117_adi_transaction transaction = {};
	int end_ret;
	int ret;

	ret = ums9117_adi_begin(&transaction);
	if (ret)
		return ret;
	ret = ums9117_adi_update_bits(&transaction, SC2720_LDO_PD_CTRL,
				      SC2720_LDO_USB_PD, value);
	end_ret = ums9117_adi_end(&transaction);
	return ret ? ret : end_ret;
}

static int sc2720_usb33_enable(struct regulator_dev *rdev)
{
	struct ums9117_adi_transaction transaction = {};
	int end_ret;
	int ret;

	ret = ums9117_adi_begin(&transaction);
	if (ret)
		return ret;
	ret = ums9117_adi_update_bits(&transaction, SC2720_LDO_USB_VSEL,
				      SC2720_LDO_USB_VSEL_MASK,
				      SC2720_LDO_USB_VSEL_3300_MV);
	if (!ret)
		ret = ums9117_adi_update_bits(&transaction, SC2720_LDO_PD_CTRL,
					      SC2720_LDO_USB_PD, 0);
	end_ret = ums9117_adi_end(&transaction);
	return ret ? ret : end_ret;
}

static int sc2720_usb33_disable(struct regulator_dev *rdev)
{
	return sc2720_usb33_update_power_down(SC2720_LDO_USB_PD);
}

static int sc2720_usb33_is_enabled(struct regulator_dev *rdev)
{
	struct ums9117_adi_transaction transaction = {};
	u16 value;
	int end_ret;
	int ret;

	ret = ums9117_adi_begin(&transaction);
	if (ret)
		return ret;
	ret = ums9117_adi_read(&transaction, SC2720_LDO_PD_CTRL, &value);
	end_ret = ums9117_adi_end(&transaction);
	if (ret)
		return ret;
	if (end_ret)
		return end_ret;
	return !(value & SC2720_LDO_USB_PD);
}

static const struct regulator_ops sc2720_usb33_ops = {
	.enable = sc2720_usb33_enable,
	.disable = sc2720_usb33_disable,
	.is_enabled = sc2720_usb33_is_enabled,
};

static const struct regulator_desc sc2720_usb33_desc = {
	.name = "sc2720-usb33",
	.ops = &sc2720_usb33_ops,
	.type = REGULATOR_VOLTAGE,
	.owner = THIS_MODULE,
	.n_voltages = 1,
	.fixed_uV = SC2720_LDO_USB_UV,
};

static int sc2720_usb33_probe(struct platform_device *pdev)
{
	struct regulator_config config = {
		.dev = &pdev->dev,
		.of_node = pdev->dev.of_node,
	};
	struct regulator_dev *rdev;

	rdev = devm_regulator_register(&pdev->dev, &sc2720_usb33_desc, &config);
	return PTR_ERR_OR_ZERO(rdev);
}

static const struct of_device_id sc2720_usb33_of_match[] = {
	{ .compatible = "fplinux,sc2720-usb33-regulator" },
	{}
};
MODULE_DEVICE_TABLE(of, sc2720_usb33_of_match);

static struct platform_driver sc2720_usb33_driver = {
	.probe = sc2720_usb33_probe,
	.driver = {
		.name = "sc2720-usb33-regulator",
		.of_match_table = sc2720_usb33_of_match,
	},
};
module_platform_driver(sc2720_usb33_driver);

MODULE_DESCRIPTION("SC2720 USB 3.3 V regulator");
MODULE_LICENSE("GPL");
