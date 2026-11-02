/* SPDX-License-Identifier: GPL-2.0-only */
#include "fplinux-pm-store.h"
#include "fplinux-pm-vm.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
	struct fplinux_pm_suite suite = { 0 };
	char *path;

	if (argc != 3)
		return 2;
	if (fplinux_pm_jar_read_suite(argv[2], &suite) != FPLINUX_PM_JAR_OK) {
		fputs("invalid suite\n", stderr);
		return 2;
	}
	path = fplinux_pm_store_open(argv[1], &suite, argv[2]);
	if (!path) {
		fprintf(stderr, "storage error %d\n", errno);
		fplinux_pm_suite_cleanup(&suite);
		return 1;
	}
	puts(path);
	free(path);
	fplinux_pm_suite_cleanup(&suite);
	return 0;
}
