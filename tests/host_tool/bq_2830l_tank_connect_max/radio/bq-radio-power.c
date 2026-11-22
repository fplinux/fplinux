// SPDX-License-Identifier: GPL-2.0-only
#include <stdarg.h>
#include <stdio.h>

#include "kernel-boundary.h"
#include "marlin2-radio.h"
#include "marlin2-bluetooth.h"
#include "marlin2-fm.h"
#include "marlin2-transport.h"
#include "marlin2-wlan.h"

/* Board owner and shared transport are linked unchanged. These fakes replace
 * platform discovery, regulator/clock/GPIO hardware, SDIO and radio interfaces.
 * The SDIO boot succeeds regardless of PA state; no RF behavior is modeled.
 */
struct marlin2_bluetooth {
	struct marlin2_channel *channel;
};
struct marlin2_fm {
	bool registered;
};
struct marlin2_wlan {
	struct marlin2_channel *channel;
};
struct cleanup_action {
	void (*function)(void *);
	void *context;
};
static struct {
	struct platform_device pdev;
	struct platform_driver *driver;
	struct device_node node;
	struct device mmc_parent;
	struct mmc_host host;
	struct mmc_card card;
	struct sdio_func func;
	struct regmap aon, pins;
	struct gpio_desc enable, reset, request;
	struct regulator wcn, pa;
	struct clk clock;
	struct marlin2_transport *transport;
	struct marlin2_bluetooth bluetooth;
	struct marlin2_fm fm;
	struct marlin2_wlan wlan;
	struct cleanup_action actions[8];
	size_t action_count;
	const struct attribute_group *group;
	struct delayed_work *cleanup_work;
	unsigned long cleanup_delay;
	void *allocation;
	bool populated;
	bool irq_active;
	int boot_error;
	int pa_error;
} fixture;

unsigned long jiffies;
static struct workqueue_struct queue;
struct workqueue_struct *system_percpu_wq = &queue;
struct sdio_driver marlin2_sdio_driver;

void *devm_kzalloc(struct device *dev, size_t bytes, int flags)
{
	assert(!fixture.allocation);
	fixture.allocation = calloc(1, bytes);
	return fixture.allocation;
}

int devm_add_action_or_reset(struct device *dev, void (*action)(void *),
			     void *context)
{
	assert(fixture.action_count < ARRAY_SIZE(fixture.actions));
	fixture.actions[fixture.action_count++] =
		(struct cleanup_action){ action, context };
	return 0;
}

void devm_remove_action(struct device *dev, void (*action)(void *),
			void *context)
{
	size_t index;

	for (index = 0; index < fixture.action_count; index++)
		if (fixture.actions[index].function == action &&
		    fixture.actions[index].context == context) {
			memmove(&fixture.actions[index],
				&fixture.actions[index + 1],
				(fixture.action_count - index - 1) *
					sizeof(fixture.actions[0]));
			fixture.action_count--;
			return;
		}
	assert(false);
}

void devm_release_action(struct device *dev, void (*action)(void *),
			 void *context)
{
	devm_remove_action(dev, action, context);
	action(context);
}

int platform_driver_register(struct platform_driver *driver)
{
	assert(!strcmp(driver->driver.name, "bq2830l-radio"));
	fixture.driver = driver;
	fixture.pdev.dev.driver = &driver->driver;
	if (!device_get_match_data(&fixture.pdev.dev))
		return -ENODEV;
	return driver->probe(&fixture.pdev);
}

void platform_driver_unregister(struct platform_driver *driver)
{
	while (fixture.action_count) {
		struct cleanup_action action =
			fixture.actions[--fixture.action_count];
		action.function(action.context);
	}
	free(fixture.allocation);
	fixture.allocation = NULL;
	fixture.pdev.dev.driver = NULL;
}

int sdio_register_driver(struct sdio_driver *driver)
{
	return 0;
}
void sdio_unregister_driver(struct sdio_driver *driver)
{
}

