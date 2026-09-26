// SPDX-License-Identifier: GPL-2.0-only
/* UMS9117 CM4 Bluetooth controller. */

#include <crypto/sha2.h>
#include <linux/bitops.h>
#include <linux/cpu.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/firmware.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/ktime.h>
#include <linux/mfd/syscon.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/suspend.h>
#include <linux/vmalloc.h>

#include <asm/barrier.h>

#include "cm4-hci.h"
#include "cm4-mailbox.h"
#include "cm4-power.h"
#include "cm4-setup.h"

#define UMS9117_CM4_FIRMWARE_FILES 4U
#define UMS9117_CM4_LOW_PHYS 0x80000000U
#define UMS9117_CM4_RESERVED_BYTES 0x00200000U
#define UMS9117_CM4_MAP_BYTES 0x00100000U
#define UMS9117_CM4_IMAGE_OFFSET 0x00020000U
#define UMS9117_CM4_IRAM_PHYS 0x50800000U
#define UMS9117_CM4_IRAM_BYTES 0x00001000U
#define UMS9117_CM4_BOOT_STACK 0x00000200U
#define UMS9117_CM4_BOOT_ENTRY 0x01020001U

#define UMS9117_PMU_CP_RESET 0x00b0U
#define UMS9117_PMU_POWER_STATUS 0x00c0U
#define UMS9117_PMU_SLEEP_CONTROL 0x00ccU
#define UMS9117_PMU_BT_DOMAIN_CONFIG 0x0104U
#define UMS9117_PMU_SET_CP_RESET 0x10b0U
#define UMS9117_PMU_SET_SLEEP_CONTROL 0x10ccU
#define UMS9117_PMU_SET_BT_DOMAIN_CONFIG 0x1104U
#define UMS9117_PMU_CLEAR_CP_RESET 0x20b0U
#define UMS9117_PMU_CLEAR_SLEEP_CONTROL 0x20ccU
#define UMS9117_PMU_CLEAR_BT_DOMAIN_CONFIG 0x2104U
#define UMS9117_AON_CM4_RESET 0x0114U
#define UMS9117_AON_CM4_BUS 0x0124U
#define UMS9117_AON_CM4_STATUS 0x0588U
#define UMS9117_AON_SET_CM4_RESET 0x1114U
#define UMS9117_AON_CLEAR_CM4_RESET 0x2114U
#define UMS9117_AON_CLEAR_CM4_BUS 0x2124U

#define UMS9117_BT_DOMAIN_STATE_SHIFT 15U
#define UMS9117_BT_DOMAIN_STATE_MASK 0x1fU
#define UMS9117_BT_DOMAIN_OFF 7U
#define UMS9117_BT_DOMAIN_ON 0U
#define UMS9117_BT_DOMAIN_RESET_CONFIG 0x0a208804U
#define UMS9117_BT_FORCE_SHUTDOWN BIT(25)
#define UMS9117_BT_AUTO_SHUTDOWN BIT(24)
#define UMS9117_BT_FORCE_DEEP_SLEEP BIT(21)
#define UMS9117_BT_SYSTEM_RESET BIT(8)
#define UMS9117_CM4_SYSTEM_RESET BIT(4)
#define UMS9117_CM4_CORE_RESET BIT(0)
#define UMS9117_CM4_RESET_MASK \
	(UMS9117_CM4_SYSTEM_RESET | UMS9117_CM4_CORE_RESET)
#define UMS9117_CM4_LOCKUP BIT(0)
#define UMS9117_CM4_BUS_PAUSE BIT(0)

#define UMS9117_BT_POWER_TIMEOUT_NS (100ULL * NSEC_PER_MSEC)
#define UMS9117_CM4_SETUP_TIMEOUT_NS (5ULL * NSEC_PER_SEC)

