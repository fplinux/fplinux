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

#define BF30A2_ID_HIGH_REG 0xfc
#define BF30A2_ID_LOW_REG 0xfd
#define BF30A2_ID_HIGH 0x3b
#define BF30A2_ID_LOW 0x02
#define BF30A2_ID_XCLK_HZ 12000000UL
#define BF30A2_MODE_XCLK_HZ 24000000UL
#define BF30A2_WIDTH 240
#define BF30A2_HEIGHT 320

struct bf30a2_reg {
	u8 address;
	u8 value;
};

struct bf30a2 {
	struct v4l2_subdev sd;
	struct media_pad pad;
	struct v4l2_ctrl_handler ctrls;
	struct clk *xclk;
	struct regulator *core;
	struct regulator *avdd;
	struct regulator *iovdd;
	struct gpio_desc *reset;
	struct gpio_desc *powerdown;
};

static const struct bf30a2_reg bf30a2_mode_regs[] = {
	{ 0xf2, 0x01 }, { 0xcf, 0xb0 }, { 0x12, 0x10 }, { 0x6b, 0x70 },
	{ 0x15, 0x10 }, { 0x00, 0x40 }, { 0x04, 0x00 }, { 0x06, 0x26 },
	{ 0x08, 0x07 }, { 0x1c, 0x12 }, { 0x1e, 0x26 }, { 0x1f, 0x01 },
	{ 0x20, 0x20 }, { 0x21, 0x20 }, { 0x34, 0x02 }, { 0x35, 0x02 },
	{ 0x36, 0x21 }, { 0x37, 0x13 }, { 0xca, 0x03 }, { 0xcb, 0x22 },
	{ 0xcc, 0x89 }, { 0xcd, 0x4c }, { 0xce, 0x6b }, { 0xcf, 0xb0 },
	{ 0xa0, 0x8e }, { 0x01, 0x16 }, { 0x02, 0x22 }, { 0x13, 0x08 },
	{ 0x87, 0x13 }, { 0x8a, 0x33 }, { 0x8b, 0x08 }, { 0x70, 0x1f },
	{ 0x71, 0x40 }, { 0x72, 0x0a }, { 0x73, 0x62 }, { 0x74, 0xa2 },
	{ 0x75, 0xbf }, { 0x76, 0x02 }, { 0x77, 0xcc }, { 0x40, 0x32 },
	{ 0x41, 0x28 }, { 0x42, 0x26 }, { 0x43, 0x1d }, { 0x44, 0x1a },
	{ 0x45, 0x14 }, { 0x46, 0x11 }, { 0x47, 0x0f }, { 0x48, 0x0e },
	{ 0x49, 0x0d }, { 0x4b, 0x0c }, { 0x4c, 0x0b }, { 0x4e, 0x0a },
	{ 0x4f, 0x09 }, { 0x50, 0x09 }, { 0x24, 0x40 }, { 0x25, 0x36 },
	{ 0x80, 0x00 }, { 0x81, 0x20 }, { 0x82, 0x40 }, { 0x83, 0x30 },
	{ 0x84, 0x50 }, { 0x85, 0x30 }, { 0x86, 0xd8 }, { 0x89, 0x45 },
	{ 0x8f, 0x81 }, { 0x91, 0xff }, { 0x92, 0x08 }, { 0x94, 0x82 },
	{ 0x95, 0xfd }, { 0x9a, 0x20 }, { 0x9e, 0xbc }, { 0xf0, 0x8f },
	{ 0x51, 0x06 }, { 0x52, 0x25 }, { 0x53, 0x2b }, { 0x54, 0x0f },
	{ 0x57, 0x2a }, { 0x58, 0x22 }, { 0x59, 0x2c }, { 0x23, 0x33 },
	{ 0xa1, 0x13 }, { 0xa2, 0x0f }, { 0xa3, 0x2a }, { 0xa4, 0x08 },
	{ 0xa5, 0x26 }, { 0xa7, 0x80 }, { 0xa8, 0x80 }, { 0xa9, 0x1e },
	{ 0xaa, 0x19 }, { 0xab, 0x18 }, { 0xae, 0xb0 }, { 0xaf, 0x04 },
	{ 0xc8, 0x10 }, { 0xc9, 0x15 }, { 0xd3, 0x0c }, { 0xd4, 0x16 },
	{ 0xee, 0x06 }, { 0xef, 0x04 }, { 0x55, 0x34 }, { 0x56, 0x9c },
	{ 0xb1, 0x98 }, { 0xb2, 0x98 }, { 0xb3, 0xc4 }, { 0xb4, 0x0c },
	{ 0xa0, 0x8f }, { 0x13, 0x07 },
};

