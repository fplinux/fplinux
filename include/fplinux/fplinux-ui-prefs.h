/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef FPLINUX_UI_PREFS_H
#define FPLINUX_UI_PREFS_H

#include <stdbool.h>

/*
 * Application preferences: string values by key in the file "preferences" of
 * the application's state directory. Changes stay in memory until
 * fplinux_ui_prefs_save() replaces the file atomically. Keys use ASCII
 * letters, digits, '.', '_' and '-' (at most 63 bytes); values are UTF-8 text
 * of at most 65535 bytes and may contain line breaks.
 */
struct fplinux_ui_prefs;

/* A missing file opens empty; NULL with errno on an unreadable file. */
struct fplinux_ui_prefs *fplinux_ui_prefs_open(const char *app_id);
/* Frees the preferences without saving. NULL is accepted. */
void fplinux_ui_prefs_close(struct fplinux_ui_prefs *prefs);

/* The returned value is valid until the key is set or removed. */
const char *fplinux_ui_prefs_get(const struct fplinux_ui_prefs *prefs,
				 const char *key, const char *fallback);
long long fplinux_ui_prefs_get_int(const struct fplinux_ui_prefs *prefs,
				   const char *key, long long fallback);
bool fplinux_ui_prefs_set(struct fplinux_ui_prefs *prefs, const char *key,
			  const char *value);
bool fplinux_ui_prefs_set_int(struct fplinux_ui_prefs *prefs, const char *key,
			      long long value);
bool fplinux_ui_prefs_remove(struct fplinux_ui_prefs *prefs, const char *key);

/* Writes, syncs and renames a temporary file; does nothing when unchanged. */
bool fplinux_ui_prefs_save(struct fplinux_ui_prefs *prefs);

#endif
