// SPDX-License-Identifier: GPL-2.0-only
#include <asm/unaligned.h>
#include <bootm.h>
#include <command.h>
#include <env.h>
#include <fs.h>
#include <image.h>
#include <linux/libfdt.h>
#include <linux/types.h>
#include <mmc.h>
#include <stdio.h>
#include <string.h>
#include <u-boot/sha256.h>

#include "stage0-handoff.h"
#include "ta1618-mmc.h"

#define TA1618_SDBOOT_FIT_PATH "FPLINUX.ITB"
#define TA1618_SDBOOT_CONFIG "nokia-ta1618"
#define TA1618_SDBOOT_KERNEL "kernel"
#define TA1618_SDBOOT_FDT "fdt"
#define TA1618_SDBOOT_VERIFY_VALUE "yes"

static int range_contains(uint32_t base, uint32_t limit, uint32_t address,
			  ulong bytes)
{
	return bytes && address >= base && address - base < limit &&
	       bytes <= limit - (address - base);
}

static int load_whole_fit(const struct fplinux_uboot_handoff *handoff,
			  uint32_t *fit_bytes)
{
	loff_t actual;
	loff_t size;

	if (fs_set_blk_dev("mmc", "0:1", FS_TYPE_FAT) ||
	    fs_size(TA1618_SDBOOT_FIT_PATH, &size) || size <= 0)
		return -EINVAL;
	if (size > handoff->fit_limit)
		return -EFBIG;
	if (fs_set_blk_dev("mmc", "0:1", FS_TYPE_FAT) ||
	    fs_read(TA1618_SDBOOT_FIT_PATH, handoff->fit_phys, 0, size,
		    &actual) ||
	    actual != size)
		return -EIO;
	*fit_bytes = size;
	return 0;
}

static int has_sha256_hash(const void *fit, int image)
{
	const void *value;
	const char *algorithm;
	int hash;
	int length;

	hash = fdt_subnode_offset(fit, image, FIT_HASH_NODENAME);
	if (hash < 0)
		return 0;
	algorithm = fdt_getprop(fit, hash, FIT_ALGO_PROP, &length);
	if (!algorithm || length != sizeof("sha256") ||
	    memcmp(algorithm, "sha256", sizeof("sha256")))
		return 0;
	value = fdt_getprop(fit, hash, FIT_VALUE_PROP, &length);
	return value && length == SHA256_SUM_LEN;
}

