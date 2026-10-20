// SPDX-License-Identifier: GPL-2.0-only
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>

#include <media/v4l2-async.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-mediabus.h>

#include "fplinux-camera-i2c.h"
#include "ums9117-fixed-format.h"

#define BF20A6_ID_HIGH_REG 0xfc
#define BF20A6_ID_LOW_REG 0xfd
#define BF20A6_ID_XCLK_HZ 12000000UL
#define BF20A6_MODE_XCLK_HZ 24000000UL
#define BF20A6_WIDTH 640
#define BF20A6_HEIGHT 480

struct bf20a6_reg {
	u8 address;
	u8 value;
};

struct bf20a6 {
	struct v4l2_subdev sd;
	struct media_pad pad;
	struct v4l2_ctrl_handler ctrls;
	struct clk *xclk;
	struct regulator *avdd;
	struct regulator *iovdd;
	struct gpio_desc *powerdown;
	struct gpio_desc *front_powerdown;
	bool clock_enabled;
};

static const struct bf20a6_reg bf20a6_mode_regs[] = {
	{ 0x12, 0x10 }, { 0x3a, 0x02 }, { 0xe1, 0x92 }, { 0xe3, 0x02 },
	{ 0xe0, 0x00 }, { 0x2a, 0x98 }, { 0xcd, 0x17 }, { 0xc0, 0x10 },
	{ 0xc6, 0x1d }, { 0x10, 0x35 }, { 0xe2, 0x09 }, { 0xe4, 0x92 },
	{ 0xe5, 0x22 }, { 0xe6, 0x24 }, { 0xe7, 0x54 }, { 0xe8, 0xf2 },
	{ 0x4a, 0x00 }, { 0x00, 0x03 }, { 0x1f, 0x01 }, { 0x22, 0x02 },
	{ 0xa0, 0x08 }, { 0x01, 0x1a }, { 0x01, 0x1a }, { 0x01, 0x1a },
	{ 0x02, 0x15 }, { 0x02, 0x15 }, { 0x02, 0x15 }, { 0x13, 0x08 },
	{ 0x8a, 0x96 }, { 0x8b, 0x06 }, { 0x87, 0x18 }, { 0x34, 0x48 },
	{ 0x35, 0x40 }, { 0x36, 0x40 }, { 0x72, 0x28 }, { 0x74, 0xd2 },
	{ 0x75, 0xa9 }, { 0x78, 0x12 }, { 0x79, 0xa0 }, { 0x7a, 0x94 },
	{ 0x7c, 0x97 }, { 0x40, 0x30 }, { 0x41, 0x30 }, { 0x42, 0x28 },
	{ 0x43, 0x1f }, { 0x44, 0x1c }, { 0x45, 0x16 }, { 0x46, 0x13 },
	{ 0x47, 0x10 }, { 0x48, 0x0d }, { 0x49, 0x0c }, { 0x4b, 0x0a },
	{ 0x4c, 0x0b }, { 0x4e, 0x09 }, { 0x4f, 0x08 }, { 0x50, 0x08 },
	{ 0x5f, 0x29 }, { 0x23, 0x33 }, { 0xa1, 0x10 }, { 0xa2, 0x0d },
	{ 0xa3, 0x30 }, { 0xa4, 0x06 }, { 0xa5, 0x22 }, { 0xa6, 0x56 },
	{ 0xa7, 0x18 }, { 0xa8, 0x1a }, { 0xa9, 0x12 }, { 0xaa, 0x12 },
	{ 0xab, 0x16 }, { 0xac, 0xb3 }, { 0xba, 0x12 }, { 0xbb, 0x12 },
	{ 0xad, 0x12 }, { 0xae, 0x56 }, { 0xaf, 0x0a }, { 0x3b, 0x30 },
	{ 0x3c, 0x12 }, { 0x3d, 0x22 }, { 0x3e, 0x3f }, { 0x3f, 0x28 },
	{ 0xb8, 0xc3 }, { 0xb9, 0xa3 }, { 0x39, 0x47 }, { 0x26, 0x13 },
	{ 0x27, 0x16 }, { 0x28, 0x14 }, { 0x29, 0x18 }, { 0xee, 0x0d },
	{ 0x13, 0x05 }, { 0x24, 0x40 }, { 0x81, 0x20 }, { 0x82, 0x40 },
	{ 0x83, 0x30 }, { 0x84, 0x58 }, { 0x85, 0x30 }, { 0x92, 0x08 },
	{ 0x86, 0xc0 }, { 0x8a, 0x96 }, { 0x91, 0xff }, { 0x94, 0x62 },
	{ 0x9a, 0x18 }, { 0xf0, 0x4d }, { 0x51, 0x07 }, { 0x52, 0x1b },
	{ 0x53, 0x7a }, { 0x54, 0x61 }, { 0x55, 0x56 }, { 0x56, 0x03 },
	{ 0x7e, 0x15 }, { 0x57, 0x37 }, { 0x58, 0x1e }, { 0x59, 0xaf },
	{ 0x5a, 0xc5 }, { 0x5b, 0x55 }, { 0x5c, 0x18 }, { 0x5d, 0x00 },
	{ 0x7d, 0x36 }, { 0x5e, 0x10 }, { 0xd6, 0x90 }, { 0xd5, 0x25 },
	{ 0xb0, 0xa4 }, { 0xb5, 0x08 }, { 0xb1, 0xc8 }, { 0xb2, 0xc8 },
	{ 0xb3, 0xd0 }, { 0xb4, 0xc0 }, { 0x32, 0x10 }, { 0xa0, 0x09 },
	{ 0x0b, 0x02 }, { 0x12, 0x10 }, { 0x3a, 0x02 }, { 0xe1, 0x92 },
};

