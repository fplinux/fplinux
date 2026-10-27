/* SPDX-License-Identifier: 0BSD */
#ifndef FPLINUX_MRE_VFS_BROKER_H
#define FPLINUX_MRE_VFS_BROKER_H

#include <stdbool.h>
#include <stdint.h>

#define MRE_VFS_PATH_BYTES 260
#define MRE_VFS_TRANSFER_BYTES 4096
#define MRE_VFS_FILE_HANDLES 16
#define MRE_VFS_FIND_HANDLES 8

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

struct mre_vfs;

/* E:\ is the executable's directory, C:\ is its private
 * state directory. All directory descriptors are borrowed and duplicated.
 * UTF-8 scalar filenames are accepted; ASCII spelling is case insensitive.
 * Dot traversal, symlinks and reserved .mre-vfs-* components are unavailable.
 * E: cannot enter state_root or mutate its ancestors. Executable .vxp/.vsm
 * paths are readable but never mutable.
 */
struct mre_vfs *mre_vfs_create(int executable_root_fd, int private_root_fd,
			       int state_root_fd, uint32_t max_file_bytes);

/* Used by the parent selector to hide protected entries in a pinned directory. */
bool mre_vfs_entry_visible(struct mre_vfs *vfs, int directory_fd,
			   const char *name);

/* Parent-only helpers use the same path policy as guest requests. Open
 * helpers return owned descriptors for the caller to close. snapshot copies a
 * bounded regular file and reports ESTALE if its body changed during the read;
 * bytes is set only on success.
 */
int mre_vfs_open_directory(struct mre_vfs *vfs, const char *virtual_path);
int mre_vfs_open_readonly(struct mre_vfs *vfs, const char *virtual_path);
int mre_vfs_snapshot(struct mre_vfs *vfs, const char *virtual_path, void *data,
		     uint32_t capacity, uint32_t *bytes);

/* Dispatch synchronously, preserving sequence and reporting POSIX errno in
 * error (zero on success). result is a handle, byte count, position or zero;
 * failure is -1.
 * COPY uses path=source/path2=destination. mode=1 validates the copy without
 * creating output and returns size=actual source bytes/count=0/result=0;
 * mode=0 performs the durable copy and returns actual copied bytes in count.
 * Other COPY modes fail. File modes are 1 readonly, 2 readwrite-existing, 4 create
 * always/readwrite and 8 append/readwrite; COPY may cross drives, RENAME may
 * not (EXDEV). Every writable CLOSE syncs the file and its parent directory.
 */
void mre_vfs_dispatch(struct mre_vfs *vfs,
		      const struct mre_vfs_request *request,
		      struct mre_vfs_response *response);

/* Close owned file and directory handles. */
void mre_vfs_destroy(struct mre_vfs *vfs);

#endif
