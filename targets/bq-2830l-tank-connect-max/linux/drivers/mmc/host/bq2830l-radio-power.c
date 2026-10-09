// SPDX-License-Identifier: GPL-2.0-only

#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/completion.h>
#include <linux/etherdevice.h>
#include <linux/firmware.h>
#include <linux/property.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/gpio/consumer.h>
#include <linux/jiffies.h>
#include <linux/mfd/syscon.h>
#include <linux/mmc/card.h>
#include <linux/mmc/host.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>

#include "bq2830l-radio.h"
#include "marlin2-bluetooth.h"
#include "marlin2-fm.h"
#include "marlin2-transport.h"
#include "marlin2-wlan.h"

#define BQ2830L_AUX0_GATE_REG 0x4
#define BQ2830L_AUX0_GATE_SET 0x1004
#define BQ2830L_AUX0_GATE_CLEAR 0x2004
#define BQ2830L_AUX0_GATE BIT(2)
#define BQ2830L_WCN_VOLTAGE_UV 1587500

static const struct {
	u32 offset;
	u32 value;
} bq2830l_radio_pins[] = {
	{ 0x054, 0x30 },     { 0x454, 0x82046 },  { 0x1b0, 0 },
	{ 0x5b0, 0x80001 },  { 0x058, 0x30 },	  { 0x458, 0x82001 },
	{ 0x064, 0x30 },     { 0x464, 0x102001 }, { 0x1c4, 0 },
	{ 0x1c8, 0 },	     { 0x1cc, 0 },	  { 0x1d0, 0 },
	{ 0x1d4, 0 },	     { 0x1d8, 0 },	  { 0x5c4, 0x102004 },
	{ 0x5c8, 0x102088 }, { 0x5cc, 0x102088 }, { 0x5d0, 0x102088 },
	{ 0x5d4, 0x102088 }, { 0x5d8, 0x102088 },
};

struct bq2830l_radio {
	struct device *dev;
	struct regmap *aon;
	struct regmap *pins;
	struct regulator *wcn;
	struct regulator *wifipa;
	struct gpio_desc *enable;
	struct gpio_desc *reset;
	struct gpio_desc *rx_request;
	struct clk_hw aux0_hw;
	struct clk *aux0;
	struct delayed_work cleanup_work;
	struct completion runtime_ready;
	struct sdio_func *runtime_func;
	struct marlin2_transport *transport;
	struct marlin2_wlan *wlan;
	struct marlin2_bluetooth *bluetooth;
	struct marlin2_fm *fm;
	/* HCI setup may take cleanup_lock while start waits for its work. */
	struct mutex bluetooth_lock;
	bool shutting_down;
	bool runtime_requested;
	/* Serializes child creation/removal and restoration of hardware state. */
	struct mutex cleanup_lock;
	/* Protects the result shared by the SDIO probe, cleanup and sysfs. */
	spinlock_t result_lock;
	u32 saved_pins[ARRAY_SIZE(bq2830l_radio_pins)];
	struct marlin2_boot_result boot;
	unsigned long deadline;
	u16 vendor;
	u16 device;
	u16 max_block_bytes;
	u8 function;
	u8 class;
	int status;
	int cleanup_error;
	int saved_voltage_uv;
	int enable_direction;
	int enable_value;
	int reset_direction;
	int reset_value;
	int rx_request_direction;
	int rx_request_value;
	bool saved_gate;
	bool pins_saved;
	bool gpio_saved;
	bool controls_ready;
	bool clock_enabled;
	bool voltage_owned;
	bool rail_enabled;
	bool wifipa_enabled;
	bool children_created;
	bool reported;
	bool cleanup_done;
	bool restored;
	bool active;
};

static void record_cleanup_error(struct bq2830l_radio *radio, int ret)
{
	unsigned long flags;

	if (!ret)
		return;
	spin_lock_irqsave(&radio->result_lock, flags);
	if (!radio->cleanup_error)
		radio->cleanup_error = ret;
	spin_unlock_irqrestore(&radio->result_lock, flags);
	dev_err(radio->dev, "failed to restore radio state: %pe\n",
		ERR_PTR(ret));
}

