/* SPDX-License-Identifier: 0BSD */
/* Native MRE format loading. Admission depends on ABI/layout, never identity.
 * Include after host_resolve; app.ads.resources owns the copied resource bytes
 * for both ABIs. The caller initializes the SDK allocation registry once. */
struct native_vxp_tags {
	uint32_t type, compiler, compressed, ram_kib, resolution;
	size_t payload_bytes;
};
static struct native_vxp_tags native_load_tags(const uint8_t *file, size_t size)
{
	if (size < 12)
		fail("VXP is too short for its trailer");
	struct native_vxp_tags tags = { .type = UINT32_MAX,
					.compiler = UINT32_MAX };
	tags.payload_bytes = read32(file + size - 12);
	if (tags.payload_bytes >= size - 12)
		fail("VXP tag offset is outside the file");
	size_t cursor = tags.payload_bytes;
	unsigned seen = 0;
	for (unsigned count = 0; count < 256; ++count) {
		if (cursor > size - 12 || size - 12 - cursor < 8)
			fail("truncated VXP tag header");
		uint32_t id = read32(file + cursor);
		uint32_t bytes = read32(file + cursor + 4);
		cursor += 8;
		if (bytes > size - 12 - cursor)
			fail("VXP tag value exceeds the file");
		if (!id) {
			if (bytes)
				fail("VXP terminator has a nonzero length");
			if (tags.compressed > 1 ||
			    tags.ram_kib > IMAGE_CAP / 1024)
				fail("VXP compression or requested RAM exceeds the native contract");
			app.ram_limit =
				(tags.ram_kib < 512 ? 512 : tags.ram_kib) *
				1024;
			/* VM_CE_INFO_RESOLUTION_REQ packs width in the upper half.
			 * Zero is explicitly unspecified in the SDK packer. */
			unsigned width = tags.resolution >> 16;
			unsigned height = tags.resolution & 0xffffu;
			if (width >= 32 && width <= 1024 && height >= 32 &&
			    height <= 1024) {
				app.screen_width = (int)width;
				app.screen_height = (int)height;
			}
			fprintf(stderr,
				"VXP format=%u compiler=%u compressed=%u RAM=%uKiB screen=%dx%d\n",
				tags.type, tags.compiler, tags.compressed,
				tags.ram_kib, app.screen_width,
				app.screen_height);
			return tags;
		}
		unsigned bit = id == 0x21 ? 1u :
			       id == 0x15 ? 2u :
			       id == 0x22 ? 4u :
			       id == 0x0f ? 8u :
			       id == 0x10 ? 16u :
					    0;
		if (bit) {
			if (bytes != 4 || (seen & bit))
				fail("duplicate or malformed numeric VXP tag 0x%x",
				     id);
			uint32_t value = read32(file + cursor);
			if (id == 0x21)
				tags.type = value;
			if (id == 0x15)
				tags.compiler = value;
			if (id == 0x22)
				tags.compressed = value;
			if (id == 0x0f)
				tags.ram_kib = value;
			if (id == 0x10)
				tags.resolution = value;
			seen |= bit;
		}
		cursor += bytes;
	}
	fail("VXP tag table has too many entries or no terminator");
}

