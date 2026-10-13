// SPDX-License-Identifier: GPL-2.0-only
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "boot-session.h"

static int read_fixture(const char *path, unsigned char *bytes, size_t count)
{
	FILE *stream = fopen(path, "rb");
	int complete;

	if (stream == NULL)
		return 0;
	complete = fread(bytes, 1, count, stream) == count &&
		   fgetc(stream) == EOF && !ferror(stream);
	return fclose(stream) == 0 && complete;
}

static int write_result(const unsigned char *tree,
			const unsigned char *session_id)
{
	FILE *stream = fopen("result.bin", "wb");
	int complete;

	if (stream == NULL)
		return 0;
	complete = fwrite(tree, 1, 2048, stream) == 2048 &&
		   fwrite(session_id, 1, 32, stream) == 32;
	return fclose(stream) == 0 && complete;
}

static int parse_count(const char *text, size_t limit, size_t *count)
{
	char *end;
	unsigned long value = strtoul(text, &end, 10);

	if (end == text || *end != '\0' || value > limit)
		return 0;
	*count = (size_t)value;
	return 1;
}

static int guards_are_intact(const unsigned char *bytes, size_t count)
{
	size_t index;

	for (index = 0; index < count; ++index) {
		if (bytes[index] != 0x6e)
			return 0;
	}
	return 1;
}

int main(int argc, char **argv)
{
	unsigned char record_storage[704];
	unsigned char record_before[704];
	unsigned char tree_storage[2176];
	unsigned char output_storage[160];
	unsigned char *record;
	unsigned char *tree = tree_storage + 64;
	unsigned char *session_id = output_storage + 64;
	size_t record_bytes;
	size_t tree_bytes;
	enum ums9117_bootstrap_session_status status;

	if (argc != 4 || !parse_count(argv[2], 513, &record_bytes) ||
	    !parse_count(argv[3], 2048, &tree_bytes))
		return 2;
	memset(record_storage, 0x6e, sizeof(record_storage));
	memset(tree_storage, 0x6e, sizeof(tree_storage));
	memset(output_storage, 0x6e, sizeof(output_storage));
	memset(session_id, 0x75, 32);
	/* Leave guards on both sides of the aligned record. */
	record = (unsigned char *)(((uintptr_t)record_storage + 63U) &
				   ~(uintptr_t)63U) +
		 64;
	if (strcmp(argv[1], "unaligned") == 0)
		++record;
	else if (strcmp(argv[1], "copy") != 0 &&
		 strcmp(argv[1], "no-output") != 0)
		return 2;
	if (!read_fixture("record.bin", record, 512) ||
	    !read_fixture("tree.bin", tree, 2048))
		return 2;
	memcpy(record_before, record_storage, sizeof(record_before));
	status = ums9117_boot_session_copy_to_dtb(
		record, record_bytes, tree, tree_bytes,
		strcmp(argv[1], "no-output") == 0 ? NULL : session_id);
	if (memcmp(record_storage, record_before, sizeof(record_before)) != 0 ||
	    !guards_are_intact(tree_storage, 64) ||
	    !guards_are_intact(tree + 2048, 64) ||
	    !guards_are_intact(output_storage, 64) ||
	    !guards_are_intact(session_id + 32, 64)) {
		fputs("record or buffer guard was modified\n", stderr);
		return 1;
	}
	if (!write_result(tree, session_id))
		return 2;
	printf("%d\n", (int)status);
	return 0;
}
