/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_TEST_SDBOOT_PART_H
#define FPLINUX_TEST_SDBOOT_PART_H

struct blk_desc {
	int unused;
};

struct disk_partition {
	int number;
};

int blk_get_device_part_str(const char *interface, const char *device,
			    struct blk_desc **desc,
			    struct disk_partition *partition, int whole_device);

#endif