static int preflight_fit_selection(const struct fplinux_uboot_handoff *handoff,
				   uint32_t fit_bytes,
				   const struct bootm_headers *images)
{
	const void *fit = (const void *)(uintptr_t)handoff->fit_phys;
	const void *fdt_data;
	const void *kernel_data;
	size_t fdt_size;
	size_t kernel_size;
	uint8_t arch;
	uint8_t compression;
	uint8_t type;
	ulong load;
	int config;
	int fdt;
	int kernel;

	if (!images->fit_uname_cfg ||
	    strcmp(images->fit_uname_cfg, TA1618_SDBOOT_CONFIG) ||
	    !images->fit_uname_os ||
	    strcmp(images->fit_uname_os, TA1618_SDBOOT_KERNEL) ||
	    images->fit_hdr_os != fit ||
	    images->os.start != handoff->fit_phys ||
	    images->os.end != handoff->fit_phys + fit_bytes)
		return -EINVAL;
	config = fit_conf_get_node(fit, images->fit_uname_cfg);
	if (config < 0 ||
	    fit_conf_get_prop_node_count(fit, config, FIT_KERNEL_PROP) != 1 ||
	    fit_conf_get_prop_node_count(fit, config, FIT_FDT_PROP) != 1 ||
	    fdt_getprop(fit, config, FIT_RAMDISK_PROP, NULL) ||
	    fdt_getprop(fit, config, FIT_LOADABLE_PROP, NULL) ||
	    fdt_getprop(fit, config, FIT_FPGA_PROP, NULL))
		return -EINVAL;
	kernel = fit_conf_get_prop_node_index(fit, config, FIT_KERNEL_PROP, 0);
	fdt = fit_conf_get_prop_node_index(fit, config, FIT_FDT_PROP, 0);
	if (kernel != images->fit_noffset_os || fdt < 0 ||
	    strcmp(fit_get_name(fit, kernel, NULL), TA1618_SDBOOT_KERNEL) ||
	    strcmp(fit_get_name(fit, fdt, NULL), TA1618_SDBOOT_FDT) ||
	    images->os.type != IH_TYPE_KERNEL ||
	    images->os.arch != IH_ARCH_ARM || images->os.os != IH_OS_LINUX ||
	    images->os.comp != IH_COMP_NONE ||
	    images->os.load != handoff->zimage_phys ||
	    images->ep != handoff->zimage_phys ||
	    fit_image_get_data(fit, kernel, &kernel_data, &kernel_size) ||
	    kernel_size != images->os.image_len ||
	    kernel_size < FPLINUX_BOOT_ZIMAGE_HEADER_BYTES ||
	    !range_contains(handoff->fit_phys, fit_bytes,
			    (uint32_t)(uintptr_t)kernel_data, kernel_size) ||
	    !range_contains(handoff->zimage_phys, handoff->zimage_limit,
			    handoff->zimage_phys, kernel_size) ||
	    get_unaligned_le32((const uint8_t *)kernel_data +
			       FPLINUX_BOOT_ZIMAGE_MAGIC_OFFSET) !=
		    FPLINUX_BOOT_ZIMAGE_MAGIC ||
	    get_unaligned_le32((const uint8_t *)kernel_data +
			       FPLINUX_BOOT_ZIMAGE_SIZE_OFFSET) !=
		    kernel_size ||
	    !has_sha256_hash(fit, kernel))
		return -EINVAL;
	if (fit_image_get_type(fit, fdt, &type) ||
	    fit_image_get_arch(fit, fdt, &arch) ||
	    fit_image_get_comp(fit, fdt, &compression) ||
	    fit_image_get_load(fit, fdt, &load) ||
	    fit_image_get_data(fit, fdt, &fdt_data, &fdt_size) ||
	    type != IH_TYPE_FLATDT || arch != IH_ARCH_ARM ||
	    compression != IH_COMP_NONE || load != handoff->dtb_phys ||
	    !range_contains(handoff->fit_phys, fit_bytes,
			    (uint32_t)(uintptr_t)fdt_data, fdt_size) ||
	    !range_contains(handoff->dtb_phys, handoff->dtb_limit,
			    handoff->dtb_phys, fdt_size) ||
	    CONFIG_SYS_FDT_PAD > handoff->dtb_limit - fdt_size ||
	    fdt_check_header(fdt_data) || fdt_totalsize(fdt_data) != fdt_size ||
	    !has_sha256_hash(fit, fdt))
		return -EINVAL;
	return 0;
}

