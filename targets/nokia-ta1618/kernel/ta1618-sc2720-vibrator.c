// SPDX-License-Identifier: GPL-2.0-only
#include <linux/bitops.h>
#include <linux/bitfield.h>
#include <linux/input.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/soc/sprd/ums9117-adi.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>

#define SC2720_CHIP_ID_LOW 0xc00U
#define SC2720_CHIP_ID_HIGH 0xc04U
#define SC2720_EXPECTED_ID_LOW 0xa003U
#define SC2720_EXPECTED_ID_HIGH 0x2720U
#define SC2720_VIBR_CTRL0 0xe00U
#define SC2720_VIBR_CTRL1 0xe04U
#define SC2720_VIBR_CTRL0_PHYS 0x40608e00U
#define SC2720_VIBR_VOLTAGE_MASK GENMASK(2, 0)
#define SC2720_VIBR_LDO_POWER_DOWN BIT(8)
#define SC2720_VIBR_SLEEP_POWER_DOWN BIT(9)
#define SC2720_VIBR_POWER_DOWN_MASK \
	(SC2720_VIBR_LDO_POWER_DOWN | SC2720_VIBR_SLEEP_POWER_DOWN)
#define SC2720_VIBR_OWNED_MASK \
	(SC2720_VIBR_VOLTAGE_MASK | SC2720_VIBR_POWER_DOWN_MASK)
#define TA1618_VIBRATOR_MAX_ON_MS 5000U

struct ta1618_vibrator {
	struct device *dev;
	struct input_dev *input;
	struct delayed_work apply_work;
	struct delayed_work cutoff_work;
	struct mutex hw_lock;
	spinlock_t state_lock;
	unsigned long cutoff_deadline;
	u16 initial_ctrl0;
	u8 voltage_code;
	bool requested_on;
	bool pulse_active;
	bool may_be_on;
	bool owns_output;
	bool suspended;
	bool tearing_down;
	bool faulted;
};

static int
ta1618_vibrator_check_identity(struct ums9117_adi_transaction *transaction)
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

static int ta1618_vibrator_read_initial(struct ta1618_vibrator *vibrator)
{
	struct ums9117_adi_transaction transaction = {};
	u16 ctrl0_first;
	u16 ctrl0_second;
	u16 ctrl1_first;
	u16 ctrl1_second;
	int end_ret;
	int ret;

	if (ums9117_adi_is_poisoned())
		return -EIO;

	ret = ums9117_adi_begin(&transaction);
	if (ret)
		return ret;
	ret = ta1618_vibrator_check_identity(&transaction);
	if (!ret)
		ret = ums9117_adi_read(&transaction, SC2720_VIBR_CTRL0,
				       &ctrl0_first);
	if (!ret)
		ret = ums9117_adi_read(&transaction, SC2720_VIBR_CTRL1,
				       &ctrl1_first);
	if (!ret)
		ret = ums9117_adi_read(&transaction, SC2720_VIBR_CTRL0,
				       &ctrl0_second);
	if (!ret)
		ret = ums9117_adi_read(&transaction, SC2720_VIBR_CTRL1,
				       &ctrl1_second);
	end_ret = ums9117_adi_end(&transaction);
	if (!ret)
		ret = end_ret;
	if (ret)
		return ret;
	if (ctrl0_first != ctrl0_second || ctrl1_first != ctrl1_second)
		return -EIO;

	vibrator->initial_ctrl0 = ctrl0_first;
	return 0;
}

static void ta1618_vibrator_record_error(int *ret, int step_ret)
{
	if (!*ret && step_ret)
		*ret = step_ret;
}

static int
ta1618_vibrator_stop_transaction(struct ta1618_vibrator *vibrator,
				 struct ums9117_adi_transaction *transaction)
{
	u16 expected = vibrator->initial_ctrl0 & SC2720_VIBR_OWNED_MASK;
	u16 readback = 0;
	int ldo_off_ret;
	int ret = 0;
	int step_ret;

