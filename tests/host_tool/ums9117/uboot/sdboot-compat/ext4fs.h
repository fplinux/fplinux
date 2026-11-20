/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_TEST_SDBOOT_EXT4FS_H
#define FPLINUX_TEST_SDBOOT_EXT4FS_H

#include <errno.h>
#include <sys/types.h>

typedef off_t loff_t;

struct blk_desc;
struct disk_partition;

void ext4fs_set_blk_dev(struct blk_desc *device,
			struct disk_partition *partition);
int ext4fs_mount_clean(void);
int ext4fs_open_nofollow(const char *path, loff_t *size);
int ext4fs_read(char *buffer, loff_t offset, loff_t size, loff_t *actual);
void ext4fs_close(void);

#endif