static int validate_bootm_selection(const struct fplinux_uboot_handoff *handoff,
				    uint32_t fit_bytes,
				    const struct bootm_headers *images,
				    uint32_t *zimage_bytes)
{
	const void *fit = (const void *)(uintptr_t)handoff->fit_phys;
	uint8_t arch;
	uint8_t compression;
	uint8_t type;
	ulong load;

	if (!images->fit_uname_cfg ||
	    strcmp(images->fit_uname_cfg, TA1618_SDBOOT_CONFIG) ||
	    !images->fit_uname_os ||
	    strcmp(images->fit_uname_os, TA1618_SDBOOT_KERNEL) ||
	    !images->fit_uname_fdt ||
	    strcmp(images->fit_uname_fdt, TA1618_SDBOOT_FDT) ||
	    images->fit_hdr_os != fit || images->fit_hdr_fdt != fit ||
	    images->os.start != handoff->fit_phys ||
	    images->os.end != handoff->fit_phys + fit_bytes ||
	    images->os.type != IH_TYPE_KERNEL ||
	    images->os.arch != IH_ARCH_ARM || images->os.os != IH_OS_LINUX ||
	    images->os.comp != IH_COMP_NONE ||
	    images->os.load != handoff->zimage_phys ||
	    images->ep != handoff->zimage_phys || !images->os.image_len ||
	    !range_contains(handoff->zimage_phys, handoff->zimage_limit,
			    handoff->zimage_phys, images->os.image_len) ||
	    images->rd_start || images->rd_end || images->initrd_start ||
	    images->initrd_end ||
	    images->ft_addr != (void *)handoff->dtb_phys || !images->ft_len ||
	    images->ft_len > handoff->dtb_limit ||
	    CONFIG_SYS_FDT_PAD > handoff->dtb_limit - images->ft_len ||
	    fdt_check_header(images->ft_addr) ||
	    fdt_totalsize(images->ft_addr) != images->ft_len ||
	    !has_sha256_hash(fit, images->fit_noffset_os) ||
	    !has_sha256_hash(fit, images->fit_noffset_fdt))
		return -EINVAL;
	if (fit_image_get_type(fit, images->fit_noffset_fdt, &type) ||
	    fit_image_get_arch(fit, images->fit_noffset_fdt, &arch) ||
	    fit_image_get_comp(fit, images->fit_noffset_fdt, &compression) ||
	    fit_image_get_load(fit, images->fit_noffset_fdt, &load) ||
	    type != IH_TYPE_FLATDT || arch != IH_ARCH_ARM ||
	    compression != IH_COMP_NONE || load != handoff->dtb_phys)
		return -EINVAL;
	if (images->os.image_len < FPLINUX_BOOT_ZIMAGE_HEADER_BYTES ||
	    get_unaligned_le32((
		    const void *)(uintptr_t)(handoff->zimage_phys +
					     FPLINUX_BOOT_ZIMAGE_MAGIC_OFFSET)) !=
		    FPLINUX_BOOT_ZIMAGE_MAGIC ||
	    get_unaligned_le32((
		    const void *)(uintptr_t)(handoff->zimage_phys +
					     FPLINUX_BOOT_ZIMAGE_SIZE_OFFSET)) !=
		    images->os.image_len)
		return -EINVAL;
	*zimage_bytes = images->os.image_len;
	return 0;
}

static int validate_prepared_fdt(const struct fplinux_uboot_handoff *handoff,
				 const struct bootm_headers *images,
				 uint32_t *dtb_bytes)
{
	int size;

	if ((handoff->dtb_phys & (FPLINUX_BOOT_FDT_ALIGNMENT - 1U)) ||
	    images->ft_addr != (void *)handoff->dtb_phys ||
	    fdt_check_header(images->ft_addr))
		return -EINVAL;
	size = fdt_totalsize(images->ft_addr);
	if (size <= 0 || size > handoff->dtb_limit)
		return -EINVAL;
	*dtb_bytes = size;
	return 0;
}

