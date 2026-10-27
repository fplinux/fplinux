/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "vfs-broker.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum sync_fault { SYNC_NONE, SYNC_FILE, SYNC_DIRECTORY };
static enum sync_fault sync_fault;

int __real_fsync(int fd);

/* Replace only the synchronization error; namespace and I/O stay real. */
int __wrap_fsync(int fd)
{
	struct stat status;

	assert(fstat(fd, &status) == 0);
	if ((sync_fault == SYNC_FILE && S_ISREG(status.st_mode)) ||
	    (sync_fault == SYNC_DIRECTORY && S_ISDIR(status.st_mode))) {
		sync_fault = SYNC_NONE;
		errno = EIO;
		return -1;
	}
	return __real_fsync(fd);
}

static struct mre_vfs_response send_request(struct mre_vfs *vfs,
					    struct mre_vfs_request *request)
{
	static uint32_t sequence;
	struct mre_vfs_response response;

	request->sequence = ++sequence;
	mre_vfs_dispatch(vfs, request, &response);
	assert(response.sequence == request->sequence);
	assert(response.error ? response.result == -1 : response.result >= 0);
	return response;
}

static struct mre_vfs_response
path_request(struct mre_vfs *vfs, uint32_t operation, const char *path,
	     const char *destination, uint32_t mode)
{
	struct mre_vfs_request request = { .operation = operation,
					   .mode = mode };

	snprintf(request.path, sizeof(request.path), "%s", path);
	if (destination)
		snprintf(request.path2, sizeof(request.path2), "%s",
			 destination);
	return send_request(vfs, &request);
}

static struct mre_vfs_response
handle_request(struct mre_vfs *vfs, uint32_t operation, int32_t handle)
{
	struct mre_vfs_request request = { .operation = operation,
					   .handle = handle };

	return send_request(vfs, &request);
}

static int32_t open_file(struct mre_vfs *vfs, const char *path, uint32_t mode)
{
	struct mre_vfs_response response =
		path_request(vfs, MRE_VFS_OPEN, path, NULL, mode);

	assert(response.error == 0 && response.result > 0);
	return response.result;
}

static void close_file(struct mre_vfs *vfs, int32_t handle)
{
	assert(handle_request(vfs, MRE_VFS_CLOSE, handle).error == 0);
}

static void file_modes(struct mre_vfs *vfs)
{
	assert(path_request(vfs, MRE_VFS_OPEN, "C:\\missing", NULL, 2).error ==
	       ENOENT);
	assert(path_request(vfs, MRE_VFS_OPEN, "C:\\missing", NULL, 3).error ==
	       EINVAL);
	int32_t file = open_file(vfs, "C:\\save", 4);
	struct mre_vfs_request request = { .operation = MRE_VFS_WRITE,
					   .handle = file,
					   .bytes = 3,
					   .data = { 'a', 'b', 'c' } };
	assert(send_request(vfs, &request).count == 3);
	request.operation = MRE_VFS_SEEK;
	request.base = 1;
	assert(send_request(vfs, &request).error == 0);
	request.operation = MRE_VFS_READ;
	struct mre_vfs_response response = send_request(vfs, &request);
	assert(response.count == 3 && !memcmp(response.data, "abc", 3));
	close_file(vfs, file);
	assert(handle_request(vfs, MRE_VFS_TELL, file).error == EBADF);

	file = open_file(vfs, "C:\\save", 2);
	request = (struct mre_vfs_request){ .operation = MRE_VFS_WRITE,
					    .handle = file,
					    .bytes = 1,
					    .data = { 'Z' } };
	assert(send_request(vfs, &request).count == 1);
	close_file(vfs, file);
	file = open_file(vfs, "C:\\save", 8);
	request.handle = file;
	request.operation = MRE_VFS_SEEK;
	request.base = 1;
	assert(send_request(vfs, &request).error == 0);
	request.operation = MRE_VFS_WRITE;
	request.data[0] = '+';
	assert(send_request(vfs, &request).count == 1);
	close_file(vfs, file);

	assert(path_request(vfs, MRE_VFS_OPEN, "E:\\large", NULL, 1).error ==
	       EFBIG);
	file = open_file(vfs, "E:\\large", 4);
	assert(handle_request(vfs, MRE_VFS_GETSIZE, file).size == 0);
	request = (struct mre_vfs_request){ .operation = MRE_VFS_SEEK,
					    .handle = file,
					    .base = 1,
					    .offset = 16 };
	assert(send_request(vfs, &request).error == 0);
	request.operation = MRE_VFS_WRITE;
	request.bytes = 1;
	request.data[0] = '!';
	assert(send_request(vfs, &request).error == EFBIG);
	request.operation = MRE_VFS_SEEK;
	request.offset = 15;
	assert(send_request(vfs, &request).error == 0);
	request.operation = MRE_VFS_WRITE;
	assert(send_request(vfs, &request).count == 1);
	close_file(vfs, file);

	int32_t handles[16];
	for (size_t i = 0; i < 16; ++i)
		handles[i] = open_file(vfs, "C:\\save", 1);
	assert(path_request(vfs, MRE_VFS_OPEN, "C:\\save", NULL, 1).error ==
	       EMFILE);
	request.handle = handles[0];
	assert(send_request(vfs, &request).error == EACCES);
	close_file(vfs, handles[0]);
	int32_t replacement = open_file(vfs, "C:\\save", 1);
	assert(replacement != handles[0]);
	close_file(vfs, replacement);
	for (size_t i = 1; i < 16; ++i)
		close_file(vfs, handles[i]);
}