	ldo_off_ret = ums9117_adi_update_bits(transaction, SC2720_VIBR_CTRL0,
					      SC2720_VIBR_LDO_POWER_DOWN,
					      SC2720_VIBR_LDO_POWER_DOWN);
	ta1618_vibrator_record_error(&ret, ldo_off_ret);
	step_ret = ums9117_adi_update_bits(transaction, SC2720_VIBR_CTRL0,
					   SC2720_VIBR_SLEEP_POWER_DOWN,
					   SC2720_VIBR_SLEEP_POWER_DOWN);
	ta1618_vibrator_record_error(&ret, step_ret);
	if (!ldo_off_ret) {
		step_ret = ums9117_adi_update_bits(transaction,
						   SC2720_VIBR_CTRL0,
						   SC2720_VIBR_VOLTAGE_MASK,
						   vibrator->initial_ctrl0);
		ta1618_vibrator_record_error(&ret, step_ret);
	}
	step_ret = ums9117_adi_read(transaction, SC2720_VIBR_CTRL0, &readback);
	ta1618_vibrator_record_error(&ret, step_ret);
	if (!step_ret && !ldo_off_ret &&
	    (readback & SC2720_VIBR_OWNED_MASK) != expected)
		ta1618_vibrator_record_error(&ret, -EIO);
	return ret;
}

static int ta1618_vibrator_restore_locked(struct ta1618_vibrator *vibrator)
{
	struct ums9117_adi_transaction transaction = {};
	int end_ret;
	int ret;

	if (!vibrator->may_be_on)
		return 0;

	ret = ums9117_adi_begin(&transaction);
	if (ret)
		return ret;
	ret = ta1618_vibrator_stop_transaction(vibrator, &transaction);
	end_ret = ums9117_adi_end(&transaction);
	ta1618_vibrator_record_error(&ret, end_ret);
	if (!ret) {
		vibrator->may_be_on = false;
		vibrator->owns_output = false;
	}
	return ret;
}

static int ta1618_vibrator_enable_locked(struct ta1618_vibrator *vibrator)
{
	struct ums9117_adi_transaction transaction = {};
	u16 expected = vibrator->voltage_code;
	u16 readback = 0;
	int stop_ret = 0;
	int end_ret;
	int ret;
	bool stop_attempted = false;

	if (vibrator->owns_output)
		return 0;
	if (vibrator->may_be_on)
		return -EIO;

	ret = ums9117_adi_begin(&transaction);
	if (ret)
		return ret;
	ret = ta1618_vibrator_check_identity(&transaction);
	if (!ret) {
		vibrator->may_be_on = true;
		ret = ums9117_adi_update_bits(&transaction, SC2720_VIBR_CTRL0,
					      SC2720_VIBR_VOLTAGE_MASK,
					      vibrator->voltage_code);
	}
	if (!ret)
		ret = ums9117_adi_update_bits(&transaction, SC2720_VIBR_CTRL0,
					      SC2720_VIBR_LDO_POWER_DOWN, 0);
	if (!ret)
		ret = ums9117_adi_update_bits(&transaction, SC2720_VIBR_CTRL0,
					      SC2720_VIBR_SLEEP_POWER_DOWN, 0);
	if (!ret)
		ret = ums9117_adi_read(&transaction, SC2720_VIBR_CTRL0,
				       &readback);
	if (!ret && (readback & SC2720_VIBR_OWNED_MASK) != expected)
		ret = -EIO;
	if (ret && vibrator->may_be_on) {
		stop_attempted = true;
		stop_ret = ta1618_vibrator_stop_transaction(vibrator,
							    &transaction);
	}
	end_ret = ums9117_adi_end(&transaction);
	ta1618_vibrator_record_error(&ret, end_ret);
	if (stop_ret)
		dev_crit(
			vibrator->dev,
			"failed to stop vibrator in ON error transaction: %d\n",
			stop_ret);
	if (!ret) {
		vibrator->owns_output = true;
		return 0;
	}
	if (stop_attempted && !stop_ret && !end_ret) {
		vibrator->may_be_on = false;
		vibrator->owns_output = false;
	}
	return ret;
}

