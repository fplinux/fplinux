// SPDX-License-Identifier: GPL-2.0-only

#include <crypto/sha2.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/firmware.h>
#include <linux/jiffies.h>
#include <linux/limits.h>
#include <linux/mmc/card.h>
#include <linux/mmc/host.h>
#include <linux/mmc/pm.h>
#include <linux/mmc/sdio_func.h>
#include <linux/mmc/sdio_ids.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pm.h>
#include <linux/property.h>
#include <linux/slab.h>

#include "bq2830l-radio.h"

#define MARLIN2_SDIO_DIRECT_PORT 0x0f
#define MARLIN2_SDIO_SYSADDR0 0x15c
#define MARLIN2_SDIO_INTERRUPT_ENABLE 0x1ca
#define MARLIN2_SDIO_PUBLIC_ENABLE 0x1c0
#define MARLIN2_SDIO_INTERRUPT_STATUS 0x1f0
#define MARLIN2_SDIO_INT_CLEAR 0x1d0
#define MARLIN2_SDIO_READY_INTERRUPT BIT(0)
#define MARLIN2_CHIP_ID_ADDRESS 0x603003fc
#define MARLIN2_BLUETOOTH_DIRECT_ADDRESS 0x51004000
#define MARLIN2_BQ_CHIP_ID 0x23490001
#define MARLIN2_SDIO_PRIORITY_ADDRESS 0x60300150
#define MARLIN2_CPU_RESET_ADDRESS 0x40060288
#define MARLIN2_WIFI_AHB_ADDRESS 0x60300004
#define MARLIN2_CALIBRATION_ADDRESS 0x70040010
#define MARLIN2_CALIBRATION_FLAG_ADDRESS 0x70040004
#define MARLIN2_CALIBRATION_ACK_ADDRESS 0x70040000
#define MARLIN2_CALIBRATION_AVAILABLE 0xcccccccc
#define MARLIN2_CALIBRATION_BEGIN 0xaaaaaaaa
#define MARLIN2_CALIBRATION_COMPLETE 0xbbbbbbbb
#define MARLIN2_IMAGE_BYTES 592896U
#define MARLIN2_CONFIG_BYTES 616U
#define MARLIN2_CALIBRATION_BYTES 3908U
#define MARLIN2_SDIO_BLOCK_BYTES 512U
#define MARLIN2_CALIBRATION_ATTEMPTS 20U
#define MARLIN2_CALIBRATION_POLL_MS 100U
#define MARLIN2_READY_TIMEOUT_MS 30000U
#define MARLIN2_READY_POLL_MS 20U

static const u8 marlin2_image_sha256[SHA256_DIGEST_SIZE] = {
	0x5f, 0xdd, 0x81, 0xda, 0x4e, 0x8e, 0xf9, 0xa5, 0x20, 0xdc, 0xb1,
	0xd6, 0x15, 0x43, 0xb0, 0xe6, 0x2c, 0xb2, 0x58, 0xbe, 0x8b, 0x7a,
	0x89, 0x66, 0xff, 0xd6, 0x48, 0xbc, 0xcc, 0x3d, 0x56, 0x90,
};

struct marlin2_interrupt_state {
	u8 direct;
	u8 public;
	bool direct_saved;
	bool public_saved;
};

struct marlin2_sdio_runtime {
	struct marlin2_interrupt_state interrupts;
	unsigned int saved_quirks;
};

static int set_address(struct sdio_func *func, u32 address)
{
	unsigned int index;
	int ret;

	for (index = 0; index < sizeof(address); index++) {
		sdio_f0_writeb(func, address >> (index * 8),
			       MARLIN2_SDIO_SYSADDR0 + index, &ret);
		if (ret)
			return ret;
	}
	return 0;
}

static int read_word(struct sdio_func *func, u32 address, u32 *value)
{
	int ret;

	ret = set_address(func, address);
	if (ret)
		return ret;
	*value = sdio_readl(func, MARLIN2_SDIO_DIRECT_PORT, &ret);
	return ret;
}