static struct bf30a2 *to_bf30a2(struct v4l2_subdev *sd)
{
	return container_of(sd, struct bf30a2, sd);
}

static int bf30a2_write(struct i2c_client *client, u8 address, u8 value)
{
	u8 data[] = { address, value };
	int ret = i2c_master_send(client, data, sizeof(data));

	if (ret < 0)
		return ret;
	return ret == sizeof(data) ? 0 : -EIO;
}

static int bf30a2_read(struct i2c_client *client, u8 address, u8 *value)
{
	struct i2c_msg messages[] = {
		{ .addr = client->addr, .len = 1, .buf = &address },
		{ .addr = client->addr,
		  .flags = I2C_M_RD,
		  .len = 1,
		  .buf = value },
	};
	int ret = i2c_transfer(client->adapter, messages, ARRAY_SIZE(messages));

	if (ret < 0)
		return ret;
	return ret == ARRAY_SIZE(messages) ? 0 : -EIO;
}

static int bf30a2_power_off(struct bf30a2 *sensor)
{
	int ret = 0;
	int step_ret;

	gpiod_set_value_cansleep(sensor->powerdown, 1);
	gpiod_set_value_cansleep(sensor->reset, 0);
	clk_disable_unprepare(sensor->xclk);
	step_ret = regulator_disable(sensor->iovdd);
	if (!ret)
		ret = step_ret;
	step_ret = regulator_disable(sensor->avdd);
	if (!ret)
		ret = step_ret;
	step_ret = regulator_disable(sensor->core);
	if (!ret)
		ret = step_ret;

	return ret;
}

static int bf30a2_power_on(struct bf30a2 *sensor)
{
	int ret;

	ret = regulator_enable(sensor->core);
	if (ret)
		return ret;
	ret = regulator_enable(sensor->avdd);
	if (ret)
		goto core_off;
	ret = regulator_enable(sensor->iovdd);
	if (ret)
		goto avdd_off;

	gpiod_set_value_cansleep(sensor->powerdown, 0);
	ret = clk_prepare_enable(sensor->xclk);
	if (ret)
		goto iovdd_off;
	msleep(20);
	gpiod_set_value_cansleep(sensor->reset, 0);
	msleep(20);
	gpiod_set_value_cansleep(sensor->reset, 1);
	msleep(100);

	return 0;

iovdd_off:
	gpiod_set_value_cansleep(sensor->powerdown, 1);
	regulator_disable(sensor->iovdd);
avdd_off:
	regulator_disable(sensor->avdd);
core_off:
	regulator_disable(sensor->core);
	return ret;
}

static int bf30a2_runtime_resume(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);

	return bf30a2_power_on(to_bf30a2(sd));
}

static int bf30a2_runtime_suspend(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);

	return bf30a2_power_off(to_bf30a2(sd));
}

static int bf30a2_init_state(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *state)
{
	struct v4l2_mbus_framefmt *format =
		v4l2_subdev_state_get_format(state, 0);

	format->width = BF30A2_WIDTH;
	format->height = BF30A2_HEIGHT;
	format->code = MEDIA_BUS_FMT_YUYV8_2X8;
	format->field = V4L2_FIELD_NONE;
	format->colorspace = V4L2_COLORSPACE_SRGB;
	format->ycbcr_enc = V4L2_YCBCR_ENC_601;
	format->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	format->xfer_func = V4L2_XFER_FUNC_SRGB;
	return 0;
}