static void ta1618_vibrator_latch_fault(struct ta1618_vibrator *vibrator)
{
	unsigned long flags;

	spin_lock_irqsave(&vibrator->state_lock, flags);
	vibrator->requested_on = false;
	vibrator->faulted = true;
	spin_unlock_irqrestore(&vibrator->state_lock, flags);
}

static void ta1618_vibrator_apply_work(struct work_struct *work)
{
	struct ta1618_vibrator *vibrator = container_of(
		to_delayed_work(work), struct ta1618_vibrator, apply_work);
	unsigned long flags;
	bool turn_on;
	int ret;

	mutex_lock(&vibrator->hw_lock);
	spin_lock_irqsave(&vibrator->state_lock, flags);
	turn_on = vibrator->pulse_active && vibrator->requested_on &&
		  !vibrator->suspended && !vibrator->tearing_down &&
		  !vibrator->faulted &&
		  time_before(jiffies, vibrator->cutoff_deadline);
	if (vibrator->requested_on && !turn_on)
		vibrator->requested_on = false;
	spin_unlock_irqrestore(&vibrator->state_lock, flags);
	if (turn_on)
		ret = ta1618_vibrator_enable_locked(vibrator);
	else
		ret = ta1618_vibrator_restore_locked(vibrator);
	mutex_unlock(&vibrator->hw_lock);

	if (!ret) {
		if (!turn_on) {
			spin_lock_irqsave(&vibrator->state_lock, flags);
			vibrator->pulse_active = false;
			spin_unlock_irqrestore(&vibrator->state_lock, flags);
		}
		return;
	}
	ta1618_vibrator_latch_fault(vibrator);
	cancel_delayed_work(&vibrator->cutoff_work);
	if (turn_on) {
		dev_err(vibrator->dev,
			"vibrator ON failed and further requests are blocked: %d\n",
			ret);
		mod_delayed_work(system_wq, &vibrator->apply_work, 0);
	} else {
		dev_crit(
			vibrator->dev,
			"vibrator OFF failed and further requests are blocked: %d\n",
			ret);
	}
}

static void ta1618_vibrator_cutoff_work(struct work_struct *work)
{
	struct ta1618_vibrator *vibrator = container_of(
		to_delayed_work(work), struct ta1618_vibrator, cutoff_work);
	unsigned long deadline;
	unsigned long delay = 0;
	unsigned long flags;
	unsigned long now;
	bool apply = false;

	spin_lock_irqsave(&vibrator->state_lock, flags);
	if (vibrator->pulse_active && vibrator->requested_on &&
	    !vibrator->suspended && !vibrator->tearing_down &&
	    !vibrator->faulted) {
		now = jiffies;
		deadline = vibrator->cutoff_deadline;
		if (time_before(now, deadline)) {
			delay = deadline - now;
		} else {
			vibrator->requested_on = false;
			apply = true;
		}
	}
	spin_unlock_irqrestore(&vibrator->state_lock, flags);

	if (delay)
		mod_delayed_work(system_wq, &vibrator->cutoff_work, delay);
	else if (apply)
		mod_delayed_work(system_wq, &vibrator->apply_work, 0);
}

static int ta1618_vibrator_play_effect(struct input_dev *input, void *data,
				       struct ff_effect *effect)
{
	struct ta1618_vibrator *vibrator = data;
	unsigned long delay = msecs_to_jiffies(TA1618_VIBRATOR_MAX_ON_MS);
	unsigned long flags;
	bool turn_on;

	(void)input;
	turn_on = effect->u.rumble.strong_magnitude ||
		  effect->u.rumble.weak_magnitude;

