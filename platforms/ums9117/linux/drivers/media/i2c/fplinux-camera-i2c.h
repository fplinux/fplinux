/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_CAMERA_I2C_H
#define FPLINUX_CAMERA_I2C_H

#include <linux/array_size.h>
#include <linux/errno.h>
#include <linux/i2c.h>
#include <linux/types.h>

static inline int fplinux_camera_i2c_write(struct i2c_client *client,
					   u8 address, u8 value)
{
	u8 data[] = { address, value };
	int ret = i2c_master_send(client, data, sizeof(data));

	if (ret < 0)
		return ret;
	return ret == sizeof(data) ? 0 : -EIO;
}

static inline int fplinux_camera_i2c_read(struct i2c_client *client, u8 address,
					  u8 *value)
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

#endif
