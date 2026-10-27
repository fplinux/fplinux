/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_SESSION_STORAGE_H
#define FPLINUX_SESSION_STORAGE_H

#include <fplinux-volumes.h>
#include <sys/types.h>

struct fplinux_session_storage_probe {
	char filesystem[16];
	char label[FPLINUX_VOLUME_LABEL_BYTES];
};

/* These replace only the block-device and mount boundary in host checks. */
struct fplinux_session_storage_io {
	void *context;
	int (*probe)(void *context, const char *device, dev_t number,
		     struct fplinux_session_storage_probe *probe);
	int (*mount)(void *context, const char *device, const char *path,
		     const char *filesystem, unsigned long flags,
		     const char *options);
	int (*sync)(void *context, const char *path);
	int (*unmount)(void *context, const char *path);
};

struct fplinux_session_storage_config {
	/* NULL fields select the runtime Linux paths. Strings are copied. */
	const char *sys_block;
	const char *dev_root;
	const char *mountinfo;
	const char *swaps;
	const char *mountpoint;
	const char *home_path;
	uid_t uid;
	gid_t gid;
	const struct fplinux_session_storage_io *io;
};

struct fplinux_session_storage;

struct fplinux_session_storage *fplinux_session_storage_create(
	const struct fplinux_session_storage_config *config);
/* Remove the fd watch first. Waits for any accepted operation to finish. */
void fplinux_session_storage_destroy(struct fplinux_session_storage *storage);
int fplinux_session_storage_fd(const struct fplinux_session_storage *storage);
/* One request/result at a time. Never performs filesystem IO on the caller. */
int fplinux_session_storage_request(struct fplinux_session_storage *storage,
				    enum fplinux_volume_operation operation,
				    uint32_t volume_id, uint32_t request_id);
/* Returns 1 for a result, 0 when not ready, -1 on invalid arguments. */
int fplinux_session_storage_take_result(struct fplinux_session_storage *storage,
					struct fplinux_volume_result *result);

#endif