static int bq2830l_aux0_prepare(struct clk_hw *hw)
{
	struct bq2830l_radio *radio =
		container_of(hw, struct bq2830l_radio, aux0_hw);
	unsigned int value;
	int ret;

	ret = regmap_read(radio->aon, BQ2830L_AUX0_GATE_REG, &value);
	if (ret)
		return ret;
	radio->saved_gate = !!(value & BQ2830L_AUX0_GATE);
	return regmap_write(radio->aon, BQ2830L_AUX0_GATE_SET,
			    BQ2830L_AUX0_GATE);
}

static void bq2830l_aux0_unprepare(struct clk_hw *hw)
{
	struct bq2830l_radio *radio =
		container_of(hw, struct bq2830l_radio, aux0_hw);
	int ret;

	ret = regmap_write(radio->aon,
			   radio->saved_gate ? BQ2830L_AUX0_GATE_SET :
					       BQ2830L_AUX0_GATE_CLEAR,
			   BQ2830L_AUX0_GATE);
	record_cleanup_error(radio, ret);
}

static const struct clk_ops bq2830l_aux0_ops = {
	.prepare = bq2830l_aux0_prepare,
	.unprepare = bq2830l_aux0_unprepare,
};

static const struct clk_init_data bq2830l_aux0_init = {
	.name = "bq2830l-aux0",
	.ops = &bq2830l_aux0_ops,
	/* This owner changes only the gate and leaves the inherited source alone. */
	.flags = CLK_IGNORE_UNUSED,
};

static int save_gpio_state(struct bq2830l_radio *radio)
{
	radio->enable_direction = gpiod_get_direction(radio->enable);
	if (radio->enable_direction < 0)
		return radio->enable_direction;
	radio->reset_direction = gpiod_get_direction(radio->reset);
	if (radio->reset_direction < 0)
		return radio->reset_direction;
	radio->enable_value = gpiod_get_raw_value_cansleep(radio->enable);
	if (radio->enable_value < 0)
		return radio->enable_value;
	radio->reset_value = gpiod_get_raw_value_cansleep(radio->reset);
	if (radio->reset_value < 0)
		return radio->reset_value;
	radio->rx_request_direction = gpiod_get_direction(radio->rx_request);
	if (radio->rx_request_direction < 0)
		return radio->rx_request_direction;
	radio->rx_request_value =
		gpiod_get_raw_value_cansleep(radio->rx_request);
	if (radio->rx_request_value < 0)
		return radio->rx_request_value;
	radio->gpio_saved = true;
	return 0;
}

static int apply_radio_pins(struct bq2830l_radio *radio)
{
	unsigned int index;
	int ret;

	for (index = 0; index < ARRAY_SIZE(bq2830l_radio_pins); index++) {
		ret = regmap_read(radio->pins, bq2830l_radio_pins[index].offset,
				  &radio->saved_pins[index]);
		if (ret)
			return ret;
	}
	radio->pins_saved = true;
	for (index = 0; index < ARRAY_SIZE(bq2830l_radio_pins); index++) {
		ret = regmap_write(radio->pins,
				   bq2830l_radio_pins[index].offset,
				   bq2830l_radio_pins[index].value);
		if (ret)
			return ret;
	}
	return 0;
}

static void restore_gpio(struct bq2830l_radio *radio, struct gpio_desc *gpio,
			 int direction, int value)
{
	int ret;

	if (direction)
		ret = gpiod_direction_input(gpio);
	else
		ret = gpiod_direction_output_raw(gpio, value);
	record_cleanup_error(radio, ret);
}

