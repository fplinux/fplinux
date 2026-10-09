/* SPDX-License-Identifier: GPL-2.0-only */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "bq2830l-radio.h"

struct sdio_device {
	u8 system_address[4];
	u8 accepted_frame[672];
	size_t accepted_bytes;
	unsigned int address_error_at;
	int transfer_error;
};

static struct sdio_device device;

void sdio_f0_writeb(struct sdio_func *func, u8 value, unsigned int address,
		    int *error)
{
	assert(func->num == 1);
	assert(address >= 0x15c && address <= 0x15f);
	if (address == device.address_error_at) {
		*error = -EACCES;
		return;
	}
	device.system_address[address - 0x15c] = value;
	*error = 0;
}

/*
 * This device fixture requires the restarting-port operation for the frame.
 * Its rejection is fixture policy, not a statement about kernel helper errno.
 */
int sdio_memcpy_toio(struct sdio_func *func, unsigned int address, void *source,
		     int bytes)
{
	(void)func;
	(void)address;
	(void)source;
	(void)bytes;
	return -EOPNOTSUPP;
}

int sdio_memcpy_toio_fixed_port(struct sdio_func *func, unsigned int address,
				void *source, int bytes)
{
	static const u8 expected_address[] = { 0x00, 0x40, 0x00, 0x51 };

	assert(func->num == 1);
	assert(address == 0x0f);
	assert(bytes == 672);
	assert(!memcmp(device.system_address, expected_address,
		       sizeof(expected_address)));
	if (device.transfer_error)
		return device.transfer_error;
	memcpy(device.accepted_frame, source, bytes);
	device.accepted_bytes = bytes;
	return 0;
}

static void build_frame(u8 frame[672])
{
	static const u8 header[] = { 0x02, 0x01, 0x00, 0x96, 0x02 };
	size_t index;

	for (index = 0; index < 667; index++)
		frame[index] = index ^ (index >> 8);
	memcpy(frame, header, sizeof(header));
	memset(frame + 667, 0, 5);
}

static void check_complete_frame(struct sdio_func *func, const u8 frame[672])
{
	memset(&device, 0, sizeof(device));
	assert(!marlin2_sdio_bt_write_locked(func, frame, 672));
	assert(device.accepted_bytes == 672);
	assert(!memcmp(device.accepted_frame, frame, 672));
}

static void check_rejected_operations(struct sdio_func *func,
				      const u8 frame[672])
{
	memset(&device, 0, sizeof(device));
	device.address_error_at = 0x15d;
	assert(marlin2_sdio_bt_write_locked(func, frame, 672) == -EACCES);
	assert(!device.accepted_bytes);

	memset(&device, 0, sizeof(device));
	device.transfer_error = -EIO;
	assert(marlin2_sdio_bt_write_locked(func, frame, 672) == -EIO);
	assert(!device.accepted_bytes);
}

int main(void)
{
	struct sdio_func func = { .num = 1 };
	u8 frame[672];

	build_frame(frame);
	check_complete_frame(&func, frame);
	check_rejected_operations(&func, frame);
	puts("Marlin2 SDIO write owner host cases passed");
	return 0;
}
