// SPDX-License-Identifier: GPL-2.0-only
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/property.h>
#include <linux/regulator/consumer.h>

#include <media/v4l2-async.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-mediabus.h>

#define BF30A2_ID_HIGH_REG 0xfc
#define BF30A2_ID_LOW_REG 0xfd
#define BF30A2_ID_XCLK_HZ 12000000UL
#define BF30A2_MODE_XCLK_HZ 24000000UL
#define BF30A2_REG_DELAY_MS 0xffff
#define BF30A2_WIDTH 240
#define BF30A2_HEIGHT 320

struct bf30a2_reg {
	u16 address;
	u8 value;
};

struct bf30a2_variant {
	const struct bf30a2_reg *mode_regs;
	size_t num_mode_regs;
	int avdd_uv;
	u8 id_high;
	u8 id_low;
};

struct bf30a2 {
	struct v4l2_subdev sd;
	const struct bf30a2_variant *variant;
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

static const struct bf30a2_reg bf3a01_mode_regs[] = {
	{ 0xfe, 0x00 },
	{ 0x3d, 0x00 },
	{ 0x30, 0x3b },
	{ 0x31, 0x3b },
	{ 0x34, 0x01 },
	{ 0x35, 0x0a },
	{ 0xfe, 0x01 },
	{ 0x51, 0x02 },
	{ 0xe0, 0xba },
	{ 0xe1, 0x03 },
	{ 0xe2, 0x05 },
	{ 0xe3, 0x42 },
	{ 0xe4, 0x22 },
	{ 0xe5, 0x03 },
	{ 0xe7, 0x05 },
	{ 0x50, 0x10 },
	{ 0xfe, 0x00 },
	{ 0x00, 0x7b },
	{ 0x02, 0x10 },
	{ 0x15, 0x0a },
	{ 0x3c, 0x9d },
	{ 0x41, 0x02 },
	{ 0x3e, 0x68 },
	{ 0x0f, 0x13 },
	{ 0xfe, 0x00 },
	{ 0xa0, 0x54 },
	{ 0xb0, 0x19 },
	{ 0xb1, 0x2d },
	{ 0xfe, 0x01 },
	{ 0x00, 0x08 },
	{ 0x0e, 0x03 },
	{ 0x0f, 0x30 },
	{ 0x10, 0x18 },
	{ 0xfe, 0x00 },
	{ 0x84, 0x62 },
	{ 0x82, 0x06 },
	{ 0x86, 0x12 },
	{ 0xfe, 0x00 },
	{ 0x60, 0x25 },
	{ 0x61, 0x2a },
	{ 0x62, 0x28 },
	{ 0x63, 0x28 },
	{ 0x64, 0x20 },
	{ 0x65, 0x1d },
	{ 0x66, 0x17 },
	{ 0x67, 0x15 },
	{ 0x68, 0x0f },
	{ 0x69, 0x0e },
	{ 0x6a, 0x0a },
	{ 0x6b, 0x06 },
	{ 0x6c, 0x05 },
	{ 0x6d, 0x04 },
	{ 0x6e, 0x02 },
	{ 0x72, 0x08 },
	{ 0x73, 0x08 },
	{ 0x74, 0x44 },
	{ 0xfe, 0x00 },
	{ 0x03, 0x90 },
	{ 0x04, 0x01 },
	{ 0xfe, 0x00 },
	{ 0xc7, 0x21 },
	{ 0xc8, 0x19 },
	{ 0xc9, 0x84 },
	{ 0xca, 0x64 },
	{ 0xcb, 0x89 },
	{ 0xcc, 0x3f },
	{ 0xcd, 0x16 },
	{ 0xfe, 0x00 },
	{ 0xc0, 0x05 },
	{ 0xc1, 0x07 },
	{ 0xc2, 0x30 },
	{ 0xc3, 0x28 },
	{ 0xc4, 0x3c },
	{ 0xc5, 0x10 },
	{ 0xc6, 0x96 },
	{ 0xfe, 0x00 },
	{ 0xb2, 0x01 },
	{ 0xb3, 0x11 },
	{ 0xa2, 0x11 },
	{ 0xa3, 0x36 },
	{ 0xa4, 0x11 },
	{ 0xa5, 0x36 },
	{ 0xa7, 0x80 },
	{ 0xa8, 0x7f },
	{ 0xa9, 0x15 },
	{ 0xaa, 0x10 },
	{ 0xab, 0x10 },
	{ 0xac, 0x2c },
	{ 0xad, 0xf0 },
	{ 0xae, 0x20 },
	{ 0xb4, 0x18 },
	{ 0xb5, 0x1a },
	{ 0xb6, 0x1c },
	{ 0xb7, 0x30 },
	{ 0xd0, 0x4c },
	{ 0xfe, 0x01 },
	{ 0x04, 0x40 },
	{ 0x09, 0x0d },
	{ 0x0a, 0x45 },
	{ 0x0b, 0x82 },
	{ 0x0c, 0x31 },
	{ 0x0d, 0x29 },
	{ 0x15, 0x22 },
	{ 0x17, 0xb5 },
	{ 0x18, 0x28 },
	{ 0x1b, 0x28 },
	{ 0x1c, 0x54 },
	{ 0x1d, 0x3c },
	{ 0x1e, 0x5d },
	{ 0x1f, 0xa0 },
	{ 0xfe, 0x00 },
	{ 0xce, 0x3e },
	{ 0xfe, 0x01 },
	{ 0x64, 0xc8 },
	{ 0x65, 0xb8 },
	{ 0xfe, 0x01 },
	{ 0x59, 0x00 },
	{ BF30A2_REG_DELAY_MS, 0x64 },
	{ BF30A2_REG_DELAY_MS, 0x64 },
	{ BF30A2_REG_DELAY_MS, 0x64 },
	{ BF30A2_REG_DELAY_MS, 0x64 },
	{ 0xfe, 0x00 },
	{ 0x3d, 0xff },
	{ 0xa0, 0x55 },
	{ 0xfe, 0x01 },
	{ 0x00, 0x05 },
};

static const struct bf30a2_variant bf30a2_variant = {
	.mode_regs = bf30a2_mode_regs,
	.num_mode_regs = ARRAY_SIZE(bf30a2_mode_regs),
	.avdd_uv = 2800000,
	.id_high = 0x3b,
	.id_low = 0x02,
};

static const struct bf30a2_variant bf3a01_variant = {
	.mode_regs = bf3a01_mode_regs,
	.num_mode_regs = ARRAY_SIZE(bf3a01_mode_regs),
	.avdd_uv = 2800000,
	.id_high = 0x00,
	.id_low = 0x01,
};

static const u8 bf3a01_brightness_levels[] = {
	0x10, 0x20, 0x30, 0x40, 0x5a, 0x66, 0x70,
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

static int bf3a01_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct bf30a2 *sensor =
		container_of(ctrl->handler, struct bf30a2, ctrls);
	struct i2c_client *client = v4l2_get_subdevdata(&sensor->sd);
	int ret;

	if (ctrl->id != V4L2_CID_BRIGHTNESS)
		return -EINVAL;

	ret = pm_runtime_get_if_in_use(&client->dev);
	if (ret <= 0)
		return ret;

	ret = bf30a2_write(client, 0xfe, 0x01);
	if (!ret)
		ret = bf30a2_write(client, 0x04,
				   bf3a01_brightness_levels[ctrl->val]);
	if (!ret)
		msleep(100);
	pm_runtime_put(&client->dev);
	return ret;
}

static const struct v4l2_ctrl_ops bf3a01_ctrl_ops = {
	.s_ctrl = bf3a01_set_ctrl,
};

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
	const struct bf30a2_variant *variant = to_bf30a2(sd)->variant;
	size_t i;
	int ret;