static void namespace_rules(struct mre_vfs *vfs)
{
	close_file(vfs, open_file(vfs, "e:\\Данные.txt", 1));
	struct mre_vfs_response first =
		path_request(vfs, MRE_VFS_FIND_OPEN, "E:\\??????.txt", NULL, 0);
	struct mre_vfs_response second =
		path_request(vfs, MRE_VFS_FIND_OPEN, "E:\\??????.txt", NULL, 0);
	assert(first.error == 0 && second.error == 0);
	assert(!strcmp(first.entry.name, "Данные.TXT"));
	assert(!strcmp(second.entry.name, "Данные.TXT"));
	assert(handle_request(vfs, MRE_VFS_FIND_NEXT, first.result).error ==
	       ENOENT);
	assert(handle_request(vfs, MRE_VFS_FIND_CLOSE, first.result).error ==
	       0);
	assert(handle_request(vfs, MRE_VFS_FIND_CLOSE, second.result).error ==
	       0);
	assert(handle_request(vfs, MRE_VFS_FIND_NEXT, first.result).error ==
	       EBADF);
	const char *invalid[] = {
		"E:\\..\\outside", "C:\\.\\outside", "E:\\.MRE-vfs-hidden",
		"E:\\bad.",	   "E:\\bad ",	     "E:\\bad\xff"
	};
	for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
		assert(path_request(vfs, MRE_VFS_OPEN, invalid[i], NULL, 1)
			       .error == EINVAL);
	assert(path_request(vfs, MRE_VFS_OPEN, "E:\\alias", NULL, 1).error ==
	       ELOOP);
	assert(path_request(vfs, MRE_VFS_OPEN, "E:\\link\\save", NULL, 1)
		       .error == ENOTDIR);

	close_file(vfs, open_file(vfs, "E:\\game.VXP", 1));
	assert(path_request(vfs, MRE_VFS_ATTR, "E:\\game.VXP", NULL, 0)
		       .entry.attributes == 0x21);
	assert(path_request(vfs, MRE_VFS_OPEN, "E:\\game.VXP", NULL, 4).error ==
	       EACCES);
	assert(path_request(vfs, MRE_VFS_DELETE, "E:\\game.VXP", NULL, 0)
		       .error == EACCES);
	assert(path_request(vfs, MRE_VFS_RENAME, "E:\\game.VXP", "E:\\ordinary",
			    0)
		       .error == EACCES);
	assert(path_request(vfs, MRE_VFS_RENAME, "E:\\Данные.TXT",
			    "E:\\new.vsm", 0)
		       .error == EACCES);

	struct mre_vfs_response entry =
		path_request(vfs, MRE_VFS_FIND_OPEN, "E:\\*.*", NULL, 0);
	int32_t iterator = entry.result;
	unsigned int found = 0;
	while (!entry.error) {
		if (!strcmp(entry.entry.name, "Данные.TXT"))
			found |= 1;
		else if (!strcmp(entry.entry.name, "game.VXP"))
			found |= 2;
		else
			assert(false);
		entry = handle_request(vfs, MRE_VFS_FIND_NEXT, iterator);
	}
	assert(found == 3 && entry.error == ENOENT);
	assert(handle_request(vfs, MRE_VFS_FIND_CLOSE, iterator).error == 0);
}

