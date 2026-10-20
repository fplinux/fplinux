/* SPDX-License-Identifier: GPL-2.0-only */
/* Python owns the trusted metadata fixtures and literal expected results. */
#include "fplinux-desktop-entry.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool field(void *context, const char *key, const char *value)
{
	unsigned int *count = context;

	++*count;
	return strcmp(key, "Reject") || strcmp(value, "true");
}

int main(int argc, char **argv)
{
	struct fplinux_desktop_entry entry;
	unsigned int extra_fields = 0;
	uid_t metadata_uid;
	bool valid;

	if (argc != 4 && argc != 5)
		return 2;
	metadata_uid = (uid_t)strtoul(argv[3], NULL, 10);
	valid = fplinux_desktop_entry_read(argv[1], argv[2],
					   argc == 5 ? argv[4] : NULL,
					   metadata_uid, &entry, field,
					   &extra_fields);
	printf("id_valid=%d\nvalid=%d\nextra_fields=%u\n",
	       fplinux_desktop_entry_valid_id(argv[2]), valid, extra_fields);
	if (valid)
		printf("app_id=%s\ntitle=%s\nexecutable=%s\nmode=%d\n",
		       entry.app_id, entry.title, entry.executable, entry.mode);
	return 0;
}
