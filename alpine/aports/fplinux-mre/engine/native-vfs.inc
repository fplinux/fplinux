/* SPDX-License-Identifier: 0BSD */
/* SDK storage adapters for the native executables. The parent owns all
 * filesystem authority; this process only checks guest memory and packs ABI
 * records. Include after guest_data/check_guest_api_stack and the IPC helpers.
 */
#include "vfs-broker.h"

struct sdk_utility_fileinfo {
	uint16_t filename[MRE_VFS_PATH_BYTES];
	int32_t size;
};
struct sdk_utility_fileinfo_ext {
	uint16_t filefullname[MRE_VFS_PATH_BYTES];
	char filename[8], extension[3];
	uint8_t attributes;
	int32_t create_datetime[6], modify_datetime[6];
	uint32_t filesize, drive, stamp;
};
_Static_assert(sizeof(struct sdk_utility_fileinfo) == 524 &&
		       _Alignof(struct sdk_utility_fileinfo) == 4 &&
		       offsetof(struct sdk_utility_fileinfo, size) == 520,
	       "SDK old directory entry ABI");
_Static_assert(
	sizeof(struct sdk_utility_fileinfo_ext) == 592 &&
		_Alignof(struct sdk_utility_fileinfo_ext) == 4 &&
		offsetof(struct sdk_utility_fileinfo_ext, filename) == 520 &&
		offsetof(struct sdk_utility_fileinfo_ext, extension) == 528 &&
		offsetof(struct sdk_utility_fileinfo_ext, attributes) == 531 &&
		offsetof(struct sdk_utility_fileinfo_ext, create_datetime) ==
			532 &&
		offsetof(struct sdk_utility_fileinfo_ext, modify_datetime) ==
			556 &&
		offsetof(struct sdk_utility_fileinfo_ext, filesize) == 580 &&
		offsetof(struct sdk_utility_fileinfo_ext, drive) == 584 &&
		offsetof(struct sdk_utility_fileinfo_ext, stamp) == 588,
	"SDK extended directory entry ABI");

typedef int32_t(MRE_ABI *utility_copy_fn)(int32_t, uint32_t, uint32_t, int32_t);

/* exchange fills sequence, waits for the matching response, and
 * buffers lifecycle/input messages without entering another guest callback.
 * copy_callback calls all four SDK arguments on a checked nested guest stack.
 */
static int utility_vfs_exchange(const struct mre_vfs_request *,
				struct mre_vfs_response *);
static uint32_t utility_copy_callback(uintptr_t callback, int32_t stage,
				      uint32_t total, uint32_t done,
				      int32_t handle);

/* A broker transfer uses about 9 KiB of wire storage. Keep it off the 64 KiB
 * guest stack. No guest callback is entered while these records are in use.
 */
static struct mre_vfs_request utility_vfs_request;
static struct mre_vfs_response utility_vfs_response;
static int utility_copy_live;
static uint32_t utility_copy_sequence;

static void utility_vfs_begin(uint32_t operation, int32_t handle)
{
	memset(&utility_vfs_request, 0, sizeof(utility_vfs_request));
	utility_vfs_request.operation = operation;
	utility_vfs_request.handle = handle;
}

static int utility_vfs_run(void)
{
	memset(&utility_vfs_response, 0, sizeof(utility_vfs_response));
	if (utility_vfs_exchange(&utility_vfs_request, &utility_vfs_response)) {
		utility_vfs_response.error = EIO;
		utility_vfs_response.result = -1;
	}
	return !utility_vfs_response.error && utility_vfs_response.result >= 0;
}

/* Every filename scalar, including the
 * terminator, is checked before reading; a long/unsupported name is an SDK
 * error, while a pointer outside guest memory terminates the unsafe guest.
 */