static void copy_and_rename(struct mre_vfs *vfs)
{
	struct mre_vfs_response response =
		path_request(vfs, MRE_VFS_COPY, "E:\\source", "C:\\copy", 1);
	assert(response.error == 0 && response.size == 7 &&
	       response.count == 0);
	int directory = mre_vfs_open_directory(vfs, "C:\\");
	struct stat status;
	assert(directory >= 0);
	assert(fstatat(directory, "copy", &status, 0) < 0 && errno == ENOENT);
	close(directory);
	response = path_request(vfs, MRE_VFS_COPY, "E:\\source", "C:\\copy", 0);
	assert(response.error == 0 && response.size == 7 &&
	       response.count == 7);
	assert(path_request(vfs, MRE_VFS_RENAME, "E:\\source", "C:\\moved", 0)
		       .error == EXDEV);
	assert(path_request(vfs, MRE_VFS_RENAME, "C:\\copy", "C:\\renamed", 0)
		       .error == 0);
	assert(path_request(vfs, MRE_VFS_COPY, "C:\\renamed", "E:\\source", 0)
		       .error == EEXIST);
	assert(path_request(vfs, MRE_VFS_DELETE, "C:\\renamed", NULL, 0).error ==
	       0);
	assert(path_request(vfs, MRE_VFS_COPY, "E:\\source", "C:\\result", 0)
		       .error == 0);
}

static void synchronization_failure(struct mre_vfs *vfs, bool copying,
				    bool directory)
{
	struct mre_vfs_response response;

	if (copying) {
		sync_fault = directory ? SYNC_DIRECTORY : SYNC_FILE;
		response = path_request(vfs, MRE_VFS_COPY, "E:\\source",
					"C:\\copied", 0);
	} else {
		int32_t file = open_file(vfs, "C:\\save", 4);
		struct mre_vfs_request request = { .operation = MRE_VFS_WRITE,
						   .handle = file,
						   .bytes = 3,
						   .data = { 'a', 'b', 'c' } };
		assert(send_request(vfs, &request).count == 3);
		sync_fault = directory ? SYNC_DIRECTORY : SYNC_FILE;
		response = handle_request(vfs, MRE_VFS_CLOSE, file);
		assert(handle_request(vfs, MRE_VFS_TELL, file).error == EBADF);
	}
	assert(sync_fault == SYNC_NONE);
	assert(response.error == EIO && response.result == -1);
}

int main(int argc, char **argv)
{
	assert(argc == 5);
	int executable = open(argv[2], O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	int private = open(argv[3], O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	int state = open(argv[4], O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	struct mre_vfs *vfs = mre_vfs_create(executable, private, state, 16);
	assert(executable >= 0 && private >= 0 && state >= 0 && vfs);
	/* The broker owns duplicates, including the protected state-root identity. */
	close(executable);
	close(private);
	close(state);
	if (!strcmp(argv[1], "modes"))
		file_modes(vfs);
	else if (!strcmp(argv[1], "namespace"))
		namespace_rules(vfs);
	else if (!strcmp(argv[1], "copy"))
		copy_and_rename(vfs);
	else {
		bool copying = !strncmp(argv[1], "copy-", 5);
		bool directory = strstr(argv[1], "directory") != NULL;
		synchronization_failure(vfs, copying, directory);
	}
	mre_vfs_destroy(vfs);
	return 0;
}
