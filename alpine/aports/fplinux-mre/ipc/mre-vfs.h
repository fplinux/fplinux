/* SPDX-License-Identifier: 0BSD */
#ifndef FPLINUX_MRE_VFS_H
#define FPLINUX_MRE_VFS_H

#include <stdint.h>

#define MRE_VFS_PATH_BYTES 260
#define MRE_VFS_TRANSFER_BYTES 4096

enum mre_vfs_operation {
	MRE_VFS_OPEN = 1,
	MRE_VFS_CLOSE,
	MRE_VFS_READ,
	MRE_VFS_WRITE,
	MRE_VFS_SEEK,
	MRE_VFS_TELL,
	MRE_VFS_GETSIZE,
	MRE_VFS_COMMIT,
	MRE_VFS_ATTR,
	MRE_VFS_SETATTR,
	MRE_VFS_MKDIR,
	MRE_VFS_RMDIR,
	MRE_VFS_RENAME,
	MRE_VFS_DELETE,
	MRE_VFS_FIND_OPEN,
	MRE_VFS_FIND_NEXT,
	MRE_VFS_FIND_CLOSE,
	MRE_VFS_COPY,
};

/* These records contain no native pointers, descriptors or ABI layout padding.
 * The engine packs the appropriate old/extended directory entry itself.
 */
struct mre_vfs_entry {
	char name[MRE_VFS_PATH_BYTES];
	uint32_t size;
	uint32_t attributes;
	int32_t create_time[6];
	int32_t modify_time[6];
};

struct mre_vfs_request {
	uint32_t sequence;
	uint32_t operation;
	int32_t handle;
	uint32_t mode;
	int32_t offset;
	int32_t base;
	uint32_t bytes;
	uint32_t attributes;
	char path[MRE_VFS_PATH_BYTES];
	char path2[MRE_VFS_PATH_BYTES];
	uint8_t data[MRE_VFS_TRANSFER_BYTES];
};

struct mre_vfs_response {
	uint32_t sequence;
	uint32_t error;
	int32_t result;
	uint32_t count;
	int32_t position;
	uint32_t size;
	struct mre_vfs_entry entry;
	uint8_t data[MRE_VFS_TRANSFER_BYTES];
};

_Static_assert(sizeof(struct mre_vfs_entry) == 316, "VFS entry wire size");
_Static_assert(sizeof(struct mre_vfs_request) == 4648, "VFS request wire size");
_Static_assert(sizeof(struct mre_vfs_response) == 4436,
	       "VFS response wire size");

#endif
