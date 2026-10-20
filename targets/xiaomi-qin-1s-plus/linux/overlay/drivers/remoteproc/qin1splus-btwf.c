// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (C) 2017 Spreadtrum Communications Inc. */

#include <linux/bitops.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/nvmem-consumer.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/remoteproc.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/uaccess.h>
#include <linux/unaligned.h>

#include <linux/soc/sprd/qin1splus-sipc.h>

#define QIN_BTWF_BASE_PHYS 0x88000000
#define QIN_BTWF_RAM_BYTES 0x180000
#define QIN_BTWF_RESERVED_BYTES 0x280000
#define QIN_BTWF_IMAGE_BYTES 0x178000
#define QIN_BTWF_CONFIG_OFFSET 0x17c000
#define QIN_BTWF_CONFIG_BYTES 616
#define QIN_BTWF_CALI_OFFSET 0x17c268
#define QIN_BTWF_CALI_BYTES 3292
#define QIN_BTWF_EFUSE_OFFSET 0x17cf44
#define QIN_BTWF_CALI_FLAG_OFFSET 0x17cf50
#define QIN_BTWF_INIT_OFFSET 0x17cf54
#define QIN_BTWF_NOGNSS_OFFSET 0x17cf5c
#define QIN_BTWF_SLEEP_STATUS_OFFSET 0x17cf64
#define QIN_BTWF_SLEEP_FLAG_OFFSET 0x17cf68
#define QIN_BTWF_TEMP_MAGIC_OFFSET 0x17cf6c
#define QIN_BTWF_TEMP_VALUE_OFFSET 0x17cf70

#define QIN_BTWF_INIT_START 0x5a5a5a5a
#define QIN_BTWF_INIT_READY 0xababbaba
#define QIN_BTWF_INIT_SUCCESS 0x13579bdf
#define QIN_BTWF_INIT_FAILED 0x88888888
#define QIN_BTWF_CALI_START 0xefeffefe
#define QIN_BTWF_CALI_DONE 0x12345678
#define QIN_BTWF_NOGNSS 0x000ab520
#define QIN_BTWF_TEMP_VALID 0x432ff678
#define QIN_BTWF_TEMP_INVALID 0x432ff679
#define QIN_BTWF_READY_ATTEMPTS 256
#define QIN_BTWF_READY_POLL_MS 20

#define SC9820E_AON_CHIP_ID0 0xe0
#define SC9820E_AON_CHIP_ID1 0xe4
#define SC9820E_AON_PLATFORM_ID0 0xe8
#define SC9820E_AON_PLATFORM_ID1 0xec
#define SC9820E_AON_CHIP_ID 0xfc
#define SC9820E_AON_WCN_REMAP 0x578
#define SC9820E_AON_WCN_REMAP_MASK GENMASK(25, 24)
#define SC9820E_AON_WCN_ENABLE 0x57c
#define SC9820E_AON_BTWF_ENABLE BIT(7)
#define SC9820E_AON_GNSS_ENABLE BIT(6)
#define SC9820E_PMU_WCN_DOMAIN 0x100
#define SC9820E_PMU_WCN_AUTO_SHUTDOWN BIT(24)
#define SC9820E_PMU_WCN_FORCE_SHUTDOWN BIT(25)
#define SC9820E_PMU_SLEEP_CTRL 0xcc
#define SC9820E_PMU_WCN_FORCE_SLEEP BIT(19)
#define SC9820E_PMU_RESET 0xb0
#define SC9820E_PMU_WCN_RESET BIT(9)
#define SC9820E_PMU_BTWF_RESET BIT(10)
#define SC9820E_PMU_WCN_DEEP_SLEEP 0x244
#define SC9820E_G6_WCN_26M 0x28
#define SC9820E_G6_WCN_26M_DISABLE BIT(2)
#define SC9820E_PUB_DMC_DUMMY 0x30a0
#define SC9820E_PUB_DMC_DUMMY_MASK GENMASK(15, 0)
#define SC9820E_ALIAS_SET 0x1000
#define SC9820E_ALIAS_CLEAR 0x2000

