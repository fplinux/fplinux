// SPDX-License-Identifier: GPL-2.0-only
/* Host sdboot consumer: real libfdt; fake MMC, filesystem and bootm runtime. */
#include <bootm.h>
#include <command.h>
#include <ext4fs.h>
#include <image.h>
#include <mmc.h>
#include <part.h>
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
static struct blk_desc fake_block;
static jmp_buf finished;
static uint32_t failure;
static size_t fit_bytes;
static int released;
static int padded_fdt;
static int autonomous;
static const char *expected_usb_status;
static const char *selector_path;
static const char *fit_path;
static const char *expected_fit;
static FILE *open_file;
static int mounted;
static unsigned int fit_opens;

static void fail(uint32_t code, uint32_t detail) __attribute__((noreturn));
static void finalize(uint32_t kernel, uint32_t kernel_size, uint32_t dtb,
		     uint32_t dtb_size) __attribute__((noreturn));

static struct fplinux_uboot_handoff handoff = {
	.fit_phys = FIT_ADDRESS,
	.fit_limit = ARENA_BYTES,
	.zimage_phys = KERNEL_ADDRESS,
	.zimage_limit = ARENA_BYTES,
	.dtb_phys = DTB_ADDRESS,
	.dtb_limit = ARENA_BYTES,
	.uboot_sha256 =
		"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
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

static int padded_tree_handoff_valid(uint32_t dtb_bytes)
{
	static const unsigned char seed[] = {
		0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
	};
	static const char seed_name[] = "rng-seed";
	const unsigned char *tree = (const void *)(uintptr_t)DTB_ADDRESS;
	const void *value;
	size_t offset;
	unsigned names = 0;
	int length;
	int chosen;

	if (dtb_bytes >= 0x1000U || fdt_check_header(tree) ||
	    fdt_totalsize(tree) != dtb_bytes || dtb_bytes < sizeof(seed_name))
		return 0;
	chosen = fdt_path_offset(tree, "/chosen");
	if (chosen < 0)
		return 0;
	value = fdt_getprop(tree, chosen, "rng-seed", &length);
	if (!value || length != sizeof(seed) ||
	    memcmp(value, seed, sizeof(seed)))
		return 0;
	for (offset = 0; offset <= dtb_bytes - sizeof(seed_name); ++offset) {
		if (!memcmp(tree + offset, seed_name, sizeof(seed_name)))
			++names;
	}
	return names == 1;
}

static void finalize(uint32_t kernel, uint32_t kernel_size, uint32_t dtb,
		     uint32_t dtb_size)
{
	const void *tree = (const void *)(uintptr_t)dtb;
	int chosen = fdt_path_offset(tree, "/chosen");
	int slot_length;
	const char *slot =
		fdt_getprop(tree, chosen, "fplinux,slot", &slot_length);

	if (kernel != KERNEL_ADDRESS || kernel_size != 64 ||
	    dtb != DTB_ADDRESS || !dtb_size)
		failure = UINT32_MAX;
	if (!slot || slot_length != 2 || slot[1] ||
	    slot[0] != expected_fit[sizeof("/system/") - 1U])
		failure = UINT32_MAX;
	if (padded_fdt && !padded_tree_handoff_valid(dtb_size)) {
		fputs("DTB handoff includes padding or changed the RNG property\n",
		      stderr);
		failure = UINT32_MAX;
	}
	if (autonomous) {
		const char *entry =
			fdt_getprop(tree, chosen, "fplinux,boot-entry", NULL);

		if (!entry || strcmp(entry, "sd") ||
		    fdt_path_offset(tree, "/fplinux-session") >= 0 ||
		    fdt_getprop(tree, chosen, "rng-seed", NULL) ||
		    !fdt_getprop(tree, chosen, "linux,initrd-end", NULL))
			failure = UINT32_MAX;
	}
	if (expected_usb_status) {
		int usb = fdt_path_offset(tree, "/usb");
		const char *status = fdt_getprop(tree, usb, "status", NULL);

		if (!status || strcmp(status, expected_usb_status)) {
			fprintf(stderr, "expected USB status %s, got %s\n",
				expected_usb_status,
				status ? status : "(missing)");
			failure = UINT32_MAX;
		}
	}
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
	released = !mounted && !open_file;
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

int blk_get_device_part_str(const char *interface, const char *device,
			    struct blk_desc **desc,
			    struct disk_partition *partition, int whole_device)
{
	if (strcmp(interface, "mmc") || strcmp(device, "0:2") || whole_device)
		return -1;
	*desc = &fake_block;
	partition->number = 2;
	return 2;
}

void ext4fs_set_blk_dev(struct blk_desc *device,
			struct disk_partition *partition)
{
	mounted = device == &fake_block && partition->number == 2;
}

int ext4fs_mount_clean(void)
{
	return mounted;
}

int ext4fs_open_nofollow(const char *path, loff_t *size)
{
	const char *fixture;
	long length;

	if (!mounted)
		return -EIO;
	if (open_file) {
		fclose(open_file);
		open_file = NULL;
	}
	if (!strcmp(path, "/system/active")) {
		fixture = selector_path;
	} else {
		++fit_opens;
		fixture = strcmp(path, expected_fit) ? NULL : fit_path;
	}
	if (!fixture)
		return -ENOENT;
	open_file = fopen(fixture, "rb");
	if (!open_file)
		return -ENOENT;
	if (fseek(open_file, 0, SEEK_END))
		return -EIO;
	length = ftell(open_file);
	if (length < 0 || fseek(open_file, 0, SEEK_SET))
		return -EIO;
	*size = length;
	return 0;
}

int ext4fs_read(char *buffer, loff_t offset, loff_t size, loff_t *actual)
{
	if (!open_file || offset || size <= 0)
		return -EIO;
	*actual = fread(buffer, 1, size, open_file);
	return ferror(open_file) ? -EIO : 0;
}

void ext4fs_close(void)
{
	if (open_file)
		fclose(open_file);
	open_file = NULL;
	mounted = 0;
}

int env_set(const char *name, const char *value)
{
	return strcmp(name, "verify") || strcmp(value, "yes");
}

int env_set_hex(const char *name, ulong value)
{
	if (!strcmp(name, "initrd_high"))
		return value != ~0UL;
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
	if (!image_enum(fit, node, "type", "ramdisk", IH_TYPE_RAMDISK, type))
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

int fit_image_get_os(const void *fit, int node, uint8_t *os)
{
	return image_enum(fit, node, "os", "linux", IH_OS_LINUX, os);
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

	if (states == (BOOTM_STATE_START | BOOTM_STATE_FINDOS)) {
		const char *config = strchr(bmi->addr_img, '#');

		if (!config || fit_conf_get_node(fit, config + 1) < 0)
			return -1;
		selected.fit_uname_cfg = config + 1;
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
		/* Like U-Boot fit_image_load(): copy, then refuse a non-FDT subimage. */
		memcpy(selected.ft_addr, data, size);
		if (fdt_check_header(selected.ft_addr))
			return -1;
		selected.ft_len = fdt_totalsize(selected.ft_addr);
		selected.fit_hdr_fdt = fit;
		selected.fit_uname_fdt = "fdt";
		selected.fit_noffset_fdt = node;
		node = fdt_path_offset(fit, "/images/ramdisk");
		if (fit_image_get_data(fit, node, &data, &size))
			return -1;
		selected.fit_hdr_rd = fit;
		selected.fit_uname_rd = "ramdisk";
		selected.rd_start = (ulong)(uintptr_t)data;
		selected.rd_end = selected.rd_start + size;
	} else if (states == BOOTM_STATE_LOADOS) {
		fit_image_get_data(fit, selected.fit_noffset_os, &data, &size);
		memcpy((void *)(uintptr_t)KERNEL_ADDRESS, data, size);
	} else if (states == BOOTM_STATE_RAMDISK) {
		selected.initrd_start = selected.rd_start;
		selected.initrd_end = selected.rd_end;
	} else if (states == BOOTM_STATE_OS_PREP && autonomous) {
		int chosen;
		uint32_t end = htobe32(selected.initrd_end);

		if (fdt_open_into(selected.ft_addr, selected.ft_addr, 0x1000))
			return -1;
		chosen = fdt_path_offset(selected.ft_addr, "/chosen");
		if (fdt_setprop(selected.ft_addr, chosen, "linux,initrd-end",
				&end, sizeof(end)))
			return -1;
	} else if (states == BOOTM_STATE_OS_PREP && padded_fdt) {
		static const char stale_name[] = "rng-seed";
		unsigned char *tree = selected.ft_addr;
		size_t active_bytes = fdt_totalsize(tree);

		/* Model unused page padding left by the external bootm runtime. */
		if (active_bytes + 32U + sizeof(stale_name) > 0x1000U)
			return -1;
		memset(tree + active_bytes, 0, 0x1000U - active_bytes);
		memcpy(tree + active_bytes + 32U, stale_name,
		       sizeof(stale_name));
		((uint32_t *)tree)[1] = htobe32(0x1000U);
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

	if (argc < 4 || argc > 6)
		return 2;
	if (argc == 6)
		expected_usb_status = argv[5];
	if (argc >= 5) {
		if (!strcmp(argv[4], "padded-fdt")) {
			padded_fdt = 1;
		} else if (!strcmp(argv[4], "autonomous")) {
			autonomous = 1;
			handoff.entry = FPLINUX_UBOOT_ENTRY_SD;
		} else {
			return 2;
		}
	}
	alignment = atoi(argv[2]);
	expected = atoi(argv[3]);
	fit_path = argv[1];
	selector_path = getenv("FPLINUX_TEST_SDBOOT_SELECTOR");
	expected_fit = getenv("FPLINUX_TEST_SDBOOT_FIT");
	if (!expected_fit)
		expected_fit = "/system/a/FPLINUX.ITB";
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
	printf("embedded_alignment=%d failure=%u released=%d fit_opens=%u\n",
	       alignment, failure, released, fit_opens);
	munmap(fit, 3 * ARENA_BYTES);
	return failure != (unsigned int)expected || !released;
}
