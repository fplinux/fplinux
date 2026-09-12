/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_TEST_SDBOOT_IMAGE_H
#define FPLINUX_TEST_SDBOOT_IMAGE_H

#include <bootm.h>
#include <linux/libfdt.h>

#define IH_TYPE_KERNEL 2
#define IH_TYPE_FLATDT 8
#define IH_ARCH_ARM 2
#define IH_OS_LINUX 5
#define IH_COMP_NONE 0
#define FIT_HASH_NODENAME "hash"
#define FIT_ALGO_PROP "algo"
#define FIT_VALUE_PROP "value"
#define FIT_KERNEL_PROP "kernel"
#define FIT_FDT_PROP "fdt"
#define FIT_RAMDISK_PROP "ramdisk"
#define FIT_LOADABLE_PROP "loadables"
#define FIT_FPGA_PROP "fpga"

int fit_check_format(const void *fit, ulong size);
int fit_conf_get_node(const void *fit, const char *name);
int fit_conf_get_prop_node_index(const void *fit, int node, const char *name,
				 int index);
int fit_image_get_data(const void *fit, int node, const void **data,
		       size_t *size);
int fit_image_get_load(const void *fit, int node, ulong *load);
int fit_image_get_type(const void *fit, int node, uint8_t *type);
int fit_image_get_arch(const void *fit, int node, uint8_t *arch);
int fit_image_get_comp(const void *fit, int node, uint8_t *comp);

#define fit_get_name fdt_get_name
#define fit_conf_get_prop_node_count fdt_stringlist_count

#endif
