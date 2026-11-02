/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "fplinux-pm-vm.h"

#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

/* Replace only the VM; the headless runner and CLI are production code. */
bool fplinux_pm_vm_start(const struct fplinux_pm_vm_config *config)
{
	int directory;

	printf("storage %s\n", config->storage_dir);
	directory = open(config->storage_dir, O_RDONLY | O_DIRECTORY);
	if (directory >= 0) {
		int record =
			openat(directory, "record", O_CREAT | O_WRONLY, 0600);

		if (record >= 0)
			close(record);
		close(directory);
	}
	return true;
}

int fplinux_pm_vm_slice(void)
{
	return FPLINUX_PM_VM_SLICE_EXITED;
}

int fplinux_pm_vm_exit_code(void)
{
	return 0;
}

void fplinux_pm_vm_request_destroy(void)
{
}

void fplinux_pm_vm_post_key(int key_code, int action)
{
	(void)key_code;
	(void)action;
}
