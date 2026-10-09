// SPDX-License-Identifier: GPL-2.0-only
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "boot-session.h"

#define ARENA_BYTES 4096U
#define TREE_BYTES 1024U
#define GUARD_BYTES 64U
#define ARENA_END_PHYS 0x83e00000U

static int guards_are_intact(const unsigned char *bytes, size_t count)
{
	size_t index;

	for (index = 0; index < count; ++index) {
		if (bytes[index] != 0x6eU)
			return 0;
	}
	return 1;
}

static int write_result(const char *path, const unsigned char *bytes,
			size_t count)
{
	FILE *stream = fopen(path, "wb");
	int complete;

	if (stream == NULL)
		return 0;
	complete = fwrite(bytes, 1, count, stream) == count;
	return fclose(stream) == 0 && complete;
}

int main(void)
{
	unsigned char arena_storage[ARENA_BYTES + 2U * GUARD_BYTES];
	unsigned char tree_storage[TREE_BYTES + 2U * GUARD_BYTES];
	unsigned char *arena = arena_storage + GUARD_BYTES;
	unsigned char *tree = tree_storage + GUARD_BYTES;
	unsigned char *destination;
	size_t bytes;
	size_t tree_bytes;
	uint32_t start = 0;
	uint32_t end = 0;
	enum ums9117_bootstrap_device_data_status status;
	FILE *stream;

	memset(arena_storage, 0x6e, sizeof(arena_storage));
	memset(tree_storage, 0x6e, sizeof(tree_storage));
	stream = fopen("tree.dtb", "rb");
	if (stream == NULL)
		return 2;
	tree_bytes = fread(tree, 1, TREE_BYTES, stream);
	if (ferror(stream) || fclose(stream) != 0)
		return 2;
	status = ums9117_boot_session_load_device_data(arena, ARENA_BYTES,
						       &destination, &bytes);
	if (status == UMS9117_BOOTSTRAP_DEVICE_DATA_OK) {
		if (destination != NULL) {
			start = ARENA_END_PHYS - ARENA_BYTES +
				(uint32_t)(destination - arena);
			end = start + (uint32_t)bytes;
		}
		if (!ums9117_boot_session_set_initrd(tree, tree_bytes, start,
						     end))
			return 1;
	}
	if (!guards_are_intact(arena_storage, GUARD_BYTES) ||
	    !guards_are_intact(arena + ARENA_BYTES, GUARD_BYTES) ||
	    !guards_are_intact(tree_storage, GUARD_BYTES) ||
	    !guards_are_intact(tree + tree_bytes,
			       TREE_BYTES + GUARD_BYTES - tree_bytes))
		return 1;
	if (!write_result("result.dtb", tree, tree_bytes) ||
	    !write_result("arena.bin", arena, ARENA_BYTES))
		return 2;
	printf("%u %lu\n", (unsigned)status, (unsigned long)bytes);
	return 0;
}
