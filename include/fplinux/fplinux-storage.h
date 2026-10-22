/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef FPLINUX_STORAGE_H
#define FPLINUX_STORAGE_H

enum fplinux_storage_state {
	/* The requested contents have not replaced the destination. */
	FPLINUX_STORAGE_UNPUBLISHED,
	/* The contents are visible, but required synchronization is incomplete. */
	FPLINUX_STORAGE_PUBLISHED_UNSYNCED,
	FPLINUX_STORAGE_DURABLE,
};

struct fplinux_storage_result {
	enum fplinux_storage_state state;
	/* The original errno on failure, or zero after confirmed synchronization. */
	int error;
};

#endif
