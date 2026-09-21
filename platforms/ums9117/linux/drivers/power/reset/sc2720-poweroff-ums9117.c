// SPDX-License-Identifier: GPL-2.0-only
#include <linux/atomic.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/input.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/processor.h>
#include <linux/reboot.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/regmap.h>
#include <linux/spi/spi.h>
#include <linux/spi/sprd-adi.h>

#define SC2720_CHIP_ID_LOW 0xc00U
#define SC2720_CHIP_ID_HIGH 0xc04U
#define SC2720_CHGR_STATUS 0xe14U
#define SC2720_EXPECTED_ID_LOW 0xa003U
#define SC2720_EXPECTED_ID_HIGH 0x2720U
#define SC2720_CHGR_STATUS_CHARGER_ON BIT(3)
#define SC2720_POWER_OFF_WAIT_MS 50U
#define SC2720_POWER_KEY_HOLD_MS 5000U

struct sc2720_power_key {
	struct input_handle handle;
	struct delayed_work hold_work;
	atomic_t down;
};

struct sc2720_poweroff {
	struct regmap *regmap;
	struct spi_device *spi;
	struct input_handler input_handler;
	struct device_node *keypad_node;
};

static void __noreturn sc2720_halt(void)
{
	for (;;)
		cpu_relax();
}

static int sc2720_read_pmic_state(struct sc2720_poweroff *poweroff,
				  unsigned int *id_low, unsigned int *id_high,
				  unsigned int *charger)
{
	static const unsigned int regs[] = {
		SC2720_CHIP_ID_LOW,
		SC2720_CHIP_ID_HIGH,
		SC2720_CHGR_STATUS,
	};
	unsigned int values[ARRAY_SIZE(regs)];
	int ret;

	ret = regmap_multi_reg_read(poweroff->regmap, regs, values,
				    ARRAY_SIZE(regs));
	if (ret)
		return ret;
	*id_low = values[0];
	*id_high = values[1];
	if (charger)
		*charger = values[2];
	return 0;
}

static int sc2720_power_key_preflight(struct sc2720_poweroff *poweroff)
{
	unsigned int id_low;
	unsigned int id_high;
	unsigned int charger;
	int ret;

	ret = sc2720_read_pmic_state(poweroff, &id_low, &id_high, &charger);
	if (ret)
		return ret;
	if (id_low != SC2720_EXPECTED_ID_LOW ||
	    id_high != SC2720_EXPECTED_ID_HIGH)
		return -ENODEV;
	return charger & SC2720_CHGR_STATUS_CHARGER_ON ? -EBUSY : 0;
}

static void sc2720_power_key_hold_work(struct work_struct *work)
{
	struct sc2720_power_key *power_key = container_of(
		to_delayed_work(work), struct sc2720_power_key, hold_work);
	struct device *dev = &power_key->handle.dev->dev;
	int ret;

	if (atomic_cmpxchg(&power_key->down, 1, 0) != 1 ||
	    READ_ONCE(system_state) != SYSTEM_RUNNING)
		return;

	ret = sc2720_power_key_preflight(container_of(power_key->handle.handler,
						      struct sc2720_poweroff,
						      input_handler));
	if (ret == -EBUSY) {
		dev_warn(dev,
			 "power-key shutdown refused: charger input active\n");
		return;
	}
	if (ret) {
		dev_err(dev, "power-key shutdown refused: %pe\n", ERR_PTR(ret));
		return;
	}

	dev_dbg(dev, "power-key orderly shutdown requested\n");
	if (READ_ONCE(system_state) != SYSTEM_RUNNING)
		return;
	/* Never force power off if userspace cannot make storage safe. */
	orderly_poweroff(false);
}

static void sc2720_power_key_event(struct input_handle *handle,
				   unsigned int type, unsigned int code,
				   int value)
{
	struct sc2720_power_key *power_key =
		container_of(handle, struct sc2720_power_key, handle);

	if (type != EV_KEY || code != KEY_POWER || value == 2)
		return;
	if (!value) {
		atomic_set(&power_key->down, 0);
		cancel_delayed_work(&power_key->hold_work);
		return;
	}
	if (value == 1 && atomic_cmpxchg(&power_key->down, 0, 1) == 0)
		schedule_delayed_work(
			&power_key->hold_work,
			msecs_to_jiffies(SC2720_POWER_KEY_HOLD_MS));
}

