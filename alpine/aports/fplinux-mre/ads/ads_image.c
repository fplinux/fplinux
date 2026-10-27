/* SPDX-License-Identifier: 0BSD */
#define _GNU_SOURCE
#include "ads_image.h"

#include <errno.h>
#include <elf.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <zlib.h>

#define ADS_BOOTSTRAP_SIZE 128u
#define ADS_MIN_RAM (512u * 1024u)
#define ADS_MAX_TAGS 128u
#define ADS_MAX_SECTIONS 512u

static int fail(char *error, size_t size, const char *format, ...)
{
	if (error && size) {
		va_list args;
		va_start(args, format);
		vsnprintf(error, size, format, args);
		va_end(args);
	}
	return -1;
}

static uint32_t le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
	       (uint32_t)p[3] << 24;
}
static uint16_t le16(const uint8_t *p)
{
	return (uint16_t)((uint16_t)p[0] | (uint16_t)p[1] << 8);
}
static void put32(uint8_t *p, uint32_t value)
{
	p[0] = (uint8_t)value;
	p[1] = (uint8_t)(value >> 8);
	p[2] = (uint8_t)(value >> 16);
	p[3] = (uint8_t)(value >> 24);
}
static int range(size_t offset, size_t length, size_t bound)
{
	return offset <= bound && length <= bound - offset;
}
static int overlaps(uint32_t a, uint32_t an, uint32_t b, uint32_t bn)
{
	return an && bn && (uint64_t)a < (uint64_t)b + bn &&
	       (uint64_t)b < (uint64_t)a + an;
}

struct ads_limits ads_default_limits(void)
{
	return (struct ads_limits){ 8u * 1024u * 1024u, 8u * 1024u * 1024u,
				    8u * 1024u * 1024u, 2048u, 64u * 1024u };
}

static int tags(const uint8_t *file, size_t size, struct ads_info *info,
		char *error, size_t error_size)
{
	if (size < 12)
		return fail(error, error_size, "truncated MRE trailer");
	size_t end = size - 12;
	size_t pos = le32(file + end);
	if (!range(pos, 8, end))
		return fail(error, error_size, "tag table outside file");
	info->tags_offset = (uint32_t)pos;
	unsigned seen = 0;
	for (unsigned count = 0; count < ADS_MAX_TAGS; ++count) {
		if (!range(pos, 8, end))
			return fail(error, error_size, "truncated tag header");
		uint32_t id = le32(file + pos), n = le32(file + pos + 4);
		pos += 8;
		if (!range(pos, n, end))
			return fail(error, error_size,
				    "tag value outside table");
		if (id == 0) {
			/* Existing originals have a certificate/signature after the
             * terminator and before the three-word trailer. It is opaque. */
			if (n)
				return fail(error, error_size,
					    "invalid tag terminator");
			return 0;
		}
		unsigned bit = id == 0x21 ? 1u :
			       id == 0x15 ? 2u :
			       id == 0x22 ? 4u :
			       id == 0x0f ? 8u :
					    0;
		if (bit) {
			if (n != 4 || (seen & bit))
				return fail(error, error_size,
					    "invalid/duplicate numeric tag");
			uint32_t value = le32(file + pos);
			if (id == 0x21)
				info->file_type = value;
			if (id == 0x15)
				info->compiler = value;
			if (id == 0x22)
				info->compressed = value;
			if (id == 0x0f)
				info->ram_kib = value;
			seen |= bit;
		}
		pos += n;
	}
	return fail(error, error_size, "too many tags/no terminator");
}

static int compressed_info(const uint8_t *file, struct ads_info *info,
			   char *error, size_t error_size)
{
	/* A compressed flat image has no ELF header. Its producer tags and
	 * descriptor jointly establish the ADS scatter-load contract. */
	if (!(info->file_type == 0 || info->file_type == 1 ||
	      info->file_type == 5 || info->compiler == 1))
		return fail(error, error_size,
			    "compressed payload has no ADS producer contract");
	if (info->tags_offset < 40 || le32(file + info->tags_offset - 4) != 36)
		return fail(error, error_size,
			    "invalid ADS compressed descriptor size");
	size_t descriptor = info->tags_offset - 40;
	const uint8_t *p = file + descriptor;
	info->ro_file_offset = le32(p);
	info->ro_file_size = le32(p + 4);
	info->ro_size = le32(p + 8);
	info->rw_file_offset = le32(p + 12);
	info->rw_file_size = le32(p + 16);
	info->rw_size = le32(p + 20);
	info->zi_size = le32(p + 24);
	info->resource_file_offset = le32(p + 28);
	info->resource_size = le32(p + 32);
	if (!range(info->ro_file_offset, info->ro_file_size, descriptor) ||
	    !range(info->rw_file_offset, info->rw_file_size, descriptor) ||
	    !range(info->resource_file_offset, info->resource_size,
		   descriptor) ||
	    overlaps(info->ro_file_offset, info->ro_file_size,
		     info->rw_file_offset, info->rw_file_size) ||
	    overlaps(info->ro_file_offset, info->ro_file_size,
		     info->resource_file_offset, info->resource_size) ||
	    overlaps(info->rw_file_offset, info->rw_file_size,
		     info->resource_file_offset, info->resource_size))
		return fail(error, error_size,
			    "ADS compressed ranges outside payload/overlap");
	if (!info->ro_file_size || (info->rw_size && !info->rw_file_size))
		return fail(error, error_size, "missing compressed segment");
	/* The flat ADS descriptor has no entry field. Its producer/consumer
     * contract starts in ARM state at byte zero, including the measured games. */
	info->entry_offset = 0;
	info->entry_thumb = 0;
	return 0;
}

