/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef FPLINUX_VOLUMES_H
#define FPLINUX_VOLUMES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FPLINUX_VOLUME_MAX 4
#define FPLINUX_VOLUME_PATH_BYTES 4096
#define FPLINUX_VOLUME_LABEL_BYTES 96

enum fplinux_volume_state {
	FPLINUX_VOLUME_AVAILABLE,
	FPLINUX_VOLUME_MOUNTED,
	FPLINUX_VOLUME_UNSUPPORTED,
	FPLINUX_VOLUME_PROTECTED,
};

enum fplinux_volume_protection {
	FPLINUX_VOLUME_UNPROTECTED,
	FPLINUX_VOLUME_SYSTEM_ROOT,
	FPLINUX_VOLUME_SWAP,
	FPLINUX_VOLUME_BOOT,
	FPLINUX_VOLUME_OTHER_MOUNT,
};

struct fplinux_volume {
	uint32_t id;
	/* The observed Linux mount ID; zero unless state is MOUNTED. */
	uint32_t mount_id;
	enum fplinux_volume_state state;
	enum fplinux_volume_protection protection;
	bool read_only;
	char label[FPLINUX_VOLUME_LABEL_BYTES];
	/* Only mounted data volumes have an application-accessible path. */
	char path[FPLINUX_VOLUME_PATH_BYTES];
};

struct fplinux_volume_snapshot {
	size_t count;
	bool home_persistent;
	struct fplinux_volume volumes[FPLINUX_VOLUME_MAX];
};

enum fplinux_volume_operation {
	FPLINUX_VOLUME_REFRESH,
	FPLINUX_VOLUME_CONNECT,
	FPLINUX_VOLUME_EJECT,
};

struct fplinux_volume_result {
	uint32_t request_id;
	/* errno value; the snapshot is also supplied after an operation fails. */
	int error;
	struct fplinux_volume_snapshot snapshot;
};

#endif
