/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "fplinux-pm-files.h"
#include "fplinux-pm-vm.h"

#include <fplinux/fplinux-volumes.h>
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

void fplinux_pm_files_volumes(const struct fplinux_volume_snapshot *snapshot)
{
	assert(snapshot->count == 1);
	const struct fplinux_volume *card = &snapshot->volumes[0];

	assert(card->state == FPLINUX_VOLUME_MOUNTED);
	assert(card->mount_id != 0);
	printf("card %s\n", card->path);
}

/* Replace the VM and its volumes consumer; the runner and CLI are real. */
bool fplinux_pm_vm_start(const struct fplinux_pm_vm_config *config)
{
	static const char contents[] = "test record\n";
	int directory, record;
	ssize_t written;

	printf("storage %s\n", config->storage_dir);
	directory = open(config->storage_dir, O_RDONLY | O_DIRECTORY);
	if (directory < 0)
		return false;
	record = openat(directory, "record", O_CREAT | O_EXCL | O_WRONLY, 0600);
	close(directory);
	if (record < 0)
		return false;
	written = write(record, contents, sizeof(contents) - 1);
	return close(record) == 0 && written == (ssize_t)(sizeof(contents) - 1);
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