/* The transport owns its I/O mutex and holds the SDIO host claim. */
int marlin2_sdio_bt_write_locked(struct sdio_func *func, const u8 *data,
				 size_t bytes)
{
	int ret;

	if (!bytes || (bytes & 7) || bytes > INT_MAX)
		return -EINVAL;
	ret = set_address(func, MARLIN2_BLUETOOTH_DIRECT_ADDRESS);
	if (ret)
		return ret;
#if IS_ENABLED(CONFIG_BQ2830L_BLUETOOTH)
	return sdio_memcpy_toio_fixed_port(func, MARLIN2_SDIO_DIRECT_PORT,
					   (void *)data, bytes);
#else
	return sdio_memcpy_toio(func, MARLIN2_SDIO_DIRECT_PORT, (void *)data,
				bytes);
#endif
}

static int write_word(struct sdio_func *func, u32 address, u32 value)
{
	int ret;

	ret = set_address(func, address);
	if (ret)
		return ret;
	sdio_writel(func, value, MARLIN2_SDIO_DIRECT_PORT, &ret);
	return ret;
}

static int update_word(struct sdio_func *func, u32 address, u32 clear, u32 set)
{
	u32 value;
	int ret;

	ret = read_word(func, address, &value);
	if (ret)
		return ret;
	return write_word(func, address, (value & ~clear) | set);
}

static int request_boot_data(struct device *dev, const struct firmware **image,
			     const struct firmware **calibration)
{
	u8 digest[SHA256_DIGEST_SIZE];
	const char *name;
	const u8 *data;
	int ret;

	ret = device_property_read_string(dev, "firmware-name", &name);
	if (ret)
		return ret;
	ret = request_firmware_direct(image, name, dev);
	if (ret)
		return ret;
	if ((*image)->size != MARLIN2_IMAGE_BYTES)
		return -EINVAL;
	sha256((*image)->data, (*image)->size, digest);
	if (memcmp(digest, marlin2_image_sha256, sizeof(digest)))
		return -EBADMSG;
	ret = device_property_read_string(dev, "calibration-name", &name);
	if (ret)
		return ret;
	ret = request_firmware_direct(calibration, name, dev);
	if (ret)
		return ret;
	if ((*calibration)->size != MARLIN2_CALIBRATION_BYTES)
		return -EINVAL;
	data = (*calibration)->data;
	/* Only an already calibrated record for this configuration is admitted. */
	if (data[0] != 2 || data[MARLIN2_CONFIG_BYTES] != 2 ||
	    data[MARLIN2_CONFIG_BYTES + 1] != 1)
		return -EINVAL;
	return 0;
}

static int download_verified(struct sdio_func *func, u32 address,
			     const struct firmware *firmware, u8 *buffer,
			     unsigned long deadline, u32 *written)
{
	size_t offset = 0;
	size_t count;
	int ret;

	while (offset < firmware->size) {
		if (time_after_eq(jiffies, deadline))
			return -ETIMEDOUT;
		count = min_t(size_t, MARLIN2_SDIO_BLOCK_BYTES,
			      firmware->size - offset);
		memcpy(buffer, firmware->data + offset, count);
		ret = set_address(func, address + offset);
		if (ret)
			return ret;
		/* Each call is one CMD53 with the same SDIO port and INC set. */
		ret = sdio_memcpy_toio(func, MARLIN2_SDIO_DIRECT_PORT, buffer,
				       count);
		if (ret)
			return ret;
		*written += count;
		ret = set_address(func, address + offset);
		if (ret)
			return ret;
		ret = sdio_memcpy_fromio(func, buffer, MARLIN2_SDIO_DIRECT_PORT,
					 count);
		if (ret)
			return ret;
		if (memcmp(buffer, firmware->data + offset, count))
			return -EBADMSG;
		offset += count;
	}
	return 0;
}

static int wait_calibration(struct sdio_func *func,
			    struct marlin2_boot_result *result,
			    unsigned long deadline)
{
	unsigned int attempt;
	int ret;

	for (attempt = 0; attempt < MARLIN2_CALIBRATION_ATTEMPTS; attempt++) {
		if (time_after_eq(jiffies, deadline))
			return -ETIMEDOUT;
		ret = read_word(func, MARLIN2_CALIBRATION_FLAG_ADDRESS,
				&result->calibration_flag);
		if (ret)
			return ret;
		if (result->calibration_flag == MARLIN2_CALIBRATION_AVAILABLE)
			return 0;
		msleep(MARLIN2_CALIBRATION_POLL_MS);
	}
	return -ETIMEDOUT;
}

