// SPDX-License-Identifier: GPL-2.0-only
#include <linux/bitfield.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/regmap.h>

#define SC2720_KPLED_CTRL0 0xdf8
#define SC2720_KPLED_LEVEL GENMASK(15, 12)
#define SC2720_KPLED_POWER_DOWN BIT(11)
#define SC2720_KPLED_MASK (SC2720_KPLED_LEVEL | SC2720_KPLED_POWER_DOWN)

struct sc2720_kpled {
	struct device *dev;
	struct regmap *regmap;
	struct led_classdev led;
	unsigned int initial;
	unsigned int current_code;
};

static int sc2720_kpled_set(struct led_classdev *led,
			    enum led_brightness brightness)
{
	struct sc2720_kpled *kpled =
		container_of(led, struct sc2720_kpled, led);
	unsigned int value;

	value = brightness ?
			FIELD_PREP(SC2720_KPLED_LEVEL, kpled->current_code) :
			kpled->initial;
	return regmap_update_bits(kpled->regmap, SC2720_KPLED_CTRL0,
				  SC2720_KPLED_MASK, value);
}

static void sc2720_kpled_restore(void *data)
{
	struct sc2720_kpled *kpled = data;
	int ret;

	ret = regmap_update_bits(kpled->regmap, SC2720_KPLED_CTRL0,
				 SC2720_KPLED_MASK, kpled->initial);
	if (ret)
		dev_err(kpled->dev, "cannot restore keypad backlight: %d\n",
			ret);
}

static int sc2720_kpled_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct led_init_data init = { .fwnode = dev_fwnode(dev) };
	struct sc2720_kpled *kpled;
	int ret;

	kpled = devm_kzalloc(dev, sizeof(*kpled), GFP_KERNEL);
	if (!kpled)
		return -ENOMEM;
	kpled->dev = dev;
	kpled->regmap = dev_get_regmap(dev->parent, NULL);
	if (!kpled->regmap)
		return -EPROBE_DEFER;
	ret = device_property_read_u32(dev, "fplinux,current-code",
				       &kpled->current_code);
	if (ret)
		return dev_err_probe(dev, ret,
				     "missing keypad LED current code\n");
	if (!kpled->current_code || kpled->current_code > 15)
		return -EINVAL;
	ret = regmap_read(kpled->regmap, SC2720_KPLED_CTRL0, &kpled->initial);
	if (ret)
		return ret;
	if (!(kpled->initial & SC2720_KPLED_POWER_DOWN))
		return dev_err_probe(dev, -EBUSY,
				     "keypad backlight already active\n");
	ret = devm_add_action_or_reset(dev, sc2720_kpled_restore, kpled);
	if (ret)
		return ret;
	kpled->led.max_brightness = 1;
	kpled->led.brightness_set_blocking = sc2720_kpled_set;
	kpled->led.flags = LED_CORE_SUSPENDRESUME;
	platform_set_drvdata(pdev, kpled);
	return devm_led_classdev_register_ext(dev, &kpled->led, &init);
}

static void sc2720_kpled_shutdown(struct platform_device *pdev)
{
	struct sc2720_kpled *kpled = platform_get_drvdata(pdev);

	led_classdev_suspend(&kpled->led);
}

static const struct of_device_id sc2720_kpled_match[] = {
	{ .compatible = "sprd,ums9117-sc2720-kpled" },
	{}
};
MODULE_DEVICE_TABLE(of, sc2720_kpled_match);

static struct platform_driver sc2720_kpled_driver = {
	.probe = sc2720_kpled_probe,
	.shutdown = sc2720_kpled_shutdown,
	.driver = {
		.name = "sc2720-kpled",
		.of_match_table = sc2720_kpled_match,
	},
};
module_platform_driver(sc2720_kpled_driver);

MODULE_DESCRIPTION("SC2720 keypad backlight");
MODULE_LICENSE("GPL");
