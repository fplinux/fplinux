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

#define GC032A_ID_HIGH_REG 0xf0
#define GC032A_ID_LOW_REG 0xf1
#define GC032A_ID_XCLK_HZ 12000000UL
#define GC032A_MODE_XCLK_HZ 24000000UL
#define GC032A_WIDTH 640
#define GC032A_HEIGHT 476

struct gc032a_reg {
	u8 address;
	u8 value;
};

struct gc032a {
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

static const struct gc032a_reg gc032a_mode_regs[] = {
	{ 0xf3, 0x83 }, { 0xf5, 0x0c }, { 0xf7, 0x01 }, { 0xf8, 0x02 },
	{ 0xf9, 0x4e }, { 0xfa, 0x10 }, { 0xfc, 0x02 }, { 0xfe, 0x02 },
	{ 0x81, 0x03 }, { 0xfe, 0x00 }, { 0x77, 0x64 }, { 0x78, 0x40 },
	{ 0x79, 0x60 }, { 0xfe, 0x00 }, { 0x03, 0x01 }, { 0x04, 0x20 },
	{ 0x05, 0x01 }, { 0x06, 0xaf }, { 0x07, 0x00 }, { 0x08, 0x08 },
	{ 0x0a, 0x00 }, { 0x0c, 0x00 }, { 0x0d, 0x01 }, { 0x0e, 0xe8 },
	{ 0x0f, 0x02 }, { 0x10, 0x88 }, { 0x17, 0x54 }, { 0x19, 0x08 },
	{ 0x1a, 0x0a }, { 0x1f, 0x40 }, { 0x20, 0x30 }, { 0x2e, 0x80 },
	{ 0x2f, 0x2b }, { 0x30, 0x1a }, { 0xfe, 0x02 }, { 0x03, 0x02 },
	{ 0x06, 0x60 }, { 0x05, 0xdf }, { 0x12, 0x89 }, { 0xfe, 0x03 },
	{ 0x51, 0x01 }, { 0x52, 0xd8 }, { 0x53, 0x25 }, { 0x54, 0x20 },
	{ 0x55, 0x00 }, { 0x59, 0x10 }, { 0x5a, 0x00 }, { 0x5b, 0x80 },
	{ 0x5c, 0x02 }, { 0x5d, 0xe0 }, { 0x5e, 0x01 }, { 0x64, 0x06 },
	{ 0x65, 0xff }, { 0x66, 0x00 }, { 0x67, 0x00 }, { 0x60, 0xb6 },
	{ 0x61, 0x80 }, { 0x62, 0x9d }, { 0x63, 0xb6 }, { 0xfe, 0x00 },
	{ 0x18, 0x02 }, { 0xfe, 0x02 }, { 0x40, 0x22 }, { 0x45, 0x00 },
	{ 0x46, 0x00 }, { 0x49, 0x20 }, { 0x4b, 0x3c }, { 0x50, 0x20 },
	{ 0x42, 0x10 }, { 0xfe, 0x01 }, { 0x0a, 0xc5 }, { 0x45, 0x00 },
	{ 0xfe, 0x00 }, { 0x40, 0xff }, { 0x41, 0x25 }, { 0x42, 0xcf },
	{ 0x43, 0x10 }, { 0x44, 0x83 }, { 0x46, 0x22 }, { 0x49, 0x03 },
	{ 0x52, 0x02 }, { 0x54, 0x00 }, { 0xfe, 0x02 }, { 0x22, 0xf6 },
	{ 0xfe, 0x01 }, { 0xc1, 0x38 }, { 0xc2, 0x4c }, { 0xc3, 0x00 },
	{ 0xc4, 0x32 }, { 0xc5, 0x24 }, { 0xc6, 0x16 }, { 0xc7, 0x08 },
	{ 0xc8, 0x08 }, { 0xc9, 0x00 }, { 0xca, 0x20 }, { 0xdc, 0x8a },
	{ 0xdd, 0xa0 }, { 0xde, 0xa6 }, { 0xdf, 0x75 }, { 0xfe, 0x01 },
	{ 0x7c, 0x09 }, { 0x65, 0x06 }, { 0x7c, 0x08 }, { 0x56, 0xf4 },
	{ 0x66, 0x0f }, { 0x67, 0x84 }, { 0x6b, 0x80 }, { 0x6d, 0x02 },
	{ 0x6e, 0xb0 }, { 0x77, 0x57 }, { 0x78, 0x4d }, { 0x79, 0x45 },
	{ 0xfe, 0x01 }, { 0x90, 0x00 }, { 0x91, 0x00 }, { 0x92, 0xf4 },
	{ 0x93, 0xd5 }, { 0x95, 0x0f }, { 0x96, 0xf4 }, { 0x97, 0x2d },
	{ 0x98, 0x0f }, { 0x9a, 0x2d }, { 0x9b, 0x0f }, { 0x9c, 0x59 },
	{ 0x9d, 0x2d }, { 0x9f, 0x67 }, { 0xa0, 0x59 }, { 0xa1, 0x00 },
	{ 0xa2, 0x00 }, { 0x86, 0x00 }, { 0x87, 0x00 }, { 0x88, 0x00 },
	{ 0x89, 0x00 }, { 0xa4, 0x00 }, { 0xa5, 0x00 }, { 0xa6, 0xd4 },
	{ 0xa7, 0x9f }, { 0xa9, 0xd4 }, { 0xaa, 0x9f }, { 0xab, 0xac },
	{ 0xac, 0x9f }, { 0xae, 0xd4 }, { 0xaf, 0xac }, { 0xb0, 0xd4 },
	{ 0xb1, 0xa3 }, { 0xb3, 0xd4 }, { 0xb4, 0xac }, { 0xb5, 0x00 },
	{ 0xb6, 0x00 }, { 0x8b, 0x00 }, { 0x8c, 0x00 }, { 0x8d, 0x00 },
	{ 0x8e, 0x00 }, { 0x94, 0x50 }, { 0x99, 0xa6 }, { 0x9e, 0xaa },
	{ 0xa3, 0x0a }, { 0x8a, 0x00 }, { 0xa8, 0x50 }, { 0xad, 0x55 },
	{ 0xb2, 0x55 }, { 0xb7, 0x05 }, { 0x8f, 0x00 }, { 0xb8, 0xb3 },
	{ 0xb9, 0xb6 }, { 0xfe, 0x01 }, { 0xd0, 0x40 }, { 0xd1, 0xf8 },
	{ 0xd2, 0x00 }, { 0xd3, 0xfa }, { 0xd4, 0x45 }, { 0xd5, 0x02 },
	{ 0xd6, 0x30 }, { 0xd7, 0xfa }, { 0xd8, 0x08 }, { 0xd9, 0x08 },
	{ 0xda, 0x58 }, { 0xdb, 0x02 }, { 0xfe, 0x00 }, { 0xfe, 0x00 },
	{ 0xba, 0x00 }, { 0xbb, 0x04 }, { 0xbc, 0x0a }, { 0xbd, 0x0e },
	{ 0xbe, 0x22 }, { 0xbf, 0x30 }, { 0xc0, 0x3d }, { 0xc1, 0x4a },
	{ 0xc2, 0x5d }, { 0xc3, 0x6b }, { 0xc4, 0x7a }, { 0xc5, 0x85 },
	{ 0xc6, 0x90 }, { 0xc7, 0xa5 }, { 0xc8, 0xb5 }, { 0xc9, 0xc2 },
	{ 0xca, 0xcc }, { 0xcb, 0xd5 }, { 0xcc, 0xde }, { 0xcd, 0xea },
	{ 0xce, 0xf5 }, { 0xcf, 0xff }, { 0xfe, 0x00 }, { 0x5a, 0x08 },
	{ 0x5b, 0x0f }, { 0x5c, 0x15 }, { 0x5d, 0x1c }, { 0x5e, 0x28 },
	{ 0x5f, 0x36 }, { 0x60, 0x45 }, { 0x61, 0x51 }, { 0x62, 0x6a },
	{ 0x63, 0x7d }, { 0x64, 0x8d }, { 0x65, 0x98 }, { 0x66, 0xa2 },
	{ 0x67, 0xb5 }, { 0x68, 0xc3 }, { 0x69, 0xcd }, { 0x6a, 0xd4 },
	{ 0x6b, 0xdc }, { 0x6c, 0xe3 }, { 0x6d, 0xf0 }, { 0x6e, 0xf9 },
	{ 0x6f, 0xff }, { 0xfe, 0x00 }, { 0x70, 0x50 }, { 0xfe, 0x00 },
	{ 0x4f, 0x01 }, { 0xfe, 0x01 }, { 0x0d, 0x00 }, { 0x12, 0xa0 },
	{ 0x13, 0x50 }, { 0x44, 0x04 }, { 0x1f, 0x30 }, { 0x20, 0x40 },
	{ 0x3e, 0x20 }, { 0x3f, 0x2d }, { 0x40, 0x40 }, { 0x41, 0x5b },
	{ 0x42, 0x82 }, { 0x43, 0xb7 }, { 0x04, 0x0a }, { 0x02, 0x79 },
	{ 0x03, 0xc0 }, { 0xfe, 0x01 }, { 0xcc, 0x08 }, { 0xcd, 0x08 },
	{ 0xce, 0xa4 }, { 0xcf, 0xec }, { 0xfe, 0x00 }, { 0x81, 0xb8 },
	{ 0x82, 0x12 }, { 0x83, 0x0a }, { 0x84, 0x01 }, { 0x86, 0x50 },
	{ 0x87, 0x18 }, { 0x88, 0x10 }, { 0x89, 0x70 }, { 0x8a, 0x20 },
	{ 0x8b, 0x10 }, { 0x8c, 0x08 }, { 0x8d, 0x0a }, { 0xfe, 0x00 },
	{ 0x8f, 0xaa }, { 0x90, 0x9c }, { 0x91, 0x52 }, { 0x92, 0x03 },
	{ 0x93, 0x03 }, { 0x94, 0x08 }, { 0x95, 0x65 }, { 0x97, 0x00 },
	{ 0x98, 0x00 }, { 0xfe, 0x00 }, { 0xa1, 0x30 }, { 0xa2, 0x41 },
	{ 0xa4, 0x30 }, { 0xa5, 0x20 }, { 0xaa, 0x30 }, { 0xac, 0x32 },
	{ 0xfe, 0x00 }, { 0xd1, 0x38 }, { 0xd2, 0x38 }, { 0xd3, 0x38 },
	{ 0xd6, 0xf4 }, { 0xd7, 0x1d }, { 0xdd, 0x73 }, { 0xde, 0x84 },
	{ 0xfe, 0x00 }, { 0x05, 0x02 }, { 0x06, 0x14 }, { 0x07, 0x00 },
	{ 0x08, 0x0c }, { 0xfe, 0x01 }, { 0x25, 0x00 }, { 0x26, 0x33 },
	{ 0x27, 0x01 }, { 0x28, 0xfe }, { 0x29, 0x01 }, { 0x2a, 0xfe },
	{ 0x2b, 0x01 }, { 0x2c, 0xfe }, { 0x2d, 0x01 }, { 0x2e, 0xfe },
	{ 0x2f, 0x01 }, { 0x30, 0xfe }, { 0x31, 0x01 }, { 0x32, 0xfe },
	{ 0x33, 0x01 }, { 0x34, 0xfe }, { 0x3c, 0x30 }, { 0xfe, 0x00 },
};

static struct gc032a *to_gc032a(struct v4l2_subdev *sd)
{
	return container_of(sd, struct gc032a, sd);
}

static int gc032a_power_off(struct gc032a *sensor)
{
	int ret;
	int step_ret;

	ret = gpiod_set_value_cansleep(sensor->powerdown, 1);
	if (sensor->clock_enabled) {
		clk_disable_unprepare(sensor->xclk);
		sensor->clock_enabled = false;
	}
	step_ret = regulator_disable(sensor->avdd);
	if (!ret)
		ret = step_ret;
	step_ret = regulator_disable(sensor->iovdd);
	if (!ret)
		ret = step_ret;
	return ret;
}

static int gc032a_start_clock(struct gc032a *sensor, unsigned long rate)
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

static int gc032a_power_on(struct gc032a *sensor)
{
	int ret;

	ret = regulator_enable(sensor->avdd);
	if (ret)
		return ret;
	ret = regulator_enable(sensor->iovdd);
	if (ret)
		goto avdd_off;
	ret = gpiod_direction_output(sensor->front_powerdown, 1);
	if (ret)
		goto iovdd_off;
	fsleep(2000);
	ret = gpiod_set_value_cansleep(sensor->powerdown, 0);
	if (ret)
		goto iovdd_off;
	fsleep(2000);
	ret = gpiod_set_value_cansleep(sensor->powerdown, 1);
	if (ret)
		goto iovdd_off;
	fsleep(2000);
	ret = gpiod_set_value_cansleep(sensor->powerdown, 0);
	if (ret)
		goto iovdd_off;
	ret = gc032a_start_clock(sensor, GC032A_ID_XCLK_HZ);
	if (ret)
		goto iovdd_off;
	return 0;

iovdd_off:
	gpiod_set_value_cansleep(sensor->powerdown, 1);
	regulator_disable(sensor->iovdd);
avdd_off:
	regulator_disable(sensor->avdd);
	return ret;
}

static int gc032a_runtime_resume(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);

