// SPDX-License-Identifier: GPL-2.0-only
#include "compat/kernel.h"
#include <linux/aw87339.h>
#include "qin-hp.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum event_type {
	GPIO,
	DELAY,
	READ,
	WRITE,
};

struct event {
	enum event_type type;
	unsigned int reg;
	unsigned int value;
};

struct managed_action {
	struct device *owner;
	void (*action)(void *);
	void *data;
};

struct fixture {
	struct gpio_desc reset;
	struct event events[256];
	size_t event_count;
	void *allocations[8];
	size_t allocation_count;
	struct managed_action actions[8];
	size_t action_count;
	struct device_node supplier_node;
	struct device_node consumer_node;
	struct i2c_client *supplier;
	struct device *consumer;
	struct device_link link;
	bool property_present;
	bool supplier_present;
	bool link_live;
	bool link_error;
	bool identification_only;
	unsigned int node_gets;
	unsigned int node_puts;
	unsigned int find_calls;
	unsigned int device_gets;
	unsigned int device_puts;
	unsigned int device_locks;
	unsigned int device_unlocks;
	unsigned int link_calls;
	unsigned int link_removals;
	int gpio_get_error;
	int supplier_action_error;
	int consumer_action_error;
	int gpio_error_call;
	unsigned int gpio_calls;
	int ids[5];
	unsigned int reads;
	unsigned int writes;
	unsigned int write_fail_first;
	unsigned int write_fail_count;
	unsigned int diagnostics;
	unsigned int successes;
};

static struct fixture *active;

static const struct event profile_writes[] = {
	{ WRITE, 1, 0x06 },  { WRITE, 2, 0xa3 }, { WRITE, 3, 0x06 },
	{ WRITE, 4, 0x05 },  { WRITE, 5, 0x10 }, { WRITE, 6, 0x09 },
	{ WRITE, 7, 0x52 },  { WRITE, 8, 0x06 }, { WRITE, 9, 0x08 },
	{ WRITE, 10, 0x96 }, { WRITE, 1, 0x0e },
};

static const struct event off_sequence[] = {
	{ WRITE, 1, 0 },
	{ GPIO, 0, 0 },
	{ DELAY, 0, 2 },
};

static const struct event reset_sequence[] = {
	{ GPIO, 0, 0 },
	{ DELAY, 0, 2 },
};

static void add_event(enum event_type type, unsigned int reg,
		      unsigned int value)
{
	assert(active->event_count < ARRAY_SIZE(active->events));
	active->events[active->event_count++] =
		(struct event){ type, reg, value };
}

void *devm_kzalloc(struct device *dev, size_t size, unsigned int flags)
{
	void *allocation = calloc(1, size);

	(void)dev;
	(void)flags;
	assert(allocation);
	assert(active->allocation_count < ARRAY_SIZE(active->allocations));
	active->allocations[active->allocation_count++] = allocation;
	return allocation;
}

int dev_err_probe(struct device *dev, int error, const char *format, ...)
{
	(void)dev;
	(void)format;
	active->diagnostics++;
	return error;
}

void dev_err(struct device *dev, const char *format, ...)
{
	(void)dev;
	(void)format;
	active->diagnostics++;
}

void dev_info(struct device *dev, const char *format, ...)
{
	(void)dev;
	(void)format;
	active->successes++;
}

void *dev_get_drvdata(struct device *dev)
{
	return dev->data;
}

void device_lock(struct device *dev)
{
	assert(dev == &active->supplier->dev && !dev->locked);
	dev->locked = true;
	active->device_locks++;
}

void device_unlock(struct device *dev)
{
	assert(dev == &active->supplier->dev && dev->locked);
	dev->locked = false;
	active->device_unlocks++;
}

bool device_is_bound(struct device *dev)
{
	assert(dev == &active->supplier->dev && dev->locked);
	return dev->bound;
}

void put_device(struct device *dev)
{
	assert(dev == &active->supplier->dev);
	assert(dev->references > 1);
	dev->references--;
	active->device_puts++;
}