struct qin_btwf {
	struct device *dev;
	struct rproc *rproc;
	struct regmap *aon;
	struct regmap *pmu;
	struct regmap *pub;
	struct regmap *g6;
	struct regulator *vddcon;
	struct regulator *vddwifipa;
	struct nvmem_cell *wifi_efuse;
	struct nvmem_cell *temp_efuse;
	void __iomem *ram;
	u32 ids[5];
	u32 efuse[4];
	u32 saved_pub;
	u32 last_cp_marker;
	u32 revision;
	int last_error;
	bool claimed;
	bool con_enabled;
	bool pa_enabled;
	bool clock_enabled;
	bool ready_observed;
	bool snapshot_valid;
	bool calibrated;
	bool sleep_initialized;
	struct qin_sipc *sipc;
	u8 calibration[QIN_BTWF_CALI_BYTES];
};

static int alias_write(struct regmap *map, u32 reg, u32 alias, u32 bits)
{
	/* SET/CLEAR aliases accept a bit mask, never a read/modify/write. */
	return regmap_write(map, reg + alias, bits);
}

static int boot_control_write(struct regmap *map, u32 reg, u32 bits)
{
	int ret;

	ret = regmap_write(map, reg, bits);
	if (!ret)
		usleep_range(10, 50);
	return ret;
}

static int read_identity(struct qin_btwf *btwf)
{
	static const u32 regs[] = {
		SC9820E_AON_CHIP_ID0,	  SC9820E_AON_CHIP_ID1,
		SC9820E_AON_PLATFORM_ID0, SC9820E_AON_PLATFORM_ID1,
		SC9820E_AON_CHIP_ID,
	};
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(regs); i++) {
		ret = regmap_read(btwf->aon, regs[i], &btwf->ids[i]);
		if (ret)
			return ret;
	}
	if (btwf->ids[0] != 0x6b4c4500 || btwf->ids[1] != 0x53686172)
		return -ENODEV;

	switch (btwf->ids[4]) {
	case 0x96360000:
		btwf->revision = 1;
		break;
	case 0x96360002:
		btwf->revision = 2;
		break;
	case 0x96360003:
		btwf->revision = 3;
		break;
	default:
		return -ENODEV;
	}
	return 0;
}

static int read_efuse_cell(struct nvmem_cell *cell, u32 *words, size_t count)
{
	size_t bytes, i;
	u8 *data;

	data = nvmem_cell_read(cell, &bytes);
	if (IS_ERR(data))
		return PTR_ERR(data);
	if (bytes != count * sizeof(u32)) {
		kfree(data);
		return -EINVAL;
	}
	for (i = 0; i < count; i++)
		words[i] = get_unaligned_le32(data + i * sizeof(u32));
	kfree(data);
	return 0;
}

static int request_exact(struct qin_btwf *btwf, const char *name, size_t bytes,
			 const struct firmware **fw)
{
	int ret;

	ret = request_firmware_direct(fw, name, btwf->dev);
	if (ret)
		return ret;
	if ((*fw)->size != bytes) {
		dev_err(btwf->dev, "%s has %zu bytes, expected %zu\n", name,
			(*fw)->size, bytes);
		release_firmware(*fw);
		*fw = NULL;
		return -EINVAL;
	}
	return 0;
}

static int check_inactive(struct qin_btwf *btwf)
{
	u32 val;
	int ret;

	ret = regmap_read(btwf->aon, SC9820E_AON_WCN_ENABLE, &val);
	if (ret)
		return ret;
	if (val & (SC9820E_AON_BTWF_ENABLE | SC9820E_AON_GNSS_ENABLE)) {
		dev_err(btwf->dev, "existing WCN enable state %08x\n", val);
		return -EBUSY;
	}
	ret = regmap_read(btwf->aon, SC9820E_AON_WCN_REMAP, &val);
	if (ret)
		return ret;
	/* The BTWF zero SET-alias write cannot clear an inherited remap. */
	if (val & SC9820E_AON_WCN_REMAP_MASK) {
		dev_err(btwf->dev, "unsupported WCN remap state %08x\n", val);
		return -EBUSY;
	}
	return 0;
}