	return gc032a_power_on(to_gc032a(sd));
}

static int gc032a_runtime_suspend(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);

	return gc032a_power_off(to_gc032a(sd));
}

static int gc032a_init_state(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *state)
{
	return ums9117_fixed_format_init_state(state, GC032A_WIDTH,
					       GC032A_HEIGHT);
}

static int gc032a_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state,
				  struct v4l2_subdev_frame_size_enum *size)
{
	return ums9117_fixed_format_enum_frame_size(size, GC032A_WIDTH,
						    GC032A_HEIGHT);
}

static int gc032a_enable_streams(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state, u32 pad,
				 u64 streams_mask)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct gc032a *sensor = to_gc032a(sd);
	size_t i;
	int ret;

	ret = pm_runtime_resume_and_get(&client->dev);
	if (ret)
		return ret;
	/* The clock rate can change only while the sensor clock is gated. */
	clk_disable_unprepare(sensor->xclk);
	sensor->clock_enabled = false;
	ret = gc032a_start_clock(sensor, GC032A_MODE_XCLK_HZ);
	if (ret)
		goto power_off;
	for (i = 0; i < ARRAY_SIZE(gc032a_mode_regs); i++) {
		const struct gc032a_reg *reg = &gc032a_mode_regs[i];

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

static int gc032a_disable_streams(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state, u32 pad,
				  u64 streams_mask)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int ret;

	ret = pm_runtime_put_sync(&client->dev);
	return ret < 0 ? ret : 0;
}