static void cleanup_radio(struct bq2830l_radio *radio)
{
	unsigned long flags;
	unsigned int index;
	int ret;

	mutex_lock(&radio->cleanup_lock);
	if (radio->cleanup_done)
		goto out_unlock;
	if (radio->children_created) {
		of_platform_depopulate(radio->dev);
		radio->children_created = false;
	}
	if (radio->controls_ready) {
		gpiod_set_value_cansleep(radio->reset, 1);
		gpiod_set_value_cansleep(radio->enable, 0);
		radio->controls_ready = false;
	}
	if (radio->clock_enabled) {
		clk_disable_unprepare(radio->aux0);
		radio->clock_enabled = false;
	}
	if (radio->wifipa_enabled) {
		ret = regulator_disable(radio->wifipa);
		record_cleanup_error(radio, ret);
		radio->wifipa_enabled = false;
	}
	if (radio->rail_enabled) {
		ret = regulator_disable(radio->wcn);
		record_cleanup_error(radio, ret);
		radio->rail_enabled = false;
	}
	if (radio->voltage_owned) {
		ret = regulator_set_voltage(radio->wcn, radio->saved_voltage_uv,
					    radio->saved_voltage_uv);
		record_cleanup_error(radio, ret);
		radio->voltage_owned = false;
	}
	if (radio->gpio_saved) {
		restore_gpio(radio, radio->rx_request,
			     radio->rx_request_direction,
			     radio->rx_request_value);
		restore_gpio(radio, radio->reset, radio->reset_direction,
			     radio->reset_value);
		restore_gpio(radio, radio->enable, radio->enable_direction,
			     radio->enable_value);
		radio->gpio_saved = false;
	}
	if (radio->pins_saved) {
		for (index = 0; index < ARRAY_SIZE(bq2830l_radio_pins);
		     index++) {
			ret = regmap_write(radio->pins,
					   bq2830l_radio_pins[index].offset,
					   radio->saved_pins[index]);
			record_cleanup_error(radio, ret);
		}
		radio->pins_saved = false;
	}
	spin_lock_irqsave(&radio->result_lock, flags);
	if (!radio->reported)
		radio->status = -ETIMEDOUT;
	radio->cleanup_done = true;
	radio->restored = !radio->cleanup_error && !radio->boot.cleanup_error;
	spin_unlock_irqrestore(&radio->result_lock, flags);
	radio->active = false;
out_unlock:
	mutex_unlock(&radio->cleanup_lock);
}

bool bq2830l_radio_runtime_requested(struct sdio_func *func)
{
	struct device *parent = func->card->host->parent->parent;
	struct bq2830l_radio *radio = dev_get_drvdata(parent);

	return READ_ONCE(radio->runtime_requested);
}

unsigned long bq2830l_radio_deadline(struct sdio_func *func)
{
	struct device *parent = func->card->host->parent->parent;
	struct bq2830l_radio *radio = dev_get_drvdata(parent);

	return radio->deadline;
}

struct gpio_desc *bq2830l_radio_rx_request(struct sdio_func *func)
{
	struct device *parent = func->card->host->parent->parent;
	struct bq2830l_radio *radio = dev_get_drvdata(parent);

	return radio->rx_request;
}

static void bq2830l_radio_cleanup_work(struct work_struct *work)
{
	struct delayed_work *delayed = to_delayed_work(work);
	struct bq2830l_radio *radio =
		container_of(delayed, struct bq2830l_radio, cleanup_work);

	cleanup_radio(radio);
}

static void bq2830l_radio_cleanup(void *data)
{
	struct bq2830l_radio *radio = data;

	cancel_delayed_work_sync(&radio->cleanup_work);
	cleanup_radio(radio);
}

void bq2830l_radio_report(struct sdio_func *func, int error,
			  const struct marlin2_boot_result *result)
{
	struct device *parent = func->card->host->parent->parent;
	struct bq2830l_radio *radio;
	unsigned long flags;

	if (!parent ||
	    !of_device_is_compatible(parent->of_node, "bq,2830l-radio"))
		return;
	radio = dev_get_drvdata(parent);
	spin_lock_irqsave(&radio->result_lock, flags);
	if (!radio->reported) {
		radio->vendor = func->vendor;
		radio->device = func->device;
		radio->function = func->num;
		radio->class = func->class;
		radio->max_block_bytes = func->max_blksize;
		radio->boot = *result;
		radio->status = error;
		radio->reported = true;
		if (radio->runtime_requested)
			radio->runtime_func = error ? NULL : func;
	}
	spin_unlock_irqrestore(&radio->result_lock, flags);
	if (READ_ONCE(radio->runtime_requested))
		complete(&radio->runtime_ready);
	else
		mod_delayed_work(system_percpu_wq, &radio->cleanup_work, 0);
}

void bq2830l_radio_runtime_cleanup_error(struct sdio_func *func, int error)
{
	struct device *parent = func->card->host->parent->parent;
	struct bq2830l_radio *radio = dev_get_drvdata(parent);
	unsigned long flags;

	if (!error)
		return;
	spin_lock_irqsave(&radio->result_lock, flags);
	if (!radio->boot.cleanup_error)
		radio->boot.cleanup_error = error;
	spin_unlock_irqrestore(&radio->result_lock, flags);
}

