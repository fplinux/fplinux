/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef FPLINUX_UI_RECENT_H
#define FPLINUX_UI_RECENT_H

#include <stdbool.h>
#include <stddef.h>

struct fplinux_ui_prefs;

#define FPLINUX_UI_RECENT_MAX 10

struct fplinux_ui_recent_entry {
	char *path;
	char *title;
};

struct fplinux_ui_recent {
	struct fplinux_ui_recent_entry items[FPLINUX_UI_RECENT_MAX];
	size_t count;
};

/* Initialize to zero before first use. Cleanup releases all owned strings. */
void fplinux_ui_recent_cleanup(struct fplinux_ui_recent *recent);
/* Failure leaves the initialized model unchanged and sets errno. */
bool fplinux_ui_recent_load(struct fplinux_ui_recent *recent,
			    const struct fplinux_ui_prefs *prefs);
/* Paths are normalized by the caller; title may be NULL. */
bool fplinux_ui_recent_record(struct fplinux_ui_recent *recent,
			      const char *path, const char *title);
bool fplinux_ui_recent_remove(struct fplinux_ui_recent *recent, size_t index);
/* Writes only to prefs memory. The caller saves and checks its result. */
bool fplinux_ui_recent_store(const struct fplinux_ui_recent *recent,
			     struct fplinux_ui_prefs *prefs);

#endif
