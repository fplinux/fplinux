/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_FILES_PREVIEW_CATALOG_H
#define FPLINUX_FILES_PREVIEW_CATALOG_H

#include <sys/types.h>

struct files_preview_catalog;

struct files_preview_handler {
	const char *icon;
	const char *program;
};

struct files_preview_catalog *files_preview_catalog_open(const char *directory,
							 uid_t metadata_uid);
void files_preview_catalog_close(struct files_preview_catalog *catalog);

/* A unique handler and its optional fields are borrowed until close. */
const struct files_preview_handler *
files_preview_catalog_find(const struct files_preview_catalog *catalog,
			   const char *path);

#endif
