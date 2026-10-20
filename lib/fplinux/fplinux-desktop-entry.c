/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "fplinux-desktop-entry.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

bool fplinux_desktop_entry_valid_id(const char *id)
{
	const unsigned char *character = (const unsigned char *)id;

	if (!*character || strlen(id) >= FPLINUX_UI_SESSION_ID_BYTES)
		return false;
	for (; *character; ++character)
		if (!((*character >= 'a' && *character <= 'z') ||
		      (*character >= 'A' && *character <= 'Z') ||
		      (*character >= '0' && *character <= '9') ||
		      *character == '.' || *character == '_' ||
		      *character == '-'))
			return false;
	return true;
}

static bool copy_value(char *destination, size_t capacity, const char *value)
{
	size_t bytes = strlen(value);

	if (bytes >= capacity)
		return false;
	memcpy(destination, value, bytes + 1);
	return true;
}

bool fplinux_desktop_entry_read(const char *directory, const char *id,
				const char *locale, uid_t metadata_uid,
				struct fplinux_desktop_entry *entry,
				bool (*field)(void *context, const char *key,
					      const char *value),
				void *context)
{
	char path[PATH_MAX];
	char line[PATH_MAX + 128];
	char name_key[sizeof("Name[]") + FPLINUX_UI_SESSION_LOCALE_BYTES];
	char localized_name[FPLINUX_UI_SESSION_TEXT_BYTES] = { 0 };
	struct stat status;
	FILE *file;
	bool group = false;
	bool application = false;
	bool has_mode = false;
	bool valid = true;
	int fd;

	memset(entry, 0, sizeof(*entry));
	if (!fplinux_desktop_entry_valid_id(id) ||
	    (locale && snprintf(name_key, sizeof(name_key), "Name[%s]",
				locale) >= (int)sizeof(name_key)) ||
	    snprintf(path, sizeof(path), "%s/%s.desktop", directory, id) >=
		    (int)sizeof(path))
		return false;
	fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0)
		return false;
	if (fstat(fd, &status) < 0 || !S_ISREG(status.st_mode) ||
	    status.st_uid != metadata_uid ||
	    (status.st_mode & (S_IWGRP | S_IWOTH))) {
		close(fd);
		return false;
	}
	file = fdopen(fd, "r");
	if (!file) {
		close(fd);
		return false;
	}
	while (fgets(line, sizeof(line), file)) {
		char *value;
		size_t bytes = strlen(line);

		if (!bytes || (line[bytes - 1] != '\n' && !feof(file))) {
			valid = false;
			break;
		}
		line[strcspn(line, "\r\n")] = '\0';
		if (line[0] == '[') {
			group = !strcmp(line, "[Desktop Entry]");
			continue;
		}
		if (!group || line[0] == '#')
			continue;
		value = strchr(line, '=');
		if (!value)
			continue;
		*value++ = '\0';
		if (!strcmp(line, "Type"))
			application = !strcmp(value, "Application");
		else if (!strcmp(line, "Name"))
			valid &= copy_value(entry->title, sizeof(entry->title),
					    value);
		else if (locale && !strcmp(line, name_key))
			valid &= copy_value(localized_name,
					    sizeof(localized_name), value);
		else if (!strcmp(line, "Exec"))
			valid &= copy_value(entry->executable,
					    sizeof(entry->executable), value);
		else if (!strcmp(line, "X-FPLinux-Mode")) {
			has_mode = true;
			if (!strcmp(value, "launcher"))
				entry->mode = FPLINUX_UI_SESSION_LAUNCHER;
			else if (!strcmp(value, "integrated"))
				entry->mode = FPLINUX_UI_SESSION_INTEGRATED;
			else if (!strcmp(value, "fullscreen"))
				entry->mode = FPLINUX_UI_SESSION_FULLSCREEN;
			else
				valid = false;
		} else if (field) {
			valid &= field(context, line, value);
		}
	}
	valid &= !ferror(file);
	fclose(file);
	/* The installed profile contains a single absolute executable. */
	valid &= application && has_mode && entry->title[0] &&
		 entry->executable[0] == '/' &&
		 !strpbrk(entry->executable, " \t\r\n%\\\"'`$") &&
		 access(entry->executable, X_OK) == 0 &&
		 ((entry->mode == FPLINUX_UI_SESSION_LAUNCHER) ==
		  !strcmp(id, FPLINUX_DESKTOP_LAUNCHER_ID));
	if (valid && localized_name[0])
		copy_value(entry->title, sizeof(entry->title), localized_name);
	copy_value(entry->app_id, sizeof(entry->app_id), id);
	return valid;
}
