// SPDX-License-Identifier: GPL-2.0-only
#include <linux/delay.h>
#include <linux/aw87339.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/pm.h>
#include <linux/slab.h>

#define AW87339_ID 0x00
#define AW87339_ID_VALUE 0x39
#define AW87339_CONTROL 0x01
#define AW87339_ATTEMPTS 5
#define AW87339_SETTLE_MS 2

struct aw87339 {
	struct i2c_client *client;
	struct gpio_desc *reset;
	struct mutex lock;
	bool powered;
	bool cleanup_failed;
	bool ready;
	bool suspended;
};

static struct i2c_driver aw87339_driver;

static int aw87339_write(struct aw87339 *amplifier, unsigned int reg,
			 unsigned int value)
{
	unsigned int attempt;
	int ret;

	for (attempt = 0; attempt < AW87339_ATTEMPTS; attempt++) {
		ret = i2c_smbus_write_byte_data(amplifier->client, reg, value);
		if (!ret)
			return 0;
		if (attempt + 1 < AW87339_ATTEMPTS)
			msleep(AW87339_SETTLE_MS);
	}
	return ret;
}

static int aw87339_power_off(struct aw87339 *amplifier)
{
	int ret = 0;
	int reset_ret;

	if (amplifier->powered)
		ret = aw87339_write(amplifier, AW87339_CONTROL, 0);
	/* Reset is an independent stop attempt even after a failed bus write. */
	reset_ret = gpiod_set_value_cansleep(amplifier->reset, 1);
	msleep(AW87339_SETTLE_MS);
	if (!reset_ret)
		amplifier->powered = false;
	ret = ret ? ret : reset_ret;
	amplifier->cleanup_failed = ret != 0;
	return ret;
}