static int qin_btwf_sanity(struct rproc *rproc, const struct firmware *fw)
{
	struct qin_btwf *btwf = rproc->priv;

	btwf->ready_observed = false;
	btwf->last_cp_marker = 0;
	btwf->last_error = fw->size == QIN_BTWF_IMAGE_BYTES ? 0 : -EINVAL;
	return btwf->last_error;
}

static int verify_image(struct qin_btwf *btwf, const struct firmware *fw)
{
	u8 bytes[256];
	size_t offset, count, i;

	/* Order the firmware copy before later shared-RAM stores. */
	wmb();
	for (offset = 0; offset < fw->size; offset += count) {
		count = min_t(size_t, sizeof(bytes), fw->size - offset);
		memcpy_fromio(bytes, btwf->ram + offset, count);
		for (i = 0; i < count; i++) {
			if (bytes[i] == fw->data[offset + i])
				continue;
			dev_err(btwf->dev,
				"firmware RAM mismatch at %zx: expected %02x read %02x\n",
				offset + i, fw->data[offset + i], bytes[i]);
			return -EIO;
		}
	}
	dev_info(btwf->dev, "firmware RAM readback matches all %zu bytes\n",
		 fw->size);
	return 0;
}

static int qin_btwf_load(struct rproc *rproc, const struct firmware *fw)
{
	struct qin_btwf *btwf = rproc->priv;
	const struct firmware *config = NULL, *cali = NULL;
	const char *config_name, *cali_name;
	int i, ret;

	if (btwf->claimed) {
		ret = -EBUSY;
		goto out;
	}
	ret = read_identity(btwf);
	if (ret)
		goto out;
	ret = check_inactive(btwf);
	if (ret)
		goto out;
	ret = read_efuse_cell(btwf->wifi_efuse, btwf->efuse, 3);
	if (ret)
		goto out;
	ret = read_efuse_cell(btwf->temp_efuse, &btwf->efuse[3], 1);
	if (ret)
		goto out;

	config_name = btwf->revision == 3 ? "qin1splus/wcn-config.bin" :
					    "qin1splus/system-config.bin";
	cali_name = btwf->revision == 3 ? "qin1splus/wcn-cali.bin" :
					  "qin1splus/system-cali.bin";
	ret = request_exact(btwf, config_name, QIN_BTWF_CONFIG_BYTES, &config);
	if (ret)
		goto out;
	ret = request_exact(btwf, cali_name, QIN_BTWF_CALI_BYTES, &cali);
	if (ret)
		goto out;

	/* This no-map RAM has one uncached AP mapping and a disabled CP. */
	writel(QIN_BTWF_INIT_START, btwf->ram + QIN_BTWF_INIT_OFFSET);
	writel(0, btwf->ram + QIN_BTWF_SLEEP_STATUS_OFFSET);
	writel(0, btwf->ram + QIN_BTWF_SLEEP_FLAG_OFFSET);
	memcpy_toio(btwf->ram, fw->data, fw->size);
	ret = verify_image(btwf, fw);
	if (ret)
		goto out;
	if (!btwf->calibrated) {
		memcpy_toio(btwf->ram + QIN_BTWF_CONFIG_OFFSET, config->data,
			    config->size);
		memcpy_toio(btwf->ram + QIN_BTWF_CALI_OFFSET, cali->data,
			    cali->size);
		writel(QIN_BTWF_CALI_START,
		       btwf->ram + QIN_BTWF_CALI_FLAG_OFFSET);
	}
	for (i = 0; i < 3; i++)
		writel(btwf->efuse[i],
		       btwf->ram + QIN_BTWF_EFUSE_OFFSET + 4 * i);
	writel(QIN_BTWF_NOGNSS, btwf->ram + QIN_BTWF_NOGNSS_OFFSET);
	if (btwf->efuse[3])
		writel(btwf->efuse[3], btwf->ram + QIN_BTWF_TEMP_VALUE_OFFSET);
	writel(btwf->efuse[3] ? QIN_BTWF_TEMP_VALID : QIN_BTWF_TEMP_INVALID,
	       btwf->ram + QIN_BTWF_TEMP_MAGIC_OFFSET);
	dev_info(btwf->dev, "revision %u prepared %s and %s, no GNSS\n",
		 btwf->revision, config_name, cali_name);
out:
	release_firmware(cali);
	release_firmware(config);
	btwf->last_error = ret;
	return ret;
}

