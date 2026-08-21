// SPDX-License-Identifier: GPL-2.0-only
#include <linux/bitops.h>
#include <linux/input.h>
#include <linux/jiffies.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/slab.h>
#include <linux/soc/sprd/ums9117-adi.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/workqueue.h>

#define SC2720_CHIP_ID_LOW 0xc00U
#define SC2720_CHIP_ID_HIGH 0xc04U
#define SC2720_EXPECTED_ID_LOW 0xa003U
#define SC2720_EXPECTED_ID_HIGH 0x2720U
#define SC2720_KPLED_CTRL0 0xdf8U
#define SC2720_KPLED_CTRL1 0xdfcU
#define SC2720_KPLED_CTRL0_PHYS 0x40608df8U
#define SC2720_KPLED_CTRL0_LEVEL_MASK GENMASK(15, 12)
#define SC2720_KPLED_CTRL0_POWER_DOWN BIT(11)
#define SC2720_KPLED_CTRL0_OWNED_MASK \
	(SC2720_KPLED_CTRL0_LEVEL_MASK | SC2720_KPLED_CTRL0_POWER_DOWN)
#define TA1618_KPLED_CUTOFF_MS 4900U
#define TA1618_KEYPAD_NAME "TA-1618 keypad"
#define TA1618_KEYPAD_PHYS "ta1618/keypad0"

struct ta1618_kpled {
	struct device *dev;
	struct led_classdev led;
	struct input_handler input_handler;
	struct mutex lock;
	spinlock_t cutoff_lock;
	struct delayed_work cutoff_work;
	unsigned long cutoff_deadline;
	u16 initial_ctrl0;
	u8 current_code;
	bool may_be_on;
	bool owns_output;
	bool tearing_down;
	bool registered;
	bool input_registered;
};

struct ta1618_kpled_input {
	struct input_handle handle;
	struct ta1618_kpled *kpled;
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
					      SC2720_KPLED_CTRL0_OWNED_MASK,
					      kpled->initial_ctrl0);
	if (!ret)
		ret = ums9117_adi_read(&transaction, SC2720_KPLED_CTRL0,
				       &readback);
	end_ret = ums9117_adi_end(&transaction);
	if (!ret)
		ret = end_ret;
	if (!ret &&
	    (readback & SC2720_KPLED_CTRL0_OWNED_MASK) !=
		    (kpled->initial_ctrl0 & SC2720_KPLED_CTRL0_OWNED_MASK))
		ret = -EIO;
	if (!ret) {
		kpled->may_be_on = false;
		kpled->owns_output = false;
	}
	return ret;
}

static void ta1618_kpled_arm_cutoff_locked(struct ta1618_kpled *kpled)
{
	unsigned long delay = msecs_to_jiffies(TA1618_KPLED_CUTOFF_MS);

	kpled->cutoff_deadline = jiffies + delay;
	mod_delayed_work(system_wq, &kpled->cutoff_work, delay);
}

static void ta1618_kpled_arm_cutoff(struct ta1618_kpled *kpled)
{
	unsigned long flags;

	spin_lock_irqsave(&kpled->cutoff_lock, flags);
	ta1618_kpled_arm_cutoff_locked(kpled);
	spin_unlock_irqrestore(&kpled->cutoff_lock, flags);
}

static int ta1618_kpled_enable_locked(struct ta1618_kpled *kpled)
{
	struct ums9117_adi_transaction transaction = {};
	u16 expected = (u16)kpled->current_code << 12;
	u16 readback = 0;
	int restore_ret;
	int end_ret;
	int ret;

	ta1618_kpled_arm_cutoff(kpled);
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
					      SC2720_KPLED_CTRL0_OWNED_MASK,
					      expected);
	if (!ret)
		ret = ums9117_adi_read(&transaction, SC2720_KPLED_CTRL0,
				       &readback);
	end_ret = ums9117_adi_end(&transaction);
	if (!ret)
		ret = end_ret;
	if (!ret && (readback & SC2720_KPLED_CTRL0_OWNED_MASK) != expected)
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
	unsigned long deadline;
	unsigned long flags;
	unsigned long now;

	mutex_lock(&kpled->lock);
	spin_lock_irqsave(&kpled->cutoff_lock, flags);
	now = jiffies;
	deadline = kpled->cutoff_deadline;
	if (!kpled->tearing_down && time_before(now, deadline))
		mod_delayed_work(system_wq, &kpled->cutoff_work,
				 deadline - now);
	else if (!kpled->tearing_down)
		led_set_brightness(&kpled->led, LED_OFF);
	spin_unlock_irqrestore(&kpled->cutoff_lock, flags);
	mutex_unlock(&kpled->lock);
}

