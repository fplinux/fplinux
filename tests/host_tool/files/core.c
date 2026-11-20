/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "files-core.h"
#include "files-job.h"
#include "fplinux-utf8.h"

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static unsigned int fail_directory_sync;
static bool fail_source_unlink;
static bool fail_data_write;
static bool reject_directory_read;

static struct directory_replacement {
	const char *path;
	const char *previous;
	const char *replacement;
} directory_replacement;

int __real_fclose(FILE *stream);
int __real_fsync(int descriptor);
struct dirent *__real_readdir(DIR *stream);
int __real_unlinkat(int descriptor, const char *name, int flags);
ssize_t __real_write(int descriptor, const void *buffer, size_t count);

int __wrap_fclose(FILE *stream)
{
	int result = __real_fclose(stream);

	/* Replace the path as the worker finishes reading the mount ID. */
	if (directory_replacement.path) {
		assert(result == 0);
		assert(rename(directory_replacement.path,
			      directory_replacement.previous) == 0);
		assert(rename(directory_replacement.replacement,
			      directory_replacement.path) == 0);
		directory_replacement.path = NULL;
	}
	return result;
}

struct dirent *__wrap_readdir(DIR *stream)
{
	assert(!reject_directory_read);
	return __real_readdir(stream);
}

ssize_t __wrap_write(int descriptor, const void *buffer, size_t count)
{
	struct stat metadata;

	assert(fstat(descriptor, &metadata) == 0);
	if (fail_data_write && S_ISREG(metadata.st_mode)) {
		fail_data_write = false;
		errno = ENOSPC;
		return -1;
	}
	return __real_write(descriptor, buffer, count);
}

int __wrap_fsync(int descriptor)
{
	struct stat metadata;

	assert(fstat(descriptor, &metadata) == 0);
	if (S_ISDIR(metadata.st_mode) && fail_directory_sync &&
	    !--fail_directory_sync) {
		errno = EIO;
		return -1;
	}
	return __real_fsync(descriptor);
}

int __wrap_unlinkat(int descriptor, const char *name, int flags)
{
	if (fail_source_unlink && !strcmp(name, "source")) {
		fail_source_unlink = false;
		errno = EACCES;
		return -1;
	}
	return __real_unlinkat(descriptor, name, flags);
}

static bool cancel(void *context)
{
	return *(bool *)context;
}

static void cancel_after_chunk(void *context, off_t bytes, off_t total,
			       bool syncing)
{
	(void)total;
	(void)syncing;
	if (bytes)
		*(bool *)context = true;
}

static void listing(const char *path)
{
	struct fplinux_files_directory directory = { 0 };
	const char *const names[] = { "a-dir", "z-dir",	  "A.txt", "fifo",
				      "link",  "raw\xff", "source" };
	const enum fplinux_files_kind kinds[] = {
		FPLINUX_FILES_KIND_DIRECTORY, FPLINUX_FILES_KIND_DIRECTORY,
		FPLINUX_FILES_KIND_FILE,      FPLINUX_FILES_KIND_OTHER,
		FPLINUX_FILES_KIND_SYMLINK,   FPLINUX_FILES_KIND_FILE,
		FPLINUX_FILES_KIND_FILE,
	};

	assert(fplinux_files_directory_read(path, &directory, NULL, NULL) == 0);
	assert(directory.count == 7);
	for (size_t i = 0; i < 7; i++) {
		assert(!strcmp(directory.entries[i].name, names[i]));
		assert(directory.entries[i].kind == kinds[i]);
	}
	fplinux_files_directory_free(&directory);
	assert(!directory.entries && !directory.count);
}

static void cancelled_listing(const char *path)
{
	struct fplinux_files_directory directory = { 0 };
	bool cancelled = true;

	assert(fplinux_files_directory_read(path, &directory, cancel,
					    &cancelled) < 0);
	assert(errno == ECANCELED);
	assert(!directory.entries && !directory.count);
}