static int power_off(struct qin_btwf *btwf)
{
	u32 val;
	int ret;

	if (!btwf->claimed) {
		qin1splus_sipc_halted(btwf->sipc);
		return 0;
	}
	/* Stop instruction execution before removing the domain or its rails. */
	ret = alias_write(btwf->aon, SC9820E_AON_WCN_ENABLE,
			  SC9820E_ALIAS_CLEAR, SC9820E_AON_BTWF_ENABLE);
	if (ret)
		return ret;
	udelay(10);
	ret = regmap_read(btwf->aon, SC9820E_AON_WCN_ENABLE, &val);
	if (ret)
		return ret;
	if (val & SC9820E_AON_BTWF_ENABLE) {
		dev_err(btwf->dev, "BTWF disable did not latch: %08x\n", val);
		return -EIO;
	}
	ret = alias_write(btwf->pmu, SC9820E_PMU_RESET, SC9820E_ALIAS_SET,
			  SC9820E_PMU_BTWF_RESET);
	if (ret)
		return ret;
	udelay(10);
	ret = regmap_read(btwf->pmu, SC9820E_PMU_RESET, &val);
	if (ret)
		return ret;
	if (!(val & SC9820E_PMU_BTWF_RESET)) {
		dev_err(btwf->dev, "BTWF reset did not latch: %08x\n", val);
		return -EIO;
	}
	ret = alias_write(btwf->pmu, SC9820E_PMU_WCN_DOMAIN,
			  SC9820E_ALIAS_CLEAR, SC9820E_PMU_WCN_AUTO_SHUTDOWN);
	if (ret)
		return ret;
	ret = alias_write(btwf->pmu, SC9820E_PMU_WCN_DOMAIN, SC9820E_ALIAS_SET,
			  SC9820E_PMU_WCN_FORCE_SHUTDOWN);
	if (ret)
		return ret;
	if (btwf->pa_enabled) {
		ret = regulator_disable(btwf->vddwifipa);
		if (ret)
			return ret;
		btwf->pa_enabled = false;
		usleep_range(10000, 30000);
	}
	if (btwf->con_enabled) {
		ret = regulator_disable(btwf->vddcon);
		if (ret)
			return ret;
		btwf->con_enabled = false;
	}
	if (btwf->clock_enabled) {
		ret = regmap_update_bits(btwf->g6, SC9820E_G6_WCN_26M,
					 SC9820E_G6_WCN_26M_DISABLE,
					 SC9820E_G6_WCN_26M_DISABLE);
		if (ret)
			return ret;
		btwf->clock_enabled = false;
	}
	/* There is no GNSS consumer: pulse only the shared WCN reset field. */
	ret = alias_write(btwf->pmu, SC9820E_PMU_RESET, SC9820E_ALIAS_SET,
			  SC9820E_PMU_WCN_RESET);
	if (ret)
		return ret;
	usleep_range(5000, 6000);
	ret = alias_write(btwf->pmu, SC9820E_PMU_RESET, SC9820E_ALIAS_CLEAR,
			  SC9820E_PMU_WCN_RESET);
	if (ret)
		return ret;
	usleep_range(5000, 6000);
	ret = alias_write(btwf->pmu, SC9820E_PMU_WCN_DEEP_SLEEP,
			  SC9820E_ALIAS_SET, BIT(0));
	if (ret)
		return ret;
	ret = regmap_update_bits(btwf->pub, SC9820E_PUB_DMC_DUMMY,
				 SC9820E_PUB_DMC_DUMMY_MASK, btwf->saved_pub);
	if (!ret) {
		btwf->claimed = false;
		qin1splus_sipc_halted(btwf->sipc);
	}
	return ret;
}