struct regmap *syscon_regmap_lookup_by_phandle(struct device_node *node,
					       const char *name)
{
	if (!strcmp(name, "sprd,aon-apb"))
		return &fixture.aon;
	if (of_device_is_compatible(node, "fplinux,itel-neo-r60-marlin2")) {
		if (!strcmp(name, "fplinux,radio-pins"))
			return &fixture.pins;
	} else if (!strcmp(name, "bq,pins")) {
		return &fixture.pins;
	}
	return ERR_PTR(-ENOENT);
}

int regmap_read(struct regmap *map, unsigned int offset, unsigned int *value)
{
	assert(offset / 4 < ARRAY_SIZE(map->words));
	*value = map->words[offset / 4];
	return 0;
}

int regmap_write(struct regmap *map, unsigned int offset, unsigned int value)
{
	if (map == &fixture.aon && offset == 0x1004)
		map->words[1] |= value;
	else if (map == &fixture.aon && offset == 0x2004)
		map->words[1] &= ~value;
	else {
		assert(offset / 4 < ARRAY_SIZE(map->words));
		map->words[offset / 4] = value;
	}
	return 0;
}

struct gpio_desc *devm_gpiod_get(struct device *dev, const char *name,
				 int flags)
{
	if (!strcmp(name, "enable"))
		return &fixture.enable;
	if (!strcmp(name, "reset"))
		return &fixture.reset;
	return &fixture.request;
}

struct regulator *devm_regulator_get_exclusive(struct device *dev,
					       const char *name)
{
	struct regulator *rail;

	if (!strcmp(name, "wcn"))
		rail = &fixture.wcn;
	else if (!strcmp(name, "wifipa"))
		rail = &fixture.pa;
	else
		return ERR_PTR(-ENODEV);
	assert(!rail->exclusive);
	rail->exclusive = true;
	return rail;
}

int regulator_get_voltage(struct regulator *rail)
{
	return rail->voltage_uv;
}

int regulator_set_voltage(struct regulator *rail, int minimum, int maximum)
{
	assert(rail == &fixture.wcn && minimum == maximum);
	rail->voltage_uv = minimum;
	return 0;
}

int regulator_enable(struct regulator *rail)
{
	bool itel = of_device_is_compatible(&fixture.node,
					    "fplinux,itel-neo-r60-marlin2");

	assert(rail->exclusive);
	if (rail == &fixture.pa) {
		assert(!!fixture.wcn.enables != itel);
		assert(!!(fixture.aon.words[1] & BIT(2)) != itel);
	} else {
		if (itel)
			assert(fixture.pa.enables);
		assert(fixture.aon.words[1] & BIT(2));
	}
	if (rail == &fixture.pa && fixture.pa_error)
		return fixture.pa_error;
	rail->enables++;
	return 0;
}

int regulator_disable(struct regulator *rail)
{
	assert(rail->enables);
	rail->enables--;
	return 0;
}

int devm_clk_hw_register(struct device *dev, struct clk_hw *hw)
{
	return 0;
}

struct clk *devm_clk_hw_get_clk(struct device *dev, struct clk_hw *hw,
				const char *name)
{
	fixture.clock.hw = hw;
	return &fixture.clock;
}

int clk_prepare_enable(struct clk *clock)
{
	return clock->hw->init->ops->prepare(clock->hw);
}

void clk_disable_unprepare(struct clk *clock)
{
	clock->hw->init->ops->unprepare(clock->hw);
}

int of_platform_populate(struct device_node *node, const void *matches,
			 const void *lookup, struct device *parent)
{
	struct marlin2_boot_result result = {
		.phase = MARLIN2_BOOT_COMPLETE,
		.ready = true,
	};

	if (of_device_is_compatible(node, "fplinux,itel-neo-r60-marlin2")) {
		result.chip_id[0] = 0x23490001;
		result.chip_id[1] = result.chip_id[0];
		result.image_bytes = 592896;
		result.image_verified = true;
		result.calibration_verified = true;
	}
	fixture.populated = true;
	marlin2_radio_report(&fixture.func, fixture.boot_error, &result);
	return 0;
}

void of_platform_depopulate(struct device *dev)
{
	fixture.populated = false;
}
bool of_device_is_compatible(struct device_node *node, const char *name)
{
	return node && !strcmp(name, node->compatible);
}