static int native_resources_absolute;
static size_t native_resource_table_end;
static uint32_t native_resource_offset(uint32_t offset)
{
	if (native_resources_absolute) {
		if (offset < app.ads.info.resource_file_offset)
			fail("resource offset precedes its section");
		offset -= app.ads.info.resource_file_offset;
	}
	if (offset > app.ads.info.resource_size)
		fail("resource offset exceeds its section");
	return offset;
}
static void load_resource_index(void)
{
	size_t cursor = 0, size = app.ads.info.resource_size;
	app.resource_count = 0;
	if (!size)
		return;
	for (;;) {
		if (cursor >= size)
			fail("resource index lacks a bounded terminator");
		const uint8_t *name = app.ads.resources + cursor;
		if (!*name) {
			cursor++;
			break;
		}
		const uint8_t *zero = memchr(name, 0, size - cursor);
		if (!zero || (size_t)(zero - name) >= 128 ||
		    app.resource_count == RESOURCE_CAP)
			fail("resource name or entry count exceeds its bound");
		size_t name_bytes = (size_t)(zero - name);
		cursor += name_bytes + 1;
		if (size - cursor < 8)
			fail("truncated resource offset/length pair");
		app.resources[app.resource_count++] = (struct sdk_resource){
			(const char *)name, name_bytes,
			read32(app.ads.resources + cursor),
			read32(app.ads.resources + cursor + 4)
		};
		cursor += 8;
	}
	native_resource_table_end = cursor;
	int relative = 1, absolute = 1;
	uint32_t base = app.ads.info.resource_file_offset;
	for (size_t i = 0; i < app.resource_count; ++i) {
		const struct sdk_resource *r = &app.resources[i];
		if (!within(r->offset, r->bytes, size) || r->offset < cursor)
			relative = 0;
		if (r->offset < base ||
		    !within(r->offset - base, r->bytes, size) ||
		    r->offset - base < cursor)
			absolute = 0;
	}
	if (app.resource_count && relative && absolute && base) {
		/* SDK resource.bin puts an eight-byte directory pointer/reserved
		 * pair after the name terminator, optionally aligned to four bytes.
		 * Its first payload boundary disambiguates otherwise valid offsets. */
		uint32_t first = UINT32_MAX;
		for (size_t i = 0; i < app.resource_count; ++i)
			if (app.resources[i].offset < first)
				first = app.resources[i].offset;
		size_t aligned = (cursor + 3) & ~(size_t)3;
		int rel_boundary = first == cursor + 8 || first == aligned + 8;
		int abs_boundary = first - base == cursor + 8 ||
				   first - base == aligned + 8;
		if (rel_boundary != abs_boundary) {
			relative = rel_boundary;
			absolute = abs_boundary;
		} else {
			fail("resource offset encoding is ambiguous");
		}
	}
	if (!relative && !absolute)
		fail("resource offsets are invalid or ambiguously encoded");
	native_resources_absolute = !relative && absolute;
	for (size_t i = 0; i < app.resource_count; ++i)
		app.resources[i].offset =
			native_resource_offset(app.resources[i].offset);
	fprintf(stderr, "RESOURCES entries=%zu encoding=%s table_end=%zu\n",
		app.resource_count,
		native_resources_absolute ? "file-absolute" :
					    "section-relative",
		cursor);
}

/* The SDK's mre-2.0 directory pairs a language ID and section/file offset;
 * its sentinel supplies the end of the preceding language table. String
 * offsets inside that table are relative to the table, in both encodings. */
static int native_prepare_strings(uint32_t language)
{
	const struct sdk_resource *directory = NULL;
	for (size_t i = 0; i < app.resource_count; ++i)
		if (!strcasecmp(app.resources[i].name, "mre-2.0")) {
			directory = &app.resources[i];
			break;
		}
	if (!directory || directory->bytes < 8 || directory->bytes % 8)
		return -3;
	const uint8_t *items = app.ads.resources + directory->offset;
	uint32_t chosen_start = 0, chosen_stop = 0;
	unsigned found = 0, ended = 0;
	for (size_t at = 0; at < directory->bytes; at += 8) {
		uint32_t id = read32(items + at);
		if (id == UINT32_MAX) {
			ended = 1;
			break;
		}
		if (at + 16 > directory->bytes)
			fail("resource language directory has no final bound");
		uint32_t start = native_resource_offset(read32(items + at + 4));
		uint32_t stop = native_resource_offset(read32(items + at + 12));
		if (start < native_resource_table_end || stop < start ||
		    !within(start, stop - start, app.ads.info.resource_size))
			fail("resource language table exceeds the resource section");
		if ((id & 0xffff0000u) == 0xffff0000u &&
		    (id & 0xffffu) == language) {
			chosen_start = start;
			chosen_stop = stop;
			found = 1;
		}
	}
	if (!ended)
		fail("resource language directory lacks its sentinel");
	if (!found)
		return -3;
	size_t bytes = chosen_stop - chosen_start;
	const uint8_t *table = app.ads.resources + chosen_start;
	if (bytes < 4)
		fail("truncated resource string table");
	uint32_t count = read32(table);
	if (!count || count > RESOURCE_CAP + 1 ||
	    (uint64_t)count * 8 + 4 > bytes)
		fail("resource string count exceeds its table");
	size_t strings = 0;
	for (uint32_t i = 0; i < count; ++i) {
		uint32_t id = read32(table + 4 + i * 8);
		if (id == UINT32_MAX)
			break;
		uint32_t offset = read32(table + 8 + i * 8);
		if ((offset & 1) || offset < 4 + count * 8 ||
		    !within(offset, 2, bytes) || strings == RESOURCE_CAP)
			fail("resource UTF16 string offset exceeds its table");
		size_t end = offset;
		while (end + 2 <= bytes && (table[end] || table[end + 1]))
			end += 2;
		if (end + 2 > bytes)
			fail("resource UTF16 string lacks a bounded terminator");
		app.string_ids[strings] = id;
		app.string_offsets[strings++] = offset;
	}
	uint8_t *copy = malloc(bytes);
	if (!copy)
		return -1;
	memcpy(copy, table, bytes);
	free(app.string_source);
	app.string_source = copy;
	app.string_source_bytes = bytes;
	app.string_count = strings;
	fprintf(stderr, "RESOURCE_STRINGS language=%u strings=%zu bytes=%zu\n",
		language, strings, bytes);
	return 0;
}