static int elf_info(const uint8_t *file, size_t size, struct ads_info *info,
		    char *error, size_t error_size)
{
	if (size < 52 || memcmp(file, "\177ELF", 4) || file[4] != 1 ||
	    file[5] != 1 || file[6] != 1 || le16(file + 16) != ET_EXEC ||
	    le16(file + 18) != EM_ARM || le32(file + 20) != EV_CURRENT ||
	    (le32(file + 36) & EF_ARM_EABIMASK) != EF_ARM_EABI_VER2 ||
	    le16(file + 40) != 52 || le16(file + 46) != 40)
		return fail(error, error_size,
			    "ADS requires little-endian ELF32 ARM");
	uint32_t shoff = le32(file + 32), entry = le32(file + 24);
	uint16_t shnum = le16(file + 48), names_index = le16(file + 50);
	if (!shnum || shnum > ADS_MAX_SECTIONS || names_index >= shnum ||
	    !range(shoff, (size_t)shnum * 40, info->tags_offset))
		return fail(error, error_size, "invalid ELF section table");
	const uint8_t *name_section = file + shoff + (size_t)names_index * 40;
	uint32_t names_offset = le32(name_section + 16),
		 names_size = le32(name_section + 20);
	if (le32(name_section + 4) != 3 ||
	    !range(names_offset, names_size, info->tags_offset))
		return fail(error, error_size,
			    "invalid ELF section name table");
	uint32_t ro_address = 0, zi_address = 0;
	unsigned seen = 0;
	for (unsigned index = 0; index < shnum; ++index) {
		const uint8_t *section = file + shoff + (size_t)index * 40;
		uint32_t kind = le32(section + 4), flags = le32(section + 8);
		uint32_t offset = le32(section + 16), n = le32(section + 20);
		if (kind != SHT_NOBITS && !range(offset, n, info->tags_offset))
			return fail(error, error_size,
				    "ELF section outside ADS payload");
		if ((kind == SHT_REL || kind == SHT_RELA) && n &&
		    (flags & SHF_ALLOC))
			return fail(
				error, error_size,
				"ADS scatter image has unapplied runtime relocations");
		uint32_t name_offset = le32(section);
		if (name_offset >= names_size ||
		    !memchr(file + names_offset + name_offset, 0,
			    names_size - name_offset))
			return fail(error, error_size,
				    "unterminated ELF section name");
		const char *name =
			(const char *)file + names_offset + name_offset;
		unsigned bit =
			!strcmp(name, "ER_RO")				  ? 1u :
			!strcmp(name, "ER_RW")				  ? 2u :
			(!strcmp(name, "ER_ZI") || !strcmp(name, ".bss")) ? 4u :
			!strcmp(name, ".vm_res")			  ? 8u :
									    0;
		if (!bit)
			continue;
		if (seen & bit)
			return fail(error, error_size, "duplicate ADS section");
		seen |= bit;
		uint32_t address = le32(section + 12);
		if ((bit == 1 && (flags & (SHF_ALLOC | SHF_EXECINSTR)) !=
					 (SHF_ALLOC | SHF_EXECINSTR)) ||
		    ((bit == 2 || bit == 4) &&
		     (flags & (SHF_ALLOC | SHF_WRITE)) !=
			     (SHF_ALLOC | SHF_WRITE)))
			return fail(
				error, error_size,
				"ADS scatter section flags do not match its role");
		if (bit != 4 &&
		    (kind != 1 || !range(offset, n, info->tags_offset)))
			return fail(error, error_size,
				    "ADS section outside ELF payload");
		if (bit == 1) {
			ro_address = address;
			info->ro_file_offset = offset;
			info->ro_file_size = info->ro_size = n;
		} else if (bit == 2) {
			if (address)
				return fail(error, error_size,
					    "ADS RW is not SB relative");
			info->rw_file_offset = offset;
			info->rw_file_size = info->rw_size = n;
		} else if (bit == 4) {
			if (kind != 8)
				return fail(error, error_size,
					    "ADS ZI is not NOBITS");
			zi_address = address;
			info->zi_size = n;
		} else {
			info->resource_file_offset = offset;
			/* Packaged MRE named resources use file offsets. Some SDK
			 * packers append payloads without updating .vm_res sh_size;
			 * the tag boundary remains the bounded resource-file end. */
			info->resource_size = info->tags_offset - offset;
		}
	}
	if (!(seen & 1u) || !(seen & 2u) || !(seen & 4u) ||
	    zi_address != info->rw_size)
		return fail(error, error_size,
			    "incomplete/noncontiguous ADS scatter sections");
	uint32_t plain_entry = entry & ~1u;
	if (plain_entry < ro_address ||
	    (uint64_t)plain_entry >= (uint64_t)ro_address + info->ro_size)
		return fail(error, error_size, "ADS entry outside RO");
	info->entry_offset = plain_entry - ro_address;
	info->entry_thumb = entry & 1u;
	return 0;
}