struct device_link *device_link_add(struct device *consumer,
				    struct device *supplier, u32 flags)
{
	assert(consumer == active->consumer);
	assert(supplier == &active->supplier->dev && supplier->locked);
	assert(flags == 2);
	assert(!active->link_live);
	active->link_calls++;
	if (active->link_error)
		return NULL;
	active->link = (struct device_link){ consumer, supplier, flags };
	active->link_live = true;
	return &active->link;
}

struct device_node *of_parse_phandle(struct device_node *node,
				     const char *property, int index)
{
	assert(node == &active->consumer_node);
	assert(strcmp(property, "fplinux,speaker-amplifier") == 0);
	assert(index == 0);
	if (!active->property_present)
		return NULL;
	active->node_gets++;
	return &active->supplier_node;
}

void of_node_put(struct device_node *node)
{
	assert(node == &active->supplier_node);
	assert(active->node_puts < active->node_gets);
	active->node_puts++;
}

struct i2c_client *of_find_i2c_device_by_node(struct device_node *node)
{
	assert(node == &active->supplier_node);
	active->find_calls++;
	if (!active->supplier_present)
		return NULL;
	active->device_gets++;
	active->supplier->dev.references++;
	return active->supplier;
}

void mutex_init(struct mutex *lock)
{
	lock->locked = false;
}

void mutex_lock(struct mutex *lock)
{
	assert(!lock->locked);
	lock->locked = true;
}

void mutex_unlock(struct mutex *lock)
{
	assert(lock->locked);
	lock->locked = false;
}

void i2c_set_clientdata(struct i2c_client *client, void *data)
{
	client->dev.data = data;
}

void *i2c_get_clientdata(struct i2c_client *client)
{
	return client->dev.data;
}

bool i2c_check_functionality(struct i2c_adapter *adapter, unsigned long mask)
{
	assert(mask == 0x180000);
	return adapter->byte_data;
}

struct gpio_desc *devm_gpiod_get(struct device *dev, const char *name,
				 int flags)
{
	assert(dev == &active->supplier->dev);
	assert(strcmp(name, "reset") == 0);
	assert(flags == GPIOD_OUT_HIGH);
	if (active->gpio_get_error)
		return ERR_PTR(active->gpio_get_error);
	active->reset.physical_level = 0;
	add_event(GPIO, 0, 0);
	return &active->reset;
}

int gpiod_set_value_cansleep(struct gpio_desc *descriptor, int value)
{
	assert(descriptor == &active->reset);
	assert(value == 0 || value == 1);
	active->gpio_calls++;
	/* Model a transaction reaching the latch before reporting an error. */
	descriptor->physical_level = !value;
	add_event(GPIO, 0, !value);
	return (int)active->gpio_calls == active->gpio_error_call ? -EIO : 0;
}

int devm_add_action_or_reset(struct device *dev, void (*action)(void *),
			     void *data)
{
	int error;

	assert(dev == &active->supplier->dev || dev == active->consumer);
	error = dev == active->consumer ? active->consumer_action_error :
					  active->supplier_action_error;
	if (error) {
		action(data);
		return error;
	}
	assert(active->action_count < ARRAY_SIZE(active->actions));
	active->actions[active->action_count++] =
		(struct managed_action){ dev, action, data };
	return 0;
}

int i2c_smbus_read_byte_data(struct i2c_client *client, unsigned int reg)
{
	assert(client == active->supplier && reg == 0);
	assert(active->reset.physical_level == 1);
	assert(active->reads < ARRAY_SIZE(active->ids));
	add_event(READ, reg, 0);
	return active->ids[active->reads++];
}

int i2c_smbus_write_byte_data(struct i2c_client *client, unsigned int reg,
			      unsigned int value)
{
	assert(client == active->supplier && reg >= 1 && reg <= 10);
	assert(value <= 0xff);
	if (active->identification_only)
		assert(reg == 1 && value == 0);
	active->writes++;
	add_event(WRITE, reg, value);
	if (active->writes >= active->write_fail_first &&
	    active->writes - active->write_fail_first <
		    active->write_fail_count)
		return -EREMOTEIO;
	return 0;
}

