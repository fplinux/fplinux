/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_FILES_BROWSE_H
#define FPLINUX_FILES_BROWSE_H

#include <linux/limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>

#define FPLINUX_FILES_NAME_MAX NAME_MAX
#define FPLINUX_FILES_PATH_MAX PATH_MAX
#define FPLINUX_FILES_DIRECTORY_LIMIT 4096

enum fplinux_files_kind {
	FPLINUX_FILES_KIND_DIRECTORY,
	FPLINUX_FILES_KIND_FILE,
	FPLINUX_FILES_KIND_SYMLINK,
	FPLINUX_FILES_KIND_OTHER,
};

struct fplinux_files_entry {
	char *name;
	enum fplinux_files_kind kind;
	off_t size;
	time_t modified;
};

struct fplinux_files_directory {
	struct fplinux_files_entry *entries;
	size_t count;
};

typedef bool (*fplinux_files_cancel_cb)(void *context);

/* Absolute paths reject dot components and never traverse symbolic links. */
int fplinux_files_child_path(const char *parent, const char *name, char *out,
			     size_t capacity);
int fplinux_files_parent_path(const char *path, char *out, size_t capacity);
/* Raw names sort bytewise, with directories first. Failure leaves output empty. */
int fplinux_files_directory_read(const char *path,
				 struct fplinux_files_directory *directory,
				 fplinux_files_cancel_cb cancel, void *context);
/* Scan from the current offset of a readable directory fd, advancing its
 * offset. The caller retains ownership of the fd on success and failure.
 */
int fplinux_files_directory_read_fd(int directory_fd,
				    struct fplinux_files_directory *directory,
				    fplinux_files_cancel_cb cancel,
				    void *context);
void fplinux_files_directory_free(struct fplinux_files_directory *directory);
/* A held directory pins removable storage for ordinary unmount. A nonzero
 * mount_id must match the kernel mount containing the opened directory.
 */
int fplinux_files_directory_open(const char *path, uint32_t mount_id);

/* Shared path validation for Files' mutation operations. */
int fplinux_files_valid_name(const char *name);
int fplinux_files_open_parent(const char *path, char *name, int root_error);

#endif
