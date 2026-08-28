// SPDX-License-Identifier: GPL-2.0-only

#include <errno.h>
#include <unistd.h>

#include "../../alpine/aports/fplinux-tyrquake/fplinux-quake-internal.h"

int main(int argc, char **argv)
{
	if (argc != 2)
		return 2;
	fplinux_quake_remove_runtime(argv[1]);
	return access(argv[1], F_OK) == -1 && errno == ENOENT ? 0 : 1;
}