struct ums9117_bluetooth {
	struct device *dev;
	struct regmap *pmu;
	struct regmap *aon;
	void __iomem *low;
	void __iomem *iram;
	struct mutex lock;
	struct notifier_block pm_notifier;
	const char *firmware_names[UMS9117_CM4_FIRMWARE_FILES];
	u32 firmware_size;
	u8 firmware_sha256[SHA256_DIGEST_SIZE];
	u8 firmware_version[UMS9117_CM4_VERSION_SIZE];
	bool started;
	bool registered;
	bool shutting_down;
	unsigned long users;
	bool suspend_prepared;
	bool poll_released;
	bool power_changed;
	bool idle_poll_owned;
	int startup_error;
};

static void cm4_barrier(void)
{
	dsb(sy);
	isb(sy);
}

static int bt_domain_state(struct ums9117_bluetooth *bt)
{
	u32 value;
	int ret;

	ret = regmap_read(bt->pmu, UMS9117_PMU_POWER_STATUS, &value);
	if (ret)
		return ret;
	return (value >> UMS9117_BT_DOMAIN_STATE_SHIFT) &
	       UMS9117_BT_DOMAIN_STATE_MASK;
}

static int read_reset(struct ums9117_bluetooth *bt, u32 *cp, u32 *cm4)
{
	int ret;

	ret = regmap_read(bt->pmu, UMS9117_PMU_CP_RESET, cp);
	if (!ret)
		ret = regmap_read(bt->aon, UMS9117_AON_CM4_RESET, cm4);
	return ret;
}

static int check_cold_state(struct ums9117_bluetooth *bt)
{
	u32 config, sleep, cp, cm4;
	int ret;

	ret = regmap_read(bt->pmu, UMS9117_PMU_BT_DOMAIN_CONFIG, &config);
	if (!ret)
		ret = regmap_read(bt->pmu, UMS9117_PMU_SLEEP_CONTROL, &sleep);
	if (!ret)
		ret = read_reset(bt, &cp, &cm4);
	if (ret)
		return ret;
	if (config != UMS9117_BT_DOMAIN_RESET_CONFIG ||
	    bt_domain_state(bt) != UMS9117_BT_DOMAIN_OFF ||
	    !(sleep & UMS9117_BT_FORCE_DEEP_SLEEP) ||
	    !(cp & UMS9117_BT_SYSTEM_RESET) ||
	    (cm4 & UMS9117_CM4_RESET_MASK) != UMS9117_CM4_RESET_MASK)
		return -EUCLEAN;
	return 0;
}

static int hold_reset(struct ums9117_bluetooth *bt)
{
	u32 cp, cm4;
	int ret;

	ret = regmap_write(bt->aon, UMS9117_AON_SET_CM4_RESET,
			   UMS9117_CM4_RESET_MASK);
	cm4_barrier();
	if (!ret)
		ret = regmap_write(bt->pmu, UMS9117_PMU_SET_CP_RESET,
				   UMS9117_BT_SYSTEM_RESET);
	cm4_barrier();
	if (!ret)
		ret = read_reset(bt, &cp, &cm4);
	if (ret)
		return ret;
	return (cp & UMS9117_BT_SYSTEM_RESET) &&
			       (cm4 & UMS9117_CM4_RESET_MASK) ==
				       UMS9117_CM4_RESET_MASK ?
		       0 :
		       -EUCLEAN;
}

static int check_stopped_state(struct ums9117_bluetooth *bt)
{
	u32 config, sleep, cp, cm4;
	int state, ret;

	ret = regmap_read(bt->pmu, UMS9117_PMU_BT_DOMAIN_CONFIG, &config);
	if (!ret)
		ret = regmap_read(bt->pmu, UMS9117_PMU_SLEEP_CONTROL, &sleep);
	if (!ret)
		ret = read_reset(bt, &cp, &cm4);
	if (ret)
		return ret;
	state = bt_domain_state(bt);
	if (state < 0)
		return state;
	if (!(config & UMS9117_BT_FORCE_SHUTDOWN) ||
	    (config & UMS9117_BT_AUTO_SHUTDOWN) ||
	    !(sleep & UMS9117_BT_FORCE_DEEP_SLEEP) ||
	    !(cp & UMS9117_BT_SYSTEM_RESET) ||
	    (cm4 & UMS9117_CM4_RESET_MASK) != UMS9117_CM4_RESET_MASK ||
	    state != UMS9117_BT_DOMAIN_OFF) {
		dev_err(bt->dev,
			"stopped state invalid: domain=%d config=%#x sleep=%#x cp_reset=%#x cm4_reset=%#x\n",
			state, config, sleep, cp, cm4);
		return -EUCLEAN;
	}
	return 0;
}