static int wait_ready(struct sdio_func *func,
		      struct marlin2_boot_result *result,
		      unsigned long deadline)
{
	unsigned long ready_deadline =
		jiffies + msecs_to_jiffies(MARLIN2_READY_TIMEOUT_MS);
	u8 pending;
	u8 clear;
	int ret;

	for (;;) {
		if (time_after_eq(jiffies, deadline) ||
		    time_after_eq(jiffies, ready_deadline))
			return -ETIMEDOUT;
		pending = sdio_f0_readb(func, MARLIN2_SDIO_INTERRUPT_STATUS,
					&ret);
		if (ret)
			return ret;
		result->interrupt_status |= pending;
		if (pending) {
			clear = sdio_f0_readb(func, MARLIN2_SDIO_INT_CLEAR,
					      &ret);
			if (ret)
				return ret;
			sdio_f0_writeb(func, clear | pending,
				       MARLIN2_SDIO_INT_CLEAR, &ret);
			if (ret)
				return ret;
		}
		if (pending & MARLIN2_SDIO_READY_INTERRUPT)
			return 0;
		msleep(MARLIN2_READY_POLL_MS);
	}
}

static int boot_firmware(struct sdio_func *func, const struct firmware *image,
			 const struct firmware *calibration, u8 *buffer,
			 struct marlin2_boot_result *result,
			 struct marlin2_interrupt_state *interrupts,
			 unsigned long deadline)
{
	u32 calibration_written = 0;
	u8 public_enabled;
	int ret;

	result->phase = MARLIN2_BOOT_IMAGE_DOWNLOAD;
	ret = download_verified(func, 0, image, buffer, deadline,
				&result->image_bytes);
	if (ret)
		return ret;
	result->image_verified = true;
	result->phase = MARLIN2_BOOT_CPU_START;
	ret = update_word(func, MARLIN2_SDIO_PRIORITY_ADDRESS, 0, BIT(31));
	if (ret)
		return ret;
	ret = update_word(func, MARLIN2_CPU_RESET_ADDRESS, BIT(0), 0);
	if (ret)
		return ret;
	result->phase = MARLIN2_BOOT_CALIBRATION_DOWNLOAD;
	ret = update_word(func, MARLIN2_WIFI_AHB_ADDRESS, 0, BIT(5));
	if (ret)
		return ret;
	ret = download_verified(func, MARLIN2_CALIBRATION_ADDRESS, calibration,
				buffer, deadline, &calibration_written);
	if (ret)
		return ret;
	result->calibration_verified = true;
	result->phase = MARLIN2_BOOT_CALIBRATION_WAIT;
	ret = wait_calibration(func, result, deadline);
	if (ret)
		return ret;
	result->phase = MARLIN2_BOOT_CALIBRATION_ACK;
	ret = write_word(func, MARLIN2_CALIBRATION_ACK_ADDRESS,
			 MARLIN2_CALIBRATION_BEGIN);
	if (ret)
		return ret;
	result->phase = MARLIN2_BOOT_READY_WAIT;
	interrupts->public =
		sdio_f0_readb(func, MARLIN2_SDIO_PUBLIC_ENABLE, &ret);
	if (ret)
		return ret;
	interrupts->public_saved = true;
	sdio_f0_writeb(func, interrupts->public | BIT(0) | BIT(1) | BIT(2),
		       MARLIN2_SDIO_PUBLIC_ENABLE, &ret);
	if (ret)
		return ret;
	public_enabled = sdio_f0_readb(func, MARLIN2_SDIO_PUBLIC_ENABLE, &ret);
	if (ret)
		return ret;
	if ((public_enabled & 7) != 7)
		return -EIO;
	ret = wait_ready(func, result, deadline);
	if (ret)
		return ret;
	result->phase = MARLIN2_BOOT_READY_ACK;
	ret = write_word(func, MARLIN2_CALIBRATION_ACK_ADDRESS,
			 MARLIN2_CALIBRATION_COMPLETE);
	if (ret)
		return ret;
	ret = read_word(func, MARLIN2_CALIBRATION_ACK_ADDRESS,
			&result->ready_flag);
	if (ret)
		return ret;
	if (result->ready_flag != MARLIN2_CALIBRATION_COMPLETE)
		return -EIO;
	result->ready = true;
	result->phase = MARLIN2_BOOT_COMPLETE;
	return 0;
}

