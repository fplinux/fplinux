/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "fplinux-files-browse.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int fplinux_files_valid_name(const char *name)
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
	if (fplinux_files_valid_name(name) < 0)
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

int fplinux_files_open_parent(const char *path, char *name, int root_error)
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

static int read_directory(int descriptor,
			  struct fplinux_files_directory *directory,
			  fplinux_files_cancel_cb cancel, void *context)
{
	struct fplinux_files_directory result = { 0 };
	size_t capacity = 0;
	DIR *stream;
	int error;

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

static bool is_empty_result(const struct fplinux_files_directory *directory)
{
	return directory && !directory->entries && !directory->count;
}

int fplinux_files_directory_read(const char *path,
				 struct fplinux_files_directory *directory,
				 fplinux_files_cancel_cb cancel, void *context)
{
	int descriptor;

	if (!is_empty_result(directory)) {
		errno = EINVAL;
		return -1;
	}
	descriptor = open_directory(path, O_RDONLY);
	return read_directory(descriptor, directory, cancel, context);
}

int fplinux_files_directory_read_fd(int directory_fd,
				    struct fplinux_files_directory *directory,
				    fplinux_files_cancel_cb cancel,
				    void *context)
{
	int descriptor;

	if (!is_empty_result(directory)) {
		errno = EINVAL;
		return -1;
	}
	/* Give fdopendir ownership without releasing the caller's mount pin. */
	descriptor = fcntl(directory_fd, F_DUPFD_CLOEXEC, 0);
	return read_directory(descriptor, directory, cancel, context);
}
