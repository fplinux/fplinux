// SPDX-License-Identifier: GPL-2.0-only
#include <stdio.h>

#include "kernel-boundary.h"
#include "bq2830l-radio.h"
#include "marlin2-transport.h"
#include "marlin2-wlan.h"

/* Board owner and shared transport are linked unchanged. These fakes replace
 * platform discovery, regulator/clock/GPIO hardware, SDIO and WLAN registration.
 * The SDIO boot succeeds regardless of PA state; no RF behavior is modeled.
 */
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
	struct marlin2_wlan wlan;
	struct cleanup_action actions[8];
	size_t action_count;
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
	fixture.driver = driver;
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
	return !strcmp(name, "sprd,aon-apb") ? &fixture.aon : &fixture.pins;
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
	assert(rail->exclusive);
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
	const struct marlin2_boot_result result = {
		.phase = MARLIN2_BOOT_COMPLETE,
		.ready = true,
	};

	fixture.populated = true;
	bq2830l_radio_report(&fixture.func, fixture.boot_error, &result);
	return 0;
}

void of_platform_depopulate(struct device *dev)
{
	fixture.populated = false;
}
bool of_device_is_compatible(struct device_node *node, const char *name)
{
	return !strcmp(name, "bq,2830l-radio");
}
bool mod_delayed_work(struct workqueue_struct *workqueue,
		      struct delayed_work *work, unsigned long delay)
{
	work->pending = true;
	return true;
}
int device_add_group(struct device *dev, const struct attribute_group *group)
{
	return 0;
}
void device_remove_group(struct device *dev,
			 const struct attribute_group *group)
{
}
int device_property_read_string(struct device *dev, const char *name,
				const char **value)
{
	return -ENOENT;
}
int request_firmware_direct(const struct firmware **firmware, const char *name,
			    struct device *dev)
{
	return -ENOENT;
}
void release_firmware(const struct firmware *firmware)
{
}
/* Sysfs rendering is compiled but is not exercised by these channel cases. */
ssize_t sysfs_emit(char *buffer, const char *format, ...)
{
	abort();
}
ssize_t sysfs_emit_at(char *buffer, ssize_t offset, const char *format, ...)
{
	abort();
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

static void setup(bool inherited_on)
{
	memset(&fixture, 0, sizeof(fixture));
	jiffies = 0;
	fixture.pdev.dev.of_node = &fixture.node;
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

	setup(inherited_on);
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
	setup(false);
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

int main(void)
{
	check_shared_lifetime(false);
	check_shared_lifetime(true);
	check_failed_boot(false);
	check_failed_boot(true);
	puts("BQ radio power host component cases passed");
	return 0;
}
