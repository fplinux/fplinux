/* SPDX-License-Identifier: 0BSD */
#define _GNU_SOURCE
#include "vfs-broker.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/fs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define VFS_NAME_BYTES 256
#define VFS_TEMP_BYTES 64
#define VFS_TEMP_ATTEMPTS 128
#define VFS_FIND_ENTRY_LIMIT 4096

_Static_assert(sizeof(struct mre_vfs_entry) == 316, "VFS entry wire size");
_Static_assert(sizeof(struct mre_vfs_request) == 4648, "VFS request wire size");
_Static_assert(sizeof(struct mre_vfs_response) == 4436,
	       "VFS response wire size");

struct vfs_file_handle {
	int32_t id;
	int fd;
	off_t position;
	bool writable;
	bool append;
	bool failed;
	int parent_fd;
	char leaf[VFS_NAME_BYTES];
	char staged[VFS_TEMP_BYTES];
};

struct find_handle {
	int32_t id;
	DIR *directory;
	char pattern[VFS_NAME_BYTES];
	unsigned int visited;
};

struct mre_vfs {
	int root_fd;
	int system_fd;
	int state_root_fd;
	struct stat state_root;
	uint32_t budget;
	int32_t next_handle;
	unsigned long temporary_sequence;
	struct vfs_file_handle files[MRE_VFS_FILE_HANDLES];
	struct find_handle finds[MRE_VFS_FIND_HANDLES];
};

static int duplicate_fd(int fd)
{
	int result;

	do {
		result = fcntl(fd, F_DUPFD_CLOEXEC, 3);
	} while (result < 0 && errno == EINTR);
	return result;
}

static int synchronize(int fd)
{
	int result;

	do {
		result = fsync(fd);
	} while (result < 0 && errno == EINTR);
	return result;
}

static bool same_file(const struct stat *first, const struct stat *second)
{
	return first->st_dev == second->st_dev &&
	       first->st_ino == second->st_ino;
}

static bool regular_file(const struct stat *status)
{
	if (!S_ISREG(status->st_mode)) {
		errno = EINVAL;
		return false;
	}
	return true;
}

static bool bounded_size(const struct mre_vfs *vfs, const struct stat *status)
{
	if (status->st_size < 0 || (uintmax_t)status->st_size > vfs->budget) {
		errno = EFBIG;
		return false;
	}
	return true;
}

static bool ascii_case_equal(const char *first, const char *second);

static bool utf8_valid(const unsigned char *text, size_t bytes)
{
	for (size_t i = 0; i < bytes;) {
		uint32_t scalar = text[i++];
		unsigned int remaining;
		uint32_t minimum;

		if (scalar < 0x80)
			continue;
		if (scalar >= 0xc2 && scalar <= 0xdf) {
			remaining = 1;
			minimum = 0x80;
			scalar &= 0x1f;
		} else if (scalar >= 0xe0 && scalar <= 0xef) {
			remaining = 2;
			minimum = 0x800;
			scalar &= 0x0f;
		} else if (scalar >= 0xf0 && scalar <= 0xf4) {
			remaining = 3;
			minimum = 0x10000;
			scalar &= 7;
		} else {
			return false;
		}
		if (remaining > bytes - i)
			return false;
		while (remaining--) {
			unsigned char continuation = text[i++];

			if ((continuation & 0xc0) != 0x80)
				return false;
			scalar = (scalar << 6) | (continuation & 0x3f);
		}
		if (scalar < minimum || scalar > 0x10ffff ||
		    (scalar >= 0xd800 && scalar <= 0xdfff))
			return false;
	}
	return true;
}

static bool component_valid(const char *start, size_t bytes, bool pattern)
{
	static const char staging_prefix[] = ".mre-vfs-";
	bool staging_name = bytes >= sizeof(staging_prefix) - 1;

	if (!bytes || bytes >= VFS_NAME_BYTES ||
	    (bytes == 1 && start[0] == '.') ||
	    (bytes == 2 && start[0] == '.' && start[1] == '.') ||
	    start[bytes - 1] == '.' || start[bytes - 1] == ' ')
		return false;
	if (!utf8_valid((const unsigned char *)start, bytes))
		return false;
	for (size_t i = 0; i < bytes; ++i) {
		unsigned char value = (unsigned char)start[i];

		if (value < 32 || value == 127 || value == '/' ||
		    value == '\\' || value == ':' || value == '"' ||
		    value == '<' || value == '>' || value == '|' ||
		    (!pattern && (value == '*' || value == '?')))
			return false;
		if (i < sizeof(staging_prefix) - 1) {
			unsigned char folded = value >= 'A' && value <= 'Z' ?
						       value + ('a' - 'A') :
						       value;

			if (folded != (unsigned char)staging_prefix[i])
				staging_name = false;
		}
	}
	/* Broker staging names are never part of the application's namespace. */
	return !staging_name;
}

static int normalize_path(const char *path, bool pattern, char *relative)
{
	size_t bytes = strnlen(path, MRE_VFS_PATH_BYTES);
	size_t cursor = 3;
	size_t output = 0;

	memset(relative, 0, MRE_VFS_PATH_BYTES);
	if (bytes >= MRE_VFS_PATH_BYTES || bytes < 3 || path[1] != ':' ||
	    path[2] != '\\' ||
	    (path[0] != 'E' && path[0] != 'e' && path[0] != 'C' &&
	     path[0] != 'c')) {
		errno = EINVAL;
		return -1;
	}
	/* The internal prefix selects a pinned directory, never a host path. */
	relative[output++] = path[0] == 'C' || path[0] == 'c' ? 'C' : 'E';
	relative[output++] = '/';
	while (cursor < bytes) {
		size_t start = cursor;

		while (cursor < bytes && path[cursor] != '\\')
			++cursor;
		if (!component_valid(path + start, cursor - start,
				     pattern && cursor == bytes)) {
			errno = EINVAL;
			return -1;
		}
		if (output && relative[output - 1] != '/')
			relative[output++] = '/';
		memcpy(relative + output, path + start, cursor - start);
		output += cursor - start;
		if (cursor < bytes)
			++cursor;
	}
	relative[output] = '\0';
	return 0;
}