static struct bf20a6 *to_bf20a6(struct v4l2_subdev *sd)
{
	return container_of(sd, struct bf20a6, sd);
}

static int bf20a6_power_off(struct bf20a6 *sensor)
{
	int ret;
	int step_ret;

	ret = gpiod_set_value_cansleep(sensor->powerdown, 1);
	step_ret = gpiod_set_value_cansleep(sensor->front_powerdown, 1);
	if (!ret)
		ret = step_ret;
	if (sensor->clock_enabled) {
		clk_disable_unprepare(sensor->xclk);
		sensor->clock_enabled = false;
	}
	step_ret = regulator_disable(sensor->iovdd);
	if (!ret)
		ret = step_ret;
	step_ret = regulator_disable(sensor->avdd);
	if (!ret)
		ret = step_ret;
	return ret;
}

static int bf20a6_start_clock(struct bf20a6 *sensor, unsigned long rate)
{
	int ret;

	ret = clk_set_rate(sensor->xclk, rate);
	if (ret)
		return ret;
	if (clk_get_rate(sensor->xclk) != rate)
		return -ERANGE;
	ret = clk_prepare_enable(sensor->xclk);
	if (!ret)
		sensor->clock_enabled = true;
	return ret;
}

static int bf20a6_power_on(struct bf20a6 *sensor)
{
	int ret;

	ret = regulator_enable(sensor->avdd);
	if (ret)
		return ret;
	ret = regulator_enable(sensor->iovdd);
	if (ret)
		goto avdd_off;
	ret = bf20a6_start_clock(sensor, BF20A6_ID_XCLK_HZ);
	if (ret)
		goto power_off;
	fsleep(2000);
	ret = gpiod_set_value_cansleep(sensor->front_powerdown, 1);
	if (ret)
		goto power_off;
	fsleep(2000);
	ret = gpiod_set_value_cansleep(sensor->powerdown, 0);
	if (ret)
		goto power_off;
	return 0;

power_off:
	bf20a6_power_off(sensor);
	return ret;

avdd_off:
	regulator_disable(sensor->avdd);
	return ret;
}

static int bf20a6_runtime_resume(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);

	return bf20a6_power_on(to_bf20a6(sd));
}

