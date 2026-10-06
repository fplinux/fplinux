/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_UMS9117_BOOT_SESSION_H
#define FPLINUX_UMS9117_BOOT_SESSION_H

#include <stddef.h>
#include <stdint.h>

#include "fplinux-handoff-protocol.h"

enum ums9117_bootstrap_session_status {
	UMS9117_BOOTSTRAP_SESSION_OK = 0,
	UMS9117_BOOTSTRAP_SESSION_LAYOUT,
	UMS9117_BOOTSTRAP_SESSION_MAGIC,
	UMS9117_BOOTSTRAP_SESSION_SIZE,
	UMS9117_BOOTSTRAP_SESSION_CRC,
	UMS9117_BOOTSTRAP_SESSION_RESERVED,
	UMS9117_BOOTSTRAP_SESSION_ID,
	UMS9117_BOOTSTRAP_SESSION_SEED,
	UMS9117_BOOTSTRAP_SESSION_CLIENT_KEY,
	UMS9117_BOOTSTRAP_SESSION_USB_CONFIG,
	UMS9117_BOOTSTRAP_SESSION_DTB,
	UMS9117_BOOTSTRAP_SESSION_OUTPUT,
};

/*
 * The record is an exact 512-byte, 64-byte-aligned region. Rejection leaves
 * all buffers intact. Success copies values without clearing the record;
 * the bootstrap transaction flushes the DTB before clearing it.
 */
enum ums9117_bootstrap_session_status ums9117_boot_session_copy_to_dtb(
	const unsigned char *record, size_t record_bytes, unsigned char *tree,
	size_t tree_bytes,
	uint8_t session_id[FPLINUX_HANDOFF_SESSION_ID_BYTES]);

enum ums9117_bootstrap_session_status ums9117_bootstrap_personalize_dtb(
	uint32_t destination, size_t bytes,
	uint8_t session_id[FPLINUX_HANDOFF_SESSION_ID_BYTES]);
const char *
ums9117_bootstrap_session_error(enum ums9117_bootstrap_session_status status);

#endif
