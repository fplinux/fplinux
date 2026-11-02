/* SPDX-License-Identifier: 0BSD */
#ifndef FPLINUX_MRE_VFS_PATH_H
#define FPLINUX_MRE_VFS_PATH_H

#include <stdbool.h>
#include <stddef.h>

#define MRE_VFS_NAME_BYTES 256

bool mre_vfs_component_valid(const char *start, size_t bytes, bool pattern);
int mre_vfs_normalize_path(const char *path, bool pattern, char *relative);
bool mre_vfs_ascii_case_equal(const char *first, const char *second);
bool mre_vfs_name_matches(const char *pattern, const char *name);

#endif