static void overflowing_listing(const char *path)
{
	struct fplinux_files_directory directory = { 0 };

	assert(fplinux_files_directory_read(path, &directory, NULL, NULL) < 0);
	assert(errno == EOVERFLOW);
	assert(!directory.entries && !directory.count);
}

static void mutate(const char *scenario, const char *source,
		   const char *destination)
{
	struct fplinux_files_operation operation;
	enum fplinux_files_action action = FPLINUX_FILES_COPY;
	int expected_error = 0;
	bool cancelled = false;
	bool retry = false;

	if (!strncmp(scenario, "move", 4))
		action = FPLINUX_FILES_MOVE;
	else if (strstr(scenario, "rename"))
		action = FPLINUX_FILES_RENAME;
	else if (strstr(scenario, "mkdir"))
		action = FPLINUX_FILES_MKDIR;
	else if (strstr(scenario, "remove") || !strcmp(scenario, "nonempty"))
		action = FPLINUX_FILES_REMOVE;
	if (!strcmp(scenario, "conflict"))
		expected_error = EEXIST;
	else if (!strcmp(scenario, "nonempty"))
		expected_error = ENOTEMPTY;
	else if (!strcmp(scenario, "cancel"))
		expected_error = ECANCELED;
	else if (!strcmp(scenario, "space")) {
		fail_data_write = true;
		expected_error = ENOSPC;
	} else if (!strcmp(scenario, "sync") ||
		   !strcmp(scenario, "move_dest_sync") ||
		   !strcmp(scenario, "mkdir_sync") ||
		   !strcmp(scenario, "rename_sync") ||
		   !strcmp(scenario, "remove_sync")) {
		fail_directory_sync = 1;
		expected_error = EIO;
		retry = true;
	} else if (!strcmp(scenario, "move_source_sync")) {
		fail_directory_sync = 2;
		expected_error = EIO;
		retry = true;
	} else if (!strcmp(scenario, "move_unlink")) {
		fail_source_unlink = true;
		expected_error = EACCES;
		retry = true;
	}
	assert(fplinux_files_operation_open(&operation, action, source,
					    action == FPLINUX_FILES_REMOVE ?
						    NULL :
						    destination) == 0);
	if (expected_error) {
		assert(fplinux_files_operation_run(
			       &operation,
			       !strcmp(scenario, "cancel") ? cancel : NULL,
			       !strcmp(scenario, "cancel") ?
				       cancel_after_chunk :
				       NULL,
			       &cancelled) < 0);
		assert(errno == expected_error &&
		       operation.result.error == expected_error);
		if (!strcmp(scenario, "mkdir_sync")) {
			assert(operation.result.state ==
			       FPLINUX_STORAGE_PUBLISHED_UNSYNCED);
			assert(operation.step ==
			       FPLINUX_FILES_SYNC_DESTINATION);
			assert(access(destination, F_OK) == 0);
		} else if (!strcmp(scenario, "rename_sync") ||
			   !strcmp(scenario, "remove_sync")) {
			assert(operation.result.state ==
			       FPLINUX_STORAGE_PUBLISHED_UNSYNCED);
			assert(operation.source_state ==
			       FPLINUX_FILES_SOURCE_REMOVED_UNSYNCED);
			assert(access(source, F_OK) < 0 && errno == ENOENT);
		} else if (!strcmp(scenario, "sync") ||
			   !strcmp(scenario, "move_dest_sync")) {
			assert(operation.result.state ==
			       FPLINUX_STORAGE_PUBLISHED_UNSYNCED);
			assert(operation.step ==
			       FPLINUX_FILES_SYNC_DESTINATION);
			assert(operation.source_state ==
			       FPLINUX_FILES_SOURCE_INTACT);
			assert(access(source, F_OK) == 0 &&
			       access(destination, F_OK) == 0);
		} else if (!strcmp(scenario, "move_unlink")) {
			assert(operation.result.state ==
			       FPLINUX_STORAGE_DURABLE);
			assert(operation.step == FPLINUX_FILES_UNLINK_SOURCE);
			assert(operation.source_state ==
			       FPLINUX_FILES_SOURCE_INTACT);
			assert(access(source, F_OK) == 0 &&
			       access(destination, F_OK) == 0);
		} else if (!strcmp(scenario, "move_source_sync")) {
			assert(operation.result.state ==
			       FPLINUX_STORAGE_DURABLE);
			assert(operation.step == FPLINUX_FILES_SYNC_SOURCE);
			assert(operation.source_state ==
			       FPLINUX_FILES_SOURCE_REMOVED_UNSYNCED);
			assert(access(source, F_OK) < 0 && errno == ENOENT);
			assert(access(destination, F_OK) == 0);
		} else {
			assert(operation.result.state ==
			       FPLINUX_STORAGE_UNPUBLISHED);
			assert(operation.source_state ==
			       FPLINUX_FILES_SOURCE_INTACT);
		}
	}
	if (!expected_error || retry) {
		assert(fplinux_files_operation_run(&operation, NULL, NULL,
						   NULL) == 0);
		assert(operation.result.state == FPLINUX_STORAGE_DURABLE);
		assert(operation.result.error == 0);
		assert(operation.step == FPLINUX_FILES_DONE);
		if (action == FPLINUX_FILES_MOVE ||
		    action == FPLINUX_FILES_RENAME ||
		    action == FPLINUX_FILES_REMOVE)
			assert(operation.source_state ==
			       FPLINUX_FILES_SOURCE_REMOVED_DURABLE);
	}
	fplinux_files_operation_close(&operation);
	assert(operation.source_parent == -1 &&
	       operation.destination_parent == -1);
}

