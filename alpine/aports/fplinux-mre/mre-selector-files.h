/* SPDX-License-Identifier: 0BSD */
#ifndef FPLINUX_MRE_SELECTOR_FILES_H
#define FPLINUX_MRE_SELECTOR_FILES_H

#include "vfs-broker.h"

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>

struct mre_selector_entry {
	char name[NAME_MAX + 1];
	bool directory;
};

struct mre_selector_entries {
	struct mre_selector_entry *items;
	size_t count;
};

void mre_selector_entries_clear(struct mre_selector_entries *entries);
/* Consumes fd, including on failure; clears earlier entries before reading. */
int mre_selector_entries_read(struct mre_selector_entries *entries,
			      struct mre_vfs *vfs, int fd, int selector_type);
bool mre_selector_append_path(char destination[MRE_VFS_PATH_BYTES],
			      const char *folder, const char *name);

#endif