const void *device_get_match_data(const struct device *dev)
{
	const struct of_device_id *match;

	for (match = dev->driver->of_match_table; match->compatible; match++)
		if (of_device_is_compatible(dev->of_node, match->compatible))
			return match->data;
	return NULL;
}
bool mod_delayed_work(struct workqueue_struct *workqueue,
		      struct delayed_work *work, unsigned long delay)
{
	work->pending = true;
	fixture.cleanup_work = work;
	fixture.cleanup_delay = delay;
	return true;
}
int device_add_group(struct device *dev, const struct attribute_group *group)
{
	fixture.group = group;
	return 0;
}
void device_remove_group(struct device *dev,
			 const struct attribute_group *group)
{
	assert(fixture.group == group);
	fixture.group = NULL;
}
int device_property_read_string(struct device *dev, const char *name,
				const char **value)
{
	if (strcmp(name, "bluetooth-config-name"))
		return -ENOENT;
	if (of_device_is_compatible(dev->of_node,
				    "fplinux,itel-neo-r60-marlin2"))
		*value = "ums9117/itel-neo-r60-plus-wcn-bluetooth.bin";
	else
		*value = "ums9117/bq-2830l-tank-connect-max-wcn-bluetooth.bin";
	return 0;
}
int device_property_read_u32(struct device *dev, const char *name, u32 *value)
{
	if (strcmp(name, "fplinux,radio-pad-0464"))
		return -ENOENT;
	*value = 0x2001;
	return 0;
}
int request_firmware_direct(const struct firmware **firmware, const char *name,
			    struct device *dev)
{
	static const u8 pskey[220] = { [0] = 0x42, [219] = 0xa5 };
	static const struct firmware bluetooth = {
		.data = pskey,
		.size = sizeof(pskey),
	};

	if (strcmp(name, "ums9117/itel-neo-r60-plus-wcn-bluetooth.bin") &&
	    strcmp(name, "ums9117/bq-2830l-tank-connect-max-wcn-bluetooth.bin"))
		return -ENOENT;
	*firmware = &bluetooth;
	return 0;
}
void release_firmware(const struct firmware *firmware)
{
}
ssize_t sysfs_emit(char *buffer, const char *format, ...)
{
	ssize_t length;
	va_list args;

	va_start(args, format);
	length = vsnprintf(buffer, 4096, format, args);
	va_end(args);
	return length;
}
ssize_t sysfs_emit_at(char *buffer, ssize_t offset, const char *format, ...)
{
	ssize_t length;
	va_list args;

	assert(offset >= 0 && offset < 4096);
	va_start(args, format);
	length = vsnprintf(buffer + offset, 4096 - offset, format, args);
	va_end(args);
	return length;
}
int dev_err_probe(struct device *dev, int error, const char *format, ...)
{
	return error;
}
void bq_radio_host_log(const void *dev, const char *format, ...)
{
}

int request_threaded_irq(int irq, void *primary,
			 irqreturn_t (*thread)(int, void *),
			 unsigned long flags, const char *name, void *context)
{
	assert(!fixture.irq_active);
	fixture.irq_active = true;
	return 0;
}
void free_irq(int irq, void *context)
{
	fixture.irq_active = false;
}
void disable_irq_nosync(int irq)
{
}
void sdio_claim_host(struct sdio_func *func)
{
}
void sdio_release_host(struct sdio_func *func)
{
}
int sdio_readsb(struct sdio_func *func, void *buffer, unsigned int address,
		size_t bytes)
{
	return -ENODATA;
}

int sdio_writesb(struct sdio_func *func, unsigned int address,
		 const void *buffer, size_t bytes)
{
	return 0;
}

int marlin2_sdio_bt_write_locked(struct sdio_func *func, const u8 *data,
				 size_t bytes)
{
	return sdio_writesb(func, 0, data, bytes);
}

static void receive_unused(void *context, u8 subtype, const u8 *data,
			   size_t bytes)
{
}
static const struct marlin2_channel_ops channel_ops = {
	.receive = receive_unused
};