static void rejected(const char *scenario, const char *source,
		     const char *destination)
{
	struct fplinux_files_operation operation;
	int expected = !strcmp(scenario, "fifo") ? EOPNOTSUPP : ELOOP;

	assert(fplinux_files_operation_open(&operation, FPLINUX_FILES_COPY,
					    source, destination) < 0);
	assert(errno == expected);
	fplinux_files_operation_close(&operation);
}

static void name_boundary(const char *path)
{
	char name[257];
	char child[FPLINUX_FILES_PATH_MAX];

	memset(name, 'a', sizeof(name));
	name[255] = '\0';
	assert(fplinux_files_child_path(path, name, child, sizeof(child)) == 0);
	name[255] = 'a';
	name[256] = '\0';
	assert(fplinux_files_child_path(path, name, child, sizeof(child)) < 0);
	assert(errno == ENAMETOOLONG);
	assert(fplinux_files_child_path(path, "raw\nname", child,
					sizeof(child)) == 0);
	assert(strstr(child, "raw\nname"));
}

static void search(void)
{
	char text[1400];

	memset(text, 'a', sizeof(text));
	strcpy(text + 1350, "ДАЛЁКО.txt");
	assert(fplinux_utf8_contains_casefold("ЁЖИК.TXT", "ёжик.txt"));
	assert(fplinux_utf8_contains_casefold(text, "далёко"));
	assert(fplinux_utf8_contains_casefold("abc", ""));
	assert(!fplinux_utf8_contains_casefold("cats", "cars"));
	assert(!fplinux_utf8_contains_casefold("ab", "abc"));
	assert(!fplinux_utf8_contains_casefold("Ёж", "еж"));
	assert(fplinux_utf8_contains_casefold("raw\xff"
					      "A",
					      "\xff"
					      "a"));
}

static struct fplinux_files_job_status
wait_for_job(struct fplinux_files_job *job)
{
	struct pollfd notification = { .fd = job->pipe[0], .events = POLLIN };
	struct fplinux_files_job_status status;

	do {
		assert(poll(&notification, 1, 3000) == 1);
		status = fplinux_files_job_collect(job);
	} while (!status.done);
	return status;
}

