/* SPDX-License-Identifier: GPL-2.0-only */
#include "fplinux-system-info.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

/* The system release file uses plain or quoted field values. */
static bool copy_field(const char *line, const char *key, char *value,
		       size_t size)
{
	size_t key_length = strlen(key);
	const char *text;
	size_t length;

	if (strncmp(line, key, key_length))
		return false;
	text = line + key_length;
	length = strlen(text);
	if (length >= 2 && (text[0] == '"' || text[0] == '\'') &&
	    text[length - 1] == text[0]) {
		text++;
		length -= 2;
	}
	if (length >= size)
		return false;
	memcpy(value, text, length);
	value[length] = '\0';
	return true;
}

bool fplinux_system_info_read_os_field(const char *path, const char *field,
				       char *value, size_t size)
{
	char key[64];
	char line[256];
	int length = snprintf(key, sizeof(key), "%s=", field);
	FILE *file;
	bool found = false;

	if (length < 0 || (size_t)length >= sizeof(key))
		return false;
	file = fopen(path, "re");
	if (!file)
		return false;
	while (fgets(line, sizeof(line), file)) {
		line[strcspn(line, "\n")] = '\0';
		if (copy_field(line, key, value, size)) {
			found = value[0] != '\0';
			break;
		}
	}
	fclose(file);
	return found;
}

bool fplinux_system_info_read_os_name(const char *path, char *name, size_t size)
{
	char line[256];
	char os_name[96] = "";
	char version[64] = "";
	char version_id[64] = "";
	const char *suffix;
	FILE *file = fopen(path, "re");
	int length;

	if (!file)
		return false;
	while (fgets(line, sizeof(line), file)) {
		line[strcspn(line, "\n")] = '\0';
		if (!copy_field(line, "NAME=", os_name, sizeof(os_name)) &&
		    !copy_field(line, "VERSION=", version, sizeof(version)))
			copy_field(line, "VERSION_ID=", version_id,
				   sizeof(version_id));
	}
	fclose(file);
	if (!os_name[0])
		return false;
	suffix = version[0] ? version : version_id;
	length = snprintf(name, size, "%s%s%s", os_name, suffix[0] ? " " : "",
			  suffix);
	return length >= 0 && (size_t)length < size;
}

bool fplinux_system_info_read_line(const char *path, char *line, size_t size)
{
	FILE *file;
	bool found;

	if (!size || size > INT_MAX)
		return false;
	file = fopen(path, "re");
	if (!file)
		return false;
	found = fgets(line, (int)size, file) != NULL;
	fclose(file);
	if (!found)
		return false;
	line[strcspn(line, "\n")] = '\0';
	return line[0] != '\0';
}
