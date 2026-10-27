/* SPDX-License-Identifier: 0BSD */
#ifndef FPLINUX_MRE_MEMORY_H
#define FPLINUX_MRE_MEMORY_H

#include <stddef.h>
#include <stdint.h>

struct mre_memory_allocation {
	void *pointer;
	size_t bytes;
};

struct mre_memory {
	struct mre_memory_allocation *allocations;
	size_t capacity, limit, bytes, count;
	uintptr_t retired[256];
	size_t retired_cursor;
};

enum mre_memory_release {
	MRE_MEMORY_UNKNOWN = -1,
	MRE_MEMORY_INTERIOR = -2,
	MRE_MEMORY_RELEASED = 0,
	MRE_MEMORY_RETIRED = 1,
};

/* Start with a zero-initialized pool. Returns 0 on success, -1 on failure.
 * Descriptor storage is separate from the SDK payload byte limit, but cannot
 * exceed byte_limit slots: every live allocation requires at least one byte. */
int mre_memory_init(struct mre_memory *pool, size_t byte_limit);
void mre_memory_destroy(struct mre_memory *pool);

/* Nonpositive sizes and failures leave existing allocations unchanged.
 * Reallocation accepts NULL or the exact base of a live SDK allocation. */
void *mre_memory_calloc(struct mre_memory *pool, int32_t bytes);
void *mre_memory_realloc(struct mre_memory *pool, void *pointer, int32_t bytes);

/* NULL is a successful no-op. Only exact live addresses are released;
 * exact recent retired addresses are harmless compatibility no-ops. */
enum mre_memory_release mre_memory_free(struct mre_memory *pool, void *pointer,
					size_t *released_bytes);

/* A zero-byte range may point one byte past a live allocation, as in the
 * existing guest range validator. No metadata or retired memory is included. */
int mre_memory_contains(const struct mre_memory *pool, const void *pointer,
			size_t bytes);
void *mre_memory_owner(const struct mre_memory *pool, const void *pointer,
		       size_t *owner_bytes);

#endif
