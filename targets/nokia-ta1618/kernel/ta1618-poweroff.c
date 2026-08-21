// SPDX-License-Identifier: GPL-2.0-only
#include <linux/atomic.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/input.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/processor.h>
#include <linux/reboot.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/syscalls.h>
#include <linux/workqueue.h>
#include <linux/soc/sprd/ums9117-adi.h>

#define SC2720_CHIP_ID_LOW 0xc00U
#define SC2720_CHIP_ID_HIGH 0xc04U
#define SC2720_POWER_PD_HW 0xc20U
#define SC2720_CHGR_STATUS 0xe14U
#define SC2720_EXPECTED_ID_LOW 0xa003U
#define SC2720_EXPECTED_ID_HIGH 0x2720U
#define SC2720_POWER_PD_HW_POWER_OFF_SEQUENCE_ENABLE BIT(0)
#define SC2720_CHGR_STATUS_CHARGER_ON BIT(3)
#define SC2720_POWER_OFF_WAIT_MS 50U
#define TA1618_POWER_KEY_HOLD_MS 5000U
#define TA1618_KEYPAD_NAME "TA-1618 keypad"
#define TA1618_KEYPAD_PHYS "ta1618/keypad0"

struct ta1618_power_key {
	struct input_handle handle;
	struct delayed_work hold_work;
	atomic_t down;
};

static struct sys_off_handler *poweroff_handler;

static void __noreturn ta1618_halt(void)
{
	for (;;)
		cpu_relax();
}

static int ta1618_read_pmic_state(u16 *id_low, u16 *id_high, u16 *charger)
{
	struct ums9117_adi_transaction transaction = {};
	int end_ret;
	int ret;

	ret = ums9117_adi_begin(&transaction);
	if (ret)
		return ret;
	ret = ums9117_adi_read(&transaction, SC2720_CHIP_ID_LOW, id_low);
	if (!ret)
		ret = ums9117_adi_read(&transaction, SC2720_CHIP_ID_HIGH,
				       id_high);
	if (!ret && charger)
		ret = ums9117_adi_read(&transaction, SC2720_CHGR_STATUS,
				       charger);
	end_ret = ums9117_adi_end(&transaction);
	if (!ret)
		ret = end_ret;
	return ret;
}

static int ta1618_power_key_preflight(void)
{
	u16 id_low;
	u16 id_high;
	u16 charger;
	int ret;

	ret = ta1618_read_pmic_state(&id_low, &id_high, &charger);
	if (ret)
		return ret;
	if (id_low != SC2720_EXPECTED_ID_LOW ||
	    id_high != SC2720_EXPECTED_ID_HIGH)
		return -ENODEV;
	return charger & SC2720_CHGR_STATUS_CHARGER_ON ? -EBUSY : 0;
}

static void ta1618_power_key_hold_work(struct work_struct *work)
{
	struct ta1618_power_key *power_key = container_of(
		to_delayed_work(work), struct ta1618_power_key, hold_work);
	int ret;

	if (atomic_cmpxchg(&power_key->down, 1, 0) != 1 ||
	    READ_ONCE(system_state) != SYSTEM_RUNNING)
		return;

	ret = ta1618_power_key_preflight();
	if (ret == -EBUSY) {
		pr_warn("TA-1618 five-second power-key shutdown refused: charger input active\n");
		return;
	}
	if (ret) {
		pr_err("TA-1618 five-second power-key shutdown refused: %d\n",
		       ret);
		return;
	}

	pr_info("TA-1618 five-second power-key hold accepted\n");
	ksys_sync();
	if (READ_ONCE(system_state) != SYSTEM_RUNNING)
		return;
	kernel_power_off();
	pr_emerg("TA-1618 power-key kernel_power_off returned\n");
	ta1618_halt();
}

static void ta1618_power_key_event(struct input_handle *handle,
				   unsigned int type, unsigned int code,
				   int value)
{
	struct ta1618_power_key *power_key =
		container_of(handle, struct ta1618_power_key, handle);

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
			msecs_to_jiffies(TA1618_POWER_KEY_HOLD_MS));
}