static void worker(const char *path)
{
	struct fplinux_files_job job;
	struct fplinux_files_job_status status;

	assert(fplinux_files_job_init(&job) == 0);
	assert(fplinux_files_job_read(&job, path, false, 0) == 0);
	assert(fplinux_files_job_read(&job, path, false, 0) < 0 &&
	       errno == EBUSY);
	status = wait_for_job(&job);
	assert(!job.active && !status.error && job.directory.count == 7);
	assert(job.directory_fd >= 0);
	assert(!fplinux_files_job_collect(&job).done);
	fplinux_files_job_reset(&job);
	assert(!job.directory.count && job.directory_fd == -1);
	fplinux_files_job_close(&job);
}

static void mount_identity(const char *path, uint32_t mount_id)
{
	struct fplinux_files_job job;
	struct fplinux_files_job_status status;
	uint32_t stale_mount_id = mount_id == 1 ? 2 : 1;
	int descriptor = fplinux_files_directory_open(path, mount_id);

	assert(descriptor >= 0 && fcntl(descriptor, F_GETFD) >= 0);
	close(descriptor);
	assert(fplinux_files_directory_open(path, stale_mount_id) < 0 &&
	       errno == ENODEV);
	assert(fplinux_files_job_init(&job) == 0);
	reject_directory_read = true;
	assert(fplinux_files_job_read(&job, path, false, stale_mount_id) == 0);
	status = wait_for_job(&job);
	assert(status.error == ENODEV && !job.directory.entries &&
	       !job.directory.count && job.directory_fd == -1);
	reject_directory_read = false;
	assert(fplinux_files_job_read(&job, path, false, mount_id) == 0);
	status = wait_for_job(&job);
	assert(!status.error && job.directory.count == 7 &&
	       job.directory_fd >= 0);
	descriptor = job.directory_fd;
	assert(fcntl(descriptor, F_GETFD) >= 0);
	fplinux_files_job_close(&job);
	assert(fcntl(descriptor, F_GETFD) < 0 && errno == EBADF);
}

static void replace_checked_directory(const char *path, uint32_t mount_id,
				      const char *previous,
				      const char *replacement)
{
	struct fplinux_files_job job;
	struct fplinux_files_job_status status;

	assert(fplinux_files_job_init(&job) == 0);
	directory_replacement = (struct directory_replacement){
		.path = path,
		.previous = previous,
		.replacement = replacement,
	};
	assert(fplinux_files_job_read(&job, path, false, mount_id) == 0);
	status = wait_for_job(&job);
	assert(!status.error && !directory_replacement.path);
	assert(job.directory.count == 1);
	assert(!strcmp(job.directory.entries[0].name, "checked.txt"));
	fplinux_files_job_close(&job);
}

int main(int argc, char **argv)
{
	assert(argc >= 3);
	if (!strcmp(argv[1], "listing"))
		listing(argv[2]);
	else if (!strcmp(argv[1], "cancel_listing"))
		cancelled_listing(argv[2]);
	else if (!strcmp(argv[1], "overflow"))
		overflowing_listing(argv[2]);
	else if (!strcmp(argv[1], "name"))
		name_boundary(argv[2]);
	else if (!strcmp(argv[1], "search"))
		search();
	else if (!strcmp(argv[1], "worker"))
		worker(argv[2]);
	else if (!strcmp(argv[1], "identity")) {
		assert(argc == 4);
		mount_identity(argv[2], (uint32_t)strtoul(argv[3], NULL, 10));
	} else if (!strcmp(argv[1], "replace_checked_directory")) {
		assert(argc == 6);
		replace_checked_directory(argv[2],
					  (uint32_t)strtoul(argv[3], NULL, 10),
					  argv[4], argv[5]);
	} else {
		assert(argc == 4);
		if (!strcmp(argv[1], "symlink") ||
		    !strcmp(argv[1], "symlink_parent") ||
		    !strcmp(argv[1], "fifo"))
			rejected(argv[1], argv[2], argv[3]);
		else
			mutate(argv[1], argv[2], argv[3]);
	}
	puts("files-core: passed");
	return 0;
}