static int bf20a6_runtime_suspend(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);

	return bf20a6_power_off(to_bf20a6(sd));
}

static int bf20a6_init_state(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *state)
{
	return ums9117_fixed_format_init_state(state, BF20A6_WIDTH,
					       BF20A6_HEIGHT);
}

static int bf20a6_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state,
				  struct v4l2_subdev_frame_size_enum *size)
{
	return ums9117_fixed_format_enum_frame_size(size, BF20A6_WIDTH,
						    BF20A6_HEIGHT);
}

static int bf20a6_enable_streams(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state, u32 pad,
				 u64 streams_mask)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct bf20a6 *sensor = to_bf20a6(sd);
	size_t i;
	int ret;

	ret = pm_runtime_resume_and_get(&client->dev);
	if (ret)
		return ret;
	/* The clock rate can change only while the sensor clock is gated. */
	clk_disable_unprepare(sensor->xclk);
	sensor->clock_enabled = false;
	ret = bf20a6_start_clock(sensor, BF20A6_MODE_XCLK_HZ);
	if (ret)
		goto power_off;
	for (i = 0; i < ARRAY_SIZE(bf20a6_mode_regs); i++) {
		const struct bf20a6_reg *reg = &bf20a6_mode_regs[i];

		ret = fplinux_camera_i2c_write(client, reg->address,
					       reg->value);
		if (ret) {
			dev_err(&client->dev, "mode write failed at %zu: %pe\n",
				i, ERR_PTR(ret));
			goto power_off;
		}
	}
	return 0;

power_off:
	pm_runtime_put_sync(&client->dev);
	return ret;
}

static int bf20a6_disable_streams(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state, u32 pad,
				  u64 streams_mask)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int ret;

	ret = pm_runtime_put_sync(&client->dev);
	return ret < 0 ? ret : 0;
}

static const struct v4l2_subdev_pad_ops bf20a6_pad_ops = {
	.enum_mbus_code = ums9117_fixed_format_enum_mbus_code,
	.enum_frame_size = bf20a6_enum_frame_size,
	.get_fmt = v4l2_subdev_get_fmt,
	.set_fmt = ums9117_fixed_format_set_fmt,
	.enable_streams = bf20a6_enable_streams,
	.disable_streams = bf20a6_disable_streams,
};

static const struct v4l2_subdev_ops bf20a6_subdev_ops = {
	.pad = &bf20a6_pad_ops,
};

static const struct v4l2_subdev_internal_ops bf20a6_internal_ops = {
	.init_state = bf20a6_init_state,
};