/* The CM4 resets are held before changing its shared radio power requests. */
static int power_off(struct ums9117_bluetooth *bt)
{
	u64 deadline;
	u32 config;
	unsigned int stable = 0;
	int ret;

	ret = regmap_read(bt->pmu, UMS9117_PMU_BT_DOMAIN_CONFIG, &config);
	if (ret)
		return ret;
	if (config & UMS9117_BT_AUTO_SHUTDOWN)
		return -EBUSY;
	ret = regmap_write(bt->pmu, UMS9117_PMU_SET_BT_DOMAIN_CONFIG,
			   UMS9117_BT_FORCE_SHUTDOWN);
	cm4_barrier();
	if (!ret)
		ret = regmap_write(bt->pmu, UMS9117_PMU_SET_SLEEP_CONTROL,
				   UMS9117_BT_FORCE_DEEP_SLEEP);
	cm4_barrier();
	if (ret)
		return ret;
	deadline = ktime_get_ns() + UMS9117_BT_POWER_TIMEOUT_NS;
	do {
		ret = bt_domain_state(bt);
		if (ret < 0)
			return ret;
		if (ret == UMS9117_BT_DOMAIN_OFF) {
			if (++stable == 2)
				return check_stopped_state(bt);
		} else {
			stable = 0;
		}
		cpu_relax();
	} while (ktime_get_ns() < deadline);
	dev_err(bt->dev, "power-off timeout: domain=%d\n", ret);
	return -ETIMEDOUT;
}

static int release_reset(struct ums9117_bluetooth *bt)
{
	int ret;

	ret = regmap_write(bt->pmu, UMS9117_PMU_CLEAR_CP_RESET,
			   UMS9117_BT_SYSTEM_RESET);
	if (!ret)
		ret = regmap_write(bt->aon, UMS9117_AON_CLEAR_CM4_RESET,
				   UMS9117_CM4_SYSTEM_RESET);
	if (!ret)
		ret = regmap_write(bt->aon, UMS9117_AON_CLEAR_CM4_RESET,
				   UMS9117_CM4_CORE_RESET);
	cm4_barrier();
	return ret;
}

static int power_on(struct ums9117_bluetooth *bt)
{
	u64 started;
	u32 cp, cm4, bus;
	unsigned int stable = 0;
	int ret;

	/* SET/CLEAR aliases leave unrelated shared PMU/AON fields untouched. */
	bt->power_changed = true;
	ret = regmap_write(bt->pmu, UMS9117_PMU_CLEAR_BT_DOMAIN_CONFIG,
			   UMS9117_BT_FORCE_SHUTDOWN);
	cm4_barrier();
	if (!ret)
		ret = regmap_write(bt->pmu, UMS9117_PMU_CLEAR_SLEEP_CONTROL,
				   UMS9117_BT_FORCE_DEEP_SLEEP);
	cm4_barrier();
	if (ret)
		return ret;
	started = ktime_get_ns();
	do {
		ret = bt_domain_state(bt);
		if (ret < 0)
			return ret;
		if (ret == UMS9117_BT_DOMAIN_ON) {
			if (++stable == 2)
				break;
		} else {
			stable = 0;
		}
		cpu_relax();
	} while (ktime_get_ns() - started < UMS9117_BT_POWER_TIMEOUT_NS);
	if (stable != 2)
		return -ETIMEDOUT;
	ret = read_reset(bt, &cp, &cm4);
	if (ret)
		return ret;
	if (!(cp & UMS9117_BT_SYSTEM_RESET) ||
	    (cm4 & UMS9117_CM4_RESET_MASK) != UMS9117_CM4_RESET_MASK)
		return -EUCLEAN;
	/* The firmware's sleep request can remain latched across CM4 reset. */
	ret = regmap_read(bt->aon, UMS9117_AON_CM4_BUS, &bus);
	if (ret)
		return ret;
	if (bus & UMS9117_CM4_BUS_PAUSE) {
		dev_info(bt->dev, "restoring paused CM4 bus: before=%#x\n",
			 bus);
		ret = regmap_write(bt->aon, UMS9117_AON_CLEAR_CM4_BUS,
				   UMS9117_CM4_BUS_PAUSE);
		cm4_barrier();
		if (!ret)
			ret = regmap_read(bt->aon, UMS9117_AON_CM4_BUS, &bus);
		if (ret)
			return ret;
		dev_info(bt->dev, "CM4 bus restored: after=%#x\n", bus);
		if (bus & UMS9117_CM4_BUS_PAUSE)
			return -EUCLEAN;
	}

	writel_relaxed(UMS9117_CM4_BOOT_STACK, bt->iram);
	writel_relaxed(UMS9117_CM4_BOOT_ENTRY, bt->iram + 4);
	cm4_barrier();
	if (readl_relaxed(bt->iram) != UMS9117_CM4_BOOT_STACK ||
	    readl_relaxed(bt->iram + 4) != UMS9117_CM4_BOOT_ENTRY)
		return -EIO;
	return 0;
}