	spin_lock_irqsave(&vibrator->state_lock, flags);
	if (vibrator->tearing_down || vibrator->suspended ||
	    vibrator->faulted) {
		spin_unlock_irqrestore(&vibrator->state_lock, flags);
		return -ESHUTDOWN;
	}
	if (turn_on) {
		if (vibrator->pulse_active) {
			spin_unlock_irqrestore(&vibrator->state_lock, flags);
			return -EBUSY;
		}
		vibrator->pulse_active = true;
		vibrator->requested_on = true;
		vibrator->cutoff_deadline = jiffies + delay;
	} else {
		if (!vibrator->pulse_active) {
			spin_unlock_irqrestore(&vibrator->state_lock, flags);
			return 0;
		}
		vibrator->requested_on = false;
	}
	spin_unlock_irqrestore(&vibrator->state_lock, flags);

	if (turn_on)
		mod_delayed_work(system_wq, &vibrator->cutoff_work, delay);
	else
		cancel_delayed_work(&vibrator->cutoff_work);
	mod_delayed_work(system_wq, &vibrator->apply_work, 0);
	return 0;
}

static int ta1618_vibrator_force_off(struct ta1618_vibrator *vibrator)
{
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&vibrator->state_lock, flags);
	vibrator->requested_on = false;
	spin_unlock_irqrestore(&vibrator->state_lock, flags);
	cancel_delayed_work_sync(&vibrator->cutoff_work);
	cancel_delayed_work_sync(&vibrator->apply_work);

	mutex_lock(&vibrator->hw_lock);
	ret = ta1618_vibrator_restore_locked(vibrator);
	mutex_unlock(&vibrator->hw_lock);
	if (ret) {
		ta1618_vibrator_latch_fault(vibrator);
	} else {
		spin_lock_irqsave(&vibrator->state_lock, flags);
		vibrator->pulse_active = false;
		spin_unlock_irqrestore(&vibrator->state_lock, flags);
	}
	return ret;
}

static void ta1618_vibrator_close(struct input_dev *input)
{
	struct ta1618_vibrator *vibrator = input_get_drvdata(input);
	int ret;

	ret = ta1618_vibrator_force_off(vibrator);
	if (ret)
		dev_crit(vibrator->dev,
			 "failed to stop vibrator while closing input: %d\n",
			 ret);
}

static int ta1618_vibrator_suspend(struct device *dev)
{
	struct ta1618_vibrator *vibrator = dev_get_drvdata(dev);
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&vibrator->state_lock, flags);
	vibrator->suspended = true;
	spin_unlock_irqrestore(&vibrator->state_lock, flags);
	ret = ta1618_vibrator_force_off(vibrator);
	if (ret)
		dev_crit(dev, "failed to stop vibrator for suspend: %d\n", ret);
	return ret;
}

static int ta1618_vibrator_resume(struct device *dev)
{
	struct ta1618_vibrator *vibrator = dev_get_drvdata(dev);
	unsigned long flags;

	spin_lock_irqsave(&vibrator->state_lock, flags);
	if (!vibrator->tearing_down && !vibrator->faulted)
		vibrator->suspended = false;
	spin_unlock_irqrestore(&vibrator->state_lock, flags);
	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(ta1618_vibrator_pm_ops, ta1618_vibrator_suspend,
				ta1618_vibrator_resume);

static void ta1618_vibrator_teardown(struct ta1618_vibrator *vibrator)
{
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&vibrator->state_lock, flags);
	if (vibrator->tearing_down) {
		spin_unlock_irqrestore(&vibrator->state_lock, flags);
		return;
	}
	vibrator->tearing_down = true;
	vibrator->requested_on = false;
	spin_unlock_irqrestore(&vibrator->state_lock, flags);

	ret = ta1618_vibrator_force_off(vibrator);
	if (ret)
		dev_crit(vibrator->dev,
			 "failed to stop vibrator during teardown: %d\n", ret);
}

