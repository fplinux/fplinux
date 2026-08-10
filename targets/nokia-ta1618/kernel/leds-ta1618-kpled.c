// SPDX-License-Identifier: GPL-2.0-only
#include <linux/bitops.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/soc/sprd/ums9117-adi.h>
#include <linux/workqueue.h>

#define SC2720_CHIP_ID_LOW 0xc00u
#define SC2720_CHIP_ID_HIGH 0xc04u
#define SC2720_EXPECTED_ID_LOW 0xa003u
#define SC2720_EXPECTED_ID_HIGH 0x2720u
#define SC2720_KPLED_CTRL0 0xdf8u
#define SC2720_KPLED_CTRL1 0xdfcu
#define SC2720_KPLED_CTRL0_PHYS 0x40608df8u
#define SC2720_KPLED_LEVEL GENMASK(15, 12)
#define SC2720_KPLED_PD BIT(11)
#define SC2720_KPLED_OWNED_MASK (SC2720_KPLED_LEVEL | SC2720_KPLED_PD)
#define TA1618_KPLED_CUTOFF_MS 4900u

struct ta1618_kpled {
	struct device *dev;
	struct led_classdev led;
	struct mutex lock;
	struct delayed_work cutoff_work;
	u16 initial_ctrl0;
	u8 current_code;
	bool may_be_on;
	bool owns_output;
	bool tearing_down;
	bool registered;
};

static int
ta1618_kpled_check_identity(struct ums9117_adi_transaction *transaction)
{
	u16 id_low;
	u16 id_high;
	int ret;

	ret = ums9117_adi_read(transaction, SC2720_CHIP_ID_LOW, &id_low);
	if (!ret)
		ret = ums9117_adi_read(transaction, SC2720_CHIP_ID_HIGH,
				       &id_high);
	if (ret)
		return ret;
	if (id_low != SC2720_EXPECTED_ID_LOW ||
	    id_high != SC2720_EXPECTED_ID_HIGH)
		return -ENODEV;
	return 0;
}

static int ta1618_kpled_read_initial(struct ta1618_kpled *kpled)
{
	struct ums9117_adi_transaction transaction = {};
	u16 ctrl1;
	int end_ret;
	int ret;

	ret = ums9117_adi_begin(&transaction);
	if (ret)
		return ret;
	ret = ta1618_kpled_check_identity(&transaction);
	if (!ret)
		ret = ums9117_adi_read(&transaction, SC2720_KPLED_CTRL0,
				       &kpled->initial_ctrl0);
	if (!ret)
		ret = ums9117_adi_read(&transaction, SC2720_KPLED_CTRL1,
				       &ctrl1);
	end_ret = ums9117_adi_end(&transaction);
	if (!ret)
		ret = end_ret;
	return ret;
}

static int ta1618_kpled_restore_locked(struct ta1618_kpled *kpled)
{
	struct ums9117_adi_transaction transaction = {};
	u16 readback = 0;
	int end_ret;
	int ret;

	if (!kpled->may_be_on)
		return 0;

	ret = ums9117_adi_begin(&transaction);
	if (ret)
		return ret;
	ret = ta1618_kpled_check_identity(&transaction);
	if (!ret)
		ret = ums9117_adi_update_bits(&transaction, SC2720_KPLED_CTRL0,
					      SC2720_KPLED_OWNED_MASK,
					      kpled->initial_ctrl0);
	if (!ret)
		ret = ums9117_adi_read(&transaction, SC2720_KPLED_CTRL0,
				       &readback);
	end_ret = ums9117_adi_end(&transaction);
	if (!ret)
		ret = end_ret;
	if (!ret && (readback & SC2720_KPLED_OWNED_MASK) !=
			    (kpled->initial_ctrl0 & SC2720_KPLED_OWNED_MASK))
		ret = -EIO;
	if (!ret) {
		kpled->may_be_on = false;
		kpled->owns_output = false;
	}
	return ret;
}

static int ta1618_kpled_enable_locked(struct ta1618_kpled *kpled)
{
	struct ums9117_adi_transaction transaction = {};
	u16 expected = (u16)kpled->current_code << 12;
	u16 readback = 0;
	int restore_ret;
	int end_ret;
	int ret;

	mod_delayed_work(system_wq, &kpled->cutoff_work,
			 msecs_to_jiffies(TA1618_KPLED_CUTOFF_MS));
	if (kpled->owns_output)
		return 0;
	if (kpled->may_be_on)
		return -EIO;

	kpled->may_be_on = true;
	ret = ums9117_adi_begin(&transaction);
	if (ret)
		goto restore;
	ret = ta1618_kpled_check_identity(&transaction);
	if (!ret)
		ret = ums9117_adi_update_bits(&transaction, SC2720_KPLED_CTRL0,
					      SC2720_KPLED_OWNED_MASK,
					      expected);
	if (!ret)
		ret = ums9117_adi_read(&transaction, SC2720_KPLED_CTRL0,
				       &readback);
	end_ret = ums9117_adi_end(&transaction);
	if (!ret)
		ret = end_ret;
	if (!ret && (readback & SC2720_KPLED_OWNED_MASK) != expected)
		ret = -EIO;
	if (!ret) {
		kpled->owns_output = true;
		return 0;
	}

restore:
	restore_ret = ta1618_kpled_restore_locked(kpled);
	if (!restore_ret)
		cancel_delayed_work(&kpled->cutoff_work);
	else
		dev_emerg(
			kpled->dev,
			"failed to restore keypad backlight after ON error: %d\n",
			restore_ret);
	return ret;
}