static int utility_vfs_path(const uint16_t *path, char out[MRE_VFS_PATH_BYTES])
{
	if (!path || ((uintptr_t)path & 1))
		return 0;
	size_t units;
	for (units = 0; units < MRE_VFS_PATH_BYTES; ++units) {
		if (!guest_data(path + units, sizeof(*path), PF_R))
			fail("SDK path is outside guest memory");
		if (!path[units])
			break;
	}
	if (units == MRE_VFS_PATH_BYTES ||
	    !mre_utf8_encode(path, units, (unsigned char *)out,
			     MRE_VFS_PATH_BYTES, NULL))
		return 0;
	for (char *c = out; *c; ++c)
		if (*c == '/')
			*c = '\\';
	if (strlen(out) >= 2 && out[1] == ':')
		return 1;
	const char *last = strrchr(app.exec_path, '\\');
	size_t directory = last ? (size_t)(last - app.exec_path) + 1 : 3;
	size_t bytes = strlen(out);
	if (out[0] == '\\')
		directory = 2;
	if (directory + bytes >= MRE_VFS_PATH_BYTES)
		return 0;
	memmove(out + directory, out, bytes + 1);
	memcpy(out, app.exec_path, directory);
	return 1;
}

static void utility_vfs_count(uint32_t *count)
{
	if (((uintptr_t)count & 3) || !guest_data(count, sizeof(*count), PF_W))
		fail("utility byte-count output is outside aligned guest memory");
	*count = 0;
}

int32_t MRE_ABI utility_file_open(const uint16_t *path, uint32_t mode,
				  uint32_t binary)
{
	check_guest_api_stack();
	utility_vfs_begin(MRE_VFS_OPEN, 0);
	if (binary > 1 || !utility_vfs_path(path, utility_vfs_request.path))
		return -2; /* SDK VM_FILE_OPEN_ERROR. */
	utility_vfs_request.mode = mode;
	return utility_vfs_run() ? utility_vfs_response.result : -2;
}

void MRE_ABI utility_file_close(int32_t handle)
{
	check_guest_api_stack();
	if (handle < 0)
		return;
	utility_vfs_begin(MRE_VFS_CLOSE, handle);
	if (!utility_vfs_run())
		fprintf(stderr, "VFS close handle=%d failed errno=%u\n", handle,
			utility_vfs_response.error);
}

static int32_t utility_file_transfer(int32_t handle, void *data,
				     uint32_t length, uint32_t *count,
				     int writing)
{
	utility_vfs_count(count);
	if (length > MRE_FILE_BUDGET ||
	    (length && !guest_data(data, length, writing ? PF_R : PF_W)))
		fail("utility file transfer exceeds checked guest memory/budget");
	uint32_t done = 0;
	while (done < length) {
		uint32_t bytes = length - done;
		if (bytes > MRE_VFS_TRANSFER_BYTES)
			bytes = MRE_VFS_TRANSFER_BYTES;
		utility_vfs_begin(writing ? MRE_VFS_WRITE : MRE_VFS_READ,
				  handle);
		utility_vfs_request.bytes = bytes;
		if (writing)
			memcpy(utility_vfs_request.data, (uint8_t *)data + done,
			       bytes);
		int success = utility_vfs_run();
		uint32_t actual = utility_vfs_response.count;
		if (actual > bytes ||
		    (success && utility_vfs_response.result != (int32_t)actual))
			fail("utility VFS returned an invalid transfer count");
		if (!writing && actual)
			memcpy((uint8_t *)data + done,
			       utility_vfs_response.data, actual);
		done += actual;
		if (!success || actual < bytes)
			break;
	}
	*count = done;
	/* SDK read/write return actual bytes; zero is EOF or an error. */
	return (int32_t)done;
}

int32_t MRE_ABI utility_file_read(int32_t handle, void *data, uint32_t length,
				  uint32_t *nread)
{
	check_guest_api_stack();
	return utility_file_transfer(handle, data, length, nread, 0);
}

int32_t MRE_ABI utility_file_write(int32_t handle, void *data, uint32_t length,
				   uint32_t *written)
{
	check_guest_api_stack();
	return utility_file_transfer(handle, data, length, written, 1);
}