static int marlin2_sdio_probe(struct sdio_func *func,
			      const struct sdio_device_id *id)
{
	struct marlin2_boot_result result = {
		.phase = MARLIN2_BOOT_FIRMWARE_REQUEST,
	};
	struct marlin2_interrupt_state interrupts = {};
	struct marlin2_sdio_runtime *runtime = NULL;
	bool persistent = bq2830l_radio_runtime_requested(func);
	const struct firmware *calibration = NULL;
	const struct firmware *image = NULL;
	struct mmc_host *host = func->card->host;
	unsigned long deadline;
	unsigned int saved_quirks;
	u8 direct_enabled;
	u8 *buffer = NULL;
	int cleanup_ret;
	int ret;

	if (func->num != 1 ||
	    !of_device_is_compatible(func->dev.of_node, "sprd,marlin2-sdio"))
		return -ENODEV;
	deadline = bq2830l_radio_deadline(func);
	if (persistent) {
		runtime =
			devm_kzalloc(&func->dev, sizeof(*runtime), GFP_KERNEL);
		if (!runtime) {
			ret = -ENOMEM;
			goto out_report;
		}
	}
	ret = request_boot_data(&func->dev, &image, &calibration);
	if (ret)
		goto out_report;
	if (func->max_blksize < MARLIN2_SDIO_BLOCK_BYTES ||
	    host->max_blk_size < MARLIN2_SDIO_BLOCK_BYTES ||
	    host->max_req_size < MARLIN2_SDIO_BLOCK_BYTES ||
	    host->max_seg_size < MARLIN2_SDIO_BLOCK_BYTES ||
	    !host->max_blk_count) {
		ret = -EPROTONOSUPPORT;
		goto out_report;
	}
	buffer = kmalloc(MARLIN2_SDIO_BLOCK_BYTES, GFP_KERNEL);
	if (!buffer) {
		ret = -ENOMEM;
		goto out_report;
	}
	sdio_claim_host(func);
	saved_quirks = func->card->quirks;
	/* The system-address window is outside the usual function-0 range. */
	func->card->quirks |= MMC_QUIRK_LENIENT_FN0;
	ret = sdio_enable_func(func);
	if (ret)
		goto out_disable;
	ret = sdio_set_block_size(func, MARLIN2_SDIO_BLOCK_BYTES);
	if (ret)
		goto out_disable;
	result.phase = MARLIN2_BOOT_CHIP_ID;
	ret = read_word(func, MARLIN2_CHIP_ID_ADDRESS, &result.chip_id[0]);
	if (ret)
		goto out_disable;
	ret = read_word(func, MARLIN2_CHIP_ID_ADDRESS, &result.chip_id[1]);
	if (ret)
		goto out_disable;
	if (result.chip_id[0] != result.chip_id[1] ||
	    result.chip_id[0] != MARLIN2_BQ_CHIP_ID) {
		ret = -ENODEV;
		goto out_disable;
	}
	interrupts.direct =
		sdio_f0_readb(func, MARLIN2_SDIO_INTERRUPT_ENABLE, &ret);
	if (ret)
		goto out_disable;
	interrupts.direct_saved = true;
	sdio_f0_writeb(func, interrupts.direct | BIT(0) | BIT(1),
		       MARLIN2_SDIO_INTERRUPT_ENABLE, &ret);
	if (ret)
		goto out_disable;
	direct_enabled =
		sdio_f0_readb(func, MARLIN2_SDIO_INTERRUPT_ENABLE, &ret);
	if (ret)
		goto out_disable;
	if ((direct_enabled & 3) != 3) {
		ret = -EIO;
		goto out_disable;
	}
	ret = boot_firmware(func, image, calibration, buffer, &result,
			    &interrupts, deadline);
	if (!ret && !persistent) {
		result.phase = MARLIN2_BOOT_GET_INFO;
		result.packet_attempted = true;
		/* No sleep permission is issued between ready ACK and this request. */
		result.packet_error = marlin2_packet_get_info_locked(
			func, bq2830l_radio_rx_request(func),
			jiffies_to_msecs(jiffies), deadline, &result.packet);
		result.phase = MARLIN2_BOOT_COMPLETE;
	}
	if (!ret && persistent) {
		runtime->interrupts = interrupts;
		runtime->saved_quirks = saved_quirks;
		sdio_set_drvdata(func, runtime);
		sdio_release_host(func);
		goto out_report;
	}

out_disable:
	if (interrupts.public_saved) {
		sdio_f0_writeb(func, interrupts.public,
			       MARLIN2_SDIO_PUBLIC_ENABLE, &cleanup_ret);
		if (cleanup_ret) {
			result.cleanup_error = cleanup_ret;
			dev_err(&func->dev,
				"failed to restore public interrupts: %pe\n",
				ERR_PTR(cleanup_ret));
			if (!ret)
				ret = cleanup_ret;
		}
	}
	if (interrupts.direct_saved) {
		sdio_f0_writeb(func, interrupts.direct,
			       MARLIN2_SDIO_INTERRUPT_ENABLE, &cleanup_ret);
		if (cleanup_ret) {
			if (!result.cleanup_error)
				result.cleanup_error = cleanup_ret;
			dev_err(&func->dev,
				"failed to restore direct interrupts: %pe\n",
				ERR_PTR(cleanup_ret));
			if (!ret)
				ret = cleanup_ret;
		}
	}
	cleanup_ret = sdio_disable_func(func);
	if (cleanup_ret) {
		if (!result.cleanup_error)
			result.cleanup_error = cleanup_ret;
		dev_err(&func->dev, "failed to disable the function: %pe\n",
			ERR_PTR(cleanup_ret));
		if (!ret)
			ret = cleanup_ret;
	}
	func->card->quirks = saved_quirks;
	sdio_release_host(func);
out_report:
	kfree(buffer);
	release_firmware(calibration);
	release_firmware(image);
	bq2830l_radio_report(func, ret, &result);
	if (ret)
		return dev_err_probe(&func->dev, ret,
				     "failed to boot radio firmware\n");
	return 0;
}