static const char *root_relative(const char *relative)
{
	return relative + 2;
}

static int path_root(const struct mre_vfs *vfs, const char *relative)
{
	return relative[0] == 'C' ? vfs->system_fd : vfs->root_fd;
}

static bool is_root(const char *relative)
{
	return !*root_relative(relative);
}

/* The state root may occur anywhere below E:, under any directory name.
 * Compare pinned identities, including every ancestor that a rename could move.
 */
static int state_ancestor(struct mre_vfs *vfs, const struct stat *candidate)
{
	int current = duplicate_fd(vfs->state_root_fd);
	int result = -1;

	if (current < 0)
		return -1;
	for (;;) {
		struct stat child, parent;
		int next;
		if (fstat(current, &child) < 0)
			break;
		if (same_file(candidate, &child)) {
			result = 1;
			break;
		}
		next = openat(current, "..",
			      O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
		if (next < 0)
			break;
		if (fstat(next, &parent) < 0) {
			close(next);
			break;
		}
		if (same_file(&child, &parent)) {
			close(next);
			result = 0;
			break;
		}
		close(current);
		current = next;
	}
	int error = errno;
	close(current);
	errno = error;
	return result;
}

bool mre_vfs_entry_visible(struct mre_vfs *vfs, int directory_fd,
			   const char *name)
{
	struct stat status;

	if (fstatat(directory_fd, name, &status, AT_SYMLINK_NOFOLLOW) < 0)
		return false;
	if (same_file(&status, &vfs->state_root)) {
		errno = EACCES;
		return false;
	}
	return true;
}

/* A FAT card supplies case folding itself. The private POSIX directory needs
 * the same ASCII SDK spelling fallback, without choosing an ambiguous alias.
 */
static int resolve_name(int parent_fd, const char *name, char *actual)
{
	struct stat status;
	DIR *directory;
	int directory_fd;
	unsigned int visited = 0;
	bool matched = false;
	int error = 0;

	memcpy(actual, name, strlen(name) + 1);
	if (fstatat(parent_fd, name, &status, AT_SYMLINK_NOFOLLOW) == 0)
		return 0;
	if (errno != ENOENT)
		return -1;
	directory_fd = openat(parent_fd, ".",
			      O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (directory_fd < 0)
		return -1;
	directory = fdopendir(directory_fd);
	if (!directory) {
		error = errno;
		close(directory_fd);
		errno = error;
		return -1;
	}
	for (;;) {
		struct dirent *found;

		errno = 0;
		found = readdir(directory);
		if (!found) {
			error = errno;
			break;
		}
		if (++visited > VFS_FIND_ENTRY_LIMIT) {
			error = EOVERFLOW;
			break;
		}
		if (!ascii_case_equal(name, found->d_name))
			continue;
		if (matched) {
			error = EEXIST;
			break;
		}
		memcpy(actual, found->d_name, strlen(found->d_name) + 1);
		matched = true;
	}
	if (closedir(directory) < 0 && !error)
		error = errno;
	if (error) {
		errno = error;
		return -1;
	}
	return 0;
}

/* Walk each ancestor from the pinned private directory without following any
 * symlink. The returned parent descriptor owns the final path operation.
 */
static int path_parent(struct mre_vfs *vfs, const char *relative, char *leaf)
{
	char components[MRE_VFS_PATH_BYTES];
	char *component;
	int parent_fd = duplicate_fd(path_root(vfs, relative));
	bool external = relative[0] == 'E';

	if (parent_fd < 0)
		return -1;
	relative = root_relative(relative);
	memcpy(components, relative, strlen(relative) + 1);
	component = components;
	for (;;) {
		char *separator = strchr(component, '/');
		int child_fd;
		char actual[VFS_NAME_BYTES];

		if (!separator) {
			if (resolve_name(parent_fd, component, leaf) < 0) {
				int error = errno;

				close(parent_fd);
				errno = error;
				return -1;
			}
			if (external &&
			    !mre_vfs_entry_visible(vfs, parent_fd, leaf) &&
			    errno != ENOENT) {
				close(parent_fd);
				errno = EACCES;
				return -1;
			}
			return parent_fd;
		}
		*separator = '\0';
		if (resolve_name(parent_fd, component, actual) < 0) {
			int error = errno;

			close(parent_fd);
			errno = error;
			return -1;
		}
		if (external &&
		    !mre_vfs_entry_visible(vfs, parent_fd, actual)) {
			close(parent_fd);
			errno = EACCES;
			return -1;
		}
		do {
			child_fd = openat(parent_fd, actual,
					  O_RDONLY | O_DIRECTORY | O_NOFOLLOW |
						  O_CLOEXEC);
		} while (child_fd < 0 && errno == EINTR);
		if (child_fd < 0) {
			int error = errno;

			close(parent_fd);
			errno = error;
			return -1;
		}
		close(parent_fd);
		parent_fd = child_fd;
		component = separator + 1;
	}
}

static int stat_leaf(int parent_fd, const char *leaf, struct stat *status)
{
	if (parent_fd < 0) {
		errno = EBADF;
		return -1;
	}
	if (fstatat(parent_fd, leaf, status, AT_SYMLINK_NOFOLLOW) < 0)
		return -1;
	if (!S_ISREG(status->st_mode) && !S_ISDIR(status->st_mode)) {
		errno = EINVAL;
		return -1;
	}
	return 0;
}

static int path_stat(struct mre_vfs *vfs, const char *relative,
		     struct stat *status)
{
	char leaf[VFS_NAME_BYTES];
	int parent_fd;
	int result;
	int error;

	if (is_root(relative))
		return fstat(path_root(vfs, relative), status);
	parent_fd = path_parent(vfs, relative, leaf);
	if (parent_fd < 0)
		return -1;
	result = stat_leaf(parent_fd, leaf, status);
	error = errno;
	close(parent_fd);
	errno = error;
	return result;
}

static bool ascii_case_equal(const char *first, const char *second)
{
	while (*first && *second) {
		unsigned char a = (unsigned char)*first++;
		unsigned char b = (unsigned char)*second++;

		if (a >= 'A' && a <= 'Z')
			a += 'a' - 'A';
		if (b >= 'A' && b <= 'Z')
			b += 'a' - 'A';
		if (a != b)
			return false;
	}
	return !*first && !*second;
}

static bool executable_name(const char *name)
{
	size_t bytes = strlen(name);

	return bytes >= 4 && (ascii_case_equal(name + bytes - 4, ".vxp") ||
			      ascii_case_equal(name + bytes - 4, ".vsm"));
}

static int authorize_mutation(struct mre_vfs *vfs, const char *relative)
{
	if (executable_name(relative)) {
		errno = EACCES;
		return -1;
	}
	if (relative[0] == 'E') {
		struct stat status;
		int result = path_stat(vfs, relative, &status);
		if (result == 0 && S_ISDIR(status.st_mode)) {
			int protected = state_ancestor(vfs, &status);
			if (protected != 0) {
				if (protected > 0)
					errno = EACCES;
				return -1;
			}
		} else if (result < 0 && errno != ENOENT) {
			return -1;
		}
	}
	return 0;
}

static int open_regular_checked(struct mre_vfs *vfs, int parent_fd,
				const char *leaf, bool writable, bool truncate)
{
	struct stat status = { 0 };
	int fd;

	if (parent_fd < 0) {
		errno = EBADF;
		return -1;
	}
	do {
		fd = openat(parent_fd, leaf,
			    (writable ? O_RDWR : O_RDONLY) | O_NOFOLLOW |
				    O_NONBLOCK | O_CLOEXEC);
	} while (fd < 0 && errno == EINTR);
	if (fd < 0)
		return -1;
	if (fstat(fd, &status) < 0 || !regular_file(&status) ||
	    (!truncate && !bounded_size(vfs, &status)) ||
	    (writable && (!(status.st_mode & 0222) || status.st_nlink != 1))) {
		int error = errno;

		if (writable && S_ISREG(status.st_mode) &&
		    (!(status.st_mode & 0222) || status.st_nlink != 1))
			error = EACCES;
		close(fd);
		errno = error;
		return -1;
	}
	/* CREATE_ALWAYS can replace an oversized ordinary file without reading or
	 * admitting its old body into the bounded SDK namespace.
	 */
	if (truncate && ftruncate(fd, 0) < 0) {
		int error = errno;

		close(fd);
		errno = error;
		return -1;
	}
	return fd;
}

static int open_regular(struct mre_vfs *vfs, int parent_fd, const char *leaf,
			bool writable)
{
	return open_regular_checked(vfs, parent_fd, leaf, writable, false);
}

static int create_staged(struct mre_vfs *vfs, int parent_fd, char *name)
{
	for (unsigned int attempt = 0; attempt < VFS_TEMP_ATTEMPTS; ++attempt) {
		int fd;
		int bytes = snprintf(name, VFS_TEMP_BYTES,
				     ".mre-vfs-%ld-%lu.tmp", (long)getpid(),
				     ++vfs->temporary_sequence);

		if (bytes < 0 || bytes >= VFS_TEMP_BYTES) {
			errno = ENAMETOOLONG;
			return -1;
		}
		do {
			fd = openat(parent_fd, name,
				    O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW |
					    O_CLOEXEC,
				    0600);
		} while (fd < 0 && errno == EINTR);
		if (fd >= 0 || errno != EEXIST)
			return fd;
	}
	errno = EEXIST;
	return -1;
}

static int write_all(int fd, const unsigned char *data, size_t bytes,
		     uint32_t *completed)
{
	*completed = 0;
	while (bytes) {
		ssize_t result = write(fd, data, bytes);

		if (result < 0 && errno == EINTR)
			continue;
		if (result <= 0) {
			if (!result)
				errno = EIO;
			return -1;
		}
		data += result;
		bytes -= (size_t)result;
		*completed += (uint32_t)result;
	}
	return 0;
}

static int copy_data(int source_fd, int destination_fd, uint32_t budget,
		     uint32_t *completed)
{
	unsigned char buffer[MRE_VFS_TRANSFER_BYTES];

	*completed = 0;
	for (;;) {
		ssize_t bytes = read(source_fd, buffer, sizeof(buffer));
		uint32_t written;

		if (bytes < 0 && errno == EINTR)
			continue;
		if (bytes < 0)
			return -1;
		if (!bytes)
			return 0;
		if ((uint32_t)bytes > budget - *completed) {
			errno = EFBIG;
			return -1;
		}
		if (write_all(destination_fd, buffer, (size_t)bytes, &written) <
		    0)
			return -1;
		*completed += written;
	}
}

static struct vfs_file_handle *find_file(struct mre_vfs *vfs, int32_t id)
{
	for (size_t i = 0; i < MRE_VFS_FILE_HANDLES; ++i)
		if (vfs->files[i].id == id && id > 0)
			return &vfs->files[i];
	errno = EBADF;
	return NULL;
}

static struct find_handle *find_iterator(struct mre_vfs *vfs, int32_t id)
{
	for (size_t i = 0; i < MRE_VFS_FIND_HANDLES; ++i)
		if (vfs->finds[i].id == id && id > 0)
			return &vfs->finds[i];
	errno = EBADF;
	return NULL;
}

static int32_t allocate_id(struct mre_vfs *vfs)
{
	if (vfs->next_handle == INT32_MAX) {
		errno = EOVERFLOW;
		return -1;
	}
	return ++vfs->next_handle;
}

static void clear_file(struct vfs_file_handle *file)
{
	int error = errno;

	if (file->fd >= 0)
		close(file->fd);
	if (file->parent_fd >= 0) {
		if (*file->staged)
			unlinkat(file->parent_fd, file->staged, 0);
		close(file->parent_fd);
	}
	memset(file, 0, sizeof(*file));
	file->fd = -1;
	file->parent_fd = -1;
	errno = error;
}

static int open_file(struct mre_vfs *vfs, const struct mre_vfs_request *request)
{
	char relative[MRE_VFS_PATH_BYTES];
	struct vfs_file_handle *file = NULL;
	struct stat status;
	bool writable = request->mode != 1;

	if (request->mode != 1 && request->mode != 2 && request->mode != 4 &&
	    request->mode != 8) {
		errno = EINVAL;
		return -1;
	}
	if (normalize_path(request->path, false, relative) < 0)
		return -1;
	if (is_root(relative)) {
		errno = EISDIR;
		return -1;
	}
	if (writable && authorize_mutation(vfs, relative) < 0)
		return -1;
	for (size_t i = 0; i < MRE_VFS_FILE_HANDLES; ++i)
		if (!vfs->files[i].id) {
			file = &vfs->files[i];
			break;
		}
	if (!file) {
		errno = EMFILE;
		return -1;
	}
	file->parent_fd = path_parent(vfs, relative, file->leaf);
	if (file->parent_fd < 0)
		return -1;
	file->writable = writable;
	file->append = request->mode == 8;
	if (request->mode == 4) {
		file->fd = openat(file->parent_fd, file->leaf,
				  O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW |
					  O_CLOEXEC,
				  0600);
		if (file->fd < 0 && errno == EEXIST) {
			file->fd = open_regular_checked(vfs, file->parent_fd,
							file->leaf, true, true);
		}
	} else {
		file->fd = open_regular(vfs, file->parent_fd, file->leaf,
					writable);
	}
	if (file->fd < 0)
		goto failure;
	if (fstat(file->fd, &status) < 0 || !regular_file(&status) ||
	    !bounded_size(vfs, &status))
		goto failure;
	file->position = file->append ? status.st_size : 0;
	file->id = allocate_id(vfs);
	if (file->id < 0)
		goto failure;
	return file->id;

failure:
	clear_file(file);
	return -1;
}

static int publish_copy(struct vfs_file_handle *file)
{
	if (synchronize(file->fd) < 0)
		return -1;
	/* On Linux, close releases the FD even when reporting an I/O error. */
	if (close(file->fd) < 0) {
		file->fd = -1;
		return -1;
	}
	file->fd = -1;
	if (renameat2(file->parent_fd, file->staged, file->parent_fd,
		      file->leaf, RENAME_NOREPLACE) < 0)
		return -1;
	file->staged[0] = '\0';
	return synchronize(file->parent_fd);
}

static int close_file(struct mre_vfs *vfs, int32_t id)
{
	struct vfs_file_handle *file = find_file(vfs, id);
	int result = 0;
	int error;

	if (!file)
		return -1;
	if (file->writable && synchronize(file->fd) < 0)
		result = -1;
	if (result == 0 && file->writable && synchronize(file->parent_fd) < 0)
		result = -1;
	error = errno;
	if (close(file->fd) < 0 && result == 0) {
		result = -1;
		error = errno;
	}
	file->fd = -1;
	clear_file(file);
	errno = error;
	return result;
}

static int transfer_file(struct mre_vfs *vfs,
			 const struct mre_vfs_request *request,
			 struct mre_vfs_response *response)
{
	struct vfs_file_handle *file = find_file(vfs, request->handle);
	struct stat status;
	bool writing = request->operation == MRE_VFS_WRITE;
	uint32_t done = 0;

	if (!file)
		return -1;
	if (request->bytes > MRE_VFS_TRANSFER_BYTES) {
		errno = EINVAL;
		return -1;
	}
	if (writing && (!file->writable || file->failed)) {
		errno = EACCES;
		return -1;
	}
	if (fstat(file->fd, &status) < 0 || !bounded_size(vfs, &status))
		return -1;
	if (writing && file->append)
		file->position = status.st_size;
	if (writing &&
	    (file->position > vfs->budget ||
	     request->bytes > vfs->budget - (uint32_t)file->position)) {
		errno = EFBIG;
		return -1;
	}
	while (done < request->bytes) {
		ssize_t bytes;

		if (writing)
			bytes = pwrite(file->fd, request->data + done,
				       request->bytes - done, file->position);
		else
			bytes = pread(file->fd, response->data + done,
				      request->bytes - done, file->position);
		if (bytes < 0 && errno == EINTR)
			continue;
		if (bytes < 0 || (writing && !bytes)) {
			if (!bytes)
				errno = EIO;
			if (writing)
				file->failed = true;
			response->count = done;
			return -1;
		}
		if (!bytes)
			break;
		done += (uint32_t)bytes;
		file->position += bytes;
	}
	response->count = done;
	return (int)done;
}

static int handle_position(struct mre_vfs *vfs,
			   const struct mre_vfs_request *request,
			   struct mre_vfs_response *response)
{
	struct vfs_file_handle *file = find_file(vfs, request->handle);
	struct stat status;
	int64_t position;

	if (!file)
		return -1;
	if (fstat(file->fd, &status) < 0 || !bounded_size(vfs, &status))
		return -1;
	response->size = (uint32_t)status.st_size;
	if (request->operation == MRE_VFS_GETSIZE)
		return 0;
	if (request->operation == MRE_VFS_TELL) {
		response->position = (int32_t)file->position;
		return response->position;
	}
	if (request->base == 1)
		position = request->offset;
	else if (request->base == 2)
		position = (int64_t)file->position + request->offset;
	else if (request->base == 3)
		position = (int64_t)status.st_size + request->offset;
	else {
		errno = EINVAL;
		return -1;
	}
	if (position < 0 || position > INT32_MAX || position > vfs->budget) {
		errno = EINVAL;
		return -1;
	}
	file->position = position;
	response->position = (int32_t)position;
	return 0;
}

static uint32_t file_attributes(const struct stat *status)
{
	if (S_ISDIR(status->st_mode))
		return 0x10;
	return 0x20 | (status->st_mode & 0222 ? 0 : 1);
}

static void time_fields(time_t value, int32_t fields[6])
{
	struct tm result;

	if (!localtime_r(&value, &result))
		return;
	fields[0] = result.tm_year + 1900;
	fields[1] = result.tm_mon + 1;
	fields[2] = result.tm_mday;
	fields[3] = result.tm_hour;
	fields[4] = result.tm_min;
	fields[5] = result.tm_sec;
}

static int metadata(struct mre_vfs *vfs, const char *relative,
		    struct mre_vfs_entry *entry)
{
	struct stat status;
	const char *leaf = strrchr(relative, '/');

	if (path_stat(vfs, relative, &status) < 0)
		return -1;
	if (S_ISREG(status.st_mode) && !bounded_size(vfs, &status))
		return -1;
	leaf = leaf ? leaf + 1 : relative;
	memcpy(entry->name, leaf, strlen(leaf) + 1);
	entry->size = S_ISREG(status.st_mode) ? (uint32_t)status.st_size : 0;
	entry->attributes = file_attributes(&status);
	if (executable_name(relative))
		entry->attributes |= 1;
	/* POSIX ctime is the available status-change time, not a birth time. */
	time_fields(status.st_ctime, entry->create_time);
	time_fields(status.st_mtime, entry->modify_time);
	return 0;
}

static int change_attributes(struct mre_vfs *vfs,
			     const struct mre_vfs_request *request,
			     const char *relative)
{
	char leaf[VFS_NAME_BYTES];
	struct stat status;
	int parent_fd;
	int fd;
	int result;
	int error;

	if (is_root(relative) || (request->attributes & ~(uint32_t)0x21)) {
		errno = EACCES;
		return -1;
	}
	if (authorize_mutation(vfs, relative) < 0)
		return -1;
	parent_fd = path_parent(vfs, relative, leaf);
	if (parent_fd < 0)
		return -1;
	fd = open_regular(vfs, parent_fd, leaf, false);
	error = errno;
	close(parent_fd);
	errno = error;
	if (fd < 0)
		return -1;
	result = fstat(fd, &status);
	if (result == 0 && status.st_nlink != 1) {
		errno = EACCES;
		result = -1;
	}
	if (result == 0) {
		mode_t mode = status.st_mode & 0777;

		mode = request->attributes & 1 ? mode & ~0222 : mode | 0200;
		result = fchmod(fd, mode);
	}
	if (result == 0)
		result = synchronize(fd);
	error = errno;
	if (close(fd) < 0 && result == 0) {
		result = -1;
		error = errno;
	}
	errno = error;
	return result;
}

static int mutate_path(struct mre_vfs *vfs,
		       const struct mre_vfs_request *request,
		       const char *relative)
{
	char leaf[VFS_NAME_BYTES];
	struct stat status;
	int parent_fd;
	int result;
	int error;

	if (is_root(relative)) {
		errno = EACCES;
		return -1;
	}
	if (authorize_mutation(vfs, relative) < 0)
		return -1;
	parent_fd = path_parent(vfs, relative, leaf);
	if (parent_fd < 0)
		return -1;
	if (request->operation == MRE_VFS_MKDIR) {
		result = mkdirat(parent_fd, leaf, 0700);
	} else if (stat_leaf(parent_fd, leaf, &status) < 0) {
		result = -1;
	} else if (request->operation == MRE_VFS_RMDIR) {
		if (!S_ISDIR(status.st_mode)) {
			errno = ENOTDIR;
			result = -1;
		} else {
			result = unlinkat(parent_fd, leaf, AT_REMOVEDIR);
		}
	} else if (!S_ISREG(status.st_mode)) {
		errno = EISDIR;
		result = -1;
	} else {
		result = unlinkat(parent_fd, leaf, 0);
	}
	if (result == 0)
		result = synchronize(parent_fd);
	error = errno;
	close(parent_fd);
	errno = error;
	return result;
}

static int copy_or_rename(struct mre_vfs *vfs,
			  const struct mre_vfs_request *request,
			  struct mre_vfs_response *response)
{
	char source[MRE_VFS_PATH_BYTES];
	char destination[MRE_VFS_PATH_BYTES];
	char source_leaf[VFS_NAME_BYTES];
	struct stat source_status;
	struct stat destination_status;
	struct vfs_file_handle transaction = { .fd = -1, .parent_fd = -1 };
	int source_parent = -1;
	int source_fd = -1;
	int result = -1;
	int error;
	bool copying = request->operation == MRE_VFS_COPY;

	if (normalize_path(request->path, false, source) < 0)
		return -1;
	if (copying && request->mode > 1) {
		errno = EINVAL;
		return -1;
	}
	if (normalize_path(request->path2, false, destination) < 0)
		return -1;
	if (is_root(source) || is_root(destination)) {
		errno = EACCES;
		return -1;
	}
	if (!copying && source[0] != destination[0]) {
		errno = EXDEV;
		return -1;
	}
	if (authorize_mutation(vfs, destination) < 0 ||
	    (!copying && authorize_mutation(vfs, source) < 0))
		return -1;
	source_parent = path_parent(vfs, source, source_leaf);
	if (source_parent < 0)
		goto cleanup;
	if (stat_leaf(source_parent, source_leaf, &source_status) < 0)
		goto cleanup;
	transaction.parent_fd = path_parent(vfs, destination, transaction.leaf);
	if (transaction.parent_fd < 0)
		goto cleanup;
	if (!copying) {
		result = renameat2(source_parent, source_leaf,
				   transaction.parent_fd, transaction.leaf,
				   RENAME_NOREPLACE);
		if (result == 0)
			result = synchronize(transaction.parent_fd);
		if (result == 0)
			result = synchronize(source_parent);
		goto cleanup;
	}
	if (!S_ISREG(source_status.st_mode)) {
		errno = EISDIR;
		goto cleanup;
	}
	source_fd = open_regular(vfs, source_parent, source_leaf, false);
	if (source_fd < 0)
		goto cleanup;
	if (fstat(source_fd, &source_status) < 0 ||
	    !bounded_size(vfs, &source_status))
		goto cleanup;
	response->size = (uint32_t)source_status.st_size;
	if (fstatat(transaction.parent_fd, transaction.leaf,
		    &destination_status, AT_SYMLINK_NOFOLLOW) == 0) {
		errno = EEXIST;
		goto cleanup;
	}
	if (errno != ENOENT)
		goto cleanup;
	if (request->mode == 1) {
		if (close(source_fd) < 0) {
			source_fd = -1;
			goto cleanup;
		}
		source_fd = -1;
		result = 0;
		goto cleanup;
	}
	transaction.fd =
		create_staged(vfs, transaction.parent_fd, transaction.staged);
	if (transaction.fd < 0)
		goto cleanup;
	if (copy_data(source_fd, transaction.fd, vfs->budget,
		      &response->count) < 0)
		goto cleanup;
	if (close(source_fd) < 0) {
		source_fd = -1;
		goto cleanup;
	}
	source_fd = -1;
	result = publish_copy(&transaction);

cleanup:
	error = errno;
	if (source_fd >= 0)
		close(source_fd);
	if (source_parent >= 0)
		close(source_parent);
	clear_file(&transaction);
	errno = error;
	return result;
}

static const char *next_character(const char *text)
{
	++text;
	while (((unsigned char)*text & 0xc0) == 0x80)
		++text;
	return text;
}

static unsigned char ascii_fold(unsigned char value)
{
	return value >= 'A' && value <= 'Z' ? value + ('a' - 'A') : value;
}

static bool name_matches(const char *pattern, const char *name)
{
	const char *star = NULL;
	const char *retry = NULL;

	/* DOS *.* includes extensionless files and directories. */
	if (strcmp(pattern, "*.*") == 0)
		pattern = "*";
	while (*name) {
		if (*pattern == '?') {
			++pattern;
			name = next_character(name);
		} else if (*pattern == *name ||
			   ascii_fold((unsigned char)*pattern) ==
				   ascii_fold((unsigned char)*name)) {
			++pattern;
			++name;
		} else if (*pattern == '*') {
			star = ++pattern;
			retry = name;
		} else if (star) {
			pattern = star;
			retry = next_character(retry);
			name = retry;
		} else {
			return false;
		}
	}
	while (*pattern == '*')
		++pattern;
	return !*pattern;
}

static int next_entry(struct mre_vfs *vfs, struct find_handle *iterator,
		      struct mre_vfs_entry *entry)
{
	for (;;) {
		struct dirent *found;
		struct stat status;
		size_t bytes;

		errno = 0;
		found = readdir(iterator->directory);
		if (!found) {
			if (!errno)
				errno = ENOENT;
			return -1;
		}
		if (++iterator->visited > VFS_FIND_ENTRY_LIMIT) {
			errno = EOVERFLOW;
			return -1;
		}
		bytes = strlen(found->d_name);
		if (!component_valid(found->d_name, bytes, false))
			continue;
		if (stat_leaf(dirfd(iterator->directory), found->d_name,
			      &status) < 0)
			continue;
		if (same_file(&status, &vfs->state_root))
			continue;
		if (!name_matches(iterator->pattern, found->d_name))
			continue;
		if (S_ISREG(status.st_mode) && !bounded_size(vfs, &status))
			continue;
		memcpy(entry->name, found->d_name, bytes + 1);
		entry->size =
			S_ISREG(status.st_mode) ? (uint32_t)status.st_size : 0;
		entry->attributes = file_attributes(&status);
		if (executable_name(found->d_name))
			entry->attributes |= 1;
		time_fields(status.st_ctime, entry->create_time);
		time_fields(status.st_mtime, entry->modify_time);
		return 0;
	}
}

static int open_iterator(struct mre_vfs *vfs, const char *path,
			 struct mre_vfs_entry *entry)
{
	char relative[MRE_VFS_PATH_BYTES];
	char leaf[VFS_NAME_BYTES];
	struct find_handle *iterator = NULL;
	int parent_fd;
	int32_t id;

	if (normalize_path(path, true, relative) < 0)
		return -1;
	if (is_root(relative)) {
		errno = EACCES;
		return -1;
	}
	for (size_t i = 0; i < MRE_VFS_FIND_HANDLES; ++i)
		if (!vfs->finds[i].id) {
			iterator = &vfs->finds[i];
			break;
		}
	if (!iterator) {
		errno = EMFILE;
		return -1;
	}
	parent_fd = path_parent(vfs, relative, leaf);
	if (parent_fd < 0)
		return -1;
	/* dup() would share the root directory's enumeration offset. Reopen the
	 * pinned directory itself so independent iterators have separate cursors.
	 */
	int directory_fd =
		openat(parent_fd, ".",
		       O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	int error = errno;

	close(parent_fd);
	errno = error;
	if (directory_fd < 0)
		return -1;
	iterator->directory = fdopendir(directory_fd);
	if (!iterator->directory) {
		error = errno;
		close(directory_fd);
		errno = error;
		return -1;
	}
	memcpy(iterator->pattern, leaf, strlen(leaf) + 1);
	iterator->visited = 0;
	id = allocate_id(vfs);
	if (id < 0 || next_entry(vfs, iterator, entry) < 0) {
		error = errno;
		closedir(iterator->directory);
		iterator->directory = NULL;
		errno = error;
		return -1;
	}
	iterator->id = id;
	return id;
}

static int close_iterator(struct mre_vfs *vfs, int32_t id)
{
	struct find_handle *iterator = find_iterator(vfs, id);
	int result;

	if (!iterator)
		return -1;
	result = closedir(iterator->directory);
	memset(iterator, 0, sizeof(*iterator));
	return result;
}

int mre_vfs_open_directory(struct mre_vfs *vfs, const char *virtual_path)
{
	char relative[MRE_VFS_PATH_BYTES];
	char leaf[VFS_NAME_BYTES];
	int parent_fd;
	int fd;
	int error;

	if (!vfs || !virtual_path) {
		errno = EACCES;
		return -1;
	}
	if (normalize_path(virtual_path, false, relative) < 0)
		return -1;
	if (is_root(relative))
		return openat(path_root(vfs, relative), ".",
			      O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	parent_fd = path_parent(vfs, relative, leaf);
	if (parent_fd < 0)
		return -1;
	do {
		fd = openat(parent_fd, leaf,
			    O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	} while (fd < 0 && errno == EINTR);
	error = errno;
	close(parent_fd);
	errno = error;
	return fd;
}

int mre_vfs_open_readonly(struct mre_vfs *vfs, const char *virtual_path)
{
	char relative[MRE_VFS_PATH_BYTES];
	char leaf[VFS_NAME_BYTES];
	int parent_fd;
	int fd;
	int error;

	if (!vfs || !virtual_path) {
		errno = EACCES;
		return -1;
	}
	if (normalize_path(virtual_path, false, relative) < 0)
		return -1;
	if (is_root(relative)) {
		errno = EISDIR;
		return -1;
	}
	parent_fd = path_parent(vfs, relative, leaf);
	if (parent_fd < 0)
		return -1;
	fd = open_regular(vfs, parent_fd, leaf, false);
	error = errno;
	close(parent_fd);
	errno = error;
	return fd;
}

int mre_vfs_snapshot(struct mre_vfs *vfs, const char *virtual_path, void *data,
		     uint32_t capacity, uint32_t *bytes)
{
	struct stat before;
	struct stat after;
	unsigned char *body = data;
	uint32_t done = 0;
	int fd;
	int result = -1;
	int error;

	if (!bytes || (!data && capacity)) {
		errno = EINVAL;
		return -1;
	}
	fd = mre_vfs_open_readonly(vfs, virtual_path);
	if (fd < 0)
		return -1;
	if (fstat(fd, &before) < 0 || !bounded_size(vfs, &before))
		goto cleanup;
	if ((uintmax_t)before.st_size > capacity) {
		errno = EFBIG;
		goto cleanup;
	}
	while (done < (uint32_t)before.st_size) {
		ssize_t copied = pread(fd, body + done,
				       (uint32_t)before.st_size - done, done);

		if (copied < 0 && errno == EINTR)
			continue;
		if (copied <= 0) {
			if (!copied)
				errno = ESTALE;
			goto cleanup;
		}
		done += (uint32_t)copied;
	}
	if (fstat(fd, &after) < 0)
		goto cleanup;
	if (!same_file(&before, &after) || before.st_size != after.st_size ||
	    before.st_mtim.tv_sec != after.st_mtim.tv_sec ||
	    before.st_mtim.tv_nsec != after.st_mtim.tv_nsec ||
	    before.st_ctim.tv_sec != after.st_ctim.tv_sec ||
	    before.st_ctim.tv_nsec != after.st_ctim.tv_nsec) {
		errno = ESTALE;
		goto cleanup;
	}
	result = 0;

cleanup:
	error = errno;
	if (close(fd) < 0 && result == 0) {
		result = -1;
		error = errno;
	}
	if (result == 0)
		*bytes = done;
	errno = error;
	return result;
}

struct mre_vfs *mre_vfs_create(int executable_root_fd, int private_root_fd,
			       int state_root_fd, uint32_t max_file_bytes)
{
	struct mre_vfs *vfs;
	struct stat status;
	int error;

	if (executable_root_fd < 0 || private_root_fd < 0 ||
	    state_root_fd < 0 || !max_file_bytes ||
	    max_file_bytes > INT32_MAX) {
		errno = EINVAL;
		return NULL;
	}
	const int roots[] = { executable_root_fd, private_root_fd,
			      state_root_fd };
	for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); ++i) {
		if (fstat(roots[i], &status) < 0)
			return NULL;
		if (!S_ISDIR(status.st_mode)) {
			errno = ENOTDIR;
			return NULL;
		}
	}
	vfs = calloc(1, sizeof(*vfs));
	if (!vfs)
		return NULL;
	vfs->root_fd = -1;
	vfs->system_fd = -1;
	vfs->state_root_fd = -1;
	vfs->budget = max_file_bytes;
	for (size_t i = 0; i < MRE_VFS_FILE_HANDLES; ++i) {
		vfs->files[i].fd = -1;
		vfs->files[i].parent_fd = -1;
	}
	vfs->root_fd = duplicate_fd(executable_root_fd);
	if (vfs->root_fd < 0)
		goto failure;
	vfs->system_fd = duplicate_fd(private_root_fd);
	if (vfs->system_fd < 0)
		goto failure;
	vfs->state_root_fd = duplicate_fd(state_root_fd);
	if (vfs->state_root_fd < 0 ||
	    fstat(state_root_fd, &vfs->state_root) < 0)
		goto failure;
	/* Starting E: inside the state tree would bypass a component walk. */
	int current = duplicate_fd(executable_root_fd);
	if (current < 0)
		goto failure;
	for (;;) {
		struct stat child, parent;
		int next;
		if (fstat(current, &child) < 0) {
			close(current);
			goto failure;
		}
		if (same_file(&child, &vfs->state_root)) {
			close(current);
			errno = EACCES;
			goto failure;
		}
		next = openat(current, "..",
			      O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
		if (next < 0 || fstat(next, &parent) < 0) {
			if (next >= 0)
				close(next);
			close(current);
			goto failure;
		}
		close(current);
		current = next;
		if (same_file(&child, &parent)) {
			close(current);
			break;
		}
	}
	return vfs;
failure:
	error = errno;
	mre_vfs_destroy(vfs);
	errno = error;
	return NULL;
}

void mre_vfs_dispatch(struct mre_vfs *vfs,
		      const struct mre_vfs_request *request,
		      struct mre_vfs_response *response)
{
	char relative[MRE_VFS_PATH_BYTES];
	int result = -1;

	if (!response)
		return;
	memset(response, 0, sizeof(*response));
	response->result = -1;
	if (!vfs || !request) {
		response->error = EINVAL;
		return;
	}
	response->sequence = request->sequence;
	errno = 0;
	switch (request->operation) {
	case MRE_VFS_OPEN:
		result = open_file(vfs, request);
		break;
	case MRE_VFS_CLOSE:
		result = close_file(vfs, request->handle);
		break;
	case MRE_VFS_READ:
	case MRE_VFS_WRITE:
		result = transfer_file(vfs, request, response);
		break;
	case MRE_VFS_SEEK:
	case MRE_VFS_TELL:
	case MRE_VFS_GETSIZE:
		result = handle_position(vfs, request, response);
		break;
	case MRE_VFS_COMMIT: {
		struct vfs_file_handle *file = find_file(vfs, request->handle);

		if (file) {
			result = synchronize(file->fd);
			if (result < 0 && file->writable)
				file->failed = true;
		}
		break;
	}
	case MRE_VFS_ATTR:
	case MRE_VFS_SETATTR:
	case MRE_VFS_MKDIR:
	case MRE_VFS_RMDIR:
	case MRE_VFS_DELETE:
		if (normalize_path(request->path, false, relative) < 0)
			break;
		if (request->operation == MRE_VFS_ATTR) {
			result = metadata(vfs, relative, &response->entry);
			if (result == 0) {
				response->size = response->entry.size;
				result = (int)response->entry.attributes;
			}
		} else if (request->operation == MRE_VFS_SETATTR) {
			result = change_attributes(vfs, request, relative);
		} else {
			result = mutate_path(vfs, request, relative);
		}
		break;
	case MRE_VFS_RENAME:
	case MRE_VFS_COPY:
		result = copy_or_rename(vfs, request, response);
		break;
	case MRE_VFS_FIND_OPEN:
		result = open_iterator(vfs, request->path, &response->entry);
		break;
	case MRE_VFS_FIND_NEXT: {
		struct find_handle *iterator =
			find_iterator(vfs, request->handle);

		if (iterator)
			result = next_entry(vfs, iterator, &response->entry);
		break;
	}
	case MRE_VFS_FIND_CLOSE:
		result = close_iterator(vfs, request->handle);
		break;
	default:
		errno = EINVAL;
		break;
	}
	response->result = result;
	if (result < 0)
		response->error = errno ? (uint32_t)errno : EIO;
}

void mre_vfs_destroy(struct mre_vfs *vfs)
{
	int error = errno;

	if (!vfs)
		return;
	for (size_t i = 0; i < MRE_VFS_FILE_HANDLES; ++i)
		clear_file(&vfs->files[i]);
	for (size_t i = 0; i < MRE_VFS_FIND_HANDLES; ++i)
		if (vfs->finds[i].directory)
			closedir(vfs->finds[i].directory);
	if (vfs->root_fd >= 0)
		close(vfs->root_fd);
	if (vfs->system_fd >= 0)
		close(vfs->system_fd);
	if (vfs->state_root_fd >= 0)
		close(vfs->state_root_fd);
	free(vfs);
	errno = error;
}
