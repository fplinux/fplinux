/* SPDX-License-Identifier: 0BSD */
#ifndef FPLINUX_MRE_VFS_BROKER_H
#define FPLINUX_MRE_VFS_BROKER_H

#include <stdbool.h>

#include "../ipc/mre-vfs.h"

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