static int prepare_fit(const struct fplinux_uboot_handoff *handoff,
		       uint32_t fit_bytes, uint32_t *zimage_bytes,
		       uint32_t *dtb_bytes)
{
	char fit_spec[32];
	struct bootm_info bmi;
	ulong fdt_high;
	int length;
	int ret;

	if (fdt_check_header((const void *)(uintptr_t)handoff->fit_phys) ||
	    fdt_totalsize((const void *)(uintptr_t)handoff->fit_phys) !=
		    fit_bytes ||
	    fit_check_format((const void *)(uintptr_t)handoff->fit_phys,
			     fit_bytes))
		return -EINVAL;
	/* Keep FIT hash checks explicit. */
	if (env_set("verify", TA1618_SDBOOT_VERIFY_VALUE))
		return -EIO;
	length = snprintf(fit_spec, sizeof(fit_spec), "%x#%s",
			  handoff->fit_phys, TA1618_SDBOOT_CONFIG);
	if (length < 0 || length >= sizeof(fit_spec))
		return -EINVAL;
	bootm_init(&bmi);
	bmi.addr_img = fit_spec;
	bmi.cmd_name = "sdboot";
	ret = bootm_run_states(&bmi, BOOTM_STATE_START | BOOTM_STATE_FINDOS);
	if (ret)
		return ret;
	ret = preflight_fit_selection(handoff, fit_bytes, bmi.images);
	if (ret)
		return ret;
	ret = bootm_run_states(&bmi, BOOTM_STATE_FINDOTHER);
	if (ret)
		return ret;
	ret = bootm_run_states(&bmi, BOOTM_STATE_LOADOS);
	if (ret)
		return ret;
	ret = validate_bootm_selection(handoff, fit_bytes, bmi.images,
				       zimage_bytes);
	if (ret)
		return ret;
	fdt_high = handoff->dtb_phys + bmi.images->ft_len + CONFIG_SYS_FDT_PAD;
	if (env_set_hex("fdt_high", fdt_high))
		return -EIO;
	ret = bootm_run_states(&bmi, BOOTM_STATE_OS_PREP);
	if (ret)
		return ret;
	return validate_prepared_fdt(handoff, bmi.images, dtb_bytes);
}

static int do_ta1618_sdboot(struct cmd_tbl *cmdtp, int flag, int argc,
			    char *const argv[])
{
	const struct fplinux_uboot_handoff *handoff = fplinux_uboot_handoff();
	const struct fplinux_stage0_ops *ops = fplinux_stage0_ops();
	struct mmc *mmc;
	uint32_t fit_bytes;
	uint32_t zimage_bytes;
	uint32_t dtb_bytes;
	int ret;
	enum fplinux_sdboot_failure_detail stage =
		FPLINUX_SDBOOT_FAILURE_HANDOFF;

	(void)cmdtp;
	(void)flag;
	(void)argc;
	(void)argv;
	if (!handoff || !ops || !handoff->fit_phys || !handoff->fit_limit)
		goto fail;
	ops->checkpoint(FPLINUX_STAGE0_CHECKPOINT_UBOOT_READY, 0U);
	stage = FPLINUX_SDBOOT_FAILURE_MMC;
	mmc = find_mmc_device(0);
	if (!mmc)
		goto fail;
	if (mmc_init(mmc))
		goto fail_release;
	stage = FPLINUX_SDBOOT_FAILURE_LOAD;
	ret = load_whole_fit(handoff, &fit_bytes);
	if (ret) {
		if (ret == -EFBIG)
			stage = FPLINUX_SDBOOT_FAILURE_BOOTM;
		goto fail_release;
	}
	ops->checkpoint(FPLINUX_STAGE0_CHECKPOINT_FIT_LOADED, fit_bytes);
	stage = FPLINUX_SDBOOT_FAILURE_BOOTM;
	ret = prepare_fit(handoff, fit_bytes, &zimage_bytes, &dtb_bytes);
	if (ret)
		goto fail_release;
	stage = FPLINUX_SDBOOT_FAILURE_RELEASE;
	ta1618_mmc_release();
	ops->checkpoint(FPLINUX_STAGE0_CHECKPOINT_LINUX_READY, zimage_bytes);
	ops->finalize_and_boot(handoff->zimage_phys, zimage_bytes,
			       handoff->dtb_phys, dtb_bytes);

fail_release:
	ta1618_mmc_release();
fail:
	if (ops)
		ops->fail(FPLINUX_STAGE0_FAILURE_SDBOOT, stage);
	printf("sdboot: stage %u failed\n", (unsigned int)stage);
	return CMD_RET_FAILURE;
}

U_BOOT_CMD(sdboot, 1, 0, do_ta1618_sdboot,
	   "verify FPLINUX.ITB and boot Linux from the TA-1618 microSD", "");
