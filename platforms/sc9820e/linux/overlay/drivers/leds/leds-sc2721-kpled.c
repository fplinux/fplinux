// SPDX-License-Identifier: GPL-2.0-only
#include <linux/bitops.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/slab.h>

#define SC2721_KPLED_CTRL0_PULLDOWN BIT(10)
#define SC2721_KPLED_CTRL1 0x04
#define SC2721_KPLED_CTRL1_LEVEL_MASK GENMASK(14, 7)
#define SC2721_KPLED_CTRL1_LEVEL_SHIFT 7
#define SC2721_KPLED_CTRL1_PD BIT(15)

struct sc2721_kpled {
	struct device *dev;
	struct regmap *regmap;
	struct led_classdev led;
	struct mutex lock;
	u32 base;
};

static int power_off(struct sc2721_kpled *kpled)
{
	int ret, error;

	ret = regmap_update_bits(kpled->regmap, kpled->base,
				 SC2721_KPLED_CTRL0_PULLDOWN,
				 SC2721_KPLED_CTRL0_PULLDOWN);
	/* A failed pulldown update must not prevent disabling the LDO. */
	error = regmap_update_bits(kpled->regmap,
				   kpled->base + SC2721_KPLED_CTRL1,
				   SC2721_KPLED_CTRL1_PD,
				   SC2721_KPLED_CTRL1_PD);
	return ret ? ret : error;
}

static int sc2721_kpled_set(struct led_classdev *led,
			    enum led_brightness brightness)
{
	struct sc2721_kpled *kpled =
		container_of(led, struct sc2721_kpled, led);
	int ret, off_error;

	mutex_lock(&kpled->lock);
	if (!brightness) {
		ret = power_off(kpled);
		goto out;
	}

	ret = regmap_update_bits(kpled->regmap,
				 kpled->base + SC2721_KPLED_CTRL1,
				 SC2721_KPLED_CTRL1_LEVEL_MASK,
				 brightness << SC2721_KPLED_CTRL1_LEVEL_SHIFT);
	if (!ret)
		ret = regmap_update_bits(kpled->regmap,
					 kpled->base + SC2721_KPLED_CTRL1,
					 SC2721_KPLED_CTRL1_PD, 0);
	if (ret) {
		/* A transport error can follow a write that reached the PMIC. */
		off_error = power_off(kpled);
		if (off_error)
			dev_err(kpled->dev,
				"failed to turn off after enable error %d: %d\n",
				ret, off_error);
	}
out:
	mutex_unlock(&kpled->lock);
	return ret;
}

static void sc2721_kpled_cleanup(void *data)
{
	struct sc2721_kpled *kpled = data;
	int ret;

	mutex_lock(&kpled->lock);
	ret = power_off(kpled);
	mutex_unlock(&kpled->lock);
	if (ret)
		dev_err(kpled->dev, "failed to turn off keypad backlight: %d\n",
			ret);
}

static int sc2721_kpled_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct led_init_data init = { .fwnode = dev_fwnode(dev) };
	struct sc2721_kpled *kpled;
	int ret;

	kpled = devm_kzalloc(dev, sizeof(*kpled), GFP_KERNEL);
	if (!kpled)
		return -ENOMEM;
	kpled->dev = dev;
	kpled->regmap = dev_get_regmap(dev->parent, NULL);
	if (!kpled->regmap)
		return dev_err_probe(dev, -EPROBE_DEFER,
				     "missing parent regmap\n");
	ret = device_property_read_u32(dev, "reg", &kpled->base);
	if (ret)
		return dev_err_probe(dev, ret,
				     "missing keypad LED registers\n");
	mutex_init(&kpled->lock);
	ret = power_off(kpled);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to establish initial OFF\n");
	/* Unregister the LED before cleanup releases its output. */
	ret = devm_add_action_or_reset(dev, sc2721_kpled_cleanup, kpled);
	if (ret)
		return ret;
	kpled->led.max_brightness = 255;
	kpled->led.brightness_set_blocking = sc2721_kpled_set;
	kpled->led.flags = LED_CORE_SUSPENDRESUME;
	platform_set_drvdata(pdev, kpled);
	return devm_led_classdev_register_ext(dev, &kpled->led, &init);
}

static void sc2721_kpled_shutdown(struct platform_device *pdev)
{
	struct sc2721_kpled *kpled = platform_get_drvdata(pdev);

	led_classdev_suspend(&kpled->led);
}

static const struct of_device_id sc2721_kpled_match[] = {
	{ .compatible = "sprd,sc2721-kpled-ldo" },
	{},
};
MODULE_DEVICE_TABLE(of, sc2721_kpled_match);

static struct platform_driver sc2721_kpled_driver = {
	.probe = sc2721_kpled_probe,
	.shutdown = sc2721_kpled_shutdown,
	.driver = {
		.name = "sc2721-kpled",
		.of_match_table = sc2721_kpled_match,
	},
};
module_platform_driver(sc2721_kpled_driver);

MODULE_DESCRIPTION("SC2721 keypad backlight in LDO mode");
MODULE_LICENSE("GPL");