static int bf30a2_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->pad || code->index)
		return -EINVAL;
	code->code = MEDIA_BUS_FMT_YUYV8_2X8;
	return 0;
}

static int bf30a2_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state,
				  struct v4l2_subdev_frame_size_enum *size)
{
	if (size->pad || size->index || size->code != MEDIA_BUS_FMT_YUYV8_2X8)
		return -EINVAL;
	size->min_width = BF30A2_WIDTH;
	size->max_width = BF30A2_WIDTH;
	size->min_height = BF30A2_HEIGHT;
	size->max_height = BF30A2_HEIGHT;
	return 0;
}

static int bf30a2_set_fmt(struct v4l2_subdev *sd,
			  struct v4l2_subdev_state *state,
			  struct v4l2_subdev_format *format)
{
	struct v4l2_mbus_framefmt *sensor_format;

	if (format->pad)
		return -EINVAL;
	sensor_format = v4l2_subdev_state_get_format(state, 0);
	format->format = *sensor_format;
	return 0;
}

static int bf30a2_enable_streams(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state, u32 pad,
				 u64 streams_mask)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	size_t i;
	int ret;

	ret = pm_runtime_resume_and_get(&client->dev);
	if (ret)
		return ret;

	for (i = 0; i < ARRAY_SIZE(bf30a2_mode_regs); i++) {
		ret = bf30a2_write(client, bf30a2_mode_regs[i].address,
				   bf30a2_mode_regs[i].value);
		if (ret) {
			dev_err(&client->dev, "mode write failed at %zu: %pe\n",
				i, ERR_PTR(ret));
			pm_runtime_put_sync(&client->dev);
			return ret;
		}
	}

	return 0;
}

static int bf30a2_disable_streams(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state, u32 pad,
				  u64 streams_mask)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int ret;

	ret = pm_runtime_put_sync(&client->dev);
	return ret < 0 ? ret : 0;
}

static const struct v4l2_subdev_pad_ops bf30a2_pad_ops = {
	.enum_mbus_code = bf30a2_enum_mbus_code,
	.enum_frame_size = bf30a2_enum_frame_size,
	.get_fmt = v4l2_subdev_get_fmt,
	.set_fmt = bf30a2_set_fmt,
	.enable_streams = bf30a2_enable_streams,
	.disable_streams = bf30a2_disable_streams,
};

static const struct v4l2_subdev_ops bf30a2_subdev_ops = {
	.pad = &bf30a2_pad_ops,
};

static const struct v4l2_subdev_internal_ops bf30a2_internal_ops = {
	.init_state = bf30a2_init_state,
};