int32_t MRE_ABI utility_file_seek(int32_t handle, int32_t offset, int32_t base)
{
	check_guest_api_stack();
	utility_vfs_begin(MRE_VFS_SEEK, handle);
	utility_vfs_request.offset = offset;
	utility_vfs_request.base = base;
	return utility_vfs_run() ? 0 : -1;
}

int32_t MRE_ABI utility_file_tell(int32_t handle)
{
	check_guest_api_stack();
	utility_vfs_begin(MRE_VFS_TELL, handle);
	return utility_vfs_run() ? utility_vfs_response.position : -1;
}

int32_t MRE_ABI utility_file_eof(int32_t handle)
{
	check_guest_api_stack();
	utility_vfs_begin(MRE_VFS_TELL, handle);
	if (!utility_vfs_run())
		return -1;
	return (uint32_t)utility_vfs_response.position >=
	       utility_vfs_response.size;
}

int32_t MRE_ABI utility_file_getfilesize(int32_t handle, uint32_t *size)
{
	check_guest_api_stack();
	utility_vfs_count(size);
	utility_vfs_begin(MRE_VFS_GETSIZE, handle);
	if (!utility_vfs_run())
		return -1;
	*size = utility_vfs_response.size;
	return 0;
}

int32_t MRE_ABI utility_file_commit(int32_t handle)
{
	check_guest_api_stack();
	utility_vfs_begin(MRE_VFS_COMMIT, handle);
	return utility_vfs_run() ? 0 : -1;
}

static int32_t utility_vfs_path_operation(const uint16_t *path,
					  uint32_t operation,
					  uint32_t attributes)
{
	utility_vfs_begin(operation, 0);
	if (!utility_vfs_path(path, utility_vfs_request.path))
		return -1;
	utility_vfs_request.attributes = attributes;
	if (!utility_vfs_run())
		return -1;
	return operation == MRE_VFS_ATTR ? utility_vfs_response.result : 0;
}

int32_t MRE_ABI utility_file_get_attributes(const uint16_t *path)
{
	check_guest_api_stack();
	return utility_vfs_path_operation(path, MRE_VFS_ATTR, 0);
}

int32_t MRE_ABI utility_file_set_attributes(const uint16_t *path,
					    uint8_t attributes)
{
	check_guest_api_stack();
	return utility_vfs_path_operation(path, MRE_VFS_SETATTR, attributes);
}

int32_t MRE_ABI utility_file_mkdir(const uint16_t *path)
{
	check_guest_api_stack();
	return utility_vfs_path_operation(path, MRE_VFS_MKDIR, 0);
}

int32_t MRE_ABI utility_file_rmdir(const uint16_t *path)
{
	check_guest_api_stack();
	return utility_vfs_path_operation(path, MRE_VFS_RMDIR, 0);
}

int32_t MRE_ABI utility_file_delete(const uint16_t *path)
{
	check_guest_api_stack();
	return utility_vfs_path_operation(path, MRE_VFS_DELETE, 0);
}

int32_t MRE_ABI utility_file_rename(const uint16_t *oldpath,
				    const uint16_t *newpath)
{
	check_guest_api_stack();
	utility_vfs_begin(MRE_VFS_RENAME, 0);
	if (!utility_vfs_path(oldpath, utility_vfs_request.path) ||
	    !utility_vfs_path(newpath, utility_vfs_request.path2))
		return -1;
	return utility_vfs_run() ? 0 : -1;
}

static size_t utility_vfs_entry_name(void)
{
	const char *name = utility_vfs_response.entry.name;
	size_t length = strnlen(name, MRE_VFS_PATH_BYTES);
	if (!length || length == MRE_VFS_PATH_BYTES || strchr(name, '\\') ||
	    strchr(name, '/') ||
	    !mre_utf8_decode((const unsigned char *)name, length, NULL, 0,
			     NULL))
		fail("SDK VFS directory name is not a bounded UTF8 basename");
	return length;
}