int32_t MRE_ABI host_res_init_with_language(uint32_t language)
{
	check_guest_api_stack();
	int status = native_prepare_strings(language);
	return status ? status : host_res_init();
}

static Elf32_Shdr native_section(const uint8_t *file, const Elf32_Ehdr *header,
				 unsigned index)
{
	if (index >= header->e_shnum)
		fail("ELF section index exceeds the section table");
	Elf32_Shdr section;
	memcpy(&section, file + header->e_shoff + index * sizeof(section),
	       sizeof(section));
	return section;
}
static void native_validate_sections(const uint8_t *file, size_t size,
				     const Elf32_Ehdr *header)
{
	if (!header->e_shnum || header->e_shnum > 512 ||
	    header->e_shentsize != sizeof(Elf32_Shdr) ||
	    !within(header->e_shoff, header->e_shnum * sizeof(Elf32_Shdr),
		    size))
		fail("ELF requires a bounded section table");
	for (unsigned i = 0; i < header->e_shnum; ++i) {
		Elf32_Shdr s = native_section(file, header, i);
		if (s.sh_type != SHT_NOBITS &&
		    !within(s.sh_offset, s.sh_size, size))
			fail("ELF section exceeds its file payload");
	}
}
static void native_copy_resources(const uint8_t *file, size_t payload_bytes,
				  const Elf32_Ehdr *header)
{
	if (header->e_shstrndx == SHN_UNDEF)
		return;
	Elf32_Shdr names = native_section(file, header, header->e_shstrndx);
	if (names.sh_type != SHT_STRTAB)
		fail("ELF section name table is not a string table");
	for (unsigned i = 0; i < header->e_shnum; ++i) {
		Elf32_Shdr s = native_section(file, header, i);
		if (s.sh_name >= names.sh_size ||
		    !memchr(file + names.sh_offset + s.sh_name, 0,
			    names.sh_size - s.sh_name))
			fail("ELF section name lacks a bounded terminator");
		if (strcmp((const char *)file + names.sh_offset + s.sh_name,
			   ".vm_res"))
			continue;
		if (s.sh_type != SHT_PROGBITS || app.ads.resources)
			fail("invalid or duplicate ELF resource section");
		app.ads.info.resource_file_offset = s.sh_offset;
		/* Absolute SDK resource offsets remain bounded by the ELF payload,
		 * including producer appendages omitted from .vm_res sh_size. */
		size_t bytes = payload_bytes - s.sh_offset;
		app.ads.info.resource_size = (uint32_t)bytes;
		app.ads.resources = malloc(bytes ? bytes : 1);
		if (!app.ads.resources)
			fail("resource section allocation failed");
		memcpy(app.ads.resources, file + s.sh_offset, bytes);
	}
}