static int guarded_memory(size_t wanted, void **mapping, size_t *mapping_size,
			  uint8_t **body, size_t *body_size)
{
	long page_result = sysconf(_SC_PAGESIZE);
	if (page_result <= 0) {
		errno = EINVAL;
		return -1;
	}
	size_t page = (size_t)page_result;
	if (wanted > SIZE_MAX - (page - 1) - 2 * page) {
		errno = EOVERFLOW;
		return -1;
	}
	size_t usable = ((wanted + page - 1) / page) * page;
	size_t total = usable + 2 * page;
	void *p = mmap(NULL, total, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1,
		       0);
	if (p == MAP_FAILED)
		return -1;
	uint8_t *bytes = (uint8_t *)p + page;
	if (mprotect(bytes, usable, PROT_READ | PROT_WRITE)) {
		int saved = errno;
		munmap(p, total);
		errno = saved;
		return -1;
	}
	*mapping = p;
	*mapping_size = total;
	*body = bytes;
	*body_size = usable;
	return 0;
}

static int inflate_exact(uint8_t *target, uint32_t target_size,
			 const uint8_t *source, uint32_t source_size)
{
	/* A one-byte destination permits a valid empty stream to finish while
     * still requiring an exact zero decoded length. */
	uint8_t empty;
	uLongf decoded = target_size ? target_size : 1;
	uLong consumed = source_size;
	int status = uncompress2(target_size ? target : &empty, &decoded,
				 source, &consumed);
	return status == Z_OK && decoded == target_size &&
			       consumed == source_size ?
		       0 :
		       -1;
}