static int sc2720_power_key_connect(struct input_handler *handler,
				    struct input_dev *dev,
				    const struct input_device_id *id)
{
	struct sc2720_poweroff *poweroff =
		container_of(handler, struct sc2720_poweroff, input_handler);
	struct sc2720_power_key *power_key;
	int ret;

	(void)id;
	if (!dev->dev.parent ||
	    dev->dev.parent->of_node != poweroff->keypad_node ||
	    !test_bit(KEY_POWER, dev->keybit))
		return -ENODEV;

	power_key = kzalloc(sizeof(*power_key), GFP_KERNEL);
	if (!power_key)
		return -ENOMEM;
	INIT_DELAYED_WORK(&power_key->hold_work, sc2720_power_key_hold_work);
	atomic_set(&power_key->down, 0);
	power_key->handle.dev = dev;
	power_key->handle.handler = handler;
	power_key->handle.name = "sc2720-power-key";

	ret = input_register_handle(&power_key->handle);
	if (ret)
		goto free;
	ret = input_open_device(&power_key->handle);
	if (ret)
		goto unregister;
	dev_dbg(&dev->dev, "power-key handler attached\n");
	return 0;

unregister:
	input_unregister_handle(&power_key->handle);
free:
	kfree(power_key);
	return ret;
}

static void sc2720_power_key_disconnect(struct input_handle *handle)
{
	struct sc2720_power_key *power_key =
		container_of(handle, struct sc2720_power_key, handle);

	atomic_set(&power_key->down, 0);
	cancel_delayed_work_sync(&power_key->hold_work);
	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(power_key);
}

static const struct input_device_id sc2720_power_key_ids[] = {
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT,
		.evbit = { BIT_MASK(EV_KEY) },
	},
	{}
};
MODULE_DEVICE_TABLE(input, sc2720_power_key_ids);

static int sc2720_power_off(struct sys_off_data *data)
{
	struct sc2720_poweroff *poweroff = data->cb_data;
	int ret;

	/* The transport owns this atomic transaction after device shutdown. */
	ret = sprd_adi_sc2720_power_off(poweroff->spi);
	if (ret)
		dev_emerg(&poweroff->spi->dev, "power-off refused: %pe\n",
			  ERR_PTR(ret));
	else {
		mdelay(SC2720_POWER_OFF_WAIT_MS);
		dev_emerg(&poweroff->spi->dev,
			  "power-off write completed but CPU still runs\n");
	}
	sc2720_halt();
}

static void sc2720_poweroff_unregister_input(void *data)
{
	input_unregister_handler(data);
}

static void sc2720_poweroff_put_keypad(void *data)
{
	of_node_put(data);
}

static int sc2720_poweroff_probe(struct platform_device *pdev)
{
	struct sc2720_poweroff *poweroff;
	unsigned int id_low;
	unsigned int id_high;
	int ret;

	poweroff = devm_kzalloc(&pdev->dev, sizeof(*poweroff), GFP_KERNEL);
	if (!poweroff)
		return -ENOMEM;
	poweroff->regmap = dev_get_regmap(pdev->dev.parent, NULL);
	if (!poweroff->regmap)
		return -EPROBE_DEFER;
	poweroff->spi = to_spi_device(pdev->dev.parent);
	poweroff->keypad_node =
		of_parse_phandle(pdev->dev.of_node, "fplinux,keypad", 0);
	if (!poweroff->keypad_node)
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "physical keypad reference is required\n");
	ret = devm_add_action_or_reset(&pdev->dev, sc2720_poweroff_put_keypad,
				       poweroff->keypad_node);
	if (ret)
		return ret;
	ret = sc2720_read_pmic_state(poweroff, &id_low, &id_high, NULL);
	if (ret)
		return ret;
	if (id_low != SC2720_EXPECTED_ID_LOW ||
	    id_high != SC2720_EXPECTED_ID_HIGH)
		return dev_err_probe(&pdev->dev, -ENODEV,
				     "unexpected SC2720 identity: %04x/%04x\n",
				     id_low, id_high);

	ret = devm_register_sys_off_handler(&pdev->dev, SYS_OFF_MODE_POWER_OFF,
					    SYS_OFF_PRIO_PLATFORM,
					    sc2720_power_off, poweroff);
	if (ret)
		return ret;
	poweroff->input_handler = (struct input_handler){
		.event = sc2720_power_key_event,
		.connect = sc2720_power_key_connect,
		.disconnect = sc2720_power_key_disconnect,
		.name = "sc2720-power-key",
		.id_table = sc2720_power_key_ids,
	};
	ret = input_register_handler(&poweroff->input_handler);
	if (ret)
		return ret;
	ret = devm_add_action_or_reset(&pdev->dev,
				       sc2720_poweroff_unregister_input,
				       &poweroff->input_handler);
	if (ret)
		return ret;
	dev_dbg(&pdev->dev, "power-off handler ready\n");
	return 0;
}

static const struct of_device_id sc2720_poweroff_of_match[] = {
	{ .compatible = "sprd,ums9117-sc2720-poweroff" },
	{},
};
MODULE_DEVICE_TABLE(of, sc2720_poweroff_of_match);

static struct platform_driver sc2720_poweroff_driver = {
	.probe = sc2720_poweroff_probe,
	.driver = {
		.name = "sc2720-poweroff-ums9117",
		.of_match_table = sc2720_poweroff_of_match,
	},
};
module_platform_driver(sc2720_poweroff_driver);

MODULE_DESCRIPTION("Guarded SC2720 power-off through UMS9117 ADI");
MODULE_LICENSE("GPL");
