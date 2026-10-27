/* SPDX-License-Identifier: 0BSD */
#ifndef FPLINUX_MRE_ADS_IMAGE_H
#define FPLINUX_MRE_ADS_IMAGE_H

#include <stddef.h>
#include <stdint.h>

/* ADS has two copies of RW: an immutable scatter-load source after RO and
 * live data addressed through r9. The original startup performs the copy. */
struct ads_limits {
	size_t max_file_bytes;
	size_t max_ro_bytes;
	size_t max_ram_bytes;
	size_t heap_bytes;
	size_t stack_bytes;
};

struct ads_info {
	uint32_t file_type, compiler, compressed, ram_kib;
	uint32_t tags_offset;
	uint32_t ro_file_offset, ro_file_size, ro_size;
	uint32_t rw_file_offset, rw_file_size, rw_size, zi_size;
	uint32_t resource_file_offset, resource_size;
	uint32_t entry_offset, entry_thumb;
	size_t source_size, bootstrap_offset, data_offset, heap_offset;
};

struct ads_image {
	struct ads_info info;
	uint8_t *memory;
	size_t memory_size;
	uint8_t *resources;
	uint8_t *stack_bottom;
	void *stack_top;
	size_t stack_size;
	void *memory_mapping, *stack_mapping;
	size_t memory_mapping_size, stack_mapping_size;
	uintptr_t entry, data_base, heap_base, heap_limit;
};

struct ads_limits ads_default_limits(void);

/* All file data is copied/decompressed before return. No open fd or original
 * file pointer is retained. Load never enables execution or calls guest code.
 * out must be empty; success transfers ownership, failure leaves it empty. */
int ads_image_load(const void *file, size_t file_size,
		   const struct ads_limits *limits, struct ads_image *out,
		   char *error, size_t error_size);
void ads_image_unload(struct ads_image *image);

/* Native ARM integration: resolver must be an ADS API wrapper, not a direct
 * hard-float C address. Bind populates SB-128 and the real return-PC stack pair.
 * The trampoline uses the identical stack_top when starting the application. */
int ads_image_bind(struct ads_image *image, uintptr_t resolver,
		   uintptr_t return_stub, char *error, size_t error_size);
int ads_image_allow_execute(struct ads_image *image, char *error,
			    size_t error_size);

/* ARM-only symbols implemented in ads_call.S. Entry and callbacks preserve r9
 * and ARM/Thumb fn bits. Single OS thread; nested calls are supported when
 * callers supply distinct guest stack areas for overlapping guest executions. */
uint32_t ads_guest_call(uintptr_t fn, void *stack_top, uintptr_t data_base,
			uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3);
void ads_guest_return(void);

#endif