void msleep(unsigned int milliseconds)
{
	assert(milliseconds == 2);
	add_event(DELAY, 0, milliseconds);
}

static void expect_event(const struct event *actual,
			 const struct event *expected)
{
	assert(actual->type == expected->type);
	assert(actual->reg == expected->reg);
	assert(actual->value == expected->value);
}

static void expect_events(const struct event *expected, size_t count)
{
	size_t index;

	assert(active->event_count == count);
	for (index = 0; index < count; index++)
		expect_event(&active->events[index], &expected[index]);
}

static void expect_reset_tail(void)
{
	size_t start;
	size_t index;

	assert(active->event_count >= ARRAY_SIZE(reset_sequence));
	start = active->event_count - ARRAY_SIZE(reset_sequence);
	for (index = 0; index < ARRAY_SIZE(reset_sequence); index++)
		expect_event(&active->events[start + index],
			     &reset_sequence[index]);
}

static void clear_io(void)
{
	active->event_count = 0;
	active->gpio_calls = 0;
	active->reads = 0;
	active->writes = 0;
}

static void clear_errors(void)
{
	active->gpio_error_call = 0;
	active->write_fail_first = 0;
	active->write_fail_count = 0;
}

static void release_device(struct device *dev)
{
	size_t index = active->action_count;

	while (index) {
		struct managed_action *action = &active->actions[--index];

		if (action->owner == dev && action->action) {
			action->action(action->data);
			action->action = NULL;
		}
	}
	/* Consumer detach is modeled, not the kernel's unbind/PM ordering. */
	if (dev == active->consumer && active->link_live) {
		active->link_live = false;
		active->link_removals++;
	}
}

static void release_fixture(void)
{
	release_device(active->consumer);
	release_device(&active->supplier->dev);
	while (active->allocation_count)
		free(active->allocations[--active->allocation_count]);
	assert(active->supplier->dev.references == 1);
	assert(active->device_gets == active->device_puts);
	assert(active->node_gets == active->node_puts);
	assert(active->device_locks == active->device_unlocks);
}

static void expect_identification_sequence(void)
{
	const struct event expected[] = {
		{ GPIO, 0, 0 },	 { DELAY, 0, 2 }, { GPIO, 0, 1 },
		{ DELAY, 0, 2 }, { READ, 0, 0 },  { WRITE, 1, 0 },
		{ GPIO, 0, 0 },	 { DELAY, 0, 2 },
	};

	expect_events(expected, ARRAY_SIZE(expected));
}