static int native_uleb(const uint8_t **cursor, const uint8_t *end,
		       uint32_t *value)
{
	uint32_t result = 0;
	for (unsigned shift = 0; shift < 35 && *cursor < end; shift += 7) {
		unsigned byte = *(*cursor)++;
		if (shift == 28 && (byte & 0xf0))
			return 0;
		result |= (byte & 0x7fu) << shift;
		if (!(byte & 0x80)) {
			*value = result;
			return 1;
		}
	}
	return 0;
}
static int native_arm_attributes(const uint8_t *data, size_t bytes)
{
	if (!bytes || data[0] != 'A')
		return 0;
	const uint8_t *cursor = data + 1, *end = data + bytes;
	while ((size_t)(end - cursor) >= 4) {
		uint32_t length = read32(cursor);
		if (length < 5 || length > (size_t)(end - cursor))
			return 0;
		const uint8_t *vendor_end = cursor + length;
		const uint8_t *zero = memchr(cursor + 4, 0, length - 4);
		if (!zero)
			return 0;
		int aeabi = (size_t)(zero - cursor) == 9 &&
			    !memcmp(cursor + 4, "aeabi", 5);
		const uint8_t *sub = zero + 1;
		while (aeabi && sub < vendor_end) {
			const uint8_t *start = sub;
			uint32_t kind;
			if (!native_uleb(&sub, vendor_end, &kind) ||
			    vendor_end - sub < 4)
				return 0;
			uint32_t n = read32(sub);
			sub += 4;
			if (n < (size_t)(sub - start) ||
			    n > (size_t)(vendor_end - start))
				return 0;
			const uint8_t *sub_end = start + n;
			unsigned arm_arch = 0, arm_isa = 0;
			while (kind == 1 && sub < sub_end) {
				uint32_t tag, value;
				if (!native_uleb(&sub, sub_end, &tag))
					return 0;
				if (tag == 4 || tag == 5 || tag == 65 ||
				    tag == 67 || (tag >= 32 && (tag & 1))) {
					zero = memchr(sub, 0,
						      (size_t)(sub_end - sub));
					if (!zero)
						return 0;
					sub = zero + 1;
					continue;
				}
				if (!native_uleb(&sub, sub_end, &value))
					return 0;
				if (tag == 6 && value <= 22)
					arm_arch = value + 1;
				if ((tag == 8 || tag == 9) && value)
					arm_isa = 1;
				if (tag == 32) {
					zero = memchr(sub, 0,
						      (size_t)(sub_end - sub));
					if (!zero)
						return 0;
					sub = zero + 1;
				}
			}
			if (kind == 1 && arm_arch && arm_isa)
				return 1;
			sub = sub_end;
		}
		cursor = vendor_end;
	}
	return 0;
}
static int native_arm_evidence(const uint8_t *file, const Elf32_Ehdr *header)
{
	for (unsigned i = 0; i < header->e_shnum; ++i) {
		Elf32_Shdr s = native_section(file, header, i);
		if (s.sh_type == SHT_ARM_ATTRIBUTES &&
		    native_arm_attributes(file + s.sh_offset, s.sh_size))
			return 1;
	}
	return 0;
}