static const char *boot_phase_name(enum marlin2_boot_phase phase)
{
	static const char *const names[] = {
		[MARLIN2_BOOT_IDLE] = "idle",
		[MARLIN2_BOOT_FIRMWARE_REQUEST] = "firmware-request",
		[MARLIN2_BOOT_CHIP_ID] = "chip-id",
		[MARLIN2_BOOT_IMAGE_DOWNLOAD] = "image-download",
		[MARLIN2_BOOT_CPU_START] = "cpu-start",
		[MARLIN2_BOOT_CALIBRATION_DOWNLOAD] = "calibration-download",
		[MARLIN2_BOOT_CALIBRATION_WAIT] = "calibration-wait",
		[MARLIN2_BOOT_CALIBRATION_ACK] = "calibration-ack",
		[MARLIN2_BOOT_READY_WAIT] = "ready-wait",
		[MARLIN2_BOOT_READY_ACK] = "ready-ack",
		[MARLIN2_BOOT_GET_INFO] = "get-info",
		[MARLIN2_BOOT_COMPLETE] = "complete",
	};

	return names[phase];
}

static ssize_t result_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct bq2830l_radio *radio = dev_get_drvdata(dev);
	unsigned long flags;
	ssize_t size;

	spin_lock_irqsave(&radio->result_lock, flags);
	size = sysfs_emit(buf, "error=%d cleanup_error=%d restored=%u",
			  radio->status, radio->cleanup_error, radio->restored);
	size += sysfs_emit_at(buf, size, " function=%u class=0x%02x",
			      radio->function, radio->class);
	size += sysfs_emit_at(buf, size, " vendor=0x%04x device=0x%04x",
			      radio->vendor, radio->device);
	size += sysfs_emit_at(buf, size, " max_block_bytes=%u chip_id0=0x%08x",
			      radio->max_block_bytes, radio->boot.chip_id[0]);
	size += sysfs_emit_at(buf, size,
			      " chip_id1=0x%08x phase=%s image_bytes=%u",
			      radio->boot.chip_id[1],
			      boot_phase_name(radio->boot.phase),
			      radio->boot.image_bytes);
	size += sysfs_emit_at(buf, size,
			      " image_verified=%u calibration_verified=%u",
			      radio->boot.image_verified,
			      radio->boot.calibration_verified);
	size += sysfs_emit_at(buf, size, " calibration_flag=0x%08x",
			      radio->boot.calibration_flag);
	size += sysfs_emit_at(buf, size, " interrupt_status=0x%02x",
			      radio->boot.interrupt_status);
	size += sysfs_emit_at(buf, size, " ready_flag=0x%08x",
			      radio->boot.ready_flag);
	size += sysfs_emit_at(buf, size, " sdio_cleanup_error=%d ready=%u",
			      radio->boot.cleanup_error, radio->boot.ready);
	size += sysfs_emit_at(buf, size, " packet_attempted=%u packet_error=%d",
			      radio->boot.packet_attempted,
			      radio->boot.packet_error);
	size += sysfs_emit_at(buf, size,
			      " packet_multi_block=%u packet_request_sent=%u",
			      radio->boot.packet.multi_block,
			      radio->boot.packet.request_sent);
	size += sysfs_emit_at(
		buf, size,
		" packet_rx_request_seen=%u packet_response_received=%u",
		radio->boot.packet.rx_request_seen,
		radio->boot.packet.response_received);
	size += sysfs_emit_at(buf, size,
			      " packet_status=%d packet_status_hex=0x%02x",
			      radio->boot.packet.status,
			      (u8)radio->boot.packet.status);
	size += sysfs_emit_at(buf, size,
			      " packet_token=0x%08x packet_response_length=%u",
			      radio->boot.packet.token,
			      radio->boot.packet.response_length);
	size += sysfs_emit_at(buf, size, " packet_header=%*phN", 12,
			      radio->boot.packet.response_header);
	if (radio->boot.packet.response_received && !radio->boot.packet_error)
		size += sysfs_emit_at(buf, size, " packet_payload=%*phN", 28,
				      radio->boot.packet.response_payload);
	size += sysfs_emit_at(
		buf, size,
		" packet_aggregates=%u packet_records=%u packet_ignored=%u",
		radio->boot.packet.aggregates, radio->boot.packet.records,
		radio->boot.packet.ignored_records);
	size += sysfs_emit_at(
		buf, size, " packet_used=%u packet_next=%u packet_puh=0x%08x",
		radio->boot.packet.used_payload_bytes,
		radio->boot.packet.next_payload_bytes,
		radio->boot.packet.last_puh);
	size += sysfs_emit_at(
		buf, size, " wifi_chip_model=0x%08x wifi_chip_version=0x%08x",
		radio->boot.packet.info.chip_model,
		radio->boot.packet.info.chip_version);
	size += sysfs_emit_at(
		buf, size,
		" wifi_firmware_version=0x%08x wifi_standards=0x%08x",
		radio->boot.packet.info.firmware_version,
		radio->boot.packet.info.standards);
	size += sysfs_emit_at(
		buf, size, " wifi_capabilities=0x%08x wifi_max_ap_stations=%u",
		radio->boot.packet.info.capabilities,
		radio->boot.packet.info.max_ap_stations);
	size += sysfs_emit_at(
		buf, size,
		" wifi_max_acl_addresses=%u wifi_max_multicast_addresses=%u",
		radio->boot.packet.info.max_acl_addresses,
		radio->boot.packet.info.max_multicast_addresses);
	size += sysfs_emit_at(
		buf, size,
		" wifi_wnm_ft_support=%u wifi_extended_features=0x%08x\n",
		radio->boot.packet.info.wnm_ft_support,
		radio->boot.packet.info.extended_features);
	spin_unlock_irqrestore(&radio->result_lock, flags);
	return size;
}
static DEVICE_ATTR_RO(result);

