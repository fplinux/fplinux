// SPDX-License-Identifier: GPL-2.0-only
/* Host sdboot consumer: real libfdt; fake MMC, filesystem and bootm runtime. */
#include <bootm.h>
#include <command.h>
#include <fs.h>
#include <image.h>
#include <mmc.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "stage0-handoff.h"

#define ARENA_BYTES 0x10000U
#define FIT_ADDRESS 0x10000000U
#define KERNEL_ADDRESS 0x10010000U
#define DTB_ADDRESS 0x10020000U

extern struct cmd_tbl test_command;
static struct bootm_headers selected;
static struct mmc fake_mmc;
static jmp_buf finished;
static uint32_t failure;
static size_t fit_bytes;
static int released;

static void fail(uint32_t code, uint32_t detail) __attribute__((noreturn));
static void finalize(uint32_t kernel, uint32_t kernel_size, uint32_t dtb,
		     uint32_t dtb_size) __attribute__((noreturn));

static const struct fplinux_uboot_handoff handoff = {
	.fit_phys = FIT_ADDRESS,
	.fit_limit = ARENA_BYTES,
	.zimage_phys = KERNEL_ADDRESS,
	.zimage_limit = ARENA_BYTES,
	.dtb_phys = DTB_ADDRESS,
	.dtb_limit = ARENA_BYTES,
};

static int checkpoint(uint32_t code, uint32_t value)
{
	(void)code;
	(void)value;
	return 0;
}

static void fail(uint32_t code, uint32_t detail)
{
	failure = code == 2 ? detail : UINT32_MAX;
	longjmp(finished, 1);
}

static void finalize(uint32_t kernel, uint32_t kernel_size, uint32_t dtb,
		     uint32_t dtb_size)
{
	if (kernel != KERNEL_ADDRESS || kernel_size != 64 ||
	    dtb != DTB_ADDRESS || !dtb_size)
		failure = UINT32_MAX;
	longjmp(finished, 1);
}

static const struct fplinux_stage0_ops ops = {
	.checkpoint = checkpoint,
	.fail = fail,
	.finalize_and_boot = finalize,
};

const struct fplinux_uboot_handoff *ums9117_uboot_handoff_get(void)
{
	return &handoff;
}

const struct fplinux_stage0_ops *ums9117_stage0_ops(void)
{
	return &ops;
}

void ums9117_mmc_release(void)
{
	released = 1;
}

struct mmc *find_mmc_device(int device)
{
	return device == 0 ? &fake_mmc : NULL;
}

int mmc_init(struct mmc *mmc)
{
	mmc->initialized = 1;
	return 0;
}

int fs_set_blk_dev(const char *interface, const char *device, int type)
{
	return strcmp(interface, "mmc") || strcmp(device, "0:1") ||
	       type != FS_TYPE_FAT;
}

int fs_size(const char *path, loff_t *size)
{
	*size = fit_bytes;
	return strcmp(path, "FPLINUX.ITB");
}

int fs_read(const char *path, ulong address, loff_t offset, loff_t size,
	    loff_t *actual)
{
	/* The fixture is already in the fake filesystem's mapped destination. */
	*actual = size;
	return strcmp(path, "FPLINUX.ITB") || address != FIT_ADDRESS ||
	       offset || size != (loff_t)fit_bytes;
}

int env_set(const char *name, const char *value)
{
	return strcmp(name, "verify") || strcmp(value, "yes");
}

int env_set_hex(const char *name, ulong value)
{
	return strcmp(name, "fdt_high") || value <= DTB_ADDRESS;
}

/* U-Boot FIT accessors backed by the real libfdt ABI. */
int fit_check_format(const void *fit, ulong size)
{
	return fdt_check_header(fit) || fdt_totalsize(fit) != size;
}

int fit_conf_get_node(const void *fit, const char *name)
{
	return fdt_subnode_offset(fit, fdt_path_offset(fit, "/configurations"),
				  name);
}

int fit_conf_get_prop_node_index(const void *fit, int node, const char *name,
				 int index)
{
	const char *image = fdt_getprop(fit, node, name, NULL);

	return image && !index ?
		       fdt_subnode_offset(fit, fdt_path_offset(fit, "/images"),
					  image) :
		       -1;
}

int fit_image_get_data(const void *fit, int node, const void **data,
		       size_t *size)
{
	int length;

	*data = fdt_getprop(fit, node, "data", &length);
	*size = length > 0 ? (size_t)length : 0;
	return *data ? 0 : -1;
}