/* Distinguish an unprepared board from an incomplete firmware installation. */
static int missing_firmware_group(struct ums9117_bluetooth *bt)
{
	const struct firmware *firmware;
	unsigned int index;
	int ret;

	for (index = 1; index < UMS9117_CM4_FIRMWARE_FILES; index++) {
		ret = request_firmware_direct(
			&firmware, bt->firmware_names[index], bt->dev);
		if (!ret) {
			release_firmware(firmware);
			return -EINVAL;
		}
		if (ret != -ENOENT)
			return ret;
	}
	return -ENODATA;
}

static int load_firmware(struct ums9117_bluetooth *bt)
{
	const struct firmware *firmware;
	u8 digest[SHA256_DIGEST_SIZE];
	void *copy;
	int ret;

	ret = request_firmware_direct(&firmware, bt->firmware_names[0],
				      bt->dev);
	if (ret == -ENOENT)
		return missing_firmware_group(bt);
	if (ret)
		return ret;
	if (firmware->size != bt->firmware_size) {
		ret = -EBADMSG;
		goto out;
	}
	sha256(firmware->data, firmware->size, digest);
	if (memcmp(digest, bt->firmware_sha256, sizeof(digest))) {
		ret = -EKEYREJECTED;
		goto out;
	}
	ret = ums9117_cm4_setup_prepare(bt->dev, bt->firmware_names + 1,
					bt->firmware_version);
	if (ret == -ENOENT)
		ret = -EINVAL;
	if (ret)
		goto out;
	copy = kvmalloc(firmware->size, GFP_KERNEL);
	if (!copy) {
		ret = -ENOMEM;
		goto out;
	}
	memcpy_toio(bt->low + UMS9117_CM4_IMAGE_OFFSET, firmware->data,
		    firmware->size);
	cm4_barrier();
	memcpy_fromio(copy, bt->low + UMS9117_CM4_IMAGE_OFFSET, firmware->size);
	sha256(copy, firmware->size, digest);
	if (memcmp(digest, bt->firmware_sha256, sizeof(digest)))
		ret = -EIO;
	kvfree(copy);
out:
	release_firmware(firmware);
	return ret;
}

static int check_running(struct ums9117_bluetooth *bt)
{
	u32 cp, cm4, status;
	int ret;

	ret = read_reset(bt, &cp, &cm4);
	if (ret)
		return ret;
	if ((cp & UMS9117_BT_SYSTEM_RESET) || (cm4 & UMS9117_CM4_RESET_MASK))
		return -EHOSTDOWN;
	/* Calibration cycles BT power; read CM4 status only while BT is on. */
	ret = bt_domain_state(bt);
	if (ret < 0)
		return ret;
	if (ret != UMS9117_BT_DOMAIN_ON)
		return 0;
	ret = regmap_read(bt->aon, UMS9117_AON_CM4_STATUS, &status);
	if (ret)
		return ret;
	return status & UMS9117_CM4_LOCKUP ? -EIO : 0;
}