static uint32_t native_relocation_symbol(const uint8_t *file,
					 const Elf32_Ehdr *header,
					 const Elf32_Shdr *rel, unsigned index,
					 int *weak)
{
	Elf32_Shdr symbols = native_section(file, header, rel->sh_link);
	if ((symbols.sh_type != SHT_DYNSYM && symbols.sh_type != SHT_SYMTAB) ||
	    symbols.sh_entsize != sizeof(Elf32_Sym) ||
	    symbols.sh_size % sizeof(Elf32_Sym) ||
	    index >= symbols.sh_size / sizeof(Elf32_Sym))
		fail("invalid ELF relocation symbol table");
	Elf32_Sym symbol;
	memcpy(&symbol, file + symbols.sh_offset + index * sizeof(symbol),
	       sizeof(symbol));
	if (symbol.st_shndx == SHN_UNDEF) {
		if (ELF32_ST_BIND(symbol.st_info) == STB_WEAK) {
			*weak = 1;
			return 0; /* ELF undefined weak semantics apply to every name. */
		}
		Elf32_Shdr names =
			native_section(file, header, symbols.sh_link);
		if (names.sh_type != SHT_STRTAB ||
		    symbol.st_name >= names.sh_size ||
		    !memchr(file + names.sh_offset + symbol.st_name, 0,
			    names.sh_size - symbol.st_name))
			fail("invalid ELF undefined symbol name");
		fail("unresolved ELF symbol '%s'; MRE imports must use the SDK resolver",
		     (const char *)file + names.sh_offset + symbol.st_name);
	}
	if (symbol.st_shndx == SHN_ABS)
		return symbol.st_value;
	if (symbol.st_shndx >= header->e_shnum ||
	    !in_segment(symbol.st_value & ~1u, 0, 0))
		fail("ELF symbol value is outside its loaded image");
	return (uint32_t)(uintptr_t)app.image + symbol.st_value;
}
static void native_relocate(const uint8_t *file, const Elf32_Ehdr *header)
{
	unsigned relocations = 0, weak_relocations = 0;
	for (unsigned i = 0; i < header->e_shnum; ++i) {
		Elf32_Shdr s = native_section(file, header, i);
		if (s.sh_type == SHT_DYNAMIC) {
			if (s.sh_entsize != sizeof(Elf32_Dyn) ||
			    s.sh_size % sizeof(Elf32_Dyn))
				fail("invalid ELF dynamic table");
			for (size_t at = 0; at < s.sh_size;
			     at += sizeof(Elf32_Dyn)) {
				Elf32_Dyn d;
				memcpy(&d, file + s.sh_offset + at, sizeof(d));
				if (d.d_tag == DT_NEEDED)
					fail("VXP shared-library dependencies are unsupported");
			}
		}
		if (s.sh_type == SHT_RELA && s.sh_size &&
		    (s.sh_flags & SHF_ALLOC))
			fail("ARM RELA relocation tables are unsupported");
		if (s.sh_type != SHT_REL || !s.sh_size ||
		    !(s.sh_flags & SHF_ALLOC))
			continue;
		if (s.sh_entsize != sizeof(Elf32_Rel) ||
		    s.sh_size % sizeof(Elf32_Rel))
			fail("invalid ELF REL table");
		for (size_t at = 0; at < s.sh_size; at += sizeof(Elf32_Rel)) {
			Elf32_Rel r;
			memcpy(&r, file + s.sh_offset + at, sizeof(r));
			unsigned type = ELF32_R_TYPE(r.r_info),
				 symbol_index = ELF32_R_SYM(r.r_info);
			if (type == R_ARM_NONE)
				continue;
			if ((r.r_offset & 3) || !in_segment(r.r_offset, 4, 0))
				fail("ELF relocation target exceeds aligned loaded memory");
			uint32_t addend = read32(app.image + r.r_offset), value;
			if (type == R_ARM_RELATIVE) {
				if (symbol_index ||
				    !in_segment(addend & ~1u, 0, 0))
					fail("ARM relative relocation has an invalid symbol or addend");
				value = (uint32_t)(uintptr_t)app.image + addend;
			} else {
				if (type != R_ARM_ABS32 &&
				    type != R_ARM_REL32 &&
				    type != R_ARM_GLOB_DAT &&
				    type != R_ARM_JUMP_SLOT)
					fail("unsupported ARM relocation type=%u",
					     type);
				int weak = 0;
				uint32_t symbol = native_relocation_symbol(
					file, header, &s, symbol_index, &weak);
				if (weak)
					weak_relocations++;
				value = (type == R_ARM_GLOB_DAT ||
					 type == R_ARM_JUMP_SLOT) ?
						symbol :
						symbol + addend;
				if (type == R_ARM_REL32)
					value -=
						(uint32_t)(uintptr_t)(app.image +
								      r.r_offset);
			}
			memcpy(app.image + r.r_offset, &value, sizeof(value));
			relocations++;
		}
	}
	fprintf(stderr, "ELF_RELOCATIONS total=%u undefined_weak=%u\n",
		relocations, weak_relocations);
}

