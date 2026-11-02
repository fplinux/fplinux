/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_FILES_CORE_H
#define FPLINUX_FILES_CORE_H

#include <fplinux/fplinux-storage.h>
#include <fplinux/fplinux-files-browse.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

typedef void (*fplinux_files_progress_cb)(void *context, off_t bytes,
					  off_t total, bool syncing);

enum fplinux_files_action {
	FPLINUX_FILES_MKDIR,
	FPLINUX_FILES_RENAME,
	FPLINUX_FILES_REMOVE,
	FPLINUX_FILES_COPY,
	FPLINUX_FILES_MOVE,
};

enum fplinux_files_source_state {
	FPLINUX_FILES_SOURCE_INTACT,
	FPLINUX_FILES_SOURCE_REMOVED_UNSYNCED,
	FPLINUX_FILES_SOURCE_REMOVED_DURABLE,
};

enum fplinux_files_step {
	FPLINUX_FILES_EXECUTE,
	FPLINUX_FILES_SYNC_DESTINATION,
	FPLINUX_FILES_UNLINK_SOURCE,
	FPLINUX_FILES_SYNC_SOURCE,
	FPLINUX_FILES_DONE,
};

/*
 * One live operation owns its directory descriptors until close. A retry calls
 * run again on the same object: it continues synchronization/source cleanup,
 * without publishing another copy or switching to another mounted volume.
 */
struct fplinux_files_operation {
	enum fplinux_files_action action;
	enum fplinux_files_step step;
	struct fplinux_storage_result result;
	enum fplinux_files_source_state source_state;
	int source_parent;
	int destination_parent;
	char source_name[FPLINUX_FILES_NAME_MAX + 1];
	char destination_name[FPLINUX_FILES_NAME_MAX + 1];
	struct stat source_metadata;
	bool cross_device;
};

/*
 * destination is a full path for mkdir/copy/move/rename, NULL for remove.
 * Copy/move are regular-file-only. Rename also accepts folders; remove only
 * accepts regular files and empty folders.
 * No destination is replaced. Return -1/errno on error; operation remains
 * closeable. run's result separates visible publication from durability.
 */
int fplinux_files_operation_open(struct fplinux_files_operation *operation,
				 enum fplinux_files_action action,
				 const char *source, const char *destination);
int fplinux_files_operation_run(struct fplinux_files_operation *operation,
				fplinux_files_cancel_cb cancel,
				fplinux_files_progress_cb progress,
				void *context);
void fplinux_files_operation_close(struct fplinux_files_operation *operation);

#endif