static bool run_identification(const char *scenario)
{
	const struct i2c_driver *driver = host_aw87339_driver;
	int expected = 0;
	size_t index;
	int ret;

	if (strcmp(scenario, "unsupported-bus") == 0) {
		active->supplier->adapter->byte_data = false;
		expected = -EOPNOTSUPP;
	} else if (strcmp(scenario, "reset-unavailable") == 0) {
		active->gpio_get_error = -EPROBE_DEFER;
		expected = -EPROBE_DEFER;
	} else if (strcmp(scenario, "cleanup-registration") == 0) {
		active->supplier_action_error = -ENOMEM;
		expected = -ENOMEM;
	} else if (strcmp(scenario, "release-partial-failure") == 0) {
		active->gpio_error_call = 1;
		expected = -EIO;
	} else if (strcmp(scenario, "wrong-id") == 0) {
		for (index = 0; index < ARRAY_SIZE(active->ids); index++)
			active->ids[index] = 0x38;
		expected = -ENODEV;
	} else if (strcmp(scenario, "read-failure") == 0) {
		for (index = 0; index < ARRAY_SIZE(active->ids); index++)
			active->ids[index] = -EREMOTEIO;
		expected = -EREMOTEIO;
	} else if (strcmp(scenario, "transient-id") == 0) {
		active->ids[0] = -EREMOTEIO;
		active->ids[1] = 0x38;
	} else if (strcmp(scenario, "off-write-failure") == 0) {
		active->write_fail_first = 1;
		active->write_fail_count = 5;
		expected = -EREMOTEIO;
	} else if (strcmp(scenario, "off-reset-failure") == 0) {
		active->gpio_error_call = 2;
		expected = -EIO;
	} else if (strcmp(scenario, "identify-off-lifecycle") != 0) {
		return false;
	}

	active->identification_only = true;
	ret = driver->probe(active->supplier);
	assert(ret == expected);
	if (!ret) {
		assert(active->successes == 1);
		assert(active->reset.physical_level == 0);
		if (strcmp(scenario, "identify-off-lifecycle") == 0) {
			expect_identification_sequence();
			assert(driver->driver.pm->suspend(
				       &active->supplier->dev) == 0);
			assert(driver->driver.pm->resume(
				       &active->supplier->dev) == 0);
			driver->shutdown(active->supplier);
			assert(active->writes == 1);
		}
	} else {
		assert(active->successes == 0);
	}
	if (strcmp(scenario, "unsupported-bus") == 0 ||
	    strcmp(scenario, "reset-unavailable") == 0)
		assert(active->event_count == 0);
	if (strcmp(scenario, "cleanup-registration") == 0)
		assert(active->reads == 0 && active->writes == 0);
	if (strcmp(scenario, "release-partial-failure") == 0)
		assert(active->reads == 0 && active->writes == 1);
	if (strcmp(scenario, "wrong-id") == 0 ||
	    strcmp(scenario, "read-failure") == 0)
		assert(active->reads == 5 && active->writes == 1);
	if (strcmp(scenario, "transient-id") == 0)
		assert(active->reads == 3 && active->writes == 1);
	if (strcmp(scenario, "off-write-failure") == 0)
		assert(active->writes == 5 &&
		       active->reset.physical_level == 0);
	release_fixture();
	if (strcmp(scenario, "off-reset-failure") == 0)
		assert(active->writes == 2 && active->gpio_calls == 3);
	if (active->gpio_calls)
		assert(active->reset.physical_level == 0);
	return true;
}

static void expect_start_sequence(void)
{
	const struct event reset[] = {
		{ GPIO, 0, 0 },
		{ DELAY, 0, 2 },
		{ GPIO, 0, 1 },
		{ DELAY, 0, 2 },
	};
	size_t index;

	assert(active->event_count ==
	       ARRAY_SIZE(reset) + ARRAY_SIZE(profile_writes));
	for (index = 0; index < ARRAY_SIZE(reset); index++)
		expect_event(&active->events[index], &reset[index]);
	for (index = 0; index < ARRAY_SIZE(profile_writes); index++)
		expect_event(&active->events[ARRAY_SIZE(reset) + index],
			     &profile_writes[index]);
	assert(active->reset.physical_level == 1);
}

static void expect_rejected_start(struct aw87339 *amplifier, int error)
{
	size_t event_count = active->event_count;

	assert(aw87339_start(amplifier, &qin_hp_speaker_profile) == error);
	assert(active->event_count == event_count);
}

static void start_and_check(struct aw87339 *amplifier)
{
	clear_io();
	assert(aw87339_start(amplifier, &qin_hp_speaker_profile) == 0);
	expect_start_sequence();
}

static void stop_and_check(struct aw87339 *amplifier)
{
	clear_io();
	assert(aw87339_stop(amplifier) == 0);
	expect_events(off_sequence, ARRAY_SIZE(off_sequence));
	assert(active->reset.physical_level == 0);
}

static void retry_cleanup_and_start(struct aw87339 *amplifier,
				    bool powered_uncertain)
{
	clear_errors();
	clear_io();
	assert(aw87339_stop(amplifier) == 0);
	if (powered_uncertain)
		expect_events(off_sequence, ARRAY_SIZE(off_sequence));
	else
		expect_events(reset_sequence, ARRAY_SIZE(reset_sequence));
	start_and_check(amplifier);
	stop_and_check(amplifier);
}