struct marlin2_bluetooth *
marlin2_bluetooth_register(struct device *dev,
			   struct marlin2_transport *transport,
			   const u8 pskey[MARLIN2_BLUETOOTH_PSKEY_BYTES])
{
	assert(pskey[0] == 0x42 && pskey[219] == 0xa5);
	fixture.bluetooth.channel = marlin2_channel_register(
		transport, MARLIN2_CLIENT_BLUETOOTH, &channel_ops, NULL);
	assert(!IS_ERR(fixture.bluetooth.channel));
	return &fixture.bluetooth;
}

int marlin2_bluetooth_wait_ready(struct marlin2_bluetooth *bt)
{
	return marlin2_channel_open(bt->channel);
}

void marlin2_bluetooth_unregister(struct marlin2_bluetooth *bt)
{
	marlin2_channel_unregister(bt->channel);
	bt->channel = NULL;
}

struct marlin2_fm *marlin2_fm_register(struct device *dev,
				       struct marlin2_transport *transport)
{
	fixture.fm.registered = true;
	return &fixture.fm;
}

void marlin2_fm_unregister(struct marlin2_fm *fm)
{
	fm->registered = false;
}

struct marlin2_wlan *
marlin2_wlan_register(struct device *dev,
		      const struct marlin2_wlan_radio_ops *ops, void *context,
		      struct marlin2_transport *transport)
{
	fixture.transport = transport;
	fixture.wlan.channel = marlin2_channel_register(
		transport, MARLIN2_CLIENT_WLAN, &channel_ops, NULL);
	assert(!IS_ERR(fixture.wlan.channel));
	return &fixture.wlan;
}

void marlin2_wlan_unregister(struct marlin2_wlan *wlan)
{
	marlin2_channel_unregister(wlan->channel);
}

static void setup(bool inherited_on, const char *compatible)
{
	memset(&fixture, 0, sizeof(fixture));
	jiffies = 0;
	fixture.pdev.dev.of_node = &fixture.node;
	fixture.node.compatible = compatible;
	fixture.mmc_parent.parent = &fixture.pdev.dev;
	fixture.host =
		(struct mmc_host){ 10240, 10240, 20, &fixture.mmc_parent };
	fixture.card.host = &fixture.host;
	fixture.card.cccr.multi_block = true;
	fixture.func.card = &fixture.card;
	fixture.func.num = 1;
	fixture.func.cur_blksize = 512;
	fixture.request.irq = 13;
	fixture.wcn.voltage_uv = 1800000;
	/* An exclusive regulator handle begins with the inherited enable count. */
	fixture.pa.enables = inherited_on ? 1 : 0;
	assert(!bq_radio_host_init());
}

static void check_restored(bool inherited_on)
{
	assert(fixture.pa.enables == (inherited_on ? 1U : 0U));
	assert(!fixture.wcn.enables && fixture.wcn.voltage_uv == 1800000);
	assert(!fixture.populated && !fixture.irq_active);
}

static void check_shared_lifetime(bool inherited_on)
{
	struct marlin2_channel *other;

	setup(inherited_on, "bq,2830l-radio");
	assert(fixture.fm.registered);
	assert(!marlin2_channel_open(fixture.wlan.channel));
	printf("active supplies: inherited_on=%u pa_enabled=%u wcn_enabled=%u\n",
	       inherited_on, fixture.pa.enables != 0, fixture.wcn.enables != 0);
	fflush(stdout);
	assert(fixture.pa.enables && fixture.wcn.enables);
	other = marlin2_channel_register(fixture.transport,
					 MARLIN2_CLIENT_BLUETOOTH, &channel_ops,
					 NULL);
	assert(!IS_ERR(other) && !marlin2_channel_open(other));
	marlin2_channel_close(fixture.wlan.channel);
	assert(fixture.pa.enables && fixture.wcn.enables);
	assert(fixture.populated && fixture.irq_active);
	marlin2_channel_close(other);
	check_restored(inherited_on);
	marlin2_channel_unregister(other);
	bq_radio_host_exit();
}

static void check_failed_boot(bool pa_failure)
{
	setup(false, "bq,2830l-radio");
	if (pa_failure)
		fixture.pa_error = -EIO;
	else
		fixture.boot_error = -EIO;
	assert(marlin2_channel_open(fixture.wlan.channel) == -EIO);
	check_restored(false);
	fixture.pa_error = 0;
	fixture.boot_error = 0;
	assert(!marlin2_channel_open(fixture.wlan.channel));
	marlin2_channel_close(fixture.wlan.channel);
	check_restored(false);
	bq_radio_host_exit();
}

