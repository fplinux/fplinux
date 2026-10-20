// SPDX-License-Identifier: GPL-2.0-only

#include <linux/backlight.h>
#include <linux/bitops.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/slab.h>

#define SC2721_MODULE_EN0 0xc08
#define SC2721_MODULE_EN0_BLTC_EN BIT(9)
#define SC2721_RTC_CLK_EN0 0xc10
#define SC2721_RTC_CLK_EN0_BLTC_EN BIT(7)
#define SC2721_RGB_CTRL 0xea0
#define SC2721_RGB_CTRL_PD_MASK (BIT(2) | BIT(0))
#define SC2721_RGB_CTRL_CURRENT_SHIFT 4
#define SC2721_BLTC_CTRL 0x180
#define SC2721_BLTC_CTRL_ON 0x0fff

struct sc2721_backlight {
	struct device *dev;
	struct regmap *regmap;
};

static unsigned int current_word(unsigned int brightness)
{
	unsigned int code;

	/* Low nonzero levels keep the backlight on with current code zero. */
	if (brightness < 25)
		return 0;

	code = ((8000 * brightness / 765) - 169) / 84;
	return code << SC2721_RGB_CTRL_CURRENT_SHIFT;
}

static int power_off(struct sc2721_backlight *backlight)
{
	int ret, error;

	/* Attempt every OFF step, retaining the first transport error. */
	ret = regmap_update_bits(backlight->regmap, SC2721_RGB_CTRL,
				 SC2721_RGB_CTRL_PD_MASK,
				 SC2721_RGB_CTRL_PD_MASK);

	error = regmap_write(backlight->regmap, SC2721_BLTC_CTRL, 0);
	if (!ret)
		ret = error;

	error = regmap_update_bits(backlight->regmap, SC2721_RTC_CLK_EN0,
				   SC2721_RTC_CLK_EN0_BLTC_EN, 0);
	if (!ret)
		ret = error;

	error = regmap_update_bits(backlight->regmap, SC2721_MODULE_EN0,
				   SC2721_MODULE_EN0_BLTC_EN, 0);
	if (!ret)
		ret = error;

	return ret;
}

static int set_brightness(struct sc2721_backlight *backlight,
			  unsigned int brightness)
{
	int ret, off_error;

	ret = regmap_update_bits(backlight->regmap, SC2721_MODULE_EN0,
				 SC2721_MODULE_EN0_BLTC_EN,
				 SC2721_MODULE_EN0_BLTC_EN);
	if (ret)
		goto turn_off;

	ret = regmap_update_bits(backlight->regmap, SC2721_RTC_CLK_EN0,
				 SC2721_RTC_CLK_EN0_BLTC_EN,
				 SC2721_RTC_CLK_EN0_BLTC_EN);
	if (ret)
		goto turn_off;

	/* The current word also clears the software and sleep power-down bits. */
	ret = regmap_write(backlight->regmap, SC2721_RGB_CTRL,
			   current_word(brightness));
	if (ret)
		goto turn_off;

	ret = regmap_write(backlight->regmap, SC2721_BLTC_CTRL,
			   SC2721_BLTC_CTRL_ON);
	if (!ret)
		return 0;

turn_off:
	off_error = power_off(backlight);
	if (off_error)
		dev_err(backlight->dev,
			"Failed to turn off after enable error %d: %d\n", ret,
			off_error);

	return ret;
}

static int sc2721_backlight_update_status(struct backlight_device *bl)
{
	struct sc2721_backlight *backlight = bl_get_data(bl);
	unsigned int brightness = backlight_get_brightness(bl);

	if (!brightness)
		return power_off(backlight);

	return set_brightness(backlight, brightness);
}

static const struct backlight_ops sc2721_backlight_ops = {
	.options = BL_CORE_SUSPENDRESUME,
	.update_status = sc2721_backlight_update_status,
};

static void sc2721_backlight_cleanup(void *data)
{
	struct sc2721_backlight *backlight = data;
	int ret;

	ret = power_off(backlight);
	if (ret)
		dev_err(backlight->dev, "Failed to turn off on removal: %d\n",
			ret);
}

static int sc2721_backlight_probe(struct platform_device *pdev)
{
	struct backlight_properties props = {
		.type = BACKLIGHT_RAW,
		.max_brightness = 255,
		.brightness = 127,
		.power = BACKLIGHT_POWER_OFF,
	};
	struct device *dev = &pdev->dev;
	struct sc2721_backlight *backlight;
	struct backlight_device *bl;
	int ret;

	backlight = devm_kzalloc(dev, sizeof(*backlight), GFP_KERNEL);
	if (!backlight)
		return -ENOMEM;

	backlight->dev = dev;
	backlight->regmap = dev_get_regmap(dev->parent, NULL);
	if (!backlight->regmap)
		return dev_err_probe(dev, -ENODEV, "Missing parent regmap\n");

	ret = power_off(backlight);
	if (ret)
		return dev_err_probe(dev, ret,
				     "Failed to establish initial OFF\n");

	/* Unregister callbacks before cleanup uses the borrowed parent regmap. */
	ret = devm_add_action_or_reset(dev, sc2721_backlight_cleanup,
				       backlight);
	if (ret)
		return ret;

	bl = devm_backlight_device_register(dev, "sprd_backlight", dev,
					    backlight, &sc2721_backlight_ops,
					    &props);
	if (IS_ERR(bl))
		return dev_err_probe(dev, PTR_ERR(bl),
				     "Failed to register backlight\n");

	platform_set_drvdata(pdev, bl);
	return 0;
}

static void sc2721_backlight_shutdown(struct platform_device *pdev)
{
	struct backlight_device *bl = platform_get_drvdata(pdev);
	int ret;

	ret = backlight_disable(bl);
	if (ret)
		dev_err(&pdev->dev, "Failed to turn off on shutdown: %d\n",
			ret);
}

static const struct of_device_id sc2721_backlight_match[] = {
	{ .compatible = "sprd,sc2721-bltc-rgb" },
	{},
};
MODULE_DEVICE_TABLE(of, sc2721_backlight_match);

static struct platform_driver sc2721_backlight_driver = {
	.probe = sc2721_backlight_probe,
	.shutdown = sc2721_backlight_shutdown,
	.driver = {
		.name = "sc2721-backlight",
		.of_match_table = sc2721_backlight_match,
	},
};
module_platform_driver(sc2721_backlight_driver);

MODULE_DESCRIPTION("SC2721 current-controlled LCD backlight");
MODULE_LICENSE("GPL");