static int start_controller(struct ums9117_bluetooth *bt)
{
	unsigned long flags;
	const u8 *prefix;
	size_t prefix_bytes;
	u64 started;
	int ret;

	ret = ums9117_cm4_mailbox_prepare(bt->low);
	if (ret)
		return ret;
	/* Ordinary idle is polled; only a quiesced system suspend releases it. */
	cpu_idle_poll_ctrl(true);
	bt->idle_poll_owned = true;
	local_irq_save(flags);
	started = ktime_get_ns();
	ret = release_reset(bt);
	local_irq_restore(flags);
	if (ret)
		return ret;
	do {
		ret = check_running(bt);
		if (ret)
			return ret;
		ret = ums9117_cm4_mailbox_poll();
		if (ret && ret != -EINPROGRESS)
			return ret;
		ret = ums9117_cm4_setup_poll();
		if (ret != -EINPROGRESS)
			break;
		usleep_range(500, 1000);
	} while (ktime_get_ns() - started < UMS9117_CM4_SETUP_TIMEOUT_NS);
	if (ret == -EINPROGRESS)
		return -ETIMEDOUT;
	if (ret)
		return ret;
	ret = ums9117_cm4_mailbox_h4_continue();
	if (ret)
		return ret;
	prefix = ums9117_cm4_setup_rx_prefix(&prefix_bytes);
	if (bt->registered)
		return ums9117_hci_transport_start(prefix, prefix_bytes);
	return ums9117_hci_runtime_register(bt->dev, prefix, prefix_bytes);
}

static int stop_controller(struct ums9117_bluetooth *bt)
{
	unsigned long flags;
	int ret, drain;

	ums9117_hci_transport_stop();
	drain = ums9117_cm4_mailbox_stop();
	local_irq_save(flags);
	ret = hold_reset(bt);
	local_irq_restore(flags);
	if (ret) {
		dev_err(bt->dev, "reset hold failed: %pe; cold boot required\n",
			ERR_PTR(ret));
		return ret;
	}
	ret = power_off(bt);
	if (ret)
		return ret;
	if (bt->idle_poll_owned) {
		cpu_idle_poll_ctrl(false);
		bt->idle_poll_owned = false;
	}
	return drain;
}

/* The controller lock serializes every physical power transition. */
static int start_locked(struct ums9117_bluetooth *bt)
{
	int ret, stopped;

	if (bt->started)
		return 0;
	ret = bt->power_changed ? check_stopped_state(bt) :
				  check_cold_state(bt);
	if (!ret)
		ret = load_firmware(bt);
	if (!ret)
		ret = power_on(bt);
	if (!ret)
		ret = start_controller(bt);
	if (ret) {
		/* Missing inputs remain retryable before the first power request. */
		if (bt->power_changed) {
			bt->startup_error = ret;
			stopped = stop_controller(bt);
			if (stopped)
				dev_err(bt->dev,
					"startup cleanup failed: %pe\n",
					ERR_PTR(stopped));
			dev_err(bt->dev,
				"startup failed: %pe; cold boot required\n",
				ERR_PTR(ret));
		}
		return ret;
	}
	bt->started = true;
	bt->registered = true;
	return 0;
}

static int stop_locked(struct ums9117_bluetooth *bt)
{
	int ret;

	if (bt->startup_error)
		return bt->startup_error;
	if (!bt->started)
		return 0;
	ret = stop_controller(bt);
	if (ret) {
		bt->startup_error = ret;
		dev_err(bt->dev,
			"controller stop failed: %pe; cold boot required\n",
			ERR_PTR(ret));
	} else {
		bt->started = false;
	}
	return ret;
}

int ums9117_cm4_get(struct device *dev, enum ums9117_cm4_user user)
{
	struct ums9117_bluetooth *bt = dev_get_drvdata(dev);
	unsigned long mask = BIT(user);
	int ret;

	mutex_lock(&bt->lock);
	if (bt->shutting_down || !bt->registered)
		ret = -ESHUTDOWN;
	else if (bt->suspend_prepared)
		ret = -EBUSY;
	else if (bt->startup_error)
		ret = bt->startup_error;
	else if (bt->users & mask)
		ret = -EALREADY;
	else {
		ret = start_locked(bt);
		if (!ret)
			bt->users |= mask;
	}
	mutex_unlock(&bt->lock);
	return ret;
}