int fit_image_get_load(const void *fit, int node, ulong *load)
{
	int length;
	const uint32_t *value = fdt_getprop(fit, node, "load", &length);

	if (!value || length != 4)
		return -1;
	*load = be32toh(*value);
	return 0;
}

static int image_enum(const void *fit, int node, const char *property,
		      const char *name, uint8_t value, uint8_t *result)
{
	const char *actual = fdt_getprop(fit, node, property, NULL);

	if (!actual || strcmp(actual, name))
		return -1;
	*result = value;
	return 0;
}

int fit_image_get_type(const void *fit, int node, uint8_t *type)
{
	if (!image_enum(fit, node, "type", "kernel", IH_TYPE_KERNEL, type))
		return 0;
	return image_enum(fit, node, "type", "flat_dt", IH_TYPE_FLATDT, type);
}

int fit_image_get_arch(const void *fit, int node, uint8_t *arch)
{
	return image_enum(fit, node, "arch", "arm", IH_ARCH_ARM, arch);
}

int fit_image_get_comp(const void *fit, int node, uint8_t *comp)
{
	return image_enum(fit, node, "compression", "none", IH_COMP_NONE, comp);
}

void bootm_init(struct bootm_info *bmi)
{
	memset(bmi, 0, sizeof(*bmi));
	bmi->images = &selected;
}

int bootm_run_states(struct bootm_info *bmi, int states)
{
	const void *fit = (const void *)(uintptr_t)FIT_ADDRESS;
	const void *data;
	size_t size;
	int node;

	(void)bmi;
	if (states == (BOOTM_STATE_START | BOOTM_STATE_FINDOS)) {
		selected.fit_uname_cfg = "test-board";
		selected.fit_uname_os = "kernel";
		selected.fit_hdr_os = fit;
		node = fdt_path_offset(fit, "/images/kernel");
		selected.fit_noffset_os = node;
		fit_image_get_data(fit, node, &data, &size);
		selected.os.start = FIT_ADDRESS;
		selected.os.end = FIT_ADDRESS + fit_bytes;
		selected.os.load = KERNEL_ADDRESS;
		selected.os.image_len = size;
		selected.os.type = IH_TYPE_KERNEL;
		selected.os.arch = IH_ARCH_ARM;
		selected.os.os = IH_OS_LINUX;
		selected.os.comp = IH_COMP_NONE;
		selected.ep = KERNEL_ADDRESS;
	} else if (states == BOOTM_STATE_FINDOTHER) {
		node = fdt_path_offset(fit, "/images/fdt");
		fit_image_get_data(fit, node, &data, &size);
		selected.ft_addr = (void *)(uintptr_t)DTB_ADDRESS;
		/* Model bootm's external copy boundary; header validation stays real. */
		memcpy(selected.ft_addr, data, size);
		if (fdt_check_header(selected.ft_addr))
			return -1;
		selected.ft_len = fdt_totalsize(selected.ft_addr);
		selected.fit_hdr_fdt = fit;
		selected.fit_uname_fdt = "fdt";
		selected.fit_noffset_fdt = node;
	} else if (states == BOOTM_STATE_LOADOS) {
		fit_image_get_data(fit, selected.fit_noffset_os, &data, &size);
		memcpy((void *)(uintptr_t)KERNEL_ADDRESS, data, size);
	}
	return 0;
}

int main(int argc, char **argv)
{
	void *fit;
	const void *dtb;
	FILE *input;
	size_t size;
	int expected, alignment;

	if (argc != 4)
		return 2;
	alignment = atoi(argv[2]);
	expected = atoi(argv[3]);
	fit = mmap((void *)(uintptr_t)FIT_ADDRESS, 3 * ARENA_BYTES,
		   PROT_READ | PROT_WRITE,
		   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
	if (fit == MAP_FAILED)
		return 2;
	input = fopen(argv[1], "rb");
	if (!input)
		return 2;
	fit_bytes = fread(fit, 1, ARENA_BYTES, input);
	fclose(input);
	fit_image_get_data(fit, fdt_path_offset(fit, "/images/fdt"), &dtb,
			   &size);
	if ((uintptr_t)dtb % 8 != (unsigned int)alignment)
		return 2;
	if (!setjmp(finished)) {
		test_command.command(&test_command, 0, 0, NULL);
		return 2;
	}
	printf("embedded_alignment=%d failure=%u released=%d\n", alignment,
	       failure, released);
	munmap(fit, 3 * ARENA_BYTES);
	return failure != (unsigned int)expected || !released;
}