static void utility_vfs_pack_entry(void *output, int extended)
{
	const struct mre_vfs_entry *entry = &utility_vfs_response.entry;
	size_t length = utility_vfs_entry_name();
	if (extended) {
		struct sdk_utility_fileinfo_ext *out = output;
		if (!mre_utf8_decode((const unsigned char *)entry->name, length,
				     out->filefullname, MRE_VFS_PATH_BYTES,
				     NULL))
			fail("SDK extended directory name exceeds UCS2 capacity");
		const char *dot = strrchr(entry->name, '.');
		size_t stem = dot ? (size_t)(dot - entry->name) : length;
		memcpy(out->filename, entry->name, stem < 8 ? stem : 8);
		if (dot) {
			size_t extension = length - stem - 1;
			memcpy(out->extension, dot + 1,
			       extension < 3 ? extension : 3);
		}
		out->attributes = (uint8_t)entry->attributes;
		memcpy(out->create_datetime, entry->create_time,
		       sizeof(entry->create_time));
		memcpy(out->modify_datetime, entry->modify_time,
		       sizeof(entry->modify_time));
		out->filesize = entry->size;
	} else {
		struct sdk_utility_fileinfo *out = output;
		if (entry->size > INT32_MAX)
			fail("utility old directory entry size exceeds SDK signed range");
		if (!mre_utf8_decode((const unsigned char *)entry->name, length,
				     out->filename, MRE_VFS_PATH_BYTES, NULL))
			fail("SDK directory name exceeds UCS2 capacity");
		out->size = (int32_t)entry->size;
	}
}

static int32_t utility_vfs_find(const uint16_t *pattern, int32_t handle,
				void *output, int first, int extended)
{
	size_t bytes = extended ? sizeof(struct sdk_utility_fileinfo_ext) :
				  sizeof(struct sdk_utility_fileinfo);
	if (((uintptr_t)output & 3) || !guest_data(output, bytes, PF_W))
		fail("utility directory output is outside aligned writable guest memory");
	memset(output, 0, bytes);
	utility_vfs_begin(first ? MRE_VFS_FIND_OPEN : MRE_VFS_FIND_NEXT,
			  handle);
	if (first && !utility_vfs_path(pattern, utility_vfs_request.path))
		return -1;
	if (!utility_vfs_run())
		return -1;
	utility_vfs_pack_entry(output, extended);
	return first ? utility_vfs_response.result : 0;
}

int32_t MRE_ABI utility_find_first_ext(uint16_t *pattern,
				       struct sdk_utility_fileinfo_ext *output)
{
	check_guest_api_stack();
	return utility_vfs_find(pattern, 0, output, 1, 1);
}

int32_t MRE_ABI utility_find_next_ext(int32_t handle,
				      struct sdk_utility_fileinfo_ext *output)
{
	check_guest_api_stack();
	return utility_vfs_find(NULL, handle, output, 0, 1);
}

static void utility_vfs_find_close(int32_t handle)
{
	if (handle < 0)
		return;
	utility_vfs_begin(MRE_VFS_FIND_CLOSE, handle);
	(void)utility_vfs_run();
}

void MRE_ABI utility_find_close_ext(int32_t handle)
{
	check_guest_api_stack();
	utility_vfs_find_close(handle);
}

int32_t MRE_ABI utility_find_first(uint16_t *pattern,
				   struct sdk_utility_fileinfo *output)
{
	check_guest_api_stack();
	return utility_vfs_find(pattern, 0, output, 1, 0);
}

int32_t MRE_ABI utility_find_next(int32_t handle,
				  struct sdk_utility_fileinfo *output)
{
	check_guest_api_stack();
	return utility_vfs_find(NULL, handle, output, 0, 0);
}

void MRE_ABI utility_find_close(int32_t handle)
{
	check_guest_api_stack();
	utility_vfs_find_close(handle);
}