static int ta1618_power_key_connect(struct input_handler *handler,
				    struct input_dev *dev,
				    const struct input_device_id *id)
{
	struct ta1618_power_key *power_key;
	int ret;

	(void)id;
	if (!dev->name || strcmp(dev->name, TA1618_KEYPAD_NAME) || !dev->phys ||
	    strcmp(dev->phys, TA1618_KEYPAD_PHYS) ||
	    !test_bit(KEY_POWER, dev->keybit))
		return -ENODEV;

	power_key = kzalloc(sizeof(*power_key), GFP_KERNEL);
	if (!power_key)
		return -ENOMEM;
	INIT_DELAYED_WORK(&power_key->hold_work, ta1618_power_key_hold_work);
	atomic_set(&power_key->down, 0);
	power_key->handle.dev = dev;
	power_key->handle.handler = handler;
	power_key->handle.name = "ta1618-power-key";

	ret = input_register_handle(&power_key->handle);
	if (ret)
		goto free;
	ret = input_open_device(&power_key->handle);
	if (ret)
		goto unregister;
	pr_info("TA-1618 five-second power-key handler attached\n");
	return 0;

unregister:
	input_unregister_handle(&power_key->handle);
free:
	kfree(power_key);
	return ret;
}

static void ta1618_power_key_disconnect(struct input_handle *handle)
{
	struct ta1618_power_key *power_key =
		container_of(handle, struct ta1618_power_key, handle);

	atomic_set(&power_key->down, 0);
	cancel_delayed_work_sync(&power_key->hold_work);
	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(power_key);
}

static const struct input_device_id ta1618_power_key_ids[] = {
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT,
		.evbit = { BIT_MASK(EV_KEY) },
	},
	{}
};
MODULE_DEVICE_TABLE(input, ta1618_power_key_ids);

static struct input_handler ta1618_power_key_handler = {
	.event = ta1618_power_key_event,
	.connect = ta1618_power_key_connect,
	.disconnect = ta1618_power_key_disconnect,
	.name = "ta1618-power-key",
	.id_table = ta1618_power_key_ids,
};

static int ta1618_power_off(struct sys_off_data *data)
{
	struct ums9117_adi_transaction transaction = {};
	u16 id_low;
	u16 id_high;
	u16 charger;
	u16 power;
	int end_ret;
	int ret;

	(void)data;
	ret = ums9117_adi_begin(&transaction);
	if (ret)
		goto fail;
	ret = ums9117_adi_read(&transaction, SC2720_CHIP_ID_LOW, &id_low);
	if (!ret)
		ret = ums9117_adi_read(&transaction, SC2720_CHIP_ID_HIGH,
				       &id_high);
	if (!ret && (id_low != SC2720_EXPECTED_ID_LOW ||
		     id_high != SC2720_EXPECTED_ID_HIGH))
		ret = -ENODEV;
	if (!ret)
		ret = ums9117_adi_read(&transaction, SC2720_CHGR_STATUS,
				       &charger);
	if (!ret && (charger & SC2720_CHGR_STATUS_CHARGER_ON))
		ret = -EBUSY;
	if (!ret)
		ret = ums9117_adi_read(&transaction, SC2720_POWER_PD_HW,
				       &power);
	if (!ret)
		ret = ums9117_adi_write_final(
			&transaction, SC2720_POWER_PD_HW,
			power | SC2720_POWER_PD_HW_POWER_OFF_SEQUENCE_ENABLE);
	if (ret)
		goto fail_locked;

	mdelay(SC2720_POWER_OFF_WAIT_MS);
	pr_emerg(
		"TA-1618 SC2720 power-off write completed but CPU still runs\n");
	ta1618_halt();

fail_locked:
	end_ret = ums9117_adi_end(&transaction);
	if (end_ret)
		pr_emerg("TA-1618 SC2720 power-off ADI release failed: %d\n",
			 end_ret);
fail:
	pr_emerg("TA-1618 SC2720 power-off refused: %d\n", ret);
	ta1618_halt();
}

static int __init ta1618_poweroff_init(void)
{
	u16 id_low;
	u16 id_high;
	int ret;

	ret = ta1618_read_pmic_state(&id_low, &id_high, NULL);
	if (ret)
		return ret;
	if (id_low != SC2720_EXPECTED_ID_LOW ||
	    id_high != SC2720_EXPECTED_ID_HIGH) {
		pr_err("TA-1618 unexpected SC2720 identity: %04x/%04x\n",
		       id_low, id_high);
		return -ENODEV;
	}

	poweroff_handler = register_sys_off_handler(SYS_OFF_MODE_POWER_OFF,
						    SYS_OFF_PRIO_PLATFORM,
						    ta1618_power_off,
						    &poweroff_handler);
	if (IS_ERR(poweroff_handler)) {
		ret = PTR_ERR(poweroff_handler);
		poweroff_handler = NULL;
		return ret;
	}
	ret = input_register_handler(&ta1618_power_key_handler);
	if (ret) {
		unregister_sys_off_handler(poweroff_handler);
		poweroff_handler = NULL;
		return ret;
	}
	pr_info("TA-1618 guarded SC2720 power-off handler ready\n");
	return 0;
}
device_initcall(ta1618_poweroff_init);