static int power_on(struct qin_btwf *btwf)
{
	int ret;

	ret = check_inactive(btwf);
	if (ret)
		return ret;
	ret = regmap_read(btwf->pub, SC9820E_PUB_DMC_DUMMY, &btwf->saved_pub);
	if (ret)
		return ret;
	btwf->claimed = true;
	ret = regmap_update_bits(btwf->pub, SC9820E_PUB_DMC_DUMMY,
				 SC9820E_PUB_DMC_DUMMY_MASK, 0x808);
	if (ret)
		return ret;
	usleep_range(10, 50);
	if (!btwf->sleep_initialized) {
		ret = alias_write(btwf->pmu, SC9820E_PMU_RESET,
				  SC9820E_ALIAS_CLEAR, SC9820E_PMU_WCN_RESET);
		if (ret)
			return ret;
		usleep_range(5000, 6000);
		ret = alias_write(btwf->pmu, SC9820E_PMU_WCN_DEEP_SLEEP,
				  SC9820E_ALIAS_SET, BIT(0));
		if (ret)
			return ret;
		btwf->sleep_initialized = true;
	}
	ret = regulator_set_voltage(btwf->vddcon, 1200000, 1200000);
	if (ret)
		return ret;
	if (btwf->revision == 1) {
		ret = regulator_set_voltage(btwf->vddwifipa, 3000000, 3000000);
		if (ret)
			return ret;
	}
	ret = regulator_enable(btwf->vddcon);
	if (ret)
		return ret;
	btwf->con_enabled = true;
	btwf->clock_enabled = true;
	ret = regmap_update_bits(btwf->g6, SC9820E_G6_WCN_26M,
				 SC9820E_G6_WCN_26M_DISABLE, 0);
	if (ret)
		return ret;
	usleep_range(10000, 30000);
	ret = regulator_enable(btwf->vddwifipa);
	if (ret)
		return ret;
	btwf->pa_enabled = true;
	ret = alias_write(btwf->pmu, SC9820E_PMU_WCN_DOMAIN,
			  SC9820E_ALIAS_CLEAR, SC9820E_PMU_WCN_AUTO_SHUTDOWN);
	if (ret)
		return ret;
	return alias_write(btwf->pmu, SC9820E_PMU_WCN_DOMAIN,
			   SC9820E_ALIAS_CLEAR, SC9820E_PMU_WCN_FORCE_SHUTDOWN);
}

static int release_cpu(struct qin_btwf *btwf)
{
	int ret;

	ret = boot_control_write(
		btwf->pmu, SC9820E_PMU_WCN_DOMAIN + SC9820E_ALIAS_CLEAR,
		SC9820E_PMU_WCN_AUTO_SHUTDOWN | SC9820E_PMU_WCN_FORCE_SHUTDOWN);
	if (ret)
		return ret;
	ret = boot_control_write(btwf->pmu,
				 SC9820E_PMU_SLEEP_CTRL + SC9820E_ALIAS_CLEAR,
				 SC9820E_PMU_WCN_FORCE_SLEEP);
	if (ret)
		return ret;
	ret = boot_control_write(
		btwf->pmu, SC9820E_PMU_RESET + SC9820E_ALIAS_CLEAR,
		SC9820E_PMU_WCN_RESET | SC9820E_PMU_BTWF_RESET);
	if (ret)
		return ret;
	return boot_control_write(btwf->aon,
				  SC9820E_AON_WCN_ENABLE + SC9820E_ALIAS_SET,
				  SC9820E_AON_BTWF_ENABLE);
}