static ssize_t boot_store(struct device *dev, struct device_attribute *attr,
			  const char *buf, size_t count);
static DEVICE_ATTR_WO(boot);

#if IS_ENABLED(CONFIG_BQ2830L_BLUETOOTH)
static ssize_t start_store(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t count);
static DEVICE_ATTR_WO(start);
#endif

static struct attribute *bq2830l_radio_attrs[] = {
	&dev_attr_result.attr,
	&dev_attr_boot.attr,
#if IS_ENABLED(CONFIG_BQ2830L_BLUETOOTH)
	&dev_attr_start.attr,
#endif
	NULL,
};

static const struct attribute_group bq2830l_radio_group = {
	.attrs = bq2830l_radio_attrs,
};

static void remove_radio_group(void *context)
{
	struct device *dev = context;

	device_remove_group(dev, &bq2830l_radio_group);
}

static int power_on_radio(struct bq2830l_radio *radio)
{
	int ret;

	ret = gpiod_direction_input(radio->rx_request);
	if (ret)
		return ret;
	ret = apply_radio_pins(radio);
	if (ret)
		return ret;
	ret = clk_prepare_enable(radio->aux0);
	if (ret)
		return ret;
	radio->clock_enabled = true;
	radio->voltage_owned = true;
	ret = regulator_set_voltage(radio->wcn, BQ2830L_WCN_VOLTAGE_UV,
				    BQ2830L_WCN_VOLTAGE_UV);
	if (ret)
		return ret;
	ret = regulator_enable(radio->wcn);
	if (ret)
		return ret;
	radio->rail_enabled = true;
	ret = regulator_enable(radio->wifipa);
	if (ret)
		return ret;
	radio->wifipa_enabled = true;
	ret = gpiod_direction_output(radio->reset, 1);
	if (ret)
		return ret;
	ret = gpiod_direction_output(radio->enable, 0);
	if (ret)
		return ret;
	radio->controls_ready = true;
	msleep(100);
	gpiod_set_value_cansleep(radio->enable, 1);
	fsleep(2000);
	gpiod_set_value_cansleep(radio->reset, 1);
	fsleep(2000);
	gpiod_set_value_cansleep(radio->reset, 0);
	fsleep(2000);
	return 0;
}