void ums9117_cm4_put(struct device *dev, enum ums9117_cm4_user user)
{
	struct ums9117_bluetooth *bt = dev_get_drvdata(dev);
	unsigned long mask = BIT(user);

	mutex_lock(&bt->lock);
	if (!(bt->users & mask))
		goto out;
	bt->users &= ~mask;
	/* PM and shutdown finish their use before releasing the last hold. */
	if (!bt->users && !bt->suspend_prepared && !bt->shutting_down)
		stop_locked(bt);
out:
	mutex_unlock(&bt->lock);
}

static void __iomem *map_memory(struct platform_device *pdev, const char *name,
				resource_size_t start, resource_size_t bytes)
{
	struct resource *resource;

	resource = platform_get_resource_byname(pdev, IORESOURCE_MEM, name);
	if (!resource || resource->start != start ||
	    resource_size(resource) != bytes)
		return IOMEM_ERR_PTR(-EINVAL);
	return devm_ioremap_resource(&pdev->dev, resource);
}

static ssize_t start_store(struct device *dev, struct device_attribute *attr,
			   const char *buffer, size_t count)
{
	struct ums9117_bluetooth *bt = dev_get_drvdata(dev);
	bool start;
	int ret;

	ret = kstrtobool(buffer, &start);
	if (ret)
		return ret;
	mutex_lock(&bt->lock);
	if (bt->suspend_prepared || bt->shutting_down) {
		ret = -EBUSY;
		goto out;
	}
	if (bt->startup_error) {
		ret = bt->startup_error;
		goto out;
	}
	if (!start) {
		ret = bt->users ? -EBUSY : stop_locked(bt);
		goto out;
	}
	if (bt->registered) {
		ret = 0;
		goto out;
	}
	/* Initial preparation still reports firmware errors synchronously. */
	ret = start_locked(bt);
out:
	mutex_unlock(&bt->lock);
	return ret ? ret : count;
}
static DEVICE_ATTR_WO(start);

static ssize_t state_show(struct device *dev, struct device_attribute *attr,
			  char *buffer)
{
	struct ums9117_bluetooth *bt = dev_get_drvdata(dev);
	const char *state;
	ssize_t bytes;

	mutex_lock(&bt->lock);
	state = bt->startup_error ? "error" :
		bt->started	  ? "running" :
				    "stopped";
	bytes = sysfs_emit(buffer, "%s\n", state);
	mutex_unlock(&bt->lock);
	return bytes;
}
static DEVICE_ATTR_RO(state);

static struct attribute *ums9117_bluetooth_attrs[] = {
	&dev_attr_start.attr,
	&dev_attr_state.attr,
	NULL,
};
ATTRIBUTE_GROUPS(ums9117_bluetooth);

static int bluetooth_pm_notify(struct notifier_block *notifier,
			       unsigned long action, void *unused)
{
	struct ums9117_bluetooth *bt =
		container_of(notifier, struct ums9117_bluetooth, pm_notifier);
	bool prepared, registered;
	int ret = 0;

	switch (action) {
	case PM_SUSPEND_PREPARE:
		/* Firmware start must finish before system sleep can be admitted. */
		if (!mutex_trylock(&bt->lock))
			return notifier_from_errno(-EBUSY);
		if (bt->startup_error)
			ret = bt->startup_error;
		else if (bt->shutting_down)
			ret = -ESHUTDOWN;
		else
			bt->suspend_prepared = true;
		registered = bt->registered;
		mutex_unlock(&bt->lock);
		/* HCI core waits may call open/close and acquire the owner lock. */
		if (!ret && registered)
			ret = ums9117_hci_suspend_prepare();
		if (ret) {
			mutex_lock(&bt->lock);
			bt->suspend_prepared = false;
			if (!bt->users)
				stop_locked(bt);
			mutex_unlock(&bt->lock);
		}
		break;
	case PM_POST_SUSPEND:
		mutex_lock(&bt->lock);
		prepared = bt->suspend_prepared;
		registered = bt->registered;
		mutex_unlock(&bt->lock);
		if (prepared && registered)
			ret = ums9117_hci_post_suspend();
		mutex_lock(&bt->lock);
		bt->suspend_prepared = false;
		if (!bt->users) {
			int stopped = stop_locked(bt);

			if (!ret)
				ret = stopped;
		}
		mutex_unlock(&bt->lock);
		if (ret)
			dev_err(bt->dev, "HCI resume failed: %pe\n",
				ERR_PTR(ret));
		break;
	default:
		return NOTIFY_DONE;
	}
	return notifier_from_errno(ret);
}