static int qin_btwf_start(struct rproc *rproc)
{
	struct qin_btwf *btwf = rproc->priv;
	u32 marker;
	int attempt, ret, stop_ret;

	ret = check_inactive(btwf);
	if (ret) {
		btwf->last_error = ret;
		return ret;
	}
	dev_info(btwf->dev, "preparing WCN shared-buffer transport\n");
	ret = qin1splus_sipc_prepare(btwf->sipc);
	if (ret) {
		btwf->last_error = ret;
		return ret;
	}
	dev_info(btwf->dev, "WCN transport prepared; enabling power\n");
	ret = power_on(btwf);
	if (ret)
		goto fail;
	dev_info(btwf->dev, "WCN power ready; publishing shared RAM\n");
	/* Publish all shared RAM before any CPU-release register write. */
	wmb();
	if (readl(btwf->ram + QIN_BTWF_INIT_OFFSET) != QIN_BTWF_INIT_START) {
		ret = -EIO;
		goto fail;
	}
	ret = qin1splus_sipc_released(btwf->sipc);
	if (ret)
		goto fail;
	dev_info(btwf->dev, "releasing WCN CPU\n");
	ret = release_cpu(btwf);
	if (ret)
		goto fail;
	for (attempt = 0; attempt < QIN_BTWF_READY_ATTEMPTS; attempt++) {
		marker = readl(btwf->ram + QIN_BTWF_INIT_OFFSET);
		if (marker != btwf->last_cp_marker)
			dev_info(btwf->dev, "CP marker %08x at poll %d\n",
				 marker, attempt);
		btwf->last_cp_marker = marker;
		if (btwf->last_cp_marker == QIN_BTWF_INIT_READY)
			break;
		msleep(QIN_BTWF_READY_POLL_MS);
	}
	if (attempt == QIN_BTWF_READY_ATTEMPTS) {
		dev_err(btwf->dev, "CP READY timeout, last marker %08x\n",
			btwf->last_cp_marker);
		writel(QIN_BTWF_INIT_FAILED, btwf->ram + QIN_BTWF_INIT_OFFSET);
		ret = -ETIMEDOUT;
		goto fail;
	}
	btwf->ready_observed = true;
	dev_info(btwf->dev, "CP wrote READY %08x at poll %d\n",
		 btwf->last_cp_marker, attempt);
	if (!btwf->calibrated) {
		memcpy_fromio(btwf->calibration,
			      btwf->ram + QIN_BTWF_CALI_OFFSET,
			      sizeof(btwf->calibration));
		btwf->snapshot_valid = true;
		writel(QIN_BTWF_CALI_DONE,
		       btwf->ram + QIN_BTWF_CALI_FLAG_OFFSET);
		btwf->calibrated = true;
	}
	writel(QIN_BTWF_INIT_SUCCESS, btwf->ram + QIN_BTWF_INIT_OFFSET);
	btwf->last_error = 0;
	return 0;
fail:
	stop_ret = power_off(btwf);
	if (stop_ret)
		dev_err(btwf->dev,
			"failed start %d; cleanup %d, ownership retained\n",
			ret, stop_ret);
	btwf->last_error = ret;
	return ret;
}

static int qin_btwf_stop(struct rproc *rproc)
{
	struct qin_btwf *btwf = rproc->priv;
	int ret;

	ret = power_off(btwf);
	btwf->last_error = ret;
	return ret;
}

static const struct rproc_ops qin_btwf_ops = {
	.sanity_check = qin_btwf_sanity,
	.load = qin_btwf_load,
	.start = qin_btwf_start,
	.stop = qin_btwf_stop,
};

static ssize_t ram_read(struct file *file, char __user *buf, size_t count,
			loff_t *offset)
{
	struct qin_btwf *btwf = file->private_data;
	u8 *bytes;
	unsigned long uncopied;

	if (*offset < 0)
		return -EINVAL;
	if (*offset >= QIN_BTWF_RAM_BYTES)
		return 0;
	count = min_t(size_t, count, QIN_BTWF_RAM_BYTES - *offset);
	count = min_t(size_t, count, SZ_4K);
	if (!count)
		return 0;
	bytes = kmalloc(count, GFP_KERNEL);
	if (!bytes)
		return -ENOMEM;
	/* Live readback may overlap AP loading or CP writes; it is not atomic. */
	memcpy_fromio(bytes, btwf->ram + *offset, count);
	uncopied = copy_to_user(buf, bytes, count);
	kfree(bytes);
	if (uncopied)
		return -EFAULT;
	*offset += count;
	return count;
}