static const struct v4l2_subdev_pad_ops gc032a_pad_ops = {
	.enum_mbus_code = ums9117_fixed_format_enum_mbus_code,
	.enum_frame_size = gc032a_enum_frame_size,
	.get_fmt = v4l2_subdev_get_fmt,
	.set_fmt = ums9117_fixed_format_set_fmt,
	.enable_streams = gc032a_enable_streams,
	.disable_streams = gc032a_disable_streams,
};

static const struct v4l2_subdev_ops gc032a_subdev_ops = {
	.pad = &gc032a_pad_ops,
};

static const struct v4l2_subdev_internal_ops gc032a_internal_ops = {
	.init_state = gc032a_init_state,
};

static int gc032a_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct v4l2_fwnode_device_properties props;
	struct gc032a *sensor;
	u8 high;
	u8 low;
	int ret;
	int power_ret;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return -EOPNOTSUPP;
	sensor = devm_kzalloc(dev, sizeof(*sensor), GFP_KERNEL);
	if (!sensor)
		return -ENOMEM;
	v4l2_i2c_subdev_init(&sensor->sd, client, &gc032a_subdev_ops);
	sensor->sd.internal_ops = &gc032a_internal_ops;

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
		devm_gpiod_get(dev, "front-powerdown", GPIOD_ASIS);
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
	ret = gc032a_power_on(sensor);
	if (ret)
		return dev_err_probe(dev, ret, "failed to power sensor\n");
	ret = fplinux_camera_i2c_read(client, GC032A_ID_HIGH_REG, &high);
	if (!ret)
		ret = fplinux_camera_i2c_read(client, GC032A_ID_LOW_REG, &low);
	power_ret = gc032a_power_off(sensor);
	if (ret)
		return dev_err_probe(dev, ret, "failed to read sensor ID\n");
	if (power_ret)
		return dev_err_probe(dev, power_ret,
				     "failed to power off sensor\n");
	if (high != 0x23 || low != 0x2a)
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

static void gc032a_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);

	v4l2_async_unregister_subdev(sd);
	pm_runtime_disable(&client->dev);
	if (!pm_runtime_status_suspended(&client->dev))
		gc032a_power_off(to_gc032a(sd));
	pm_runtime_set_suspended(&client->dev);
	v4l2_subdev_cleanup(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(sd->ctrl_handler);
}

static const struct of_device_id gc032a_of_match[] = {
	{ .compatible = "galaxycore,gc032a" },
	{}
};
MODULE_DEVICE_TABLE(of, gc032a_of_match);

static const struct dev_pm_ops gc032a_pm_ops = { RUNTIME_PM_OPS(
	gc032a_runtime_suspend, gc032a_runtime_resume, NULL) };

static struct i2c_driver gc032a_i2c_driver = {
	.driver = {
		.name = "gc032a",
		.of_match_table = gc032a_of_match,
		.pm = pm_ptr(&gc032a_pm_ops),
	},
	.probe = gc032a_probe,
	.remove = gc032a_remove,
};
module_i2c_driver(gc032a_i2c_driver);

MODULE_DESCRIPTION("GC032A two-bit serial camera sensor");
MODULE_LICENSE("GPL");
