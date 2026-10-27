/* SPDX-License-Identifier: GPL-2.0-only */
#include "launcher-catalogue.h"
#include "fplinux-desktop-entry.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

struct display_fields {
	struct fplinux_launcher_entry *entry;
};

static bool read_display_field(void *context, const char *key,
			       const char *value)
{
	struct display_fields *fields = context;
	struct fplinux_launcher_entry *entry = fields->entry;

	if (!strcmp(key, "Categories"))
		entry->group = fplinux_launcher_group_from_categories(value);
	return true;
}

bool fplinux_launcher_read_entry(const char *directory, const char *filename,
				 const char *locale, uid_t metadata_uid,
				 struct fplinux_launcher_entry *entry)
{
	struct fplinux_desktop_entry desktop;
	struct display_fields fields = { .entry = entry };
	size_t length = strlen(filename);

	memset(entry, 0, sizeof(*entry));
	entry->group = fplinux_launcher_group_from_categories(NULL);
	entry->icon_valid = true;
	if (length <= 8 || strcmp(filename + length - 8, ".desktop") ||
	    length - 8 >= sizeof(entry->id))
		return false;
	memcpy(entry->id, filename, length - 8);
	if (!fplinux_desktop_entry_read(directory, entry->id, locale,
					metadata_uid, &desktop,
					read_display_field, &fields) ||
	    desktop.hidden || desktop.no_display ||
	    desktop.mode == FPLINUX_UI_SESSION_LAUNCHER)
		return false;
	strcpy(entry->name, desktop.title);
	strcpy(entry->icon, desktop.icon);
	entry->icon_valid = desktop.icon_valid;
	return true;
}

/* Freedesktop main and additional categories that select a group. */
static const struct {
	const char *category;
	enum fplinux_launcher_group group;
} category_groups[] = {
	{ "Network", FPLINUX_LAUNCHER_GROUP_COMMUNICATION },
	{ "Telephony", FPLINUX_LAUNCHER_GROUP_COMMUNICATION },
	{ "Email", FPLINUX_LAUNCHER_GROUP_COMMUNICATION },
	{ "Chat", FPLINUX_LAUNCHER_GROUP_COMMUNICATION },
	{ "InstantMessaging", FPLINUX_LAUNCHER_GROUP_COMMUNICATION },
	{ "IRCClient", FPLINUX_LAUNCHER_GROUP_COMMUNICATION },
	{ "VideoConference", FPLINUX_LAUNCHER_GROUP_COMMUNICATION },
	{ "ContactManagement", FPLINUX_LAUNCHER_GROUP_COMMUNICATION },
	{ "Office", FPLINUX_LAUNCHER_GROUP_ORGANISE },
	{ "Calendar", FPLINUX_LAUNCHER_GROUP_ORGANISE },
	{ "ProjectManagement", FPLINUX_LAUNCHER_GROUP_ORGANISE },
	{ "TextEditor", FPLINUX_LAUNCHER_GROUP_ORGANISE },
	{ "WordProcessor", FPLINUX_LAUNCHER_GROUP_ORGANISE },
	{ "Spreadsheet", FPLINUX_LAUNCHER_GROUP_ORGANISE },
	{ "AudioVideo", FPLINUX_LAUNCHER_GROUP_MEDIA },
	{ "Audio", FPLINUX_LAUNCHER_GROUP_MEDIA },
	{ "Video", FPLINUX_LAUNCHER_GROUP_MEDIA },
	{ "Player", FPLINUX_LAUNCHER_GROUP_MEDIA },
	{ "Recorder", FPLINUX_LAUNCHER_GROUP_MEDIA },
	{ "Music", FPLINUX_LAUNCHER_GROUP_MEDIA },
	{ "Graphics", FPLINUX_LAUNCHER_GROUP_MEDIA },
	{ "Photography", FPLINUX_LAUNCHER_GROUP_MEDIA },
	{ "Viewer", FPLINUX_LAUNCHER_GROUP_MEDIA },
	{ "Game", FPLINUX_LAUNCHER_GROUP_MEDIA },
	{ "FileManager", FPLINUX_LAUNCHER_GROUP_MEDIA },
	{ "Utility", FPLINUX_LAUNCHER_GROUP_TOOLS },
	{ "Calculator", FPLINUX_LAUNCHER_GROUP_TOOLS },
	{ "Clock", FPLINUX_LAUNCHER_GROUP_TOOLS },
	{ "Accessibility", FPLINUX_LAUNCHER_GROUP_TOOLS },
	{ "Development", FPLINUX_LAUNCHER_GROUP_TOOLS },
	{ "Education", FPLINUX_LAUNCHER_GROUP_TOOLS },
	{ "Science", FPLINUX_LAUNCHER_GROUP_TOOLS },
	{ "System", FPLINUX_LAUNCHER_GROUP_SYSTEM },
	{ "Settings", FPLINUX_LAUNCHER_GROUP_SYSTEM },
	{ "HardwareSettings", FPLINUX_LAUNCHER_GROUP_SYSTEM },
	{ "TerminalEmulator", FPLINUX_LAUNCHER_GROUP_SYSTEM },
	{ "PackageManager", FPLINUX_LAUNCHER_GROUP_SYSTEM },
	{ "Monitor", FPLINUX_LAUNCHER_GROUP_SYSTEM },
	{ "Security", FPLINUX_LAUNCHER_GROUP_SYSTEM },
};