static loff_t ram_llseek(struct file *file, loff_t offset, int whence)
{
	return fixed_size_llseek(file, offset, whence, QIN_BTWF_RAM_BYTES);
}

static const struct file_operations ram_fops = {
	.open = simple_open,
	.read = ram_read,
	.llseek = ram_llseek,
};

static ssize_t calibration_read(struct file *file, struct kobject *kobj,
				const struct bin_attribute *attr, char *buf,
				loff_t offset, size_t count)
{
	struct device *dev = kobj_to_dev(kobj);
	struct qin_btwf *btwf = dev_get_drvdata(dev);
	ssize_t ret;

	ret = mutex_lock_interruptible(&btwf->rproc->lock);
	if (ret)
		return ret;
	if (!btwf->snapshot_valid)
		ret = -ENODATA;
	else
		ret = memory_read_from_buffer(buf, count, &offset,
					      btwf->calibration,
					      sizeof(btwf->calibration));
	mutex_unlock(&btwf->rproc->lock);
	return ret;
}

static const struct bin_attribute bin_attr_calibration = {
	.attr = { .name = "calibration", .mode = 0400 },
	.size = QIN_BTWF_CALI_BYTES,
	.read = calibration_read,
};

static ssize_t boot_evidence_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct qin_btwf *btwf = dev_get_drvdata(dev);
	ssize_t ret;

	ret = mutex_lock_interruptible(&btwf->rproc->lock);
	if (ret)
		return ret;
	ret = sysfs_emit(
		buf,
		"revision=%u ready_observed=%u cp_marker=%08x calibration=%u ownership=%u error=%d\n",
		btwf->revision, btwf->ready_observed, btwf->last_cp_marker,
		btwf->snapshot_valid, btwf->claimed, btwf->last_error);
	mutex_unlock(&btwf->rproc->lock);
	return ret;
}
static DEVICE_ATTR_RO(boot_evidence);

static struct attribute *qin_btwf_attrs[] = {
	&dev_attr_boot_evidence.attr,
	NULL,
};

static const struct bin_attribute *const qin_btwf_bin_attrs[] = {
	&bin_attr_calibration,
	NULL,
};

static const struct attribute_group qin_btwf_group = {
	.attrs = qin_btwf_attrs,
	.bin_attrs = qin_btwf_bin_attrs,
};

