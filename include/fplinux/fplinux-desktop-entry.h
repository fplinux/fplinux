/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_DESKTOP_ENTRY_H
#define FPLINUX_DESKTOP_ENTRY_H

/* Internal to launcher/session; not installed as an application API. */
#include <linux/limits.h>
#include <stdbool.h>
#include <sys/types.h>

#include "fplinux-ui-session.h"

#define FPLINUX_DESKTOP_LAUNCHER_ID "org.fplinux.launcher"

struct fplinux_desktop_entry {
	char app_id[FPLINUX_UI_SESSION_ID_BYTES];
	char title[FPLINUX_UI_SESSION_TEXT_BYTES];
	char executable[PATH_MAX];
	enum fplinux_ui_session_mode mode;
};

/* Nonempty ASCII ID, shorter than FPLINUX_UI_SESSION_ID_BYTES. */
__attribute__((visibility("hidden"))) bool
fplinux_desktop_entry_valid_id(const char *id);

/*
 * Read directory/id.desktop with O_CLOEXEC and O_NOFOLLOW, requiring a regular
 * file owned by metadata_uid and not writable by group or others. Validate
 * Type, the base Name, Exec and X-FPLinux-Mode with the session's limits.
 * Only org.fplinux.launcher may declare launcher mode, and it must do so.
 * A nonempty Name[locale] replaces the valid base Name; NULL uses the base.
 * field, if supplied, receives other keys in [Desktop Entry]; key/value are
 * borrowed until it returns. False rejects the entry. This lets the launcher
 * interpret display metadata without another parser. NULL ignores other keys.
 * Output is usable only on success. This does not decide launch privileges.
 */
__attribute__((visibility("hidden"))) bool fplinux_desktop_entry_read(
	const char *directory, const char *id, const char *locale,
	uid_t metadata_uid, struct fplinux_desktop_entry *entry,
	bool (*field)(void *context, const char *key, const char *value),
	void *context);

#endif