static int ta1618_kpled_set(struct led_classdev *led,
			    enum led_brightness brightness)
{
	struct ta1618_kpled *kpled =
		container_of(led, struct ta1618_kpled, led);
	int ret;

	mutex_lock(&kpled->lock);
	if (kpled->tearing_down) {
		ret = -ESHUTDOWN;
	} else if (brightness == LED_OFF) {
		ret = ta1618_kpled_restore_locked(kpled);
		if (!ret)
			cancel_delayed_work(&kpled->cutoff_work);
	} else {
		ret = ta1618_kpled_enable_locked(kpled);
	}
	mutex_unlock(&kpled->lock);
	return ret;
}

static void ta1618_kpled_cutoff_work(struct work_struct *work)
{
	struct ta1618_kpled *kpled = container_of(
		to_delayed_work(work), struct ta1618_kpled, cutoff_work);

	mutex_lock(&kpled->lock);
	if (!kpled->tearing_down)
		led_set_brightness(&kpled->led, LED_OFF);
	mutex_unlock(&kpled->lock);
}

static void ta1618_kpled_teardown(struct ta1618_kpled *kpled)
{
	bool registered;
	int ret;

	mutex_lock(&kpled->lock);
	if (kpled->tearing_down) {
		mutex_unlock(&kpled->lock);
		return;
	}
	kpled->tearing_down = true;
	registered = kpled->registered;
	kpled->registered = false;
	mutex_unlock(&kpled->lock);

	if (registered)
		devm_led_classdev_unregister(kpled->dev, &kpled->led);
	cancel_delayed_work_sync(&kpled->cutoff_work);

	mutex_lock(&kpled->lock);
	ret = ta1618_kpled_restore_locked(kpled);
	mutex_unlock(&kpled->lock);
	if (ret)
		dev_emerg(
			kpled->dev,
			"failed to restore keypad backlight during teardown: %d\n",
			ret);
}

static int ta1618_kpled_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct led_init_data init_data = {};
	struct ta1618_kpled *kpled;
	struct resource *resource;
	u32 current_code;
	int ret;

	resource = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!resource || resource->start != SC2720_KPLED_CTRL0_PHYS ||
	    resource_size(resource) != sizeof(u32))
		return dev_err_probe(dev, -EINVAL,
				     "expected KPLED CTRL0 resource %#x/4\n",
				     SC2720_KPLED_CTRL0_PHYS);

	ret = device_property_read_u32(dev, "fplinux,current-code",
				       &current_code);
	if (ret)
		return dev_err_probe(dev, ret, "missing current code\n");
	if (current_code < 1 || current_code > 15)
		return dev_err_probe(
			dev, -EINVAL,
			"current code must be in the range 1..15\n");

	kpled = devm_kzalloc(dev, sizeof(*kpled), GFP_KERNEL);
	if (!kpled)
		return -ENOMEM;
	kpled->dev = dev;
	kpled->current_code = current_code;
	mutex_init(&kpled->lock);
	INIT_DELAYED_WORK(&kpled->cutoff_work, ta1618_kpled_cutoff_work);

	ret = ta1618_kpled_read_initial(kpled);
	if (ret)
		return dev_err_probe(
			dev, ret,
			"failed to read SC2720 keypad backlight state\n");
	if (!(kpled->initial_ctrl0 & SC2720_KPLED_PD))
		return dev_err_probe(
			dev, -EBUSY,
			"refusing unexpected enabled keypad backlight\n");

	kpled->led.max_brightness = 1;
	kpled->led.brightness_set_blocking = ta1618_kpled_set;
	kpled->led.flags = LED_RETAIN_AT_SHUTDOWN;
	init_data.fwnode = dev_fwnode(dev);
	ret = devm_led_classdev_register_ext(dev, &kpled->led, &init_data);
	if (ret)
		return dev_err_probe(
			dev, ret, "failed to register keypad backlight LED\n");
	kpled->registered = true;
	platform_set_drvdata(pdev, kpled);
	return 0;
}

static void ta1618_kpled_remove(struct platform_device *pdev)
{
	ta1618_kpled_teardown(platform_get_drvdata(pdev));
}

static void ta1618_kpled_shutdown(struct platform_device *pdev)
{
	ta1618_kpled_teardown(platform_get_drvdata(pdev));
}

static const struct of_device_id ta1618_kpled_of_match[] = {
	{ .compatible = "fplinux,ta1618-kpled" },
	{}
};
MODULE_DEVICE_TABLE(of, ta1618_kpled_of_match);

static struct platform_driver ta1618_kpled_driver = {
	.probe = ta1618_kpled_probe,
	.remove = ta1618_kpled_remove,
	.shutdown = ta1618_kpled_shutdown,
	.driver = {
		.name = "ta1618-kpled",
		.of_match_table = ta1618_kpled_of_match,
	},
};
module_platform_driver(ta1618_kpled_driver);

MODULE_DESCRIPTION("Nokia TA-1618 SC2720 keypad backlight driver");
MODULE_LICENSE("GPL");