static void ta1618_kpled_input_event(struct input_handle *handle,
				     unsigned int type, unsigned int code,
				     int value)
{
	struct ta1618_kpled_input *input =
		container_of(handle, struct ta1618_kpled_input, handle);
	struct ta1618_kpled *kpled = input->kpled;
	unsigned long flags;

	(void)code;
	if (type != EV_KEY || value != 1)
		return;

	spin_lock_irqsave(&kpled->cutoff_lock, flags);
	if (!kpled->tearing_down) {
		ta1618_kpled_arm_cutoff_locked(kpled);
		led_set_brightness(&kpled->led, kpled->led.max_brightness);
	}
	spin_unlock_irqrestore(&kpled->cutoff_lock, flags);
}

static int ta1618_kpled_input_connect(struct input_handler *handler,
				      struct input_dev *dev,
				      const struct input_device_id *id)
{
	struct ta1618_kpled *kpled =
		container_of(handler, struct ta1618_kpled, input_handler);
	struct ta1618_kpled_input *input;
	int ret;

	(void)id;
	if (!dev->name || strcmp(dev->name, TA1618_KEYPAD_NAME) || !dev->phys ||
	    strcmp(dev->phys, TA1618_KEYPAD_PHYS))
		return -ENODEV;

	input = kzalloc(sizeof(*input), GFP_KERNEL);
	if (!input)
		return -ENOMEM;
	input->kpled = kpled;
	input->handle.dev = dev;
	input->handle.handler = handler;
	input->handle.name = "ta1618-kpled";

	ret = input_register_handle(&input->handle);
	if (ret)
		goto free;
	ret = input_open_device(&input->handle);
	if (ret)
		goto unregister;
	return 0;

unregister:
	input_unregister_handle(&input->handle);
free:
	kfree(input);
	return ret;
}

static void ta1618_kpled_input_disconnect(struct input_handle *handle)
{
	struct ta1618_kpled_input *input =
		container_of(handle, struct ta1618_kpled_input, handle);

	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(input);
}

static const struct input_device_id ta1618_kpled_input_ids[] = {
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT,
		.evbit = { BIT_MASK(EV_KEY) },
	},
	{}
};
MODULE_DEVICE_TABLE(input, ta1618_kpled_input_ids);

static void ta1618_kpled_teardown(struct ta1618_kpled *kpled)
{
	bool input_registered;
	bool registered;
	unsigned long flags;
	int ret;

	mutex_lock(&kpled->lock);
	if (kpled->tearing_down) {
		mutex_unlock(&kpled->lock);
		return;
	}
	spin_lock_irqsave(&kpled->cutoff_lock, flags);
	kpled->tearing_down = true;
	spin_unlock_irqrestore(&kpled->cutoff_lock, flags);
	input_registered = kpled->input_registered;
	kpled->input_registered = false;
	registered = kpled->registered;
	kpled->registered = false;
	mutex_unlock(&kpled->lock);

	if (input_registered)
		input_unregister_handler(&kpled->input_handler);
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
	spin_lock_init(&kpled->cutoff_lock);
	INIT_DELAYED_WORK(&kpled->cutoff_work, ta1618_kpled_cutoff_work);

	ret = ta1618_kpled_read_initial(kpled);
	if (ret)
		return dev_err_probe(
			dev, ret,
			"failed to read SC2720 keypad backlight state\n");
	if (!(kpled->initial_ctrl0 & SC2720_KPLED_CTRL0_POWER_DOWN))
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

	kpled->input_handler.event = ta1618_kpled_input_event;
	kpled->input_handler.connect = ta1618_kpled_input_connect;
	kpled->input_handler.disconnect = ta1618_kpled_input_disconnect;
	kpled->input_handler.name = "ta1618-kpled";
	kpled->input_handler.id_table = ta1618_kpled_input_ids;
	ret = input_register_handler(&kpled->input_handler);
	if (ret) {
		kpled->registered = false;
		devm_led_classdev_unregister(dev, &kpled->led);
		return dev_err_probe(
			dev, ret, "failed to register keypad input handler\n");
	}
	kpled->input_registered = true;
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