static int start_radio_locked(struct bq2830l_radio *radio)
{
	struct device *dev = radio->dev;
	unsigned long flags;
	int ret;

	if (radio->active)
		return -EBUSY;
	radio->active = true;
	radio->deadline = jiffies + msecs_to_jiffies(BQ2830L_RADIO_TIMEOUT_MS);
	spin_lock_irqsave(&radio->result_lock, flags);
	memset(&radio->boot, 0, sizeof(radio->boot));
	radio->vendor = 0;
	radio->device = 0;
	radio->function = 0;
	radio->class = 0;
	radio->max_block_bytes = 0;
	radio->status = -EINPROGRESS;
	radio->reported = false;
	radio->cleanup_done = false;
	radio->cleanup_error = 0;
	radio->restored = false;
	spin_unlock_irqrestore(&radio->result_lock, flags);
	radio->saved_voltage_uv = regulator_get_voltage(radio->wcn);
	if (radio->saved_voltage_uv < 0) {
		ret = radio->saved_voltage_uv;
		goto out_error;
	}
	ret = save_gpio_state(radio);
	if (ret)
		goto out_error;
	ret = power_on_radio(radio);
	if (ret)
		goto out_error;
	if (time_after_eq(jiffies, radio->deadline)) {
		ret = -ETIMEDOUT;
		goto out_error;
	}
	radio->children_created = true;
	if (!radio->runtime_requested)
		mod_delayed_work(system_percpu_wq, &radio->cleanup_work,
				 radio->deadline - jiffies);
	ret = of_platform_populate(dev->of_node, NULL, NULL, dev);
	if (ret)
		goto out_error;
	return 0;

out_error:
	spin_lock_irqsave(&radio->result_lock, flags);
	radio->status = ret;
	radio->reported = true;
	spin_unlock_irqrestore(&radio->result_lock, flags);
	return ret;
}

static ssize_t boot_store(struct device *dev, struct device_attribute *attr,
			  const char *buf, size_t count)
{
	struct bq2830l_radio *radio = dev_get_drvdata(dev);
	int ret;

	if (!sysfs_streq(buf, "1"))
		return -EINVAL;
	mutex_lock(&radio->cleanup_lock);
	ret = READ_ONCE(radio->shutting_down) ? -ENODEV :
						start_radio_locked(radio);
	mutex_unlock(&radio->cleanup_lock);
	if (ret && ret != -EBUSY)
		mod_delayed_work(system_percpu_wq, &radio->cleanup_work, 0);
	return ret ? ret : count;
}

#if IS_ENABLED(CONFIG_BQ2830L_WLAN)
static int get_mac(void *context, u8 mac[ETH_ALEN])
{
	struct bq2830l_radio *radio = context;
	const struct firmware *address = NULL;
	const char *name;
	int ret;

	ret = device_property_read_string(radio->dev, "mac-address-name",
					  &name);
	if (ret)
		return ret;
	ret = request_firmware_direct(&address, name, radio->dev);
	if (ret)
		return ret;
	if (address->size != ETH_ALEN || !is_valid_ether_addr(address->data)) {
		ret = -EINVAL;
		goto out_address;
	}
	ether_addr_copy(mac, address->data);
	ret = 0;
out_address:
	release_firmware(address);
	return ret;
}
#endif

static int acquire_radio(void *context, struct sdio_func **func,
			 struct gpio_desc **rx_request)
{
	struct bq2830l_radio *radio = context;
	unsigned long waited;
	int ret;

	mutex_lock(&radio->cleanup_lock);
	if (READ_ONCE(radio->shutting_down)) {
		mutex_unlock(&radio->cleanup_lock);
		return -ENODEV;
	}
	if (radio->active) {
		ret = -EBUSY;
		goto out_unlock;
	}
	reinit_completion(&radio->runtime_ready);
	radio->runtime_func = NULL;
	radio->runtime_requested = true;
	ret = start_radio_locked(radio);
	if (!ret) {
		waited = wait_for_completion_timeout(
			&radio->runtime_ready,
			max_t(long, 1, radio->deadline - jiffies));
		ret = waited ? radio->status : -ETIMEDOUT;
		if (!ret && !radio->runtime_func)
			ret = -ENODEV;
	}
	if (!ret) {
		*func = radio->runtime_func;
		*rx_request = radio->rx_request;
	}
out_unlock:
	mutex_unlock(&radio->cleanup_lock);
	if (ret && ret != -EBUSY) {
		cleanup_radio(radio);
		radio->runtime_requested = false;
		radio->runtime_func = NULL;
	}
	return ret;
}

static void release_radio(void *context)
{
	struct bq2830l_radio *radio = context;

	cleanup_radio(radio);
	radio->runtime_requested = false;
	radio->runtime_func = NULL;
}

#if IS_ENABLED(CONFIG_BQ2830L_WLAN)
static const struct marlin2_wlan_radio_ops wlan_radio_ops = {
	.get_mac = get_mac,
};
#endif

