/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_TEST_SDBOOT_FS_H
#define FPLINUX_TEST_SDBOOT_FS_H

#include <errno.h>
#include <sys/types.h>

typedef off_t loff_t;

#define FS_TYPE_FAT 1
int fs_set_blk_dev(const char *interface, const char *device, int type);
int fs_size(const char *path, loff_t *size);
int fs_read(const char *path, unsigned long address, loff_t offset, loff_t size,
	    loff_t *actual);

#endif
