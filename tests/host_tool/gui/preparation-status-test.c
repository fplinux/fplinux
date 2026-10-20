/* SPDX-License-Identifier: GPL-2.0-only */
#include "preparation.h"

#include <inttypes.h>
#include <stdio.h>

int main(int argc, char **argv)
{
	struct fplinux_preparation_status status = {
		.stage = FPLINUX_PREPARATION_READY,
		.completed = 41,
		.total = 42,
	};
	bool valid;

	if (argc != 2)
		return 2;
	valid = fplinux_preparation_read(argv[1], &status);
	printf("%s %u %" PRIu64 " %" PRIu64 "\n", valid ? "valid" : "invalid",
	       status.stage, status.completed, status.total);
	return 0;
}