static struct device_attribute *radio_attribute(const char *name)
{
	struct attribute **attribute;

	for (attribute = fixture.group->attrs; *attribute; attribute++)
		if (!strcmp((*attribute)->name, name))
			return container_of(*attribute, struct device_attribute,
					    attr);
	return NULL;
}

static void check_bq_shutdown(void)
{
	struct device_attribute *start;

	setup(false, "bq,2830l-radio");
	fixture.pins.words[0x464 / 4] = 0x2001;
	start = radio_attribute("start");
	assert(start && start->store);
	assert(!marlin2_channel_open(fixture.wlan.channel));
	assert(start->store(&fixture.pdev.dev, start, "1", 1) == 1);
	assert(fixture.populated && fixture.irq_active &&
	       fixture.fm.registered);
	assert(fixture.pins.words[0x464 / 4] == 0x102001);

	fixture.driver->shutdown(&fixture.pdev);
	check_restored(false);
	assert(!fixture.bluetooth.channel && !fixture.fm.registered);
	assert(!fixture.group && !(fixture.aon.words[1] & BIT(2)));
	assert(fixture.pins.words[0x464 / 4] == 0x2001);
	bq_radio_host_exit();
}

static void check_itel_runtime(void)
{
	struct device_attribute *start;

	setup(false, "fplinux,itel-neo-r60-marlin2");
	assert(fixture.wlan.channel && !fixture.fm.registered);
	assert(!fixture.populated && !fixture.cleanup_work);
	start = radio_attribute("start");
	assert(start && start->store);
	fixture.pins.words[0x464 / 4] = 0x102001;

	assert(!marlin2_channel_open(fixture.wlan.channel));
	assert(fixture.populated && fixture.wcn.enables && fixture.pa.enables);
	assert(fixture.pins.words[0x464 / 4] == 0x2001);
	assert(fixture.irq_active && !fixture.cleanup_work);
	assert(start->store(&fixture.pdev.dev, start, "1", 1) == 1);
	marlin2_channel_close(fixture.wlan.channel);
	assert(fixture.populated && fixture.irq_active);
	assert(fixture.wcn.enables && fixture.pa.enables);

	bq_radio_host_exit();
	check_restored(false);
	assert(!fixture.bluetooth.channel);
	assert(fixture.pins.words[0x464 / 4] == 0x102001);
	assert(!(fixture.aon.words[1] & BIT(2)));
}

static void check_itel_diagnostic(void)
{
	struct device_attribute *result;
	struct device_attribute *boot;
	char text[4096];

	setup(false, "fplinux,itel-neo-r60-marlin2");
	result = radio_attribute("result");
	boot = radio_attribute("boot");
	assert(result && result->show && boot && boot->store);
	assert(boot->store(&fixture.pdev.dev, boot, "1", 1) == 1);
	assert(fixture.populated && fixture.cleanup_work &&
	       !fixture.cleanup_delay);
	fixture.cleanup_work->work.function(&fixture.cleanup_work->work);
	check_restored(false);
	assert(result->show(&fixture.pdev.dev, result, text) > 0);
	assert(strstr(text, "error=0 cleanup_error=0 restored=1"));
	assert(strstr(text, "chip_id0=0x23490001 chip_id1=0x23490001"));
	assert(strstr(text, "phase=complete image_bytes=592896"));
	assert(strstr(text, "image_verified=1 calibration_verified=1"));
	bq_radio_host_exit();
}

int main(int argc, char **argv)
{
	if (argc == 1) {
		check_shared_lifetime(false);
		check_shared_lifetime(true);
		check_failed_boot(false);
		check_failed_boot(true);
		check_bq_shutdown();
	} else {
		assert(argc == 2 && !strcmp(argv[1], "itel-only"));
	}
	check_itel_runtime();
	check_itel_diagnostic();
	puts("Marlin2 radio power host component cases passed");
	return 0;
}