static void unregister_bluetooth_pm(void *data)
{
	struct ums9117_bluetooth *bt = data;

	unregister_pm_notifier(&bt->pm_notifier);
}

static int bluetooth_suspend(struct device *dev)
{
	struct ums9117_bluetooth *bt = dev_get_drvdata(dev);

	return bt->started ? ums9117_hci_suspend() : 0;
}

static int bluetooth_suspend_noirq(struct device *dev)
{
	struct ums9117_bluetooth *bt = dev_get_drvdata(dev);

	if (bt->started && bt->idle_poll_owned) {
		cpu_idle_poll_ctrl(false);
		bt->idle_poll_owned = false;
		bt->poll_released = true;
	}
	return 0;
}

static int bluetooth_resume_noirq(struct device *dev)
{
	struct ums9117_bluetooth *bt = dev_get_drvdata(dev);

	if (bt->poll_released) {
		cpu_idle_poll_ctrl(true);
		bt->idle_poll_owned = true;
		bt->poll_released = false;
	}
	return 0;
}

static int bluetooth_resume(struct device *dev)
{
	struct ums9117_bluetooth *bt = dev_get_drvdata(dev);

	return bt->started ? ums9117_hci_resume() : 0;
}

static const struct dev_pm_ops bluetooth_pm_ops = {
	.suspend = bluetooth_suspend,
	.suspend_noirq = bluetooth_suspend_noirq,
	.resume_noirq = bluetooth_resume_noirq,
	.resume = bluetooth_resume,
};

