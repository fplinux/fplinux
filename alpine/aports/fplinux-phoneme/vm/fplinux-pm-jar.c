/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * MIDlet suite JAR reader: finds entries through the ZIP central directory,
 * extracts stored and deflated entries and parses the main section of
 * META-INF/MANIFEST.MF.
 *
 * The JAR comes from a removable card and is untrusted. Every offset and size
 * is checked against the file before use. The central directory is the only
 * source of entry sizes: with general-purpose flag bit 3 the local header
 * carries zeros and the real values follow the data.
 */
#define _POSIX_C_SOURCE 200809L

#include "fplinux-pm-vm.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#define ZIP_LOCAL_SIGNATURE 0x04034b50U
#define ZIP_CENTRAL_SIGNATURE 0x02014b50U
#define ZIP_END_SIGNATURE 0x06054b50U
#define ZIP_END64_LOCATOR_SIGNATURE 0x07064b50U
#define ZIP_LOCAL_BYTES 30U
#define ZIP_CENTRAL_BYTES 46U
#define ZIP_END_BYTES 22U
#define ZIP_END64_LOCATOR_BYTES 20U
#define ZIP_COMMENT_MAX_BYTES 65535U
#define ZIP_FLAG_ENCRYPTED 0x0001U
#define ZIP_METHOD_STORED 0U
#define ZIP_METHOD_DEFLATED 8U

/* Larger directories or entry names are not plausible in a MIDlet suite. */
#define JAR_DIRECTORY_MAX_BYTES (4U * 1024U * 1024U)
#define JAR_NAME_MAX_BYTES 1024U
#define JAR_READ_CHUNK_BYTES 4096U

struct jar {
	int fd;
	uint64_t file_size;
	/* Entry data ends where the central directory starts. */
	uint32_t directory_offset;
	uint32_t directory_size;
	size_t entry_count;
	unsigned char *directory;
};

struct jar_entry {
	unsigned int flags;
	unsigned int method;
	uint32_t crc;
	uint32_t compressed_size;
	uint32_t size;
	uint32_t local_offset;
};

static uint16_t get16(const unsigned char *bytes)
{
	return (uint16_t)(bytes[0] | bytes[1] << 8);
}