static int qin_btwf_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *mem;
	struct resource res;
	struct qin_btwf *btwf;
	struct rproc *rproc;
	int ret;

	rproc = devm_rproc_alloc(dev, "qin1splus-btwf", &qin_btwf_ops,
				 "wcnmodem.bin", sizeof(*btwf));
	if (!rproc)
		return -ENOMEM;
	btwf = rproc->priv;
	btwf->dev = dev;
	btwf->rproc = rproc;
	btwf->sipc = devm_qin1splus_sipc_get(dev);
	if (IS_ERR(btwf->sipc))
		return dev_err_probe(dev, PTR_ERR(btwf->sipc),
				     "WCN transport\n");
	rproc->auto_boot = false;
	rproc->recovery_disabled = true;
	platform_set_drvdata(pdev, btwf);

	mem = of_parse_phandle(dev->of_node, "memory-region", 0);
	if (!mem)
		return -EINVAL;
	ret = of_address_to_resource(mem, 0, &res);
	if (!ret && !of_property_read_bool(mem, "no-map"))
		ret = -EINVAL;
	of_node_put(mem);
	if (ret)
		return ret;
	if (res.start != QIN_BTWF_BASE_PHYS ||
	    resource_size(&res) != QIN_BTWF_RESERVED_BYTES)
		return -EINVAL;
	btwf->ram = devm_ioremap(dev, res.start, QIN_BTWF_RAM_BYTES);
	if (!btwf->ram)
		return -ENOMEM;

	btwf->aon = syscon_regmap_lookup_by_phandle(dev->of_node,
						    "sprd,aon-syscon");
	if (IS_ERR(btwf->aon))
		return dev_err_probe(dev, PTR_ERR(btwf->aon), "AON syscon\n");
	btwf->pmu = syscon_regmap_lookup_by_phandle(dev->of_node,
						    "sprd,pmu-syscon");
	if (IS_ERR(btwf->pmu))
		return dev_err_probe(dev, PTR_ERR(btwf->pmu), "PMU syscon\n");
	btwf->pub = syscon_regmap_lookup_by_phandle(dev->of_node,
						    "sprd,pub-syscon");
	if (IS_ERR(btwf->pub))
		return dev_err_probe(dev, PTR_ERR(btwf->pub), "PUB syscon\n");
	btwf->g6 =
		syscon_regmap_lookup_by_phandle(dev->of_node, "sprd,g6-syscon");
	if (IS_ERR(btwf->g6))
		return dev_err_probe(dev, PTR_ERR(btwf->g6), "G6 syscon\n");
	btwf->vddcon = devm_regulator_get(dev, "vddcon");
	if (IS_ERR(btwf->vddcon))
		return dev_err_probe(dev, PTR_ERR(btwf->vddcon), "vddcon\n");
	btwf->vddwifipa = devm_regulator_get(dev, "vddwifipa");
	if (IS_ERR(btwf->vddwifipa))
		return dev_err_probe(dev, PTR_ERR(btwf->vddwifipa),
				     "vddwifipa\n");
	btwf->wifi_efuse = devm_nvmem_cell_get(dev, "wifi-calibration");
	if (IS_ERR(btwf->wifi_efuse))
		return dev_err_probe(dev, PTR_ERR(btwf->wifi_efuse),
				     "Wi-Fi efuse\n");
	btwf->temp_efuse = devm_nvmem_cell_get(dev, "temperature-calibration");
	if (IS_ERR(btwf->temp_efuse))
		return dev_err_probe(dev, PTR_ERR(btwf->temp_efuse),
				     "temperature efuse\n");
	ret = devm_rproc_add(dev, rproc);
	if (ret)
		return ret;
	ret = devm_device_add_group(dev, &qin_btwf_group);
	if (ret)
		return ret;
	/* rproc deletion drains debugfs readers before devres unmaps RAM. */
	if (!IS_ERR_OR_NULL(rproc->dbg_dir))
		debugfs_create_file_size("ram", 0400, rproc->dbg_dir, btwf,
					 &ram_fops, QIN_BTWF_RAM_BYTES);
	return 0;
}

static void qin_btwf_shutdown(struct platform_device *pdev)
{
	struct qin_btwf *btwf = platform_get_drvdata(pdev);
	int ret;

	mutex_lock(&btwf->rproc->lock);
	ret = power_off(btwf);
	mutex_unlock(&btwf->rproc->lock);
	if (ret)
		dev_err(btwf->dev, "shutdown failed: %d\n", ret);
}

static int qin_btwf_suspend(struct device *dev)
{
	struct qin_btwf *btwf = dev_get_drvdata(dev);
	int ret;

	mutex_lock(&btwf->rproc->lock);
	ret = btwf->claimed ? -EBUSY : 0;
	mutex_unlock(&btwf->rproc->lock);
	return ret;
}

static DEFINE_SIMPLE_DEV_PM_OPS(qin_btwf_pm_ops, qin_btwf_suspend, NULL);

static const struct of_device_id qin_btwf_match[] = {
	{ .compatible = "fplinux,qin1splus-btwf" },
	{}
};
MODULE_DEVICE_TABLE(of, qin_btwf_match);

static struct platform_driver qin_btwf_driver = {
	.probe = qin_btwf_probe,
	.shutdown = qin_btwf_shutdown,
	.driver = {
		.name = "qin1splus-btwf",
		.of_match_table = qin_btwf_match,
		.pm = pm_sleep_ptr(&qin_btwf_pm_ops),
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(qin_btwf_driver);

MODULE_DESCRIPTION("Qin 1s+ manual BTWF firmware boot owner");
MODULE_LICENSE("GPL");