int ads_image_load(const void *input, size_t size,
		   const struct ads_limits *limits, struct ads_image *out,
		   char *error, size_t error_size)
{
	struct ads_image image = { 0 };
	struct ads_limits defaults = ads_default_limits();
	const struct ads_limits *cap = limits ? limits : &defaults;
	const uint8_t *file = input;
	if (!out)
		return fail(error, error_size, "missing output");
	memset(out, 0, sizeof(*out));
	if (!file || size > cap->max_file_bytes || size > UINT32_MAX ||
	    cap->max_ram_bytes > UINT32_MAX || cap->heap_bytes > UINT32_MAX ||
	    cap->stack_bytes < 3072 || cap->stack_bytes > cap->max_ram_bytes)
		return fail(error, error_size,
			    "file/allocation limits exceeded");
	image.info.file_type = UINT32_MAX;
	image.info.compiler = UINT32_MAX;
	if (tags(file, size, &image.info, error, error_size))
		return -1;
	if (image.info.compressed > 1)
		return fail(error, error_size, "invalid compression tag");
	if ((image.info.compressed ?
		     compressed_info(file, &image.info, error, error_size) :
		     elf_info(file, size, &image.info, error, error_size)))
		return -1;
	struct ads_info *info = &image.info;
	if (info->ro_size < 4 || info->ro_size > cap->max_ro_bytes ||
	    (!info->entry_thumb && (info->entry_offset & 3u)) ||
	    (info->entry_thumb && (info->entry_offset & 1u)))
		return fail(error, error_size,
			    "invalid RO size/entry alignment");
	uint64_t source = (uint64_t)info->ro_size + info->rw_size;
	uint64_t data = (source + ADS_BOOTSTRAP_SIZE + 7u) & ~UINT64_C(7);
	uint64_t heap = data + info->rw_size + info->zi_size;
	uint64_t needed = heap + cap->heap_bytes;
	uint64_t requested = (uint64_t)info->ram_kib * 1024u;
	uint64_t ram = requested > ADS_MIN_RAM ? requested : ADS_MIN_RAM;
	if (needed > ram)
		ram = needed;
	if (source > UINT32_MAX || needed > cap->max_ram_bytes ||
	    ram > cap->max_ram_bytes)
		return fail(error, error_size,
			    "ADS image/RAM exceeds allocation cap");
	info->source_size = (size_t)source;
	info->data_offset = (size_t)data;
	info->bootstrap_offset = info->data_offset - ADS_BOOTSTRAP_SIZE;
	info->heap_offset = (size_t)heap;
	if (guarded_memory((size_t)ram, &image.memory_mapping,
			   &image.memory_mapping_size, &image.memory,
			   &image.memory_size) ||
	    guarded_memory(cap->stack_bytes, &image.stack_mapping,
			   &image.stack_mapping_size, &image.stack_bottom,
			   &image.stack_size)) {
		fail(error, error_size, "guarded allocation: %s",
		     strerror(errno));
		goto bad;
	}
	image.stack_top = image.stack_bottom + image.stack_size;
	if (info->compressed) {
		if (inflate_exact(image.memory, info->ro_size,
				  file + info->ro_file_offset,
				  info->ro_file_size) ||
		    (info->rw_file_size &&
		     inflate_exact(image.memory + info->ro_size, info->rw_size,
				   file + info->rw_file_offset,
				   info->rw_file_size))) {
			fail(error, error_size,
			     "compressed length/checksum/stream mismatch");
			goto bad;
		}
	} else {
		memcpy(image.memory, file + info->ro_file_offset,
		       info->ro_size);
		memcpy(image.memory + info->ro_size,
		       file + info->rw_file_offset, info->rw_size);
	}
	if (info->resource_size) {
		image.resources = malloc(info->resource_size);
		if (!image.resources) {
			fail(error, error_size, "resource allocation failed");
			goto bad;
		}
		memcpy(image.resources, file + info->resource_file_offset,
		       info->resource_size);
	}
	image.entry = ((uintptr_t)image.memory + info->entry_offset) |
		      info->entry_thumb;
	image.data_base = (uintptr_t)image.memory + info->data_offset;
	image.heap_base = (uintptr_t)image.memory + info->heap_offset;
	image.heap_limit = image.heap_base + cap->heap_bytes;
	*out = image;
	if (error && error_size)
		error[0] = 0;
	return 0;
bad:
	ads_image_unload(&image);
	return -1;
}

void ads_image_unload(struct ads_image *image)
{
	if (!image)
		return;
	free(image->resources);
	if (image->stack_mapping)
		munmap(image->stack_mapping, image->stack_mapping_size);
	if (image->memory_mapping)
		munmap(image->memory_mapping, image->memory_mapping_size);
	memset(image, 0, sizeof(*image));
}

int ads_image_bind(struct ads_image *image, uintptr_t resolver,
		   uintptr_t return_stub, char *error, size_t error_size)
{
	if (!image || !image->memory || !image->stack_top || !resolver ||
	    !return_stub || image->data_base > UINT32_MAX ||
	    image->heap_limit > UINT32_MAX ||
	    (uintptr_t)image->stack_top > UINT32_MAX || resolver > UINT32_MAX ||
	    return_stub > UINT32_MAX || image->stack_size > UINT32_MAX)
		return fail(error, error_size,
			    "bootstrap requires valid native 32-bit addresses");
	uintptr_t initial_sp = (uintptr_t)image->stack_top - 8;
	uint8_t *bootstrap = image->memory + image->info.bootstrap_offset;
	put32(bootstrap, (uint32_t)initial_sp);
	put32(bootstrap + 4, (uint32_t)resolver);
	put32(bootstrap + 8, (uint32_t)image->heap_base);
	put32(bootstrap + 12, (uint32_t)image->heap_limit);
	put32(bootstrap + 16, (uint32_t)image->stack_size);
	put32((uint8_t *)initial_sp, 0);
	put32((uint8_t *)initial_sp + 4, (uint32_t)return_stub);
	return 0;
}

int ads_image_allow_execute(struct ads_image *image, char *error,
			    size_t error_size)
{
#if defined(__arm__) && UINTPTR_MAX == UINT32_MAX
	if (!image || !image->memory)
		return fail(error, error_size, "missing loaded image");
	/* ADS startup and live data share an allocation. Preserve that real layout;
     * this initial compatibility module does not claim W^X. */
	__builtin___clear_cache((char *)image->memory,
				(char *)image->memory +
					image->info.source_size);
	if (mprotect(image->memory, image->memory_size,
		     PROT_READ | PROT_WRITE | PROT_EXEC))
		return fail(error, error_size, "executable mapping: %s",
			    strerror(errno));
	return 0;
#else
	(void)image;
	return fail(error, error_size,
		    "execution is available only in native 32-bit ARM runtime");
#endif
}