static void run_start_write_failure(struct aw87339 *amplifier,
				    unsigned int failed_transfer,
				    bool cleanup_write_failure,
				    bool cleanup_reset_failure)
{
	const struct event off = { WRITE, 1, 0 };
	unsigned int off_attempts = cleanup_write_failure ? 5 : 1;
	unsigned int write_index = 0;
	size_t event_index;

	assert(failed_transfer < ARRAY_SIZE(profile_writes));
	active->write_fail_first = failed_transfer + 1;
	active->write_fail_count = cleanup_write_failure ? 10 : 5;
	active->gpio_error_call = cleanup_reset_failure ? 3 : 0;
	assert(aw87339_start(amplifier, &qin_hp_speaker_profile) == -EREMOTEIO);
	assert(active->writes == failed_transfer + 5 + off_attempts);
	assert(active->gpio_calls == 3 && active->reset.physical_level == 0);
	expect_reset_tail();
	for (event_index = 0; event_index < active->event_count;
	     event_index++) {
		const struct event *event = &active->events[event_index];

		if (event->type != WRITE)
			continue;
		if (write_index < failed_transfer)
			expect_event(event, &profile_writes[write_index]);
		else if (write_index < failed_transfer + 5)
			expect_event(event, &profile_writes[failed_transfer]);
		else
			expect_event(event, &off);
		if (failed_transfer < 10)
			assert(event->reg != 1 || event->value != 0x0e);
		write_index++;
	}
	assert(write_index == active->writes);
	if (cleanup_write_failure || cleanup_reset_failure) {
		expect_rejected_start(amplifier, -EBUSY);
		retry_cleanup_and_start(amplifier, cleanup_reset_failure);
	} else {
		clear_errors();
		start_and_check(amplifier);
		stop_and_check(amplifier);
	}
}

static void run_partial_gpio_failure(struct aw87339 *amplifier, bool release)
{
	const struct event assert_failed[] = {
		{ GPIO, 0, 0 },
		{ GPIO, 0, 0 },
		{ DELAY, 0, 2 },
	};
	const struct event release_failed[] = {
		{ GPIO, 0, 0 },	 { DELAY, 0, 2 }, { GPIO, 0, 1 },
		{ WRITE, 1, 0 }, { GPIO, 0, 0 },  { DELAY, 0, 2 },
	};

	active->gpio_error_call = release ? 2 : 1;
	assert(aw87339_start(amplifier, &qin_hp_speaker_profile) == -EIO);
	if (release)
		expect_events(release_failed, ARRAY_SIZE(release_failed));
	else
		expect_events(assert_failed, ARRAY_SIZE(assert_failed));
	assert(active->reset.physical_level == 0);
	clear_errors();
	start_and_check(amplifier);
	stop_and_check(amplifier);
}

static void run_stop_failure(struct aw87339 *amplifier, bool reset_failure)
{
	const struct event off = { WRITE, 1, 0 };
	unsigned int off_attempts = reset_failure ? 1 : 5;
	unsigned int writes = 0;
	size_t index;

	start_and_check(amplifier);
	clear_io();
	active->gpio_error_call = reset_failure ? 1 : 0;
	active->write_fail_first = 1;
	active->write_fail_count = reset_failure ? 0 : 5;
	assert(aw87339_stop(amplifier) == (reset_failure ? -EIO : -EREMOTEIO));
	assert(active->writes == off_attempts && active->gpio_calls == 1);
	expect_reset_tail();
	for (index = 0; index < active->event_count; index++) {
		if (active->events[index].type == WRITE) {
			expect_event(&active->events[index], &off);
			writes++;
		}
	}
	assert(writes == off_attempts && active->reset.physical_level == 0);
	expect_rejected_start(amplifier, -EBUSY);
	/* A second failed stop must not discharge the outstanding obligation. */
	clear_errors();
	clear_io();
	active->gpio_error_call = 1;
	assert(aw87339_stop(amplifier) == -EIO);
	assert(active->writes == (reset_failure ? 1U : 0U));
	expect_rejected_start(amplifier, -EBUSY);
	retry_cleanup_and_start(amplifier, reset_failure);
}