static void native_load_elf(const uint8_t *file, size_t size,
			    const Elf32_Ehdr *header)
{
	if ((header->e_flags & EF_ARM_EABIMASK) != EF_ARM_EABI_VER5 ||
	    (header->e_flags & EF_ARM_ABI_FLOAT_HARD))
		fail("GCC MRE requires ARM EABI5 with the base/soft-float calling convention");
	native_validate_sections(file, size, header);
	if (header->e_machine != EM_ARM && !(header->e_machine == EM_NONE &&
					     native_arm_evidence(file, header)))
		fail("ELF has no validated ARM machine or architecture attributes");
	if (header->e_type != ET_DYN)
		fail("GCC MRE requires a relocatable ET_DYN image");
	if (!header->e_phnum || header->e_phnum > 128 ||
	    header->e_phentsize != sizeof(Elf32_Phdr) ||
	    !within(header->e_phoff, header->e_phnum * sizeof(Elf32_Phdr),
		    size))
		fail("invalid ELF program header table");
	uint32_t extent = 0;
	for (unsigned i = 0; i < header->e_phnum; ++i) {
		Elf32_Phdr p;
		memcpy(&p, file + header->e_phoff + i * sizeof(p), sizeof(p));
		if (p.p_type == PT_TLS)
			fail("ELF thread-local storage is unsupported");
		if (p.p_type == PT_INTERP &&
		    (!p.p_filesz || !within(p.p_offset, p.p_filesz, size) ||
		     !memchr(file + p.p_offset, 0, p.p_filesz)))
			fail("invalid ELF interpreter metadata");
		/* MRE gcc_entry receives a resolver; PT_INTERP is producer metadata. */
		if (p.p_type != PT_LOAD)
			continue;
		if (!p.p_memsz || p.p_filesz > p.p_memsz ||
		    !within(p.p_offset, p.p_filesz, size) ||
		    !within(p.p_vaddr, p.p_memsz, IMAGE_CAP) ||
		    (p.p_flags & ~(PF_R | PF_W | PF_X)))
			fail("invalid ELF load segment bounds or flags");
		if (p.p_align > 1 &&
		    ((p.p_align & (p.p_align - 1)) ||
		     p.p_vaddr % p.p_align != p.p_offset % p.p_align))
			fail("invalid ELF load segment alignment");
		if (app.segment_count == SEGMENT_CAP)
			fail("ELF has too many load segments");
		for (size_t j = 0; j < app.segment_count; ++j) {
			const struct segment *s = &app.segments[j];
			if ((uint64_t)p.p_vaddr < (uint64_t)s->addr + s->size &&
			    (uint64_t)s->addr < (uint64_t)p.p_vaddr + p.p_memsz)
				fail("ELF load segments overlap");
		}
		app.segments[app.segment_count++] =
			(struct segment){ p.p_vaddr, p.p_memsz, p.p_flags };
		if (p.p_vaddr + p.p_memsz > extent)
			extent = p.p_vaddr + p.p_memsz;
	}
	uint32_t entry = header->e_entry & ~1u;
	if (!extent || (!(header->e_entry & 1) && (entry & 3)) ||
	    !in_segment(entry, header->e_entry & 1 ? 2 : 4, PF_X))
		fail("ELF entry is outside aligned executable memory");
	app.image_size = (extent + app.page_size - 1) & ~(app.page_size - 1);
	app.image = mmap(NULL, app.image_size, PROT_READ | PROT_WRITE,
			 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (app.image == MAP_FAILED ||
	    (uintptr_t)app.image > UINT32_MAX - app.image_size)
		fail("native 32-bit image allocation failed");
	for (unsigned i = 0; i < header->e_phnum; ++i) {
		Elf32_Phdr p;
		memcpy(&p, file + header->e_phoff + i * sizeof(p), sizeof(p));
		if (p.p_type == PT_LOAD)
			memcpy(app.image + p.p_vaddr, file + p.p_offset,
			       p.p_filesz);
	}
	native_copy_resources(file, size, header);
	native_relocate(file, header);
	__builtin___clear_cache((char *)app.image,
				(char *)app.image + app.image_size);
	for (size_t page = 0; page < app.image_size; page += app.page_size) {
		int prot = PROT_NONE;
		for (size_t i = 0; i < app.segment_count; ++i) {
			const struct segment *s = &app.segments[i];
			if (page < (uint64_t)s->addr + s->size &&
			    s->addr < page + app.page_size) {
				if (s->flags & PF_R)
					prot |= PROT_READ;
				if (s->flags & PF_W)
					prot |= PROT_WRITE;
				if (s->flags & PF_X)
					prot |= PROT_EXEC;
			}
		}
		/* Preserve each producer's native mapping, including shared RWE pages. */
		if (mprotect(app.image + page, app.page_size, prot))
			fail("setting ELF page protections failed");
	}
	app.entry = (uintptr_t)app.image + header->e_entry;
	fprintf(stderr,
		"NATIVE_IMAGE ABI=GCC base=%p bytes=%zu entry=%p thumb=%u\n",
		app.image, app.image_size, (void *)app.entry,
		(unsigned)(app.entry & 1));
}

static void load_vxp(const char *path)
{
	long page_size = sysconf(_SC_PAGESIZE);
	if (page_size <= 0 ||
	    ((unsigned long)page_size & ((unsigned long)page_size - 1)))
		fail("invalid native memory page size");
	app.page_size = (size_t)page_size;
	if (!app.screen_width || !app.screen_height) {
		app.screen_width = MRE_FRAME_WIDTH;
		app.screen_height = MRE_FRAME_HEIGHT;
	}
	FILE *input = fopen(path, "rb");
	if (!input)
		fail("opening VXP: %s", strerror(errno));
	struct stat status;
	if (fstat(fileno(input), &status) || !S_ISREG(status.st_mode) ||
	    status.st_size < 0 || status.st_size > FILE_CAP)
		fail("VXP file type or size exceeds its bound");
	size_t bytes = (size_t)status.st_size;
	uint8_t *file = malloc(bytes ? bytes : 1);
	if (!file || fread(file, 1, bytes, input) != bytes || fclose(input))
		fail("reading VXP failed");
	struct native_vxp_tags tags = native_load_tags(file, bytes);
	Elf32_Ehdr header = { 0 };
	int elf = tags.payload_bytes >= sizeof(header) &&
		  !memcmp(file, ELFMAG, SELFMAG);
	if (elf) {
		memcpy(&header, file, sizeof(header));
		if (header.e_ident[EI_CLASS] != ELFCLASS32 ||
		    header.e_ident[EI_DATA] != ELFDATA2LSB ||
		    header.e_ident[EI_VERSION] != EV_CURRENT ||
		    header.e_version != EV_CURRENT ||
		    header.e_ehsize != sizeof(header))
			fail("VXP requires a little-endian ELF32 payload");
	}
	app.ads_abi = tags.compressed ||
		      (elf && header.e_type == ET_EXEC &&
		       (header.e_flags & EF_ARM_EABIMASK) == EF_ARM_EABI_VER2);
	if (app.ads_abi) {
		char error[256];
		if (ads_image_load(file, bytes, NULL, &app.ads, error,
				   sizeof(error)) ||
		    ads_image_bind(&app.ads, (uintptr_t)ads_host_resolve,
				   (uintptr_t)ads_guest_return, error,
				   sizeof(error)) ||
		    ads_image_allow_execute(&app.ads, error, sizeof(error)))
			fail("ADS image: %s", error);
		app.image = app.ads.memory;
		app.image_size = app.ads.memory_size;
		app.entry = app.ads.entry;
		app.stack_low = app.ads.stack_bottom;
		app.stack_top = app.ads.stack_top;
		app.ram_limit = (uint32_t)app.ads.memory_size;
		app.segments[0] = (struct segment){ 0, app.ads.info.ro_size,
						    PF_R | PF_X };
		app.segments[1] = (struct segment){
			app.ads.info.ro_size,
			(uint32_t)(app.ads.memory_size - app.ads.info.ro_size),
			PF_R | PF_W
		};
		app.segment_count = 2;
		fprintf(stderr,
			"NATIVE_IMAGE ABI=ADS RO=%u RW=%u ZI=%u resources=%u SB=%p\n",
			app.ads.info.ro_size, app.ads.info.rw_size,
			app.ads.info.zi_size, app.ads.info.resource_size,
			(void *)app.ads.data_base);
	} else {
		if (!elf)
			fail("VXP payload has no supported ELF or compressed ADS contract");
		native_load_elf(file, tags.payload_bytes, &header);
	}
	load_resource_index();
	(void)native_prepare_strings(
		44); /* SDK English, the configured default locale. */
	memcpy(app.game_language, "en", 3);
	free(file);
}