static int bf20a6_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct v4l2_fwnode_device_properties props;
	struct bf20a6 *sensor;
	u8 high;
	u8 low;
	int ret;
	int power_ret;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return -EOPNOTSUPP;
	sensor = devm_kzalloc(dev, sizeof(*sensor), GFP_KERNEL);
	if (!sensor)
		return -ENOMEM;
	v4l2_i2c_subdev_init(&sensor->sd, client, &bf20a6_subdev_ops);
	sensor->sd.internal_ops = &bf20a6_internal_ops;

	sensor->xclk = devm_clk_get(dev, "xclk");
	if (IS_ERR(sensor->xclk))
		return dev_err_probe(dev, PTR_ERR(sensor->xclk),
				     "failed to get xclk\n");
	sensor->avdd = devm_regulator_get(dev, "avdd");
	if (IS_ERR(sensor->avdd))
		return dev_err_probe(dev, PTR_ERR(sensor->avdd),
				     "failed to get analog supply\n");
	sensor->iovdd = devm_regulator_get(dev, "iovdd");
	if (IS_ERR(sensor->iovdd))
		return dev_err_probe(dev, PTR_ERR(sensor->iovdd),
				     "failed to get IO supply\n");
	sensor->powerdown = devm_gpiod_get(dev, "powerdown", GPIOD_OUT_HIGH);
	if (IS_ERR(sensor->powerdown))
		return dev_err_probe(dev, PTR_ERR(sensor->powerdown),
				     "failed to get powerdown GPIO\n");
	sensor->front_powerdown =
		devm_gpiod_get(dev, "front-powerdown", GPIOD_OUT_HIGH);
	if (IS_ERR(sensor->front_powerdown))
		return dev_err_probe(
			dev, PTR_ERR(sensor->front_powerdown),
			"failed to get companion powerdown GPIO\n");

	ret = regulator_set_voltage(sensor->avdd, 2800000, 2800000);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to set analog voltage\n");
	ret = regulator_set_voltage(sensor->iovdd, 1800000, 1800000);
	if (ret)
		return dev_err_probe(dev, ret, "failed to set IO voltage\n");
	ret = bf20a6_power_on(sensor);
	if (ret)
		return dev_err_probe(dev, ret, "failed to power sensor\n");
	ret = fplinux_camera_i2c_read(client, BF20A6_ID_HIGH_REG, &high);
	if (!ret)
		ret = fplinux_camera_i2c_read(client, BF20A6_ID_LOW_REG, &low);
	power_ret = bf20a6_power_off(sensor);
	if (ret)
		return dev_err_probe(dev, ret, "failed to read sensor ID\n");
	if (power_ret)
		return dev_err_probe(dev, power_ret,
				     "failed to power off sensor\n");
	if (high != 0x20 || low != 0xa6)
		return dev_err_probe(dev, -ENODEV,
				     "unexpected sensor ID %02x%02x\n", high,
				     low);

	ret = v4l2_fwnode_device_parse(dev, &props);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to parse sensor properties\n");
	ret = v4l2_ctrl_handler_init(&sensor->ctrls, 2);
	if (ret)
		return ret;
	ret = v4l2_ctrl_new_fwnode_properties(&sensor->ctrls, NULL, &props);
	if (ret)
		goto ctrls_cleanup;
	sensor->sd.ctrl_handler = &sensor->ctrls;
	sensor->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	sensor->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	sensor->pad.flags = MEDIA_PAD_FL_SOURCE;
	ret = media_entity_pads_init(&sensor->sd.entity, 1, &sensor->pad);
	if (ret)
		goto ctrls_cleanup;
	ret = v4l2_subdev_init_finalize(&sensor->sd);
	if (ret)
		goto entity_cleanup;

	pm_runtime_set_suspended(dev);
	pm_runtime_enable(dev);
	ret = v4l2_async_register_subdev_sensor(&sensor->sd);
	if (ret)
		goto pm_disable;
	return 0;

pm_disable:
	pm_runtime_disable(dev);
	v4l2_subdev_cleanup(&sensor->sd);
entity_cleanup:
	media_entity_cleanup(&sensor->sd.entity);
ctrls_cleanup:
	v4l2_ctrl_handler_free(&sensor->ctrls);
	return ret;
}

static void bf20a6_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);

	v4l2_async_unregister_subdev(sd);
	pm_runtime_disable(&client->dev);
	if (!pm_runtime_status_suspended(&client->dev))
		bf20a6_power_off(to_bf20a6(sd));
	pm_runtime_set_suspended(&client->dev);
	v4l2_subdev_cleanup(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(sd->ctrl_handler);
}

static const struct of_device_id bf20a6_of_match[] = {
	{ .compatible = "byd,bf20a6" },
	{},
};
MODULE_DEVICE_TABLE(of, bf20a6_of_match);

static const struct dev_pm_ops bf20a6_pm_ops = { RUNTIME_PM_OPS(
	bf20a6_runtime_suspend, bf20a6_runtime_resume, NULL) };

static struct i2c_driver bf20a6_i2c_driver = {
	.driver = {
		.name = "bf20a6",
		.of_match_table = bf20a6_of_match,
		.pm = pm_ptr(&bf20a6_pm_ops),
	},
	.probe = bf20a6_probe,
	.remove = bf20a6_remove,
};
module_i2c_driver(bf20a6_i2c_driver);

MODULE_DESCRIPTION("BF20A6 camera sensor");
MODULE_LICENSE("GPL");
