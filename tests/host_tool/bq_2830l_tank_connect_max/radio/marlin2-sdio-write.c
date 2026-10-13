/* SPDX-License-Identifier: GPL-2.0-only */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bq2830l-radio.h"

struct sdio_device {
	u8 system_address[4];
	u8 accepted_frame[672];
	size_t accepted_bytes;
	unsigned int address_error_at;
	int transfer_error;
	bool booting;
	bool persistent;
	bool claimed;
	bool enabled;
	u8 direct_interrupts;
	u8 public_interrupts;
	u32 calibration_ack;
	u8 downloaded[512];
	unsigned int downloaded_address;
	int downloaded_bytes;
	int enable_error;
	int ready_error;
	int public_restore_error;
	int direct_restore_error;
	int disable_error;
	unsigned int public_restores;
	unsigned int direct_restores;
	unsigned int disables;
	int reported_error;
	int runtime_cleanup_error;
	struct marlin2_boot_result reported_boot;
	void *runtime_allocation;
};

static struct sdio_device device;
unsigned long jiffies;

static u32 selected_address(void)
{
	return (u32)device.system_address[0] |
	       (u32)device.system_address[1] << 8 |
	       (u32)device.system_address[2] << 16 |
	       (u32)device.system_address[3] << 24;
}

void sdio_f0_writeb(struct sdio_func *func, u8 value, unsigned int address,
		    int *error)
{
	assert(func->num == 1);
	if (device.booting) {
		assert(device.claimed);
		if (address == 0x1c0) {
			if (value == 0x80) {
				device.public_restores++;
				if (device.public_restore_error) {
					*error = device.public_restore_error;
					return;
				}
			}
			device.public_interrupts = value;
			*error = 0;
			return;
		}
		if (address == 0x1ca) {
			if (value == 0x40) {
				device.direct_restores++;
				if (device.direct_restore_error) {
					*error = device.direct_restore_error;
					return;
				}
			}
			device.direct_interrupts = value;
			*error = 0;
			return;
		}
		if (address == 0x1d0) {
			*error = 0;
			return;
		}
	}
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
	if (!device.booting)
		return -EOPNOTSUPP;
	assert(func->num == 1);
	assert(device.claimed && device.enabled);
	assert(address == 0x0f);
	assert(bytes > 0 && bytes <= 512);
	if (device.transfer_error)
		return device.transfer_error;
	memcpy(device.downloaded, source, bytes);
	device.downloaded_bytes = bytes;
	device.downloaded_address = selected_address();
	return 0;
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

/*
 * Cleanup cases link the complete driver. Firmware, hashing, time, the board
 * owner and the SDIO device are controlled doubles; no firmware executes.
 */
void msleep(unsigned int milliseconds)
{
	jiffies += milliseconds;
}

void *kmalloc(size_t bytes, int flags)
{
	return malloc(bytes);
}

void kfree(void *pointer)
{
	free(pointer);
}

void *devm_kzalloc(struct device *dev, size_t bytes, int flags)
{
	assert(!device.runtime_allocation);
	device.runtime_allocation = calloc(1, bytes);
	return device.runtime_allocation;
}

int device_property_read_string(struct device *dev, const char *property,
				const char **value)
{
	if (!strcmp(property, "firmware-name"))
		*value = "image";
	else if (!strcmp(property, "calibration-name"))
		*value = "calibration";
	else
		return -EINVAL;
	return 0;
}

int request_firmware_direct(const struct firmware **firmware, const char *name,
			    struct device *dev)
{
	static const u8 image_data[592896];
	static const u8 calibration_data[3908] = {
		[0] = 2,
		[616] = 2,
		[617] = 1,
	};
	static const struct firmware image = {
		.data = image_data,
		.size = sizeof(image_data),
	};
	static const struct firmware calibration = {
		.data = calibration_data,
		.size = sizeof(calibration_data),
	};

	if (!strcmp(name, "image"))
		*firmware = &image;
	else if (!strcmp(name, "calibration"))
		*firmware = &calibration;
	else
		return -ENOENT;
	return 0;
}

void release_firmware(const struct firmware *firmware)
{
}

void sha256(const u8 *data, size_t bytes, u8 *digest)
{
	static const u8 accepted_digest[32] = {
		0x5f, 0xdd, 0x81, 0xda, 0x4e, 0x8e, 0xf9, 0xa5,
		0x20, 0xdc, 0xb1, 0xd6, 0x15, 0x43, 0xb0, 0xe6,
		0x2c, 0xb2, 0x58, 0xbe, 0x8b, 0x7a, 0x89, 0x66,
		0xff, 0xd6, 0x48, 0xbc, 0xcc, 0x3d, 0x56, 0x90,
	};

	assert(bytes == 592896);
	memcpy(digest, accepted_digest, sizeof(accepted_digest));
}

bool of_device_is_compatible(void *node, const char *name)
{
	return !strcmp(name, "sprd,marlin2-sdio");
}

int dev_err_probe(struct device *dev, int error, const char *format, ...)
{
	return error;
}

void sdio_set_drvdata(struct sdio_func *func, void *data)
{
	func->dev.driver_data = data;
}

void *sdio_get_drvdata(struct sdio_func *func)
{
	return func->dev.driver_data;
}

int sdio_set_host_pm_flags(struct sdio_func *func, unsigned int flags)
{
	return -EOPNOTSUPP;
}

void sdio_claim_host(struct sdio_func *func)
{
	assert(!device.claimed);
	device.claimed = true;
}

void sdio_release_host(struct sdio_func *func)
{
	assert(device.claimed);
	device.claimed = false;
}

int sdio_enable_func(struct sdio_func *func)
{
	assert(device.claimed);
	if (device.enable_error)
		return device.enable_error;
	device.enabled = true;
	return 0;
}

int sdio_disable_func(struct sdio_func *func)
{
	assert(device.claimed);
	device.disables++;
	if (device.disable_error)
		return device.disable_error;
	device.enabled = false;
	return 0;
}

int sdio_set_block_size(struct sdio_func *func, unsigned int bytes)
{
	assert(device.claimed && device.enabled);
	func->cur_blksize = bytes;
	return 0;
}

int sdio_memcpy_fromio(struct sdio_func *func, void *destination,
		       unsigned int address, int bytes)
{
	assert(device.claimed && device.enabled);
	assert(address == 0x0f);
	assert(bytes == device.downloaded_bytes);
	assert(selected_address() == device.downloaded_address);
	memcpy(destination, device.downloaded, bytes);
	return 0;
}

u32 sdio_readl(struct sdio_func *func, unsigned int address, int *error)
{
	assert(device.claimed && device.enabled);
	assert(address == 0x0f);
	*error = 0;
	switch (selected_address()) {
	case 0x603003fc:
		return 0x23490001;
	case 0x60300150:
	case 0x40060288:
	case 0x60300004:
		return 0;
	case 0x70040004:
		return 0xcccccccc;
	case 0x70040000:
		return device.calibration_ack;
	default:
		abort();
	}
}

void sdio_writel(struct sdio_func *func, u32 value, unsigned int address,
		 int *error)
{
	assert(device.claimed && device.enabled);
	assert(address == 0x0f);
	if (selected_address() == 0x70040000)
		device.calibration_ack = value;
	*error = 0;
}

u8 sdio_f0_readb(struct sdio_func *func, unsigned int address, int *error)
{
	assert(device.claimed && device.enabled);
	*error = 0;
	switch (address) {
	case 0x1ca:
		return device.direct_interrupts;
	case 0x1c0:
		return device.public_interrupts;
	case 0x1f0:
		*error = device.ready_error;
		return 1;
	case 0x1d0:
		return 0;
	default:
		abort();
	}
}

bool bq2830l_radio_runtime_requested(struct sdio_func *func)
{
	return device.persistent;
}

unsigned long bq2830l_radio_deadline(struct sdio_func *func)
{
	return 60000;
}

struct gpio_desc *bq2830l_radio_rx_request(struct sdio_func *func)
{
	return NULL;
}

void bq2830l_radio_report(struct sdio_func *func, int error,
			  const struct marlin2_boot_result *result)
{
	assert(!device.claimed);
	device.reported_error = error;
	device.reported_boot = *result;
}

void bq2830l_radio_runtime_cleanup_error(struct sdio_func *func, int error)
{
	assert(device.claimed);
	assert(sdio_get_drvdata(func));
	if (!device.runtime_cleanup_error)
		device.runtime_cleanup_error = error;
}

int marlin2_packet_get_info_locked(struct sdio_func *func,
				   struct gpio_desc *request, u32 token,
				   unsigned long deadline,
				   struct marlin2_packet_result *result)
{
	assert(device.claimed && device.enabled);
	return -EOPNOTSUPP;
}

static void prepare_boot(struct sdio_func *func)
{
	free(device.runtime_allocation);
	memset(&device, 0, sizeof(device));
	device.booting = true;
	device.direct_interrupts = 0x40;
	device.public_interrupts = 0x80;
	func->card->quirks = 0x20;
	sdio_set_drvdata(func, NULL);
	jiffies = 0;
}

static void check_probe_restoration(struct sdio_func *func)
{
	prepare_boot(func);
	device.enable_error = -EBUSY;
	device.disable_error = -EIO;
	assert(marlin2_sdio_driver.probe(func, NULL) == -EBUSY);
	assert(device.reported_error == -EBUSY);
	assert(device.reported_boot.cleanup_error == -EIO);
	assert(!device.public_restores && !device.direct_restores);
	assert(device.disables == 1);
	assert(func->card->quirks == 0x20 && !device.claimed);

	prepare_boot(func);
	device.transfer_error = -EIO;
	device.direct_restore_error = -EACCES;
	device.disable_error = -EPERM;
	assert(marlin2_sdio_driver.probe(func, NULL) == -EIO);
	assert(device.reported_error == -EIO);
	assert(device.reported_boot.cleanup_error == -EACCES);
	assert(!device.public_restores && device.direct_restores == 1);
	assert(device.public_interrupts == 0x80);
	assert(device.direct_interrupts == 0x43);
	assert(device.disables == 1);
	assert(func->card->quirks == 0x20 && !device.claimed);

	prepare_boot(func);
	device.ready_error = -ETIMEDOUT;
	device.public_restore_error = -EACCES;
	device.direct_restore_error = -ERANGE;
	device.disable_error = -EPERM;
	assert(marlin2_sdio_driver.probe(func, NULL) == -ETIMEDOUT);
	assert(device.reported_error == -ETIMEDOUT);
	assert(device.reported_boot.cleanup_error == -EACCES);
	assert(device.public_restores == 1 && device.direct_restores == 1);
	assert(device.disables == 1);
	assert(func->card->quirks == 0x20 && !device.claimed);
}

static void check_completed_probe_cleanup_error(struct sdio_func *func)
{
	prepare_boot(func);
	device.direct_restore_error = -ERANGE;
	device.disable_error = -EIO;
	assert(marlin2_sdio_driver.probe(func, NULL) == -ERANGE);
	assert(device.reported_error == -ERANGE);
	assert(device.reported_boot.ready);
	assert(device.reported_boot.cleanup_error == -ERANGE);
	assert(device.public_interrupts == 0x80);
	assert(device.public_restores == 1 && device.direct_restores == 1);
	assert(device.disables == 1);
	assert(func->card->quirks == 0x20 && !device.claimed);

	prepare_boot(func);
	assert(!marlin2_sdio_driver.probe(func, NULL));
	assert(!device.reported_error && !device.reported_boot.cleanup_error);
	assert(device.public_interrupts == 0x80);
	assert(device.direct_interrupts == 0x40);
	assert(!device.enabled && device.disables == 1);
	assert(func->card->quirks == 0x20 && !device.claimed);
}

static void check_runtime_remove(struct sdio_func *func)
{
	static const struct {
		int public_error;
		int direct_error;
		int disable_error;
		int first_error;
	} cases[] = {
		{ -EACCES, -ERANGE, -EPERM, -EACCES },
		{ 0, -ERANGE, -EPERM, -ERANGE },
		{ 0, 0, -EPERM, -EPERM },
		{ 0, 0, 0, 0 },
	};
	size_t index;

	for (index = 0; index < sizeof(cases) / sizeof(cases[0]); index++) {
		prepare_boot(func);
		device.persistent = true;
		assert(!marlin2_sdio_driver.probe(func, NULL));
		assert(sdio_get_drvdata(func));
		assert(device.enabled && !device.disables);
		assert(device.public_interrupts == 0x87);
		assert(device.direct_interrupts == 0x43);
		assert(func->card->quirks == 0x21 && !device.claimed);
		device.public_restore_error = cases[index].public_error;
		device.direct_restore_error = cases[index].direct_error;
		device.disable_error = cases[index].disable_error;
		marlin2_sdio_driver.remove(func);
		assert(device.runtime_cleanup_error ==
		       cases[index].first_error);
		assert(device.public_restores == 1 &&
		       device.direct_restores == 1);
		assert(device.disables == 1);
		if (!cases[index].public_error)
			assert(device.public_interrupts == 0x80);
		if (!cases[index].direct_error)
			assert(device.direct_interrupts == 0x40);
		if (!cases[index].disable_error)
			assert(!device.enabled);
		assert(func->card->quirks == 0x20 && !device.claimed);
		assert(!sdio_get_drvdata(func));
		marlin2_sdio_driver.remove(func);
		assert(device.disables == 1);
	}
	free(device.runtime_allocation);
	device.runtime_allocation = NULL;
}

int main(int argc, char **argv)
{
	struct mmc_host host = {
		.max_req_size = 512,
		.max_seg_size = 512,
		.max_blk_count = 1,
		.max_blk_size = 512,
	};
	struct mmc_card card = { .host = &host };
	struct sdio_func func = { .card = &card, .num = 1, .max_blksize = 512 };
	u8 frame[672];

	if (argc == 2 && !strcmp(argv[1], "cleanup")) {
		check_probe_restoration(&func);
		check_completed_probe_cleanup_error(&func);
		check_runtime_remove(&func);
		puts("Marlin2 SDIO cleanup host cases passed");
		return 0;
	}
	if (argc != 1)
		return 2;
	build_frame(frame);
	check_complete_frame(&func, frame);
	check_rejected_operations(&func, frame);
	puts("Marlin2 SDIO write owner host cases passed");
	return 0;
}