static const struct marlin2_transport_radio_ops transport_radio_ops = {
	.acquire = acquire_radio,
	.release = release_radio,
	.write_bluetooth_locked = marlin2_sdio_bt_write_locked,
};

#if IS_ENABLED(CONFIG_BQ2830L_BLUETOOTH)
static ssize_t start_store(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct bq2830l_radio *radio = dev_get_drvdata(dev);
	struct marlin2_bluetooth *bluetooth;
	const struct firmware *configuration = NULL;
	const char *name;
	int ret;

	if (!sysfs_streq(buf, "1"))
		return -EINVAL;
	mutex_lock(&radio->bluetooth_lock);
	if (READ_ONCE(radio->shutting_down)) {
		ret = -ENODEV;
		goto out_unlock;
	}
	if (radio->bluetooth) {
		ret = 0;
		goto out_unlock;
	}
	ret = device_property_read_string(dev, "bluetooth-config-name", &name);
	if (ret)
		goto out_unlock;
	ret = request_firmware_direct(&configuration, name, dev);
	if (ret) {
		if (ret == -ENOENT)
			ret = -ENODATA;
		goto out_unlock;
	}
	if (configuration->size != MARLIN2_BLUETOOTH_PSKEY_BYTES) {
		ret = -EINVAL;
		goto out_configuration;
	}
	bluetooth = marlin2_bluetooth_register(dev, radio->transport,
					       configuration->data);
	if (IS_ERR(bluetooth)) {
		ret = PTR_ERR(bluetooth);
		goto out_configuration;
	}
	ret = marlin2_bluetooth_wait_ready(bluetooth);
	if (ret)
		marlin2_bluetooth_unregister(bluetooth);
	else
		radio->bluetooth = bluetooth;
out_configuration:
	release_firmware(configuration);
out_unlock:
	mutex_unlock(&radio->bluetooth_lock);
	return ret ? ret : count;
}
#endif

static void unregister_consumers(void *context)
{
	struct bq2830l_radio *radio = context;

	mutex_lock(&radio->bluetooth_lock);
	WRITE_ONCE(radio->shutting_down, true);
#if IS_ENABLED(CONFIG_BQ2830L_BLUETOOTH)
	if (radio->bluetooth) {
		marlin2_bluetooth_unregister(radio->bluetooth);
		radio->bluetooth = NULL;
	}
#endif
	mutex_unlock(&radio->bluetooth_lock);
#if IS_ENABLED(CONFIG_BQ2830L_WLAN)
	if (radio->wlan) {
		marlin2_wlan_unregister(radio->wlan);
		radio->wlan = NULL;
	}
#endif
#if IS_ENABLED(CONFIG_BQ2830L_FM)
	if (radio->fm) {
		marlin2_fm_unregister(radio->fm);
		radio->fm = NULL;
	}
#endif
	marlin2_transport_destroy(radio->transport);
	radio->transport = NULL;
}

