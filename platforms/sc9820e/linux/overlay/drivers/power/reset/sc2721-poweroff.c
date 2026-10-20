// SPDX-License-Identifier: GPL-2.0-only
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/processor.h>
#include <linux/reboot.h>
#include <linux/regmap.h>
#include <linux/spi/spi.h>
#include <linux/spi/sprd-adi.h>

#define SC2721_CHIP_ID_HIGH 0xc04
#define SC2721_CHIP_ID 0x2721
#define SC2721_POWER_OFF_WAIT_MS 50

static int __noreturn sc2721_power_off(struct sys_off_data *data)
{
	struct spi_device *spi = data->cb_data;
	int ret;

	ret = sprd_adi_sc2721_power_off(spi);
	if (ret)
		dev_emerg(&spi->dev, "power-off failed: %pe\n", ERR_PTR(ret));
	else {
		mdelay(SC2721_POWER_OFF_WAIT_MS);
		dev_emerg(&spi->dev,
			  "power-off requested but CPU still runs\n");
	}

	/* Shutdown is final even when the power request fails. */
	for (;;)
		cpu_relax();
}

static int sc2721_poweroff_probe(struct platform_device *pdev)
{
	struct device *parent = pdev->dev.parent;
	struct spi_device *spi;
	struct regmap *map;
	u32 value;
	int ret;

	if (!parent || parent->bus != &spi_bus_type ||
	    !of_device_is_compatible(parent->of_node, "sprd,sc2721"))
		return -ENODEV;
	spi = to_spi_device(parent);
	if (!spi->controller->dev.parent ||
	    !of_device_is_compatible(spi->controller->dev.parent->of_node,
				     "sprd,r3p0-adi"))
		return -ENODEV;
	map = dev_get_regmap(parent, NULL);
	if (!map)
		return -EPROBE_DEFER;
	ret = regmap_read(map, SC2721_CHIP_ID_HIGH, &value);
	if (ret)
		return ret;
	if (value != SC2721_CHIP_ID)
		return -ENODEV;

	/* This exact PMIC sequence must run before a firmware fallback. */
	return devm_register_sys_off_handler(&pdev->dev, SYS_OFF_MODE_POWER_OFF,
					     SYS_OFF_PRIO_HIGH,
					     sc2721_power_off, spi);
}

static const struct of_device_id sc2721_poweroff_of_match[] = {
	{ .compatible = "sprd,sc2721-poweroff" },
	{},
};
MODULE_DEVICE_TABLE(of, sc2721_poweroff_of_match);

static struct platform_driver sc2721_poweroff_driver = {
	.probe = sc2721_poweroff_probe,
	.driver = {
		.name = "sc2721-poweroff",
		.of_match_table = sc2721_poweroff_of_match,
	},
};
module_platform_driver(sc2721_poweroff_driver);

MODULE_DESCRIPTION("SC2721 power-off through atomic ADI access");
MODULE_LICENSE("GPL");
