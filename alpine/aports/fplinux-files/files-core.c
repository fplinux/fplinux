/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "files-core.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

static int valid_name(const char *name)
{
	if (name == NULL || name[0] == '\0' || strchr(name, '/') != NULL ||
	    strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
		errno = EINVAL;
		return -1;
	}
	if (strnlen(name, FPLINUX_FILES_NAME_MAX + 1) >
	    FPLINUX_FILES_NAME_MAX) {
		errno = ENAMETOOLONG;
		return -1;
	}
	return 0;
}

static int normalize_path(const char *path, char *out)
{
	size_t length = 1;
	const char *component;

	if (path == NULL || path[0] != '/') {
		errno = EINVAL;
		return -1;
	}
	if (strnlen(path, FPLINUX_FILES_PATH_MAX) == FPLINUX_FILES_PATH_MAX) {
		errno = ENAMETOOLONG;
		return -1;
	}
	out[0] = '/';
	component = path + 1;
	while (*component != '\0') {
		const char *end;
		size_t component_length;

		if (*component == '/') {
			component++;
			continue;
		}
		end = strchr(component, '/');
		if (end == NULL)
			end = component + strlen(component);
		component_length = (size_t)(end - component);
		if ((component_length == 1 && component[0] == '.') ||
		    (component_length == 2 && component[0] == '.' &&
		     component[1] == '.')) {
			errno = EINVAL;
			return -1;
		}
		if (component_length > FPLINUX_FILES_NAME_MAX) {
			errno = ENAMETOOLONG;
			return -1;
		}
		if (length > 1)
			out[length++] = '/';
		memcpy(out + length, component, component_length);
		length += component_length;
		component = end;
	}
	out[length] = '\0';
	return 0;
}

int fplinux_files_child_path(const char *parent, const char *name, char *out,
			     size_t capacity)
{
	char normalized[FPLINUX_FILES_PATH_MAX];
	size_t parent_length;
	size_t name_length;
	size_t separator;
	size_t required;

	if (out == NULL) {
		errno = EINVAL;
		return -1;
	}
	if (valid_name(name) < 0)
		return -1;
	if (normalize_path(parent, normalized) < 0)
		return -1;
	parent_length = strlen(normalized);
	name_length = strlen(name);
	separator = parent_length == 1 ? 0 : 1;
	required = parent_length + separator + name_length + 1;
	if (required > capacity || required > FPLINUX_FILES_PATH_MAX) {
		errno = ENAMETOOLONG;
		return -1;
	}
	/* Build in the local buffer so out may alias either input. */
	if (separator != 0)
		normalized[parent_length++] = '/';
	memmove(normalized + parent_length, name, name_length + 1);
	memmove(out, normalized, required);
	return 0;
}

int fplinux_files_parent_path(const char *path, char *out, size_t capacity)
{
	char normalized[FPLINUX_FILES_PATH_MAX];
	char *separator;
	size_t required;

	if (out == NULL) {
		errno = EINVAL;
		return -1;
	}
	if (normalize_path(path, normalized) < 0)
		return -1;
	separator = strrchr(normalized, '/');
	if (separator == normalized)
		separator[1] = '\0';
	else
		*separator = '\0';
	required = strlen(normalized) + 1;
	if (required > capacity) {
		errno = ENAMETOOLONG;
		return -1;
	}
	memmove(out, normalized, required);
	return 0;
}

static int open_directory_component(int parent, const char *name, int flags)
{
	int descriptor = openat(parent, name,
				flags | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);

	if (descriptor < 0 && (errno == ENOTDIR || errno == ELOOP)) {
		int error = errno;
		struct stat metadata;

		if (fstatat(parent, name, &metadata, AT_SYMLINK_NOFOLLOW) ==
			    0 &&
		    S_ISLNK(metadata.st_mode))
			error = ELOOP;
		errno = error;
	}
	return descriptor;
}