static int ta1618_vibrator_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct ta1618_vibrator *vibrator;
	struct resource *resource;
	u32 voltage_code;
	int ret;

	resource = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!resource || resource->start != SC2720_VIBR_CTRL0_PHYS ||
	    resource_size(resource) != sizeof(u32))
		return dev_err_probe(dev, -EINVAL,
				     "expected VIBR CTRL0 resource %#x/4\n",
				     SC2720_VIBR_CTRL0_PHYS);

	ret = device_property_read_u32(dev, "fplinux,voltage-code",
				       &voltage_code);
	if (ret)
		return dev_err_probe(dev, ret, "missing voltage code\n");
	if (voltage_code > FIELD_MAX(SC2720_VIBR_VOLTAGE_MASK))
		return dev_err_probe(
			dev, -EINVAL,
			"voltage code must be in the range 0..7\n");

	vibrator = devm_kzalloc(dev, sizeof(*vibrator), GFP_KERNEL);
	if (!vibrator)
		return -ENOMEM;
	vibrator->dev = dev;
	vibrator->voltage_code = voltage_code;
	mutex_init(&vibrator->hw_lock);
	spin_lock_init(&vibrator->state_lock);
	INIT_DELAYED_WORK(&vibrator->apply_work, ta1618_vibrator_apply_work);
	INIT_DELAYED_WORK(&vibrator->cutoff_work, ta1618_vibrator_cutoff_work);

	ret = ta1618_vibrator_read_initial(vibrator);
	if (ret)
		return dev_err_probe(
			dev, ret,
			"failed to read stable SC2720 vibrator state\n");
	if ((vibrator->initial_ctrl0 & SC2720_VIBR_POWER_DOWN_MASK) !=
	    SC2720_VIBR_POWER_DOWN_MASK)
		return dev_err_probe(dev, -EBUSY,
				     "refusing unexpected enabled vibrator\n");

	vibrator->input = devm_input_allocate_device(dev);
	if (!vibrator->input)
		return -ENOMEM;
	vibrator->input->name = "TA-1618 vibrator";
	vibrator->input->phys = "fplinux/vibrator0";
	vibrator->input->id.bustype = BUS_HOST;
	vibrator->input->close = ta1618_vibrator_close;
	input_set_drvdata(vibrator->input, vibrator);
	input_set_capability(vibrator->input, EV_FF, FF_RUMBLE);

	ret = input_ff_create_memless(vibrator->input, vibrator,
				      ta1618_vibrator_play_effect);
	if (ret)
		return dev_err_probe(
			dev, ret, "failed to create force-feedback device\n");
	platform_set_drvdata(pdev, vibrator);
	ret = input_register_device(vibrator->input);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to register vibrator input\n");

	return 0;
}

static void ta1618_vibrator_remove(struct platform_device *pdev)
{
	ta1618_vibrator_teardown(platform_get_drvdata(pdev));
}

static void ta1618_vibrator_shutdown(struct platform_device *pdev)
{
	ta1618_vibrator_teardown(platform_get_drvdata(pdev));
}

static const struct of_device_id ta1618_vibrator_of_match[] = {
	{ .compatible = "fplinux,ta1618-sc2720-vibrator" },
	{}
};
MODULE_DEVICE_TABLE(of, ta1618_vibrator_of_match);

static struct platform_driver ta1618_vibrator_driver = {
	.probe = ta1618_vibrator_probe,
	.remove = ta1618_vibrator_remove,
	.shutdown = ta1618_vibrator_shutdown,
	.driver = {
		.name = "ta1618-sc2720-vibrator",
		.of_match_table = ta1618_vibrator_of_match,
		.pm = pm_sleep_ptr(&ta1618_vibrator_pm_ops),
	},
};
module_platform_driver(ta1618_vibrator_driver);

MODULE_DESCRIPTION("Nokia TA-1618 SC2720 vibrator driver");
MODULE_LICENSE("GPL");
