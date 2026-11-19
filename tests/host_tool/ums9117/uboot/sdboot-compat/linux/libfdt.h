/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_TEST_SDBOOT_LIBFDT_H
#define FPLINUX_TEST_SDBOOT_LIBFDT_H

/* ABI declarations for the libfdt runtime supplied by the pinned dtc package. */
#include <endian.h>
#include <stdint.h>
#include <string.h>

int fdt_check_header(const void *fdt);
int fdt_pack(void *fdt);
int fdt_open_into(const void *fdt, void *buf, int bufsize);
int fdt_del_node(void *fdt, int node);
int fdt_delprop(void *fdt, int node, const char *name);
int fdt_setprop(void *fdt, int node, const char *name, const void *val,
		int len);
int fdt_path_offset(const void *fdt, const char *path);
int fdt_subnode_offset(const void *fdt, int parent, const char *name);
const void *fdt_getprop(const void *fdt, int node, const char *name, int *len);
const char *fdt_get_name(const void *fdt, int node, int *len);
int fdt_stringlist_count(const void *fdt, int node, const char *property);

#define fdt_setprop_string(fdt, node, name, value) \
	fdt_setprop(fdt, node, name, value, strlen(value) + 1)

static inline uint32_t fdt_totalsize(const void *fdt)
{
	return be32toh(((const uint32_t *)fdt)[1]);
}

#endif
