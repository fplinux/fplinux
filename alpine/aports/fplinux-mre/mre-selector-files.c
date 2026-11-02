/* SPDX-License-Identifier: 0BSD */
#define _GNU_SOURCE
#include "mre-selector-files.h"
#include "text-codec.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define DIRECTORY_LIMIT 4096U

void mre_selector_entries_clear(struct mre_selector_entries *entries)
{
	free(entries->items);
	entries->items = NULL;
	entries->count = 0;
}

static bool valid_name(const char *name)
{
	size_t bytes = strlen(name);
	if (!bytes || bytes > NAME_MAX || !strcmp(name, ".") ||
	    !strcmp(name, "..") || strpbrk(name, "\\/:"))
		return false;
	for (size_t i = 0; i < bytes; ++i)
		if ((unsigned char)name[i] < 32U ||
		    (unsigned char)name[i] == 127U)
			return false;
	return mre_utf8_decode((const unsigned char *)name, bytes, NULL,
			       NAME_MAX + 1U, NULL);
}

static bool suffix(const char *name, const char *extension)
{
	size_t bytes = strlen(name), tail = strlen(extension);
	return bytes > tail && !strcasecmp(name + bytes - tail, extension);
}

static bool image_name(const char *name)
{
	return suffix(name, ".jpg") || suffix(name, ".jpeg") ||
	       suffix(name, ".png") || suffix(name, ".bmp") ||
	       suffix(name, ".gif") || suffix(name, ".abm");
}

static int entry_compare(const void *a, const void *b)
{
	const struct mre_selector_entry *first = a, *second = b;
	if (first->directory != second->directory)
		return first->directory ? -1 : 1;
	return strcasecmp(first->name, second->name);
}

/* Only plain directory entries become rows; the broker revalidates every
 * selected path before an operation. No filename grants guest authority. */
int mre_selector_entries_read(struct mre_selector_entries *entries,
			      struct mre_vfs *vfs, int fd, int selector_type)
{
	DIR *folder;
	struct dirent *entry;
	int error = 0;
	mre_selector_entries_clear(entries);
	if (fd < 0)
		return -1;
	folder = fdopendir(fd);
	if (!folder) {
		close(fd);
		return -1;
	}
	int folder_fd = dirfd(folder);
	if (folder_fd < 0) {
		int saved_error = errno;
		closedir(folder);
		errno = saved_error;
		return -1;
	}
	for (;;) {
		errno = 0;
		entry = readdir(folder);
		if (!entry) {
			error = errno;
			break;
		}
		struct stat st;
		const char *name = entry->d_name;
		bool directory;
		if (!valid_name(name) ||
		    !mre_vfs_entry_visible(vfs, folder_fd, name) ||
		    !strncmp(name, ".mre-vfs-", 9) ||
		    fstatat(folder_fd, name, &st, AT_SYMLINK_NOFOLLOW) < 0)
			continue;
		directory = S_ISDIR(st.st_mode);
		if (!directory) {
			if (!S_ISREG(st.st_mode) ||
			    (selector_type == 1 && !image_name(name)) ||
			    (selector_type == 2 && !suffix(name, ".txt")))
				continue;
		}
		if (entries->count == DIRECTORY_LIMIT) {
			error = E2BIG;
			break;
		}
		struct mre_selector_entry *grown = realloc(
			entries->items, (entries->count + 1U) * sizeof(*grown));
		if (!grown) {
			error = ENOMEM;
			break;
		}
		entries->items = grown;
		snprintf(grown[entries->count].name, NAME_MAX + 1, "%s", name);
		grown[entries->count++].directory = directory;
	}
	closedir(folder);
	if (error) {
		mre_selector_entries_clear(entries);
		errno = error;
		return -1;
	}
	qsort(entries->items, entries->count, sizeof(*entries->items),
	      entry_compare);
	return 0;
}

bool mre_selector_append_path(char destination[MRE_VFS_PATH_BYTES],
			      const char *folder, const char *name)
{
	size_t bytes = strlen(folder), name_bytes = strlen(name);
	bool slash = bytes && folder[bytes - 1U] != '\\';
	if (!valid_name(name) ||
	    bytes + name_bytes + (slash ? 1U : 0U) >= MRE_VFS_PATH_BYTES)
		return false;
	memcpy(destination, folder, bytes);
	if (slash)
		destination[bytes++] = '\\';
	memcpy(destination + bytes, name, name_bytes + 1U);
	return true;
}
