// SPDX-License-Identifier: GPL-2.0-only
#include <linux/atomic.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/input/ums9117-keypad.h>
#include <linux/module.h>
#include <linux/notifier.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/processor.h>
#include <linux/reboot.h>
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

struct sc2720_poweroff {
	struct device *dev;
	struct regmap *regmap;
	struct spi_device *spi;
	struct notifier_block power_key_notifier;
	struct delayed_work hold_work;
	atomic_t power_key_down;
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
	struct sc2720_poweroff *poweroff = container_of(
		to_delayed_work(work), struct sc2720_poweroff, hold_work);
	int ret;

	if (atomic_cmpxchg(&poweroff->power_key_down, 1, 0) != 1 ||
	    READ_ONCE(system_state) != SYSTEM_RUNNING)
		return;

	ret = sc2720_power_key_preflight(poweroff);
	if (ret == -EBUSY) {
		dev_warn(poweroff->dev,
			 "power-key shutdown refused: charger input active\n");
		return;
	}
	if (ret) {
		dev_err(poweroff->dev, "power-key shutdown refused: %pe\n",
			ERR_PTR(ret));
		return;
	}

	dev_dbg(poweroff->dev, "power-key orderly shutdown requested\n");
	if (READ_ONCE(system_state) != SYSTEM_RUNNING)
		return;
	/* Never force power off if userspace cannot make storage safe. */
	orderly_poweroff(false);
}

static int sc2720_power_key_event(struct notifier_block *notifier,
				  unsigned long state, void *data)
{
	struct sc2720_poweroff *poweroff = container_of(
		notifier, struct sc2720_poweroff, power_key_notifier);
	const struct ums9117_keypad_power_event *event = data;

	if (event->keypad_node != poweroff->keypad_node)
		return NOTIFY_DONE;
	if (state == UMS9117_KEYPAD_POWER_RELEASE) {
		atomic_set(&poweroff->power_key_down, 0);
		cancel_delayed_work(&poweroff->hold_work);
	} else if (state == UMS9117_KEYPAD_POWER_PRESS &&
		   atomic_cmpxchg(&poweroff->power_key_down, 0, 1) == 0) {
		schedule_delayed_work(
			&poweroff->hold_work,
			msecs_to_jiffies(SC2720_POWER_KEY_HOLD_MS));
	}
	return NOTIFY_OK;
}

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

static void sc2720_poweroff_unregister_keypad(void *data)
{
	struct sc2720_poweroff *poweroff = data;

	ums9117_keypad_unregister_power_notifier(&poweroff->power_key_notifier);
	atomic_set(&poweroff->power_key_down, 0);
	cancel_delayed_work_sync(&poweroff->hold_work);
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
	poweroff->dev = &pdev->dev;
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
	INIT_DELAYED_WORK(&poweroff->hold_work, sc2720_power_key_hold_work);
	poweroff->power_key_notifier.notifier_call = sc2720_power_key_event;
	ret = ums9117_keypad_register_power_notifier(
		&poweroff->power_key_notifier);
	if (ret)
		return ret;
	ret = devm_add_action_or_reset(
		&pdev->dev, sc2720_poweroff_unregister_keypad, poweroff);
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