static void run_suspend_resume(struct aw87339 *amplifier, bool resume_failure)
{
	const struct dev_pm_ops *pm = host_aw87339_driver->driver.pm;

	start_and_check(amplifier);
	clear_io();
	assert(pm->suspend(&active->supplier->dev) == 0);
	expect_events(off_sequence, ARRAY_SIZE(off_sequence));
	expect_rejected_start(amplifier, -ENODEV);
	clear_io();
	if (resume_failure) {
		active->gpio_error_call = 1;
		assert(pm->resume(&active->supplier->dev) == -EIO);
		expect_events(reset_sequence, ARRAY_SIZE(reset_sequence));
		expect_rejected_start(amplifier, -ENODEV);
		clear_errors();
		clear_io();
	}
	assert(pm->resume(&active->supplier->dev) == 0);
	expect_events(reset_sequence, ARRAY_SIZE(reset_sequence));
	assert(active->reset.physical_level == 0 && active->writes == 0);
	start_and_check(amplifier);
	stop_and_check(amplifier);
}

static void run_get(const char *scenario)
{
	struct device_driver wrong_driver = { .name = "other-amplifier" };
	struct device *supplier = &active->supplier->dev;
	struct device_driver *saved_driver = supplier->driver;
	void *saved_data = supplier->data;
	struct aw87339 *amplifier;
	bool managed = strcmp(scenario, "get-managed-lifetime") == 0;
	bool action_failure = strcmp(scenario, "get-action-failure") == 0;
	bool link_failure = strcmp(scenario, "get-link-failure") == 0;
	int expected = -EPROBE_DEFER;

	if (strcmp(scenario, "get-missing-property") == 0) {
		active->property_present = false;
		expected = -EINVAL;
	} else if (strcmp(scenario, "get-missing-supplier") == 0) {
		active->supplier_present = false;
	} else if (strcmp(scenario, "get-unbound-supplier") == 0) {
		supplier->bound = false;
	} else if (strcmp(scenario, "get-unready-supplier") == 0) {
		host_aw87339_driver->shutdown(active->supplier);
		clear_io();
	} else if (strcmp(scenario, "get-missing-clientdata") == 0) {
		supplier->data = NULL;
	} else if (strcmp(scenario, "get-wrong-driver") == 0) {
		supplier->driver = &wrong_driver;
	} else if (link_failure) {
		active->link_error = true;
		expected = -ENOMEM;
	} else if (action_failure) {
		active->consumer_action_error = -ENOMEM;
		expected = -ENOMEM;
	} else {
		assert(managed);
	}

	amplifier =
		devm_aw87339_get(active->consumer, "fplinux,speaker-amplifier");
	assert(active->event_count == 0 && !supplier->locked);
	assert(active->node_gets == active->node_puts);
	assert(active->device_locks == active->device_unlocks);
	assert(active->find_calls == (active->property_present ? 1U : 0U));
	assert(active->device_gets ==
	       (active->property_present && active->supplier_present ? 1U :
								       0U));
	assert(active->device_locks == active->device_gets);
	assert(active->link_calls ==
	       (managed || action_failure || link_failure ? 1U : 0U));
	assert(active->link_live == (managed || action_failure));
	if (managed) {
		assert(!IS_ERR(amplifier) && amplifier == saved_data);
		assert(supplier->references == 2 && active->device_puts == 0);
		assert(active->link.consumer == active->consumer);
		assert(active->link.supplier == supplier &&
		       active->link.flags == 2);
		start_and_check(amplifier);
		stop_and_check(amplifier);
		clear_io();
		release_device(active->consumer);
		assert(supplier->references == 1 && active->device_puts == 1);
		assert(active->link_removals == 1 && !active->link_live);
		release_device(active->consumer);
		assert(active->device_puts == 1 && active->event_count == 0);
	} else {
		assert(IS_ERR(amplifier) && PTR_ERR(amplifier) == expected);
		assert(supplier->references == 1);
		assert(active->device_gets == active->device_puts);
		release_device(active->consumer);
		assert(active->link_removals == (action_failure ? 1U : 0U));
	}
	supplier->data = saved_data;
	supplier->driver = saved_driver;
}

