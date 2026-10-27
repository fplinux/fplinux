/* SPDX-License-Identifier: 0BSD */
#include "mre-memory.h"

#include <stdlib.h>
#include <string.h>

static void forget_retired(struct mre_memory *pool, uintptr_t address,
			   size_t bytes)
{
	for (size_t i = 0; i < 256; ++i)
		if (pool->retired[i] >= address &&
		    pool->retired[i] - address < bytes)
			pool->retired[i] = 0;
}

static void retire(struct mre_memory *pool, uintptr_t address)
{
	for (size_t i = 0; i < 256; ++i)
		if (pool->retired[i] == address)
			return;
	pool->retired[pool->retired_cursor] = address;
	pool->retired_cursor = (pool->retired_cursor + 1) % 256;
}

static size_t vacant_slot(struct mre_memory *pool)
{
	for (size_t i = 0; i < pool->capacity; ++i)
		if (!pool->allocations[i].pointer)
			return i;
	if (pool->capacity >= pool->limit)
		return SIZE_MAX;

	size_t capacity = pool->capacity > pool->limit - pool->capacity ?
				  pool->limit :
				  pool->capacity * 2;
	if (capacity > SIZE_MAX / sizeof(*pool->allocations))
		return SIZE_MAX;
	struct mre_memory_allocation *allocations =
		realloc(pool->allocations, capacity * sizeof(*allocations));
	if (!allocations)
		return SIZE_MAX;
	memset(allocations + pool->capacity, 0,
	       (capacity - pool->capacity) * sizeof(*allocations));
	size_t slot = pool->capacity;
	pool->allocations = allocations;
	pool->capacity = capacity;
	return slot;
}

int mre_memory_init(struct mre_memory *pool, size_t byte_limit)
{
	size_t capacity = byte_limit < 256 ? byte_limit : 256;
	struct mre_memory_allocation *allocations = NULL;
	if (capacity) {
		if (capacity > SIZE_MAX / sizeof(*allocations))
			return -1;
		allocations = calloc(capacity, sizeof(*allocations));
		if (!allocations)
			return -1;
	}
	*pool = (struct mre_memory){ .allocations = allocations,
				     .capacity = capacity,
				     .limit = byte_limit };
	return 0;
}

void mre_memory_destroy(struct mre_memory *pool)
{
	for (size_t i = 0; i < pool->capacity; ++i)
		free(pool->allocations[i].pointer);
	free(pool->allocations);
	*pool = (struct mre_memory){ 0 };
}

void *mre_memory_calloc(struct mre_memory *pool, int32_t bytes)
{
	if (bytes <= 0 || (size_t)bytes > pool->limit - pool->bytes)
		return NULL;
	void *pointer = calloc(1, (size_t)bytes);
	if (!pointer)
		return NULL;
	size_t slot = vacant_slot(pool);
	if (slot == SIZE_MAX) {
		free(pointer);
		return NULL;
	}
	forget_retired(pool, (uintptr_t)pointer, (size_t)bytes);
	pool->allocations[slot] =
		(struct mre_memory_allocation){ pointer, (size_t)bytes };
	pool->bytes += (size_t)bytes;
	pool->count++;
	return pointer;
}

void *mre_memory_realloc(struct mre_memory *pool, void *pointer, int32_t bytes)
{
	if (bytes <= 0)
		return NULL;
	if (!pointer)
		return mre_memory_calloc(pool, bytes);

	size_t slot = SIZE_MAX;
	for (size_t i = 0; i < pool->capacity; ++i)
		if (pool->allocations[i].pointer == pointer) {
			slot = i;
			break;
		}
	if (slot == SIZE_MAX)
		return NULL;
	size_t old_bytes = pool->allocations[slot].bytes;
	if ((size_t)bytes > pool->limit - (pool->bytes - old_bytes))
		return NULL;

	uintptr_t old_address = (uintptr_t)pointer;
	void *replacement = realloc(pointer, (size_t)bytes);
	if (!replacement)
		return NULL;
	if ((uintptr_t)replacement != old_address)
		retire(pool, old_address);
	forget_retired(pool, (uintptr_t)replacement, (size_t)bytes);
	pool->allocations[slot] =
		(struct mre_memory_allocation){ replacement, (size_t)bytes };
	pool->bytes = pool->bytes - old_bytes + (size_t)bytes;
	return replacement;
}

enum mre_memory_release mre_memory_free(struct mre_memory *pool, void *pointer,
					size_t *released_bytes)
{
	if (released_bytes)
		*released_bytes = 0;
	if (!pointer)
		return MRE_MEMORY_RELEASED;
	for (size_t i = 0; i < pool->capacity; ++i) {
		struct mre_memory_allocation *allocation =
			&pool->allocations[i];
		if (allocation->pointer != pointer)
			continue;
		if (released_bytes)
			*released_bytes = allocation->bytes;
		pool->bytes -= allocation->bytes;
		pool->count--;
		retire(pool, (uintptr_t)pointer);
		*allocation = (struct mre_memory_allocation){ 0 };
		free(pointer);
		return MRE_MEMORY_RELEASED;
	}
	if (mre_memory_owner(pool, pointer, NULL))
		return MRE_MEMORY_INTERIOR;
	for (size_t i = 0; i < 256; ++i)
		if (pool->retired[i] == (uintptr_t)pointer)
			return MRE_MEMORY_RETIRED;
	return MRE_MEMORY_UNKNOWN;
}

int mre_memory_contains(const struct mre_memory *pool, const void *pointer,
			size_t bytes)
{
	uintptr_t address = (uintptr_t)pointer;
	for (size_t i = 0; i < pool->capacity; ++i) {
		const struct mre_memory_allocation *allocation =
			&pool->allocations[i];
		if (!allocation->pointer)
			continue;
		uintptr_t low = (uintptr_t)allocation->pointer;
		if (address >= low && address - low <= allocation->bytes &&
		    bytes <= allocation->bytes - (address - low))
			return 1;
	}
	return 0;
}

void *mre_memory_owner(const struct mre_memory *pool, const void *pointer,
		       size_t *owner_bytes)
{
	if (owner_bytes)
		*owner_bytes = 0;
	uintptr_t address = (uintptr_t)pointer;
	for (size_t i = 0; i < pool->capacity; ++i) {
		const struct mre_memory_allocation *allocation =
			&pool->allocations[i];
		uintptr_t low = (uintptr_t)allocation->pointer;
		if (allocation->pointer && address >= low &&
		    address - low < allocation->bytes) {
			if (owner_bytes)
				*owner_bytes = allocation->bytes;
			return allocation->pointer;
		}
	}
	return NULL;
}
