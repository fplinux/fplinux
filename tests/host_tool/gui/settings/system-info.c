/* SPDX-License-Identifier: GPL-2.0-only */
#include "fplinux-system-info.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
	char value[64];

	assert(argc == 5);
	assert(fplinux_system_info_read_os_name(argv[1], value, sizeof(value)));
	assert(!strcmp(value, "FPLinux 1.0.0"));
	assert(fplinux_system_info_read_os_field(argv[1], "BUILD_ID", value,
						 sizeof(value)));
	assert(!strcmp(value, "012345abcdef"));
	assert(!fplinux_system_info_read_os_field(argv[1], "BUILD_ID", value,
						  12));
	assert(!fplinux_system_info_read_os_field(argv[1], "UNKNOWN", value,
						  sizeof(value)));
	assert(fplinux_system_info_read_os_name(argv[2], value, sizeof(value)));
	assert(!strcmp(value, "FPLinux Rolling"));
	assert(!fplinux_system_info_read_os_name(argv[2], value, 15));
	assert(!fplinux_system_info_read_os_name(argv[3], value,
						 sizeof(value)));
	assert(fplinux_system_info_read_line(argv[4], value, sizeof(value)));
	assert(!strcmp(value, "Phone model"));
	assert(!fplinux_system_info_read_line(argv[4], value, 0));
	assert(!fplinux_system_info_read_line(argv[3], value, sizeof(value)));
	puts("system-info: all checks passed");
	return 0;
}