static uint32_t get32(const unsigned char *bytes)
{
	return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 |
	       (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

/* Reads exactly size bytes; a file that ends early is damaged. */
static enum fplinux_pm_jar_status read_at(int fd, uint64_t offset, void *buffer,
					  size_t size)
{
	unsigned char *bytes = buffer;

	while (size) {
		ssize_t count = pread(fd, bytes, size, (off_t)offset);

		if (count < 0) {
			if (errno == EINTR)
				continue;
			return FPLINUX_PM_JAR_IO;
		}
		if (!count)
			return FPLINUX_PM_JAR_FORMAT;
		bytes += count;
		size -= (size_t)count;
		offset += (uint64_t)count;
	}
	return FPLINUX_PM_JAR_OK;
}

static enum fplinux_pm_jar_status parse_end(struct jar *jar,
					    const unsigned char *tail,
					    size_t position,
					    uint64_t end_offset)
{
	const unsigned char *end = tail + position;
	unsigned int entry_count = get16(end + 10);
	uint32_t directory_size = get32(end + 12);
	uint32_t directory_offset = get32(end + 16);

	/* Multi-disk archives and ZIP64 are not MIDlet suites. */
	if (get16(end + 4) || get16(end + 6) || get16(end + 8) != entry_count)
		return FPLINUX_PM_JAR_FORMAT;
	if (entry_count == 0xffffU || directory_size == UINT32_MAX ||
	    directory_offset == UINT32_MAX)
		return FPLINUX_PM_JAR_FORMAT;
	if (position >= ZIP_END64_LOCATOR_BYTES &&
	    get32(end - ZIP_END64_LOCATOR_BYTES) == ZIP_END64_LOCATOR_SIGNATURE)
		return FPLINUX_PM_JAR_FORMAT;

	if (directory_size > JAR_DIRECTORY_MAX_BYTES ||
	    (uint64_t)entry_count * ZIP_CENTRAL_BYTES > directory_size ||
	    (uint64_t)directory_offset + directory_size > end_offset)
		return FPLINUX_PM_JAR_FORMAT;
	jar->entry_count = entry_count;
	jar->directory_offset = directory_offset;
	jar->directory_size = directory_size;
	return FPLINUX_PM_JAR_OK;
}

/*
 * The end record is the last 22 bytes before the archive comment. Searching
 * back from the end, accept the first signature whose comment length reaches
 * exactly the end of the file, so a signature inside a comment is skipped.
 */
static enum fplinux_pm_jar_status
find_end(struct jar *jar, const unsigned char *tail, size_t tail_size)
{
	uint64_t tail_offset = jar->file_size - tail_size;
	size_t position = tail_size - ZIP_END_BYTES;

	for (;;) {
		const unsigned char *end = tail + position;

		if (get32(end) == ZIP_END_SIGNATURE &&
		    position + ZIP_END_BYTES + get16(end + 20) == tail_size)
			return parse_end(jar, tail, position,
					 tail_offset + position);
		if (!position)
			return FPLINUX_PM_JAR_FORMAT;
		--position;
	}
}

static enum fplinux_pm_jar_status jar_open(struct jar *jar, const char *path)
{
	enum fplinux_pm_jar_status status;
	struct stat file;
	unsigned char *tail;
	size_t tail_size;

	jar->fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
	if (jar->fd < 0 || fstat(jar->fd, &file))
		return FPLINUX_PM_JAR_IO;
	if (!S_ISREG(file.st_mode)) {
		errno = EINVAL;
		return FPLINUX_PM_JAR_IO;
	}
	if (file.st_size < (off_t)ZIP_END_BYTES)
		return FPLINUX_PM_JAR_FORMAT;
	jar->file_size = (uint64_t)file.st_size;

	tail_size = ZIP_END_BYTES + ZIP_COMMENT_MAX_BYTES;
	if (jar->file_size < tail_size)
		tail_size = (size_t)jar->file_size;
	tail = malloc(tail_size);
	if (!tail)
		return FPLINUX_PM_JAR_NO_MEMORY;
	status = read_at(jar->fd, jar->file_size - tail_size, tail, tail_size);
	if (status == FPLINUX_PM_JAR_OK)
		status = find_end(jar, tail, tail_size);
	free(tail);
	if (status != FPLINUX_PM_JAR_OK)
		return status;

	jar->directory = malloc(jar->directory_size ? jar->directory_size : 1);
	if (!jar->directory)
		return FPLINUX_PM_JAR_NO_MEMORY;
	return read_at(jar->fd, jar->directory_offset, jar->directory,
		       jar->directory_size);
}

static void jar_close(struct jar *jar)
{
	free(jar->directory);
	jar->directory = NULL;
	if (jar->fd >= 0)
		close(jar->fd);
	jar->fd = -1;
}

static enum fplinux_pm_jar_status
parse_central_entry(const unsigned char *record, struct jar_entry *entry)
{
	entry->flags = get16(record + 8);
	entry->method = get16(record + 10);
	entry->crc = get32(record + 16);
	entry->compressed_size = get32(record + 20);
	entry->size = get32(record + 24);
	entry->local_offset = get32(record + 42);

	if (entry->flags & ZIP_FLAG_ENCRYPTED)
		return FPLINUX_PM_JAR_FORMAT;
	if (entry->compressed_size == UINT32_MAX || entry->size == UINT32_MAX ||
	    entry->local_offset == UINT32_MAX)
		return FPLINUX_PM_JAR_FORMAT;
	if (entry->method == ZIP_METHOD_STORED)
		return entry->compressed_size == entry->size ?
			       FPLINUX_PM_JAR_OK :
			       FPLINUX_PM_JAR_FORMAT;
	return entry->method == ZIP_METHOD_DEFLATED ? FPLINUX_PM_JAR_OK :
						      FPLINUX_PM_JAR_FORMAT;
}

static enum fplinux_pm_jar_status
jar_find(const struct jar *jar, const char *name, struct jar_entry *entry)
{
	size_t name_length = strlen(name);
	size_t offset = 0;
	size_t index;

	for (index = 0; index < jar->entry_count; ++index) {
		const unsigned char *record = jar->directory + offset;
		size_t remaining = jar->directory_size - offset;
		size_t record_name_length;
		size_t record_size;

		if (remaining < ZIP_CENTRAL_BYTES ||
		    get32(record) != ZIP_CENTRAL_SIGNATURE)
			return FPLINUX_PM_JAR_FORMAT;
		record_name_length = get16(record + 28);
		record_size = ZIP_CENTRAL_BYTES + record_name_length +
			      get16(record + 30) + get16(record + 32);
		if (record_name_length > JAR_NAME_MAX_BYTES ||
		    record_size > remaining)
			return FPLINUX_PM_JAR_FORMAT;
		if (record_name_length == name_length &&
		    !memcmp(record + ZIP_CENTRAL_BYTES, name, name_length))
			return parse_central_entry(record, entry);
		offset += record_size;
	}
	return FPLINUX_PM_JAR_NO_ENTRY;
}

static enum fplinux_pm_jar_status inflate_entry(int fd, uint64_t offset,
						const struct jar_entry *entry,
						unsigned char *output)
{
	unsigned char input[JAR_READ_CHUNK_BYTES];
	uint32_t remaining = entry->compressed_size;
	enum fplinux_pm_jar_status status = FPLINUX_PM_JAR_OK;
	z_stream stream;
	int result;

	memset(&stream, 0, sizeof(stream));
	if (inflateInit2(&stream, -MAX_WBITS) != Z_OK)
		return FPLINUX_PM_JAR_NO_MEMORY;
	stream.next_out = output;
	stream.avail_out = entry->size;
	for (;;) {
		if (!stream.avail_in && remaining) {
			uint32_t chunk = remaining < sizeof(input) ?
						 remaining :
						 (uint32_t)sizeof(input);

			status = read_at(fd, offset, input, chunk);
			if (status != FPLINUX_PM_JAR_OK)
				break;
			offset += chunk;
			remaining -= chunk;
			stream.next_in = input;
			stream.avail_in = chunk;
		}
		result = inflate(&stream, Z_NO_FLUSH);
		if (result == Z_STREAM_END) {
			/* The stream must fill exactly the declared size. */
			if (stream.avail_out)
				status = FPLINUX_PM_JAR_FORMAT;
			break;
		}
		if (result != Z_OK) {
			/* Z_BUF_ERROR: input ended or output overflowed. */
			status = result == Z_MEM_ERROR ?
					 FPLINUX_PM_JAR_NO_MEMORY :
					 FPLINUX_PM_JAR_FORMAT;
			break;
		}
	}
	inflateEnd(&stream);
	return status;
}

/* On any result *data is either NULL or a buffer the caller frees. */
static enum fplinux_pm_jar_status jar_extract(const struct jar *jar,
					      const struct jar_entry *entry,
					      unsigned char **data)
{
	unsigned char header[ZIP_LOCAL_BYTES];
	enum fplinux_pm_jar_status status;
	uint64_t data_offset;

	if ((uint64_t)entry->local_offset + ZIP_LOCAL_BYTES >
	    jar->directory_offset)
		return FPLINUX_PM_JAR_FORMAT;
	status = read_at(jar->fd, entry->local_offset, header, sizeof(header));
	if (status != FPLINUX_PM_JAR_OK)
		return status;
	if (get32(header) != ZIP_LOCAL_SIGNATURE)
		return FPLINUX_PM_JAR_FORMAT;
	data_offset = (uint64_t)entry->local_offset + ZIP_LOCAL_BYTES +
		      get16(header + 26) + get16(header + 28);
	if (data_offset > jar->directory_offset ||
	    entry->compressed_size > jar->directory_offset - data_offset)
		return FPLINUX_PM_JAR_FORMAT;

	*data = malloc(entry->size ? entry->size : 1);
	if (!*data)
		return FPLINUX_PM_JAR_NO_MEMORY;
	if (entry->method == ZIP_METHOD_STORED)
		status = read_at(jar->fd, data_offset, *data, entry->size);
	else
		status = inflate_entry(jar->fd, data_offset, entry, *data);
	if (status == FPLINUX_PM_JAR_OK &&
	    crc32(0L, *data, (uInt)entry->size) != entry->crc)
		status = FPLINUX_PM_JAR_FORMAT;
	return status;
}

enum fplinux_pm_jar_status
fplinux_pm_jar_read_entry(const char *jar_path, const char *entry_path,
			  size_t max_size, unsigned char **data, size_t *size)
{
	struct jar jar = { .fd = -1 };
	enum fplinux_pm_jar_status status;
	struct jar_entry entry;
	int saved_errno;

	*data = NULL;
	*size = 0;
	if (entry_path[0] == '/')
		++entry_path;
	status = jar_open(&jar, jar_path);
	if (status == FPLINUX_PM_JAR_OK)
		status = jar_find(&jar, entry_path, &entry);
	if (status == FPLINUX_PM_JAR_OK && entry.size > max_size)
		status = FPLINUX_PM_JAR_TOO_LARGE;
	if (status == FPLINUX_PM_JAR_OK)
		status = jar_extract(&jar, &entry, data);

	saved_errno = errno;
	if (status == FPLINUX_PM_JAR_OK) {
		*size = entry.size;
	} else {
		free(*data);
		*data = NULL;
	}
	jar_close(&jar);
	errno = saved_errno;
	return status;
}

/* --- Manifest main section ----------------------------------------------- */

struct text {
	const char *start; /* NULL: absent */
	size_t length;
};

struct manifest_attributes {
	struct text name;
	struct text vendor;
	struct text version;
	struct text midlets[FPLINUX_PM_JAR_MIDLETS_MAX + 1]; /* by <n> */
};

struct midlet_fields {
	struct text name;
	struct text icon;
	struct text class_name;
};

static bool is_blank(char character)
{
	return character == ' ' || character == '\t';
}

static char ascii_lower(char character)
{
	return character >= 'A' && character <= 'Z' ?
		       (char)(character - 'A' + 'a') :
		       character;
}

static struct text trim(const char *start, size_t length)
{
	while (length && is_blank(*start)) {
		++start;
		--length;
	}
	while (length && is_blank(start[length - 1]))
		--length;
	return (struct text){ start, length };
}

/* Attribute names compare case-insensitively: real JARs vary. */
static bool name_is(const char *name, size_t length, const char *expected)
{
	size_t index;

	if (length != strlen(expected))
		return false;
	for (index = 0; index < length; ++index)
		if (ascii_lower(name[index]) != ascii_lower(expected[index]))
			return false;
	return true;
}

/* Returns n of "MIDlet-<n>" for 1 <= n <= FPLINUX_PM_JAR_MIDLETS_MAX, else 0. */
static size_t midlet_number(const char *name, size_t length)
{
	static const char prefix[] = "MIDlet-";
	const size_t prefix_length = sizeof(prefix) - 1;
	size_t number = 0;
	size_t index;

	if (length <= prefix_length || !name_is(name, prefix_length, prefix) ||
	    name[prefix_length] == '0')
		return 0;
	for (index = prefix_length; index < length; ++index) {
		if (name[index] < '0' || name[index] > '9')
			return 0;
		number = number * 10 + (size_t)(name[index] - '0');
		if (number > FPLINUX_PM_JAR_MIDLETS_MAX)
			return 0;
	}
	return number;
}

/* "Name: value" splits at the first ':'; the last duplicate wins. */
static void store_attribute(struct manifest_attributes *attributes,
			    struct text line)
{
	const char *colon = memchr(line.start, ':', line.length);
	size_t name_length;
	struct text value;
	size_t number;

	if (!colon)
		return;
	name_length = (size_t)(colon - line.start);
	value = trim(colon + 1, line.length - name_length - 1);
	if (name_is(line.start, name_length, "MIDlet-Name"))
		attributes->name = value;
	else if (name_is(line.start, name_length, "MIDlet-Vendor"))
		attributes->vendor = value;
	else if (name_is(line.start, name_length, "MIDlet-Version"))
		attributes->version = value;
	else if ((number = midlet_number(line.start, name_length)))
		attributes->midlets[number] = value;
}

/* Returns the next physical line without its CRLF, LF or CR ending. */
static bool next_line(const char **cursor, const char *end, struct text *line)
{
	const char *start = *cursor;
	const char *position = start;

	if (start == end)
		return false;
	while (position < end && *position != '\r' && *position != '\n')
		++position;
	*line = (struct text){ start, (size_t)(position - start) };
	if (position < end) {
		if (*position == '\r' && position + 1 < end &&
		    position[1] == '\n')
			++position;
		++position;
	}
	*cursor = position;
	return true;
}

/*
 * Joins continuation lines into scratch, which needs size bytes: every byte
 * written there comes from a different byte of the manifest.
 */
static void read_main_section(const char *manifest, size_t size, char *scratch,
			      struct manifest_attributes *attributes)
{
	const char *cursor = manifest;
	const char *end = manifest + size;
	struct text logical = { NULL, 0 };
	struct text line;
	char *output = scratch;

	if (size >= 3 && !memcmp(manifest, "\xef\xbb\xbf", 3))
		cursor += 3;
	while (next_line(&cursor, end, &line)) {
		if (!line.length) {
			if (logical.start)
				break;
			continue;
		}
		if (line.start[0] == ' ') {
			if (logical.start) {
				memcpy(output, line.start + 1, line.length - 1);
				output += line.length - 1;
				logical.length += line.length - 1;
			}
			continue;
		}
		if (logical.start)
			store_attribute(attributes, logical);
		memcpy(output, line.start, line.length);
		logical = (struct text){ output, line.length };
		output += line.length;
	}
	if (logical.start)
		store_attribute(attributes, logical);
}

/* "name, icon, class": the icon lies between the first and the last comma. */
static bool split_midlet(struct text value, struct midlet_fields *fields)
{
	const char *first;
	const char *last;

	if (!value.start)
		return false;
	first = memchr(value.start, ',', value.length);
	if (!first)
		return false;
	last = value.start + value.length - 1;
	while (*last != ',')
		--last;
	if (first == last)
		return false;
	fields->name = trim(value.start, (size_t)(first - value.start));
	fields->icon = trim(first + 1, (size_t)(last - first - 1));
	fields->class_name =
		trim(last + 1, (size_t)(value.start + value.length - last - 1));
	return fields->name.length && fields->class_name.length;
}

static char *copy_text(struct text text)
{
	return strndup(text.start ? text.start : "", text.length);
}

static enum fplinux_pm_jar_status
build_suite(const struct manifest_attributes *attributes,
	    struct fplinux_pm_suite *suite)
{
	struct midlet_fields fields[FPLINUX_PM_JAR_MIDLETS_MAX];
	size_t count = 0;
	size_t index;

	while (count < FPLINUX_PM_JAR_MIDLETS_MAX &&
	       split_midlet(attributes->midlets[count + 1], &fields[count]))
		++count;
	if (!count)
		return FPLINUX_PM_JAR_NO_MIDLETS;

	suite->midlets = calloc(count, sizeof(*suite->midlets));
	if (!suite->midlets)
		return FPLINUX_PM_JAR_NO_MEMORY;
	suite->midlet_count = count;
	for (index = 0; index < count; ++index) {
		struct fplinux_pm_midlet *midlet = &suite->midlets[index];

		midlet->name = copy_text(fields[index].name);
		midlet->icon = copy_text(fields[index].icon);
		midlet->class_name = copy_text(fields[index].class_name);
		if (!midlet->name || !midlet->icon || !midlet->class_name)
			goto no_memory;
	}
	suite->name = copy_text(attributes->name);
	suite->vendor = copy_text(attributes->vendor);
	suite->version = copy_text(attributes->version);
	if (suite->name && suite->vendor && suite->version)
		return FPLINUX_PM_JAR_OK;
no_memory:
	fplinux_pm_suite_cleanup(suite);
	return FPLINUX_PM_JAR_NO_MEMORY;
}

enum fplinux_pm_jar_status
fplinux_pm_jar_read_suite(const char *jar_path, struct fplinux_pm_suite *suite)
{
	struct manifest_attributes attributes;
	enum fplinux_pm_jar_status status;
	unsigned char *manifest;
	char *scratch;
	size_t size;

	memset(suite, 0, sizeof(*suite));
	status = fplinux_pm_jar_read_entry(jar_path, "META-INF/MANIFEST.MF",
					   FPLINUX_PM_JAR_MANIFEST_MAX,
					   &manifest, &size);
	if (status != FPLINUX_PM_JAR_OK)
		return status;
	scratch = malloc(size + 1);
	if (!scratch) {
		free(manifest);
		return FPLINUX_PM_JAR_NO_MEMORY;
	}
	memset(&attributes, 0, sizeof(attributes));
	read_main_section((const char *)manifest, size, scratch, &attributes);
	status = build_suite(&attributes, suite);
	free(scratch);
	free(manifest);
	return status;
}

void fplinux_pm_suite_cleanup(struct fplinux_pm_suite *suite)
{
	size_t index;

	for (index = 0; index < suite->midlet_count; ++index) {
		free(suite->midlets[index].name);
		free(suite->midlets[index].icon);
		free(suite->midlets[index].class_name);
	}
	free(suite->midlets);
	free(suite->name);
	free(suite->vendor);
	free(suite->version);
	memset(suite, 0, sizeof(*suite));
}
