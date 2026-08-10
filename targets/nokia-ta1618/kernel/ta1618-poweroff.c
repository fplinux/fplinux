// SPDX-License-Identifier: GPL-2.0-only
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/processor.h>
#include <linux/reboot.h>
#include <linux/soc/sprd/ums9117-adi.h>

#define SC2720_CHIP_ID_LOW 0xc00u
#define SC2720_CHIP_ID_HIGH 0xc04u
#define SC2720_POWER_PD_HW 0xc20u
#define SC2720_CHGR_STATUS 0xe14u
#define SC2720_EXPECTED_ID_LOW 0xa003u
#define SC2720_EXPECTED_ID_HIGH 0x2720u
#define SC2720_PWR_OFF_SEQ_EN BIT(0)
#define SC2720_CHGR_ON BIT(3)
#define SC2720_POWER_OFF_WAIT_MS 50u

static struct sys_off_handler *poweroff_handler;

static void __noreturn ta1618_halt(void)
{
	for (;;)
		cpu_relax();
}

static int ta1618_read_pmic_identity(u16 *id_low, u16 *id_high)
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
	end_ret = ums9117_adi_end(&transaction);
	if (!ret)
		ret = end_ret;
	return ret;
}

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
	if (!ret && (charger & SC2720_CHGR_ON))
		ret = -EBUSY;
	if (!ret)
		ret = ums9117_adi_read(&transaction, SC2720_POWER_PD_HW,
				       &power);
	if (!ret)
		ret = ums9117_adi_write_final(&transaction, SC2720_POWER_PD_HW,
					      power | SC2720_PWR_OFF_SEQ_EN);
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

	ret = ta1618_read_pmic_identity(&id_low, &id_high);
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
	pr_info("TA-1618 guarded SC2720 power-off handler ready\n");
	return 0;
}
device_initcall(ta1618_poweroff_init);