static int open_directory(const char *path, int final_flags)
{
	char normalized[FPLINUX_FILES_PATH_MAX];
	char *component;
	int descriptor;

	if (normalize_path(path, normalized) < 0)
		return -1;
	component = normalized + 1;
	descriptor = open("/", (*component == '\0' ? final_flags : O_PATH) |
				       O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
	if (descriptor < 0)
		return -1;
	while (*component != '\0') {
		char *separator = strchr(component, '/');
		int next;
		int error;

		if (separator != NULL)
			*separator = '\0';
		next = open_directory_component(
			descriptor, component,
			separator == NULL ? final_flags : O_PATH);
		error = errno;
		close(descriptor);
		if (next < 0) {
			errno = error;
			return -1;
		}
		descriptor = next;
		if (separator == NULL)
			break;
		component = separator + 1;
	}
	return descriptor;
}

static int open_parent(const char *path, char *name, int root_error)
{
	char normalized[FPLINUX_FILES_PATH_MAX];
	char *separator;

	if (normalize_path(path, normalized) < 0)
		return -1;
	if (normalized[1] == '\0') {
		errno = root_error;
		return -1;
	}
	separator = strrchr(normalized, '/');
	memcpy(name, separator + 1, strlen(separator + 1) + 1);
	if (separator == normalized)
		separator[1] = '\0';
	else
		*separator = '\0';
	return open_directory(normalized, O_RDONLY);
}

static int check_mount(int descriptor, uint32_t expected)
{
	char path[64];
	char *line = NULL;
	size_t capacity = 0;
	unsigned int mount_id = 0;
	FILE *stream;
	int error;

	if (!expected)
		return 0;
	snprintf(path, sizeof(path), "/proc/self/fdinfo/%d", descriptor);
	stream = fopen(path, "re");
	if (!stream)
		return -1;
	while (getline(&line, &capacity, stream) >= 0) {
		if (sscanf(line, "mnt_id:\t%u", &mount_id) == 1)
			break;
	}
	error = ferror(stream)	     ? (errno ? errno : EIO) :
		mount_id == expected ? 0 :
				       ENODEV;
	free(line);
	fclose(stream);
	if (error) {
		errno = error;
		return -1;
	}
	return 0;
}

int fplinux_files_directory_open(const char *path, uint32_t mount_id)
{
	int descriptor = open_directory(path, O_RDONLY);
	int error;

	if (descriptor < 0)
		return -1;
	if (check_mount(descriptor, mount_id) == 0)
		return descriptor;
	error = errno;
	close(descriptor);
	errno = error;
	return -1;
}

static enum fplinux_files_kind kind_from_mode(mode_t mode)
{
	if (S_ISDIR(mode))
		return FPLINUX_FILES_KIND_DIRECTORY;
	if (S_ISREG(mode))
		return FPLINUX_FILES_KIND_FILE;
	if (S_ISLNK(mode))
		return FPLINUX_FILES_KIND_SYMLINK;
	return FPLINUX_FILES_KIND_OTHER;
}

static int compare_entries(const void *first, const void *second)
{
	const struct fplinux_files_entry *left = first;
	const struct fplinux_files_entry *right = second;
	int left_directory = left->kind == FPLINUX_FILES_KIND_DIRECTORY;
	int right_directory = right->kind == FPLINUX_FILES_KIND_DIRECTORY;

	if (left_directory != right_directory)
		return left_directory ? -1 : 1;
	return strcmp(left->name, right->name);
}

void fplinux_files_directory_free(struct fplinux_files_directory *directory)
{
	if (directory == NULL)
		return;
	for (size_t index = 0; index < directory->count; index++)
		free(directory->entries[index].name);
	free(directory->entries);
	directory->entries = NULL;
	directory->count = 0;
}

int fplinux_files_directory_read(const char *path,
				 struct fplinux_files_directory *directory,
				 fplinux_files_cancel_cb cancel, void *context)
{
	struct fplinux_files_directory result = { 0 };
	size_t capacity = 0;
	int descriptor;
	DIR *stream;
	int error;

	if (directory == NULL || directory->entries != NULL ||
	    directory->count != 0) {
		errno = EINVAL;
		return -1;
	}
	descriptor = open_directory(path, O_RDONLY);
	if (descriptor < 0)
		return -1;
	stream = fdopendir(descriptor);
	if (stream == NULL) {
		error = errno;
		close(descriptor);
		errno = error;
		return -1;
	}
	for (;;) {
		struct dirent *entry;
		struct stat metadata;
		char *name;

		if (cancel && cancel(context)) {
			errno = ECANCELED;
			goto failed;
		}
		errno = 0;
		entry = readdir(stream);
		if (entry == NULL) {
			if (errno != 0)
				goto failed;
			break;
		}
		if (strcmp(entry->d_name, ".") == 0 ||
		    strcmp(entry->d_name, "..") == 0)
			continue;
		if (fstatat(descriptor, entry->d_name, &metadata,
			    AT_SYMLINK_NOFOLLOW) < 0) {
			if (errno == ENOENT)
				continue;
			goto failed;
		}
		if (result.count == FPLINUX_FILES_DIRECTORY_LIMIT) {
			errno = EOVERFLOW;
			goto failed;
		}
		if (result.count == capacity) {
			struct fplinux_files_entry *entries;
			size_t next_capacity = capacity == 0 ? 32 :
							       capacity * 2;

			if (next_capacity > FPLINUX_FILES_DIRECTORY_LIMIT)
				next_capacity = FPLINUX_FILES_DIRECTORY_LIMIT;
			entries = realloc(result.entries,
					  next_capacity * sizeof(*entries));
			if (entries == NULL)
				goto failed;
			result.entries = entries;
			capacity = next_capacity;
		}
		name = strdup(entry->d_name);
		if (name == NULL)
			goto failed;
		result.entries[result.count++] = (struct fplinux_files_entry){
			.name = name,
			.kind = kind_from_mode(metadata.st_mode),
			.size = metadata.st_size,
			.modified = metadata.st_mtime,
		};
	}
	if (closedir(stream) < 0) {
		error = errno;
		fplinux_files_directory_free(&result);
		errno = error;
		return -1;
	}
	if (result.count > 1)
		qsort(result.entries, result.count, sizeof(*result.entries),
		      compare_entries);
	*directory = result;
	return 0;

failed:
	error = errno;
	closedir(stream);
	fplinux_files_directory_free(&result);
	errno = error;
	return -1;
}

static int supported_type(const struct stat *metadata, int allow_directory)
{
	if (S_ISREG(metadata->st_mode) ||
	    (allow_directory && S_ISDIR(metadata->st_mode)))
		return 0;
	if (S_ISLNK(metadata->st_mode))
		errno = ELOOP;
	else if (S_ISDIR(metadata->st_mode))
		errno = EISDIR;
	else
		errno = EOPNOTSUPP;
	return -1;
}

static int rename_no_replace(int source_parent, const char *source_name,
			     int destination_parent,
			     const char *destination_name)
{
	int result = (int)syscall(SYS_renameat2, source_parent, source_name,
				  destination_parent, destination_name,
				  RENAME_NOREPLACE);

	/* A check followed by plain rename would permit an unsafe overwrite. */
	if (result < 0 && (errno == ENOSYS || errno == EINVAL))
		errno = EOPNOTSUPP;
	return result;
}

static int open_regular_file(int parent, const char *name,
			     struct stat *metadata)
{
	int descriptor;
	int error;

	if (fstatat(parent, name, metadata, AT_SYMLINK_NOFOLLOW) < 0 ||
	    supported_type(metadata, 0) < 0)
		return -1;
	/* Nonblocking also avoids waiting on a FIFO substituted after fstatat. */
	descriptor = openat(parent, name,
			    O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
	if (descriptor < 0)
		return -1;
	if (fstat(descriptor, metadata) == 0 &&
	    supported_type(metadata, 0) == 0)
		return descriptor;
	error = errno;
	close(descriptor);
	errno = error;
	return -1;
}

static int same_file(const struct stat *first, const struct stat *second)
{
	return first->st_dev == second->st_dev &&
	       first->st_ino == second->st_ino;
}

static void remove_created_file(int parent, const char *name,
				const struct stat *created)
{
	struct stat current;

	if (fstatat(parent, name, &current, AT_SYMLINK_NOFOLLOW) == 0 &&
	    same_file(&current, created))
		unlinkat(parent, name, 0);
}

static int check_cancelled(fplinux_files_cancel_cb cancel, void *context)
{
	if (cancel != NULL && cancel(context)) {
		errno = ECANCELED;
		return -1;
	}
	return 0;
}

static int create_temporary_file(int parent, char *name,
				 const char *destination_name, mode_t mode)
{
	static atomic_uint next_id;

	for (unsigned int attempt = 0; attempt < 32; attempt++) {
		unsigned int id = atomic_fetch_add_explicit(
			&next_id, 1, memory_order_relaxed);
		int descriptor;

		snprintf(name, FPLINUX_FILES_NAME_MAX + 1,
			 ".fplinux-copy-%ld-%u", (long)getpid(), id);
		if (strcmp(name, destination_name) == 0)
			continue;
		descriptor = openat(parent, name,
				    O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC |
					    O_NOFOLLOW,
				    mode);
		if (descriptor >= 0 || errno != EEXIST)
			return descriptor;
	}
	errno = EEXIST;
	return -1;
}

static int copy_open_file(int source, const struct stat *metadata,
			  int destination_parent, const char *destination_name,
			  fplinux_files_cancel_cb cancel,
			  fplinux_files_progress_cb progress, void *context)
{
	char buffer[64 * 1024];
	char temporary_name[FPLINUX_FILES_NAME_MAX + 1];
	struct stat created;
	struct stat existing;
	int destination;
	int error;
	off_t bytes = 0;

	if (check_cancelled(cancel, context) < 0)
		return -1;
	if (fstatat(destination_parent, destination_name, &existing,
		    AT_SYMLINK_NOFOLLOW) == 0) {
		errno = EEXIST;
		return -1;
	}
	if (errno != ENOENT)
		return -1;
	destination = create_temporary_file(destination_parent, temporary_name,
					    destination_name,
					    metadata->st_mode & 0777);
	if (destination < 0)
		return -1;
	if (fstat(destination, &created) < 0) {
		error = errno;
		close(destination);
		unlinkat(destination_parent, temporary_name, 0);
		errno = error;
		return -1;
	}
	for (;;) {
		ssize_t count;
		size_t written = 0;

		if (check_cancelled(cancel, context) < 0)
			goto failed;
		count = read(source, buffer, sizeof(buffer));
		if (count < 0) {
			if (errno == EINTR)
				continue;
			goto failed;
		}
		if (count == 0)
			break;
		while (written < (size_t)count) {
			ssize_t output = write(destination, buffer + written,
					       (size_t)count - written);

			if (output < 0) {
				if (errno == EINTR)
					continue;
				goto failed;
			}
			if (output == 0) {
				errno = EIO;
				goto failed;
			}
			written += (size_t)output;
		}
		bytes += count;
		if (progress)
			progress(context, bytes, metadata->st_size, false);
	}
	if (check_cancelled(cancel, context) < 0)
		goto failed;
	if (progress)
		progress(context, bytes, metadata->st_size, true);
	while (fsync(destination) < 0) {
		if (errno != EINTR || check_cancelled(cancel, context) < 0)
			goto failed;
	}
	if (close(destination) < 0) {
		error = errno;
		remove_created_file(destination_parent, temporary_name,
				    &created);
		errno = error;
		return -1;
	}
	if (check_cancelled(cancel, context) < 0 ||
	    rename_no_replace(destination_parent, temporary_name,
			      destination_parent, destination_name) < 0) {
		error = errno;
		remove_created_file(destination_parent, temporary_name,
				    &created);
		errno = error;
		return -1;
	}
	return 0;

failed:
	error = errno;
	close(destination);
	remove_created_file(destination_parent, temporary_name, &created);
	errno = error;
	return -1;
}

void fplinux_files_operation_close(struct fplinux_files_operation *operation)
{
	if (operation->source_parent >= 0)
		close(operation->source_parent);
	if (operation->destination_parent >= 0)
		close(operation->destination_parent);
	operation->source_parent = -1;
	operation->destination_parent = -1;
}

int fplinux_files_operation_open(struct fplinux_files_operation *operation,
				 enum fplinux_files_action action,
				 const char *source, const char *destination)
{
	*operation = (struct fplinux_files_operation){
		.action = action,
		.source_parent = -1,
		.destination_parent = -1,
	};
	if (action != FPLINUX_FILES_MKDIR) {
		operation->source_parent =
			open_parent(source, operation->source_name, EBUSY);
		if (operation->source_parent < 0)
			goto failed;
		if (fstatat(operation->source_parent, operation->source_name,
			    &operation->source_metadata,
			    AT_SYMLINK_NOFOLLOW) < 0 ||
		    supported_type(&operation->source_metadata,
				   action == FPLINUX_FILES_RENAME ||
					   action == FPLINUX_FILES_REMOVE) < 0)
			goto failed;
	}
	if (action != FPLINUX_FILES_REMOVE) {
		operation->destination_parent = open_parent(
			destination, operation->destination_name, EEXIST);
		if (operation->destination_parent < 0 ||
		    valid_name(operation->destination_name) < 0)
			goto failed;
	}
	return 0;

failed:
	operation->result.error = errno;
	fplinux_files_operation_close(operation);
	errno = operation->result.error;
	return -1;
}

static int execute_operation(struct fplinux_files_operation *operation,
			     fplinux_files_cancel_cb cancel,
			     fplinux_files_progress_cb progress, void *context)
{
	int source = operation->source_parent;
	int destination = operation->destination_parent;
	const char *source_name = operation->source_name;
	const char *destination_name = operation->destination_name;
	int input;
	int result;
	int error;

	if (check_cancelled(cancel, context) < 0)
		return -1;
	switch (operation->action) {
	case FPLINUX_FILES_MKDIR:
		return mkdirat(destination, destination_name, 0777);
	case FPLINUX_FILES_REMOVE:
		result = unlinkat(source, source_name,
				  S_ISDIR(operation->source_metadata.st_mode) ?
					  AT_REMOVEDIR :
					  0);
		if (!result)
			operation->source_state =
				FPLINUX_FILES_SOURCE_REMOVED_UNSYNCED;
		return result;
	case FPLINUX_FILES_RENAME:
	case FPLINUX_FILES_MOVE:
		result = rename_no_replace(source, source_name, destination,
					   destination_name);
		if (!result) {
			operation->source_state =
				FPLINUX_FILES_SOURCE_REMOVED_UNSYNCED;
			return 0;
		}
		if (operation->action != FPLINUX_FILES_MOVE || errno != EXDEV)
			return -1;
		operation->cross_device = true;
		break;
	case FPLINUX_FILES_COPY:
		break;
	default:
		errno = EINVAL;
		return -1;
	}
	input = open_regular_file(source, source_name,
				  &operation->source_metadata);
	if (input < 0)
		return -1;
	result = copy_open_file(input, &operation->source_metadata, destination,
				destination_name, cancel, progress, context);
	error = errno;
	close(input);
	errno = error;
	return result;
}

static int sync_directory(int descriptor)
{
	int result;

	do {
		result = fsync(descriptor);
	} while (result < 0 && errno == EINTR);
	return result;
}

int fplinux_files_operation_run(struct fplinux_files_operation *operation,
				fplinux_files_cancel_cb cancel,
				fplinux_files_progress_cb progress,
				void *context)
{
	struct stat current;
	bool removes_source = operation->action == FPLINUX_FILES_MOVE ||
			      operation->action == FPLINUX_FILES_RENAME;

	operation->result.error = 0;
	if (operation->step == FPLINUX_FILES_EXECUTE) {
		if (execute_operation(operation, cancel, progress, context) < 0)
			goto failed;
		operation->result.state = FPLINUX_STORAGE_PUBLISHED_UNSYNCED;
		operation->step = operation->action == FPLINUX_FILES_REMOVE ?
					  FPLINUX_FILES_SYNC_SOURCE :
					  FPLINUX_FILES_SYNC_DESTINATION;
	}
	if (progress)
		progress(context, 0, 0, true);
	if (operation->step == FPLINUX_FILES_SYNC_DESTINATION) {
		if (sync_directory(operation->destination_parent) < 0)
			goto failed;
		operation->result.state = FPLINUX_STORAGE_DURABLE;
		operation->step = operation->cross_device ?
					  FPLINUX_FILES_UNLINK_SOURCE :
				  removes_source ? FPLINUX_FILES_SYNC_SOURCE :
						   FPLINUX_FILES_DONE;
	}
	if (operation->step == FPLINUX_FILES_UNLINK_SOURCE) {
		/* A durable destination is retained if source cleanup cannot finish. */
		if (check_cancelled(cancel, context) < 0 ||
		    fstatat(operation->source_parent, operation->source_name,
			    &current, AT_SYMLINK_NOFOLLOW) < 0)
			goto failed;
		if (!same_file(&operation->source_metadata, &current)) {
			errno = ESTALE;
			goto failed;
		}
		if (unlinkat(operation->source_parent, operation->source_name,
			     0) < 0)
			goto failed;
		operation->source_state = FPLINUX_FILES_SOURCE_REMOVED_UNSYNCED;
		operation->step = FPLINUX_FILES_SYNC_SOURCE;
	}
	if (operation->step == FPLINUX_FILES_SYNC_SOURCE) {
		if (sync_directory(operation->source_parent) < 0)
			goto failed;
		operation->source_state = FPLINUX_FILES_SOURCE_REMOVED_DURABLE;
		operation->result.state = FPLINUX_STORAGE_DURABLE;
		operation->step = FPLINUX_FILES_DONE;
	}
	return 0;

failed:
	operation->result.error = errno;
	return -1;
}
