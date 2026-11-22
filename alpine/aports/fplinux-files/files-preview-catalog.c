/* SPDX-License-Identifier: GPL-2.0-only */
#define _POSIX_C_SOURCE 200809L
#include "files-preview-catalog.h"
#include "fplinux-desktop-entry.h"

#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct catalog_entry {
	struct fplinux_desktop_entry desktop;
	struct files_preview_handler handler;
	struct catalog_entry *next;
	char program[];
};

struct files_preview_catalog {
	struct catalog_entry *entries;
};

static bool read_preview_field(void *context, const char *key,
			       const char *value)
{
	char *program = context;
	size_t bytes;

	if (strcmp(key, "X-FPLinux-FileIcon"))
		return true;
	bytes = strlen(value);
	program[0] = '\0';
	if (bytes < PATH_MAX)
		memcpy(program, value, bytes + 1);
	return true;
}

static bool program_valid(const char *program, uid_t metadata_uid)
{
	struct stat status;

	return program[0] == '/' &&
	       !strpbrk(program, " \t\r\n\v\f%\\\"'`$;&|<>") &&
	       lstat(program, &status) == 0 && S_ISREG(status.st_mode) &&
	       status.st_uid == metadata_uid &&
	       !(status.st_mode & (S_IWGRP | S_IWOTH)) &&
	       access(program, X_OK) == 0;
}

static bool add_entry(struct files_preview_catalog *catalog,
		      const char *directory, const char *filename,
		      uid_t metadata_uid)
{
	static const char suffix[] = ".desktop";
	struct fplinux_desktop_entry desktop;
	char id[FPLINUX_UI_SESSION_ID_BYTES];
	char program[PATH_MAX] = { 0 };
	struct catalog_entry *entry;
	size_t bytes = strlen(filename);
	size_t program_bytes = 0;
	size_t suffix_bytes = sizeof(suffix) - 1;

	if (bytes <= suffix_bytes || bytes - suffix_bytes >= sizeof(id) ||
	    strcmp(filename + bytes - suffix_bytes, suffix))
		return true;
	memcpy(id, filename, bytes - suffix_bytes);
	id[bytes - suffix_bytes] = '\0';
	if (!fplinux_desktop_entry_read(directory, id, NULL, metadata_uid,
					&desktop, read_preview_field,
					program) ||
	    desktop.hidden || desktop.mode == FPLINUX_UI_SESSION_LAUNCHER ||
	    !desktop.extensions[0])
		return true;
	if (program_valid(program, metadata_uid))
		program_bytes = strlen(program) + 1;
	entry = malloc(sizeof(*entry) + program_bytes);
	if (!entry)
		return false;
	entry->desktop = desktop;
	entry->handler.icon = desktop.icon_valid && desktop.icon[0] ?
				      entry->desktop.icon :
				      NULL;
	entry->handler.program = NULL;
	if (program_bytes) {
		memcpy(entry->program, program, program_bytes);
		entry->handler.program = entry->program;
	}
	entry->next = catalog->entries;
	catalog->entries = entry;
	return true;
}

struct files_preview_catalog *files_preview_catalog_open(const char *directory,
							 uid_t metadata_uid)
{
	struct files_preview_catalog *catalog;
	DIR *entries = opendir(directory);

	if (!entries)
		return NULL;
	catalog = calloc(1, sizeof(*catalog));
	if (!catalog)
		goto fail;
	for (;;) {
		struct dirent *item;

		errno = 0;
		item = readdir(entries);
		if (!item) {
			if (errno)
				goto fail;
			break;
		}
		if (!add_entry(catalog, directory, item->d_name, metadata_uid))
			goto fail;
	}
	closedir(entries);
	return catalog;

fail:
	closedir(entries);
	files_preview_catalog_close(catalog);
	return NULL;
}

void files_preview_catalog_close(struct files_preview_catalog *catalog)
{
	if (!catalog)
		return;
	while (catalog->entries) {
		struct catalog_entry *entry = catalog->entries;

		catalog->entries = entry->next;
		free(entry);
	}
	free(catalog);
}

const struct files_preview_handler *
files_preview_catalog_find(const struct files_preview_catalog *catalog,
			   const char *path)
{
	const struct files_preview_handler *handler = NULL;

	if (!catalog)
		return NULL;
	for (const struct catalog_entry *entry = catalog->entries; entry;
	     entry = entry->next) {
		if (!fplinux_desktop_entry_handles(&entry->desktop, path))
			continue;
		if (handler)
			return NULL;
		handler = &entry->handler;
	}
	return handler;
}