static void run_api(const char *scenario, const char *argument)
{
	struct aw87339 *amplifier;

	amplifier =
		devm_aw87339_get(active->consumer, "fplinux,speaker-amplifier");
	assert(!IS_ERR(amplifier));
	if (strcmp(scenario, "start-stop-profile") == 0) {
		start_and_check(amplifier);
		expect_rejected_start(amplifier, -EBUSY);
		stop_and_check(amplifier);
		clear_io();
		assert(aw87339_stop(amplifier) == 0);
		expect_events(reset_sequence, ARRAY_SIZE(reset_sequence));
	} else if (strcmp(scenario, "start-configuration-failure") == 0) {
		char *end;
		unsigned long transfer;

		assert(argument);
		transfer = strtoul(argument, &end, 10);
		assert(*end == '\0' && transfer < 10);
		run_start_write_failure(amplifier, transfer, false, false);
	} else if (strcmp(scenario, "start-active-control-failure") == 0) {
		run_start_write_failure(amplifier, 10, false, false);
	} else if (strcmp(scenario, "start-cleanup-write-failure") == 0) {
		run_start_write_failure(amplifier, 0, true, false);
	} else if (strcmp(scenario, "start-cleanup-reset-failure") == 0) {
		run_start_write_failure(amplifier, 0, false, true);
	} else if (strcmp(scenario, "start-assert-partial-failure") == 0) {
		run_partial_gpio_failure(amplifier, false);
	} else if (strcmp(scenario, "start-release-partial-failure") == 0) {
		run_partial_gpio_failure(amplifier, true);
	} else if (strcmp(scenario, "stop-write-failure") == 0) {
		run_stop_failure(amplifier, false);
	} else if (strcmp(scenario, "stop-reset-failure") == 0) {
		run_stop_failure(amplifier, true);
	} else if (strcmp(scenario, "resume-reset-failure") == 0) {
		run_suspend_resume(amplifier, true);
	} else {
		assert(strcmp(scenario, "suspend-resume-stay-off") == 0);
		run_suspend_resume(amplifier, false);
	}
}

int main(int argc, char **argv)
{
	struct fixture fixture = {
		.property_present = true,
		.supplier_present = true,
		.ids = { 0x39, 0x39, 0x39, 0x39, 0x39 },
	};
	struct i2c_adapter adapter = { .byte_data = true };
	struct i2c_client client = {
		.dev.references = 1,
		.adapter = &adapter,
	};
	struct device consumer = { .of_node = &fixture.consumer_node };
	const char *scenario;

	assert(argc == 2 || argc == 3);
	scenario = argv[1];
	active = &fixture;
	fixture.supplier = &client;
	fixture.consumer = &consumer;
	client.dev.of_node = &fixture.supplier_node;
	if (run_identification(scenario)) {
		puts("PASS AW87339 host identification/OFF lifecycle");
		return 0;
	}
	assert(host_aw87339_driver->probe(&client) == 0);
	client.dev.driver = &host_aw87339_driver->driver;
	client.dev.bound = true;
	clear_io();
	if (strncmp(scenario, "get-", 4) == 0) {
		run_get(scenario);
	} else {
		run_api(scenario, argc == 3 ? argv[2] : NULL);
	}
	release_fixture();
	assert(fixture.reset.physical_level == 0);
	puts("PASS AW87339 host API/profile/lifetime component");
	return 0;
}
