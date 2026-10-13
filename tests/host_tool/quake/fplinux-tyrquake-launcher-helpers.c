// SPDX-License-Identifier: GPL-2.0-only

#include <errno.h>
#include <string.h>
#include <unistd.h>

#include "../../../alpine/aports/fplinux-tyrquake/fplinux-quake-internal.h"

int main(int argc, char **argv)
{
	if (argc == 3 && !strcmp(argv[1], "write-controls")) {
		fplinux_quake_write_controls(argv[2]);
		return 0;
	}
	if (argc != 2)
		return 2;
	fplinux_quake_remove_runtime(argv[1]);
	return access(argv[1], F_OK) == -1 && errno == ENOENT ? 0 : 1;
}