static int bq2830l_radio_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct bq2830l_radio *radio;
	int ret;

	radio = devm_kzalloc(dev, sizeof(*radio), GFP_KERNEL);
	if (!radio)
		return -ENOMEM;
	radio->dev = dev;
	radio->status = -EAGAIN;
	radio->cleanup_done = true;
	mutex_init(&radio->cleanup_lock);
	mutex_init(&radio->bluetooth_lock);
	spin_lock_init(&radio->result_lock);
	INIT_DELAYED_WORK(&radio->cleanup_work, bq2830l_radio_cleanup_work);
	init_completion(&radio->runtime_ready);
	platform_set_drvdata(pdev, radio);
	radio->aon =
		syscon_regmap_lookup_by_phandle(dev->of_node, "sprd,aon-apb");
	if (IS_ERR(radio->aon))
		return dev_err_probe(dev, PTR_ERR(radio->aon),
				     "AON registers unavailable\n");
	radio->pins = syscon_regmap_lookup_by_phandle(dev->of_node, "bq,pins");
	if (IS_ERR(radio->pins))
		return dev_err_probe(dev, PTR_ERR(radio->pins),
				     "pin registers unavailable\n");
	radio->enable = devm_gpiod_get(dev, "enable", GPIOD_ASIS);
	if (IS_ERR(radio->enable))
		return dev_err_probe(dev, PTR_ERR(radio->enable),
				     "enable GPIO unavailable\n");
	radio->reset = devm_gpiod_get(dev, "reset", GPIOD_ASIS);
	if (IS_ERR(radio->reset))
		return dev_err_probe(dev, PTR_ERR(radio->reset),
				     "reset GPIO unavailable\n");
	radio->rx_request = devm_gpiod_get(dev, "rx-request", GPIOD_ASIS);
	if (IS_ERR(radio->rx_request))
		return dev_err_probe(dev, PTR_ERR(radio->rx_request),
				     "packet request GPIO unavailable\n");
	radio->wcn = devm_regulator_get_exclusive(dev, "wcn");
	if (IS_ERR(radio->wcn))
		return dev_err_probe(dev, PTR_ERR(radio->wcn),
				     "WCN regulator unavailable\n");
	radio->wifipa = devm_regulator_get_exclusive(dev, "wifipa");
	if (IS_ERR(radio->wifipa))
		return dev_err_probe(dev, PTR_ERR(radio->wifipa),
				     "WIFIPA regulator unavailable\n");
	radio->aux0_hw.init = &bq2830l_aux0_init;
	ret = devm_clk_hw_register(dev, &radio->aux0_hw);
	if (ret)
		return ret;
	radio->aux0 = devm_clk_hw_get_clk(dev, &radio->aux0_hw, "radio");
	if (IS_ERR(radio->aux0))
		return PTR_ERR(radio->aux0);
	ret = devm_add_action_or_reset(dev, bq2830l_radio_cleanup, radio);
	if (ret)
		return ret;
	radio->transport =
		marlin2_transport_create(dev, &transport_radio_ops, radio);
	if (IS_ERR(radio->transport))
		return PTR_ERR(radio->transport);
	ret = devm_add_action_or_reset(dev, unregister_consumers, radio);
	if (ret)
		return ret;
#if IS_ENABLED(CONFIG_BQ2830L_WLAN)
	{
		radio->wlan = marlin2_wlan_register(dev, &wlan_radio_ops, radio,
						    radio->transport);
		if (IS_ERR(radio->wlan)) {
			ret = PTR_ERR(radio->wlan);
			radio->wlan = NULL;
			return dev_err_probe(
				dev, ret, "wireless interface unavailable\n");
		}
	}
#endif
#if IS_ENABLED(CONFIG_BQ2830L_FM)
	radio->fm = marlin2_fm_register(dev, radio->transport);
	if (IS_ERR(radio->fm)) {
		ret = PTR_ERR(radio->fm);
		radio->fm = NULL;
		return dev_err_probe(dev, ret, "FM interface unavailable\n");
	}
#endif
	ret = device_add_group(dev, &bq2830l_radio_group);
	if (ret)
		return ret;
	return devm_add_action_or_reset(dev, remove_radio_group, dev);
}

static void bq2830l_radio_shutdown(struct platform_device *pdev)
{
	struct bq2830l_radio *radio = platform_get_drvdata(pdev);

	devm_release_action(&pdev->dev, remove_radio_group, &pdev->dev);
	devm_remove_action(&pdev->dev, unregister_consumers, radio);
	unregister_consumers(radio);
	bq2830l_radio_cleanup(radio);
}

static const struct of_device_id bq2830l_radio_of_match[] = {
	{ .compatible = "bq,2830l-radio" },
	{},
};
MODULE_DEVICE_TABLE(of, bq2830l_radio_of_match);

static struct platform_driver bq2830l_radio_driver = {
	.probe = bq2830l_radio_probe,
	.shutdown = bq2830l_radio_shutdown,
	.driver = {
		.name = "bq2830l-radio",
		.of_match_table = bq2830l_radio_of_match,
	},
};

static int __init bq2830l_radio_init(void)
{
	int ret;

	ret = sdio_register_driver(&marlin2_sdio_driver);
	if (ret)
		return ret;
	ret = platform_driver_register(&bq2830l_radio_driver);
	if (ret)
		sdio_unregister_driver(&marlin2_sdio_driver);
	return ret;
}
module_init(bq2830l_radio_init);

static void __exit bq2830l_radio_exit(void)
{
	platform_driver_unregister(&bq2830l_radio_driver);
	sdio_unregister_driver(&marlin2_sdio_driver);
}
module_exit(bq2830l_radio_exit);

MODULE_DESCRIPTION("BQ 2830L external radio RAM boot control");
MODULE_LICENSE("GPL");