enum fplinux_launcher_group
fplinux_launcher_group_from_categories(const char *categories)
{
	const char *start = categories;

	while (start && *start) {
		size_t length = strcspn(start, ";");

		for (size_t i = 0;
		     i < sizeof(category_groups) / sizeof(*category_groups);
		     ++i)
			if (strlen(category_groups[i].category) == length &&
			    !memcmp(category_groups[i].category, start, length))
				return category_groups[i].group;
		start += length;
		if (*start == ';')
			++start;
	}
	return FPLINUX_LAUNCHER_GROUP_TOOLS;
}

/* Invalid UTF-8 contributes its bytes one at a time. */
static uint32_t next_codepoint(const unsigned char **text)
{
	const unsigned char *bytes = *text;
	uint32_t codepoint;
	size_t count;

	if (bytes[0] >= 0xc2 && bytes[0] <= 0xdf) {
		codepoint = bytes[0] & 0x1f;
		count = 2;
	} else if (bytes[0] >= 0xe0 && bytes[0] <= 0xef) {
		codepoint = bytes[0] & 0x0f;
		count = 3;
	} else {
		*text = bytes + 1;
		return bytes[0];
	}
	for (size_t i = 1; i < count; ++i) {
		if ((bytes[i] & 0xc0) != 0x80) {
			*text = bytes + 1;
			return bytes[0];
		}
		codepoint = codepoint << 6 | (bytes[i] & 0x3f);
	}
	*text = bytes + count;
	return codepoint;
}

static uint32_t sort_weight(uint32_t codepoint)
{
	if (codepoint >= 'A' && codepoint <= 'Z')
		codepoint += 'a' - 'A';
	else if (codepoint >= 0x0410 && codepoint <= 0x042f)
		codepoint += 0x20;
	else if (codepoint >= 0x0400 && codepoint <= 0x040f)
		codepoint += 0x50;
	/* ё (U+0451) follows е (U+0435) in the Russian alphabet. */
	if (codepoint == 0x0451)
		return 0x0435 * 2 + 1;
	return codepoint * 2;
}

int fplinux_launcher_compare_names(const char *left, const char *right)
{
	const unsigned char *first = (const unsigned char *)left;
	const unsigned char *second = (const unsigned char *)right;

	while (*first && *second) {
		uint32_t first_weight = sort_weight(next_codepoint(&first));
		uint32_t second_weight = sort_weight(next_codepoint(&second));

		if (first_weight != second_weight)
			return first_weight < second_weight ? -1 : 1;
	}
	if (*first || *second)
		return *first ? 1 : -1;
	return strcmp(left, right);
}