static int bf30a2_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct v4l2_fwnode_device_properties props;
	struct bf30a2 *sensor;
	u8 high;
	u8 low;
	int ret;
	int power_ret;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return -EOPNOTSUPP;

	sensor = devm_kzalloc(dev, sizeof(*sensor), GFP_KERNEL);
	if (!sensor)
		return -ENOMEM;
	v4l2_i2c_subdev_init(&sensor->sd, client, &bf30a2_subdev_ops);
	sensor->sd.internal_ops = &bf30a2_internal_ops;

	sensor->xclk = devm_clk_get(dev, "xclk");
	if (IS_ERR(sensor->xclk))
		return dev_err_probe(dev, PTR_ERR(sensor->xclk),
				     "failed to get xclk\n");
	sensor->core = devm_regulator_get(dev, "camera-core");
	if (IS_ERR(sensor->core))
		return dev_err_probe(dev, PTR_ERR(sensor->core),
				     "failed to get core supply\n");
	sensor->avdd = devm_regulator_get(dev, "avdd");
	if (IS_ERR(sensor->avdd))
		return dev_err_probe(dev, PTR_ERR(sensor->avdd),
				     "failed to get analog supply\n");
	sensor->iovdd = devm_regulator_get(dev, "iovdd");
	if (IS_ERR(sensor->iovdd))
		return dev_err_probe(dev, PTR_ERR(sensor->iovdd),
				     "failed to get IO supply\n");
	sensor->reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(sensor->reset))
		return dev_err_probe(dev, PTR_ERR(sensor->reset),
				     "failed to get reset GPIO\n");
	sensor->powerdown = devm_gpiod_get(dev, "powerdown", GPIOD_OUT_HIGH);
	if (IS_ERR(sensor->powerdown))
		return dev_err_probe(dev, PTR_ERR(sensor->powerdown),
				     "failed to get powerdown GPIO\n");

	ret = regulator_set_voltage(sensor->core, 1250000, 1250000);
	if (ret)
		return dev_err_probe(dev, ret, "failed to set core voltage\n");
	ret = regulator_set_voltage(sensor->avdd, 2800000, 2800000);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to set analog voltage\n");
	ret = regulator_set_voltage(sensor->iovdd, 1800000, 1800000);
	if (ret)
		return dev_err_probe(dev, ret, "failed to set IO voltage\n");
	ret = clk_set_rate(sensor->xclk, BF30A2_ID_XCLK_HZ);
	if (ret)
		return dev_err_probe(dev, ret, "failed to set ID clock\n");
	if (clk_get_rate(sensor->xclk) != BF30A2_ID_XCLK_HZ)
		return -ERANGE;

	ret = bf30a2_power_on(sensor);
	if (ret)
		return dev_err_probe(dev, ret, "failed to power sensor\n");
	ret = bf30a2_read(client, BF30A2_ID_HIGH_REG, &high);
	if (!ret)
		ret = bf30a2_read(client, BF30A2_ID_LOW_REG, &low);
	power_ret = bf30a2_power_off(sensor);
	if (ret)
		return dev_err_probe(dev, ret, "failed to read sensor ID\n");
	if (power_ret)
		return dev_err_probe(dev, power_ret,
				     "failed to power off sensor\n");
	if (high != BF30A2_ID_HIGH || low != BF30A2_ID_LOW)
		return dev_err_probe(dev, -ENODEV,
				     "unexpected sensor ID %02x%02x\n", high,
				     low);

	ret = clk_set_rate(sensor->xclk, BF30A2_MODE_XCLK_HZ);
	if (ret)
		return dev_err_probe(dev, ret, "failed to set mode clock\n");
	if (clk_get_rate(sensor->xclk) != BF30A2_MODE_XCLK_HZ)
		return -ERANGE;

	ret = v4l2_fwnode_device_parse(dev, &props);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to parse sensor properties\n");
	ret = v4l2_ctrl_handler_init(&sensor->ctrls, 1);
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

static void bf30a2_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);

	v4l2_async_unregister_subdev(sd);
	pm_runtime_disable(&client->dev);
	if (!pm_runtime_status_suspended(&client->dev))
		bf30a2_power_off(to_bf30a2(sd));
	pm_runtime_set_suspended(&client->dev);
	v4l2_subdev_cleanup(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(sd->ctrl_handler);
}

static const struct of_device_id bf30a2_of_match[] = { { .compatible =
								 "byd,bf30a2" },
						       {} };
MODULE_DEVICE_TABLE(of, bf30a2_of_match);

static const struct dev_pm_ops bf30a2_pm_ops = { RUNTIME_PM_OPS(
	bf30a2_runtime_suspend, bf30a2_runtime_resume, NULL) };

static struct i2c_driver bf30a2_i2c_driver = {
	.driver = {
		.name = "bf30a2",
		.of_match_table = bf30a2_of_match,
		.pm = pm_ptr(&bf30a2_pm_ops),
	},
	.probe = bf30a2_probe,
	.remove = bf30a2_remove,
};
module_i2c_driver(bf30a2_i2c_driver);

MODULE_DESCRIPTION("BF30A2 camera sensor");
MODULE_LICENSE("GPL");