int32_t MRE_ABI utility_get_path(uint16_t *fullpath, uint16_t *output)
{
	check_guest_api_stack();
	char path[MRE_VFS_PATH_BYTES];
	if (!utility_vfs_path(fullpath, path))
		return -1;
	const char *last = strrchr(path, '\\');
	if (!last)
		return -1;
	size_t length = (size_t)(last - path) + 1;
	if (((uintptr_t)output & 1) ||
	    !guest_data(output, (length + 1) * sizeof(*output), PF_W))
		fail("utility directory path output is outside guest memory");
	if (!mre_utf8_decode((const unsigned char *)path, length, output,
			     length + 1, NULL))
		return -1;
	return 0;
}

static int32_t utility_vfs_copy_error(uint32_t error)
{
	switch (error) {
	case EINVAL:
	case EISDIR:
	case ENOTDIR:
	case ENAMETOOLONG:
		return -3;
	case ENOENT:
		return -19;
	case EEXIST:
		return -36;
	case ENOMEM:
		return -44;
	case EBUSY:
	case EMFILE:
	case ENFILE:
		return -46;
	default:
		return -1;
	}
}

static void utility_vfs_copy_notify(utility_copy_fn callback, int32_t stage,
				    int32_t handle)
{
	if (!callback)
		return;
	uint32_t result =
		utility_copy_callback((uintptr_t)callback, stage, 0, 0, handle);
	/* The SDK header gives no abort rule for this return value. Preserve
	 * and record the actual guest result without inventing cancellation.
	 */
	fprintf(stderr,
		"VFS copy callback stage=%d handle=%d guest-return=%" PRId32
		"\n",
		stage, handle, (int32_t)result);
}

int32_t MRE_ABI utility_file_copy(const uint16_t *destination,
				  const uint16_t *source,
				  utility_copy_fn callback)
{
	check_guest_api_stack();
	if (utility_copy_live)
		return -46;
	if (callback && !guest_code((uintptr_t)callback))
		fail("utility copy callback is outside guest code");
	char source_path[MRE_VFS_PATH_BYTES],
		destination_path[MRE_VFS_PATH_BYTES] = { 0 };
	if (!utility_vfs_path(source, source_path))
		return -3;
	/* The preflight reports a valid source with an invalid destination through
	 * the same broker error path before the SDK START callback. */
	int destination_valid = utility_vfs_path(destination, destination_path);
	if (!destination_valid)
		destination_path[0] = '\0';
	utility_copy_live = 1;
	if (++utility_copy_sequence > INT32_MAX)
		utility_copy_sequence = 1;
	int32_t handle = (int32_t)utility_copy_sequence;
	utility_vfs_copy_notify(callback, 0, handle); /* PREPARE. */
	utility_vfs_begin(MRE_VFS_COPY, handle);
	memcpy(utility_vfs_request.path, source_path, strlen(source_path) + 1);
	memcpy(utility_vfs_request.path2, destination_path,
	       strlen(destination_path) + 1);
	utility_vfs_request.mode = 1; /* Parent validation-only COPY phase. */
	int32_t result;
	if (!utility_vfs_run()) {
		result = destination_valid ?
				 utility_vfs_copy_error(
					 utility_vfs_response.error) :
				 -3;
	} else {
		utility_vfs_copy_notify(callback, 1,
					handle); /* START after validation. */
		/* The real callback may enter other APIs; rebuild the shared request. */
		utility_vfs_begin(MRE_VFS_COPY, handle);
		memcpy(utility_vfs_request.path, source_path,
		       strlen(source_path) + 1);
		memcpy(utility_vfs_request.path2, destination_path,
		       strlen(destination_path) + 1);
		result = utility_vfs_run() ?
				 0 :
				 utility_vfs_copy_error(
					 utility_vfs_response.error);
		if (!result &&
		    utility_vfs_response.count != utility_vfs_response.size)
			fail("utility VFS copy completion byte count does not match source size");
	}
	utility_vfs_copy_notify(callback, result ? -1 : 3,
				handle); /* FAIL / DONE. */
	utility_copy_live = 0;
	return result;
}
