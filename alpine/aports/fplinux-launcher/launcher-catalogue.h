/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_LAUNCHER_CATALOGUE_H
#define FPLINUX_LAUNCHER_CATALOGUE_H

#include <stdbool.h>
#include <sys/types.h>

#include "fplinux-ui-session.h"

/* Catalogue groups in display order. */
enum fplinux_launcher_group {
	FPLINUX_LAUNCHER_GROUP_COMMUNICATION,
	FPLINUX_LAUNCHER_GROUP_ORGANISE,
	FPLINUX_LAUNCHER_GROUP_MEDIA,
	FPLINUX_LAUNCHER_GROUP_TOOLS,
	FPLINUX_LAUNCHER_GROUP_SYSTEM,
};

/* Display metadata; a bad Icon keeps the entry with the generic app glyph. */
struct fplinux_launcher_entry {
	char id[FPLINUX_UI_SESSION_ID_BYTES];
	char name[FPLINUX_UI_SESSION_TEXT_BYTES];
	char icon[FPLINUX_UI_SESSION_ID_BYTES];
	enum fplinux_launcher_group group;
	bool icon_valid;
};

/*
 * Validate filename's desktop entry with the session's rules, then apply the
 * catalogue's Hidden/NoDisplay filter and select the locale's name.
 * The system uses metadata_uid 0; host fixtures may use their owner's UID.
 */
bool fplinux_launcher_read_entry(const char *directory, const char *filename,
				 const char *locale, uid_t metadata_uid,
				 struct fplinux_launcher_entry *entry);

/*
 * Maps a desktop entry's semicolon-separated Categories value. The first
 * listed category with a mapping decides; none, or NULL, selects Tools.
 */
enum fplinux_launcher_group
fplinux_launcher_group_from_categories(const char *categories);

/*
 * Orders UTF-8 names alphabetically without regard to case: Latin before
 * Cyrillic, Ё after Е. Names that differ only in case keep byte order.
 */
int fplinux_launcher_compare_names(const char *left, const char *right);

#endif
