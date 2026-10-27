/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "runtime-paths.h"
#include "mre-ipc.h"
#include "text-codec.h"

#include <errno.h>
#include <fcntl.h>
#include <openssl/evp.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static bool normalize_path(const char *path, char normalized[PATH_MAX])
{
	size_t used = 1;

	if (!path || path[0] != '/' || strlen(path) >= PATH_MAX) {
		errno = EINVAL;
		return false;
	}
	normalized[0] = '/';
	while (*path) {
		while (*path == '/')
			++path;
		const char *start = path;
		while (*path && *path != '/')
			++path;
		size_t bytes = (size_t)(path - start);
		if (!bytes || (bytes == 1 && start[0] == '.'))
			continue;
		if (bytes == 2 && !memcmp(start, "..", 2)) {
			while (used > 1 && normalized[used - 1] != '/')
				--used;
			if (used > 1)
				--used;
			continue;
		}
		if (bytes > NAME_MAX || memchr(start, '\\', bytes) ||
		    memchr(start, ':', bytes) ||
		    !mre_utf8_decode((const unsigned char *)start, bytes, NULL,
				     0, NULL)) {
			errno = EINVAL;
			return false;
		}
		for (size_t i = 0; i < bytes; ++i)
			if ((unsigned char)start[i] < 32 || start[i] == 127) {
				errno = EINVAL;
				return false;
			}
		if (used > 1)
			normalized[used++] = '/';
		memcpy(normalized + used, start, bytes);
		used += bytes;
	}
	normalized[used] = '\0';
	return true;
}

static bool same_snapshot_input(const struct stat *first,
				const struct stat *second)
{
	return first->st_dev == second->st_dev &&
	       first->st_ino == second->st_ino &&
	       first->st_size == second->st_size &&
	       first->st_mtim.tv_sec == second->st_mtim.tv_sec &&
	       first->st_mtim.tv_nsec == second->st_mtim.tv_nsec &&
	       first->st_ctim.tv_sec == second->st_ctim.tv_sec &&
	       first->st_ctim.tv_nsec == second->st_ctim.tv_nsec;
}

static int snapshot_executable(int directory, const char *name)
{
	struct stat before, after;
	unsigned char block[16384], extra;
	int input = -1, snapshot = -1;
	int error;

	input = openat(directory, name,
		       O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
	if (input < 0 || fstat(input, &before) < 0)
		goto failed;
	if (!S_ISREG(before.st_mode) || before.st_size < 12 ||
	    before.st_size > MRE_EXECUTABLE_BYTES) {
		errno = ENOEXEC;
		goto failed;
	}
	snapshot =
		memfd_create("mre-original", MFD_CLOEXEC | MFD_ALLOW_SEALING);
	if (snapshot < 0)
		goto failed;
	size_t remaining = (size_t)before.st_size;
	while (remaining) {
		size_t take = remaining < sizeof(block) ? remaining :
							  sizeof(block);
		ssize_t count = read(input, block, take);
		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0) {
			if (!count)
				errno = ESTALE;
			goto failed;
		}
		size_t written = 0;
		while (written < (size_t)count) {
			ssize_t part = write(snapshot, block + written,
					     (size_t)count - written);
			if (part < 0 && errno == EINTR)
				continue;
			if (part <= 0)
				goto failed;
			written += (size_t)part;
		}
		remaining -= (size_t)count;
	}
	ssize_t tail;
	do {
		tail = read(input, &extra, 1);
	} while (tail < 0 && errno == EINTR);
	if (tail != 0 || fstat(input, &after) < 0)
		goto failed;
	if (!same_snapshot_input(&before, &after)) {
		errno = ESTALE;
		goto failed;
	}
	if (lseek(snapshot, 0, SEEK_SET) < 0 ||
	    fcntl(snapshot, F_ADD_SEALS,
		  F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL) < 0)
		goto failed;
	close(input);
	return snapshot;
failed:
	error = errno;
	if (input >= 0)
		close(input);
	if (snapshot >= 0)
		close(snapshot);
	errno = error;
	return -1;
}

bool mre_document_prepare(struct mre_document *document, const char *path)
{
	unsigned char digest[EVP_MAX_MD_SIZE];
	unsigned int digest_bytes;
	char folder[PATH_MAX];
	int error;

	*document =
		(struct mre_document){ .directory_fd = -1, .snapshot_fd = -1 };
	if (!normalize_path(path, document->path))
		return false;
	const char *leaf = strrchr(document->path, '/') + 1;
	size_t bytes = strlen(leaf);
	if (bytes < 5 || bytes + 4 > MRE_VFS_PATH_BYTES ||
	    strcasecmp(leaf + bytes - 4, ".vxp")) {
		errno = ENOEXEC;
		return false;
	}
	memcpy(document->name, leaf, bytes + 1);
	if (!EVP_Digest(document->path, strlen(document->path), digest,
			&digest_bytes, EVP_sha256(), NULL) ||
	    digest_bytes != 32) {
		errno = EIO;
		return false;
	}
	for (size_t i = 0; i < 32; ++i)
		snprintf(document->state_key + i * 2, 3, "%02x", digest[i]);
	memcpy(folder, document->path, strlen(document->path) + 1);
	size_t parent_bytes = (size_t)(leaf - document->path - 1);
	folder[parent_bytes ? parent_bytes : 1] = '\0';
	document->directory_fd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (document->directory_fd < 0)
		return false;
	char *component = folder + 1;
	while (*component) {
		char *slash = strchr(component, '/');
		if (slash)
			*slash = '\0';
		int child =
			openat(document->directory_fd, component,
			       O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
		if (child < 0)
			goto failed;
		close(document->directory_fd);
		document->directory_fd = child;
		if (!slash)
			break;
		component = slash + 1;
	}
	document->snapshot_fd =
		snapshot_executable(document->directory_fd, document->name);
	if (document->snapshot_fd < 0)
		goto failed;
	return true;
failed:
	error = errno;
	mre_document_release(document);
	errno = error;
	return false;
}

void mre_document_release(struct mre_document *document)
{
	if (document->directory_fd >= 0)
		close(document->directory_fd);
	if (document->snapshot_fd >= 0)
		close(document->snapshot_fd);
	document->directory_fd = document->snapshot_fd = -1;
}

int mre_document_open_state(const struct mre_document *document, int state_root)
{
	int apps, folder, error;

	if (mkdirat(state_root, "apps", 0700) < 0 && errno != EEXIST)
		return -1;
	apps = openat(state_root, "apps",
		      O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (apps < 0)
		return -1;
	if (mkdirat(apps, document->state_key, 0700) < 0 && errno != EEXIST) {
		error = errno;
		close(apps);
		errno = error;
		return -1;
	}
	folder = openat(apps, document->state_key,
			O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	error = errno;
	close(apps);
	errno = error;
	return folder;
}