static int ums9117_bluetooth_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct ums9117_bluetooth *bt;
	int ret;

	if (IS_ENABLED(CONFIG_SMP) || !IS_ENABLED(CONFIG_PREEMPT_NONE))
		return -EOPNOTSUPP;
	if (region_intersects(UMS9117_CM4_LOW_PHYS, UMS9117_CM4_RESERVED_BYTES,
			      IORESOURCE_SYSTEM_RAM,
			      IORES_DESC_NONE) != REGION_DISJOINT)
		return dev_err_probe(dev, -EBUSY,
				     "memory overlaps System RAM\n");
	bt = devm_kzalloc(dev, sizeof(*bt), GFP_KERNEL);
	if (!bt)
		return -ENOMEM;
	bt->dev = dev;
	mutex_init(&bt->lock);
	if (of_property_count_strings(dev->of_node, "firmware-name") !=
		    UMS9117_CM4_FIRMWARE_FILES ||
	    of_property_count_u32_elems(dev->of_node, "fplinux,cm4-size") !=
		    1 ||
	    of_property_count_u8_elems(dev->of_node, "fplinux,cm4-version") !=
		    UMS9117_CM4_VERSION_SIZE ||
	    of_property_count_u8_elems(dev->of_node, "fplinux,cm4-sha256") !=
		    SHA256_DIGEST_SIZE)
		return dev_err_probe(
			dev, -EINVAL,
			"Bluetooth firmware names, size, version and digest are required\n");
	ret = of_property_read_string_array(dev->of_node, "firmware-name",
					    bt->firmware_names,
					    UMS9117_CM4_FIRMWARE_FILES);
	if (ret < 0)
		return ret;
	ret = of_property_read_u8_array(dev->of_node, "fplinux,cm4-sha256",
					bt->firmware_sha256,
					SHA256_DIGEST_SIZE);
	if (ret)
		return ret;
	ret = of_property_read_u8_array(dev->of_node, "fplinux,cm4-version",
					bt->firmware_version,
					UMS9117_CM4_VERSION_SIZE);
	if (ret)
		return ret;
	ret = of_property_read_u32(dev->of_node, "fplinux,cm4-size",
				   &bt->firmware_size);
	if (ret)
		return ret;
	if (!bt->firmware_size ||
	    bt->firmware_size >
		    UMS9117_CM4_MAP_BYTES - UMS9117_CM4_IMAGE_OFFSET)
		return dev_err_probe(
			dev, -EINVAL,
			"firmware size exceeds its memory window\n");
	bt->aon = syscon_regmap_lookup_by_phandle(dev->of_node, "sprd,aon-apb");
	if (IS_ERR(bt->aon))
		return dev_err_probe(dev, PTR_ERR(bt->aon),
				     "AON syscon unavailable\n");
	bt->pmu = syscon_regmap_lookup_by_phandle(dev->of_node, "sprd,pmu-apb");
	if (IS_ERR(bt->pmu))
		return dev_err_probe(dev, PTR_ERR(bt->pmu),
				     "PMU syscon unavailable\n");
	bt->low = map_memory(pdev, "sipc-cm4", UMS9117_CM4_LOW_PHYS,
			     UMS9117_CM4_MAP_BYTES);
	if (IS_ERR(bt->low))
		return dev_err_probe(dev, PTR_ERR(bt->low),
				     "memory unavailable\n");
	bt->iram = map_memory(pdev, "boot-vector", UMS9117_CM4_IRAM_PHYS,
			      UMS9117_CM4_IRAM_BYTES);
	if (IS_ERR(bt->iram))
		return dev_err_probe(dev, PTR_ERR(bt->iram),
				     "boot vector unavailable\n");
	/* Binding a mailbox channel resets its FIFO; the CM4 must be stopped. */
	ret = check_cold_state(bt);
	if (ret)
		return dev_err_probe(dev, ret,
				     "transport is not in cold state\n");
	ret = ums9117_cm4_mailbox_init(dev);
	if (ret)
		return dev_err_probe(dev, ret,
				     "mailbox resources unavailable\n");

	platform_set_drvdata(pdev, bt);
	bt->pm_notifier.notifier_call = bluetooth_pm_notify;
	ret = register_pm_notifier(&bt->pm_notifier);
	if (ret)
		return ret;
	ret = devm_add_action_or_reset(dev, unregister_bluetooth_pm, bt);
	if (ret)
		return ret;
	dev_dbg(dev, "transport ready for firmware start\n");
	return 0;
}

static void ums9117_bluetooth_shutdown(struct platform_device *pdev)
{
	struct ums9117_bluetooth *bt = platform_get_drvdata(pdev);
	bool registered;

	mutex_lock(&bt->lock);
	bt->shutting_down = true;
	registered = bt->registered;
	mutex_unlock(&bt->lock);
	/* Device teardown invokes client close callbacks, including owner put. */
	if (registered)
		ums9117_hci_runtime_unregister();
	mutex_lock(&bt->lock);
	bt->registered = false;
	if (bt->power_changed)
		stop_controller(bt);
	mutex_unlock(&bt->lock);
}

static const struct of_device_id ums9117_bluetooth_match[] = {
	{ .compatible = "fplinux,ums9117-bluetooth" },
	{}
};
MODULE_DEVICE_TABLE(of, ums9117_bluetooth_match);

static struct platform_driver ums9117_bluetooth_driver = {
	.probe = ums9117_bluetooth_probe,
	.shutdown = ums9117_bluetooth_shutdown,
	.driver = {
		.name = "ums9117-bluetooth",
		.of_match_table = ums9117_bluetooth_match,
		.suppress_bind_attrs = true,
		.dev_groups = ums9117_bluetooth_groups,
		.pm = &bluetooth_pm_ops,
	},
};

static int __init ums9117_bluetooth_init(void)
{
	return platform_driver_register(&ums9117_bluetooth_driver);
}
late_initcall(ums9117_bluetooth_init);

MODULE_DESCRIPTION("UMS9117 CM4 Bluetooth HCI transport");
MODULE_LICENSE("GPL");