static void marlin2_sdio_remove(struct sdio_func *func)
{
	struct marlin2_sdio_runtime *runtime = sdio_get_drvdata(func);
	int ret;

	if (!runtime)
		return;
	/* The board first stops the network IRQ and FIFO work, then removes us. */
	sdio_claim_host(func);
	if (runtime->interrupts.public_saved) {
		sdio_f0_writeb(func, runtime->interrupts.public,
			       MARLIN2_SDIO_PUBLIC_ENABLE, &ret);
		bq2830l_radio_runtime_cleanup_error(func, ret);
		if (ret)
			dev_err(&func->dev,
				"failed to restore public interrupts: %pe\n",
				ERR_PTR(ret));
	}
	if (runtime->interrupts.direct_saved) {
		sdio_f0_writeb(func, runtime->interrupts.direct,
			       MARLIN2_SDIO_INTERRUPT_ENABLE, &ret);
		bq2830l_radio_runtime_cleanup_error(func, ret);
		if (ret)
			dev_err(&func->dev,
				"failed to restore direct interrupts: %pe\n",
				ERR_PTR(ret));
	}
	ret = sdio_disable_func(func);
	bq2830l_radio_runtime_cleanup_error(func, ret);
	if (ret)
		dev_err(&func->dev, "failed to disable radio function: %pe\n",
			ERR_PTR(ret));
	func->card->quirks = runtime->saved_quirks;
	sdio_release_host(func);
	sdio_set_drvdata(func, NULL);
}

static int marlin2_sdio_suspend(struct device *dev)
{
	struct sdio_func *func = dev_to_sdio_func(dev);

	if (!sdio_get_drvdata(func))
		return 0;
	/* The board retains the firmware until the last consumer closes. */
	return sdio_set_host_pm_flags(func, MMC_PM_KEEP_POWER);
}

static int marlin2_sdio_resume(struct device *dev)
{
	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(marlin2_sdio_pm_ops, marlin2_sdio_suspend,
				marlin2_sdio_resume);

static const struct sdio_device_id marlin2_sdio_ids[] = {
	{ SDIO_DEVICE(SDIO_ANY_ID, SDIO_ANY_ID) },
	{},
};
MODULE_DEVICE_TABLE(sdio, marlin2_sdio_ids);

struct sdio_driver marlin2_sdio_driver = {
	.name = "marlin2-sdio",
	.id_table = marlin2_sdio_ids,
	.probe = marlin2_sdio_probe,
	.remove = marlin2_sdio_remove,
	.drv = {
		.pm = pm_sleep_ptr(&marlin2_sdio_pm_ops),
	},
};

MODULE_DESCRIPTION("Marlin2 SDIO RAM firmware boot");
MODULE_LICENSE("GPL");