	ret = pm_runtime_resume_and_get(&client->dev);
	if (ret)
		return ret;

	for (i = 0; i < variant->num_mode_regs; i++) {
		const struct bf30a2_reg *reg = &variant->mode_regs[i];

		if (reg->address == BF30A2_REG_DELAY_MS) {
			msleep(reg->value);
			continue;
		}
		ret = bf30a2_write(client, reg->address, reg->value);
		if (ret) {
			dev_err(&client->dev, "mode write failed at %zu: %pe\n",
				i, ERR_PTR(ret));
			goto power_off;
		}
	}

	if (variant == &bf3a01_variant) {
		ret = __v4l2_ctrl_handler_setup(sd->ctrl_handler);
		if (ret)
			goto power_off;
	}

	return 0;

power_off:
	pm_runtime_put_sync(&client->dev);
	return ret;
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
	sensor->variant = device_get_match_data(dev);
	if (!sensor->variant)
		return -ENODEV;
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
	ret = regulator_set_voltage(sensor->avdd, sensor->variant->avdd_uv,
				    sensor->variant->avdd_uv);
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
	if (high != sensor->variant->id_high || low != sensor->variant->id_low)
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
	ret = v4l2_ctrl_handler_init(&sensor->ctrls, 2);
	if (ret)
		return ret;
	ret = v4l2_ctrl_new_fwnode_properties(&sensor->ctrls, NULL, &props);
	if (ret)
		goto ctrls_cleanup;
	if (sensor->variant == &bf3a01_variant) {
		v4l2_ctrl_new_std(&sensor->ctrls, &bf3a01_ctrl_ops,
				  V4L2_CID_BRIGHTNESS, 0,
				  ARRAY_SIZE(bf3a01_brightness_levels) - 1, 1,
				  3);
		if (sensor->ctrls.error) {
			ret = sensor->ctrls.error;
			goto ctrls_cleanup;
		}
		/* Mode upload and live controls share the sensor register bank. */
		sensor->sd.state_lock = sensor->ctrls.lock;
	}
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

static const struct of_device_id bf30a2_of_match[] = {
	{ .compatible = "byd,bf30a2", .data = &bf30a2_variant },
	{ .compatible = "byd,bf3a01", .data = &bf3a01_variant },
	{}
};
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

MODULE_DESCRIPTION("BF30A2 and BF3A01 camera sensors");
MODULE_LICENSE("GPL");