int aw87339_stop(struct aw87339 *amplifier)
{
	int ret;

	mutex_lock(&amplifier->lock);
	ret = aw87339_power_off(amplifier);
	mutex_unlock(&amplifier->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(aw87339_stop);

int aw87339_start(struct aw87339 *amplifier,
		  const struct aw87339_profile *profile)
{
	unsigned int index;
	int cleanup_ret;
	int ret;

	mutex_lock(&amplifier->lock);
	if (!amplifier->ready || amplifier->suspended) {
		ret = -ENODEV;
		goto out;
	}
	if (amplifier->powered || amplifier->cleanup_failed) {
		ret = -EBUSY;
		goto out;
	}
	ret = gpiod_set_value_cansleep(amplifier->reset, 1);
	if (ret)
		goto failed;
	msleep(AW87339_SETTLE_MS);
	amplifier->powered = true;
	ret = gpiod_set_value_cansleep(amplifier->reset, 0);
	if (ret)
		goto failed;
	msleep(AW87339_SETTLE_MS);
	ret = aw87339_write(amplifier, AW87339_CONTROL,
			    profile->initial_control);
	if (ret)
		goto failed;
	for (index = 0; index < ARRAY_SIZE(profile->registers_2_to_10);
	     index++) {
		ret = aw87339_write(amplifier, index + 2,
				    profile->registers_2_to_10[index]);
		if (ret)
			goto failed;
	}
	ret = aw87339_write(amplifier, AW87339_CONTROL,
			    profile->active_control);
	if (!ret)
		goto out;
failed:
	cleanup_ret = aw87339_power_off(amplifier);
	if (cleanup_ret)
		dev_err(&amplifier->client->dev,
			"amplifier start cleanup failed: %d\n", cleanup_ret);
out:
	mutex_unlock(&amplifier->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(aw87339_start);

static void aw87339_cleanup(void *data)
{
	struct aw87339 *amplifier = data;
	int ret;

	mutex_lock(&amplifier->lock);
	amplifier->ready = false;
	ret = aw87339_power_off(amplifier);
	mutex_unlock(&amplifier->lock);

	if (ret)
		dev_err(&amplifier->client->dev,
			"amplifier shutdown failed: %d\n", ret);
}

static void aw87339_put_client(void *data)
{
	struct i2c_client *client = data;

	put_device(&client->dev);
}

struct aw87339 *devm_aw87339_get(struct device *consumer, const char *property)
{
	struct device_node *node;
	struct device_link *link;
	struct i2c_client *client;
	struct aw87339 *amplifier;
	int ret;

	node = of_parse_phandle(consumer->of_node, property, 0);
	if (!node)
		return ERR_PTR(-EINVAL);
	client = of_find_i2c_device_by_node(node);
	of_node_put(node);
	if (!client)
		return ERR_PTR(-EPROBE_DEFER);
	device_lock(&client->dev);
	amplifier = i2c_get_clientdata(client);
	if (!device_is_bound(&client->dev) ||
	    client->dev.driver != &aw87339_driver.driver || !amplifier ||
	    !amplifier->ready) {
		ret = -EPROBE_DEFER;
		goto unlock;
	}
	link = device_link_add(consumer, &client->dev,
			       DL_FLAG_AUTOREMOVE_CONSUMER);
	if (!link) {
		ret = -ENOMEM;
		goto unlock;
	}
	device_unlock(&client->dev);
	ret = devm_add_action_or_reset(consumer, aw87339_put_client, client);
	return ret ? ERR_PTR(ret) : amplifier;
unlock:
	device_unlock(&client->dev);
	put_device(&client->dev);
	return ERR_PTR(ret);
}
EXPORT_SYMBOL_GPL(devm_aw87339_get);

static int aw87339_read_id(struct aw87339 *amplifier)
{
	unsigned int attempt;
	int ret;

	for (attempt = 0; attempt < AW87339_ATTEMPTS; attempt++) {
		ret = i2c_smbus_read_byte_data(amplifier->client, AW87339_ID);
		if (ret == AW87339_ID_VALUE)
			return 0;
		if (attempt + 1 < AW87339_ATTEMPTS)
			msleep(AW87339_SETTLE_MS);
	}
	return ret < 0 ? ret : -ENODEV;
}

static int aw87339_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct aw87339 *amplifier;
	int ret;
	int cleanup_ret;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_SMBUS_BYTE_DATA))
		return -EOPNOTSUPP;
	amplifier = devm_kzalloc(dev, sizeof(*amplifier), GFP_KERNEL);
	if (!amplifier)
		return -ENOMEM;
	amplifier->client = client;
	mutex_init(&amplifier->lock);
	amplifier->reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(amplifier->reset))
		return dev_err_probe(dev, PTR_ERR(amplifier->reset),
				     "failed to assert amplifier reset\n");
	i2c_set_clientdata(client, amplifier);
	ret = devm_add_action_or_reset(dev, aw87339_cleanup, amplifier);
	if (ret)
		return ret;

	msleep(AW87339_SETTLE_MS);
	/* A failed GPIO transaction may still have reached the output latch. */
	amplifier->powered = true;
	ret = gpiod_set_value_cansleep(amplifier->reset, 0);
	if (!ret) {
		msleep(AW87339_SETTLE_MS);
		ret = aw87339_read_id(amplifier);
	}
	cleanup_ret = aw87339_power_off(amplifier);
	if (cleanup_ret)
		dev_err(dev,
			"amplifier reset after identification failed: %d\n",
			cleanup_ret);
	if (ret)
		return dev_err_probe(dev, ret,
				     "amplifier identification failed\n");
	if (cleanup_ret)
		return cleanup_ret;
	amplifier->ready = true;
	dev_info(dev, "AW87339 identified and held in reset\n");
	return 0;
}

static int aw87339_suspend(struct device *dev)
{
	struct aw87339 *amplifier = dev_get_drvdata(dev);
	int ret;

	mutex_lock(&amplifier->lock);
	amplifier->suspended = true;
	ret = aw87339_power_off(amplifier);
	mutex_unlock(&amplifier->lock);
	return ret;
}

static int aw87339_resume(struct device *dev)
{
	struct aw87339 *amplifier = dev_get_drvdata(dev);
	int ret;

	mutex_lock(&amplifier->lock);
	ret = aw87339_power_off(amplifier);
	if (!ret)
		amplifier->suspended = false;
	mutex_unlock(&amplifier->lock);
	return ret;
}

static DEFINE_SIMPLE_DEV_PM_OPS(aw87339_pm_ops, aw87339_suspend,
				aw87339_resume);

static void aw87339_shutdown(struct i2c_client *client)
{
	aw87339_cleanup(i2c_get_clientdata(client));
}

static const struct of_device_id aw87339_match[] = {
	{ .compatible = "awinic,aw87339" },
	{}
};
MODULE_DEVICE_TABLE(of, aw87339_match);

static struct i2c_driver aw87339_driver = {
	.probe = aw87339_probe,
	.shutdown = aw87339_shutdown,
	.driver = {
		.name = "aw87339",
		.of_match_table = aw87339_match,
		.pm = pm_sleep_ptr(&aw87339_pm_ops),
	},
};
module_i2c_driver(aw87339_driver);

MODULE_DESCRIPTION("AW87339 amplifier power and reset control");
MODULE_LICENSE("GPL");
