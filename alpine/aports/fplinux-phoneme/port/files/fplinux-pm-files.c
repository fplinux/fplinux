// SPDX-License-Identifier: GPL-2.0-only
/* fplinux-check: package-embedded */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <kni.h>
#include <linux/fs.h>
#include <linux/openat2.h>
#include <pcsl_string.h>
#include <sni.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <fplinux/fplinux-files-browse.h>
#include <fplinux/fplinux-volumes.h>

#include "fplinux-pm-files.h"

enum file_root { MY_FILES, MEMORY_CARD };
enum file_info { MODE, SIZE, MODIFIED, TOTAL, AVAILABLE, USED };
enum file_mode { READABLE = 1, WRITABLE = 2, DIRECTORY = 4 };
enum file_operation {
	CREATE,
	MKDIR,
	DELETE,
	RENAME,
	TRUNCATE,
	SET_READABLE,
	SET_WRITABLE,
};

struct file_stream {
	struct file_stream *next;
	int fd;
	bool writable;
};

static struct file_stream *streams;
static struct fplinux_volume_snapshot volumes;

void fplinux_pm_files_volumes(const struct fplinux_volume_snapshot *snapshot)
{
	volumes = *snapshot;
}

void fplinux_pm_files_cleanup(void)
{
	while (streams) {
		struct file_stream *stream = streams;

		streams = stream->next;
		close(stream->fd);
		free(stream);
	}
}

static bool string_path(jstring string, char path[PATH_MAX])
{
	jchar chars[PATH_MAX];
	jsize code_units;
	jsize bytes;

	if (KNI_IsNullHandle(string)) {
		errno = EINVAL;
		return false;
	}
	code_units = KNI_GetStringLength(string);
	if (code_units < 0 || code_units >= PATH_MAX) {
		errno = ENAMETOOLONG;
		return false;
	}
	KNI_GetStringRegion(string, 0, code_units, chars);
	for (jsize i = 0; i < code_units; ++i) {
		if (!chars[i]) {
			errno = EINVAL;
			return false;
		}
	}
	if (pcsl_utf16_convert_to_utf8(chars, code_units, (jbyte *)path,
				       PATH_MAX - 1,
				       &bytes) != PCSL_STRING_OK ||
	    bytes < 0 || bytes >= PATH_MAX) {
		errno = ENAMETOOLONG;
		return false;
	}
	path[bytes] = '\0';
	return true;
}

static int scoped_open(int root, const char *path, int flags, mode_t mode)
{
	struct open_how how = {
		.flags = (unsigned int)flags | O_CLOEXEC,
		.mode = mode,
		.resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS |
			   RESOLVE_NO_XDEV,
	};

	return (int)syscall(SYS_openat2, root, *path ? path : ".", &how,
			    sizeof(how));
}

static int root_open(int root)
{
	if (root == MY_FILES) {
		const char *home = getenv("HOME");
		int home_fd, files_fd, error;

		if (!home) {
			errno = ENOENT;
			return -1;
		}
		home_fd = fplinux_files_directory_open(home, 0);
		if (home_fd < 0)
			return -1;
		if (mkdirat(home_fd, "Files", 0700) < 0 && errno != EEXIST) {
			error = errno;
			close(home_fd);
			errno = error;
			return -1;
		}
		files_fd = scoped_open(home_fd, "Files", O_RDONLY | O_DIRECTORY,
				       0);
		error = errno;
		close(home_fd);
		errno = error;
		return files_fd;
	}
	if (root == MEMORY_CARD) {
		for (size_t i = 0; i < volumes.count; ++i) {
			const struct fplinux_volume *volume =
				&volumes.volumes[i];

			if (volume->state == FPLINUX_VOLUME_MOUNTED &&
			    volume->protection == FPLINUX_VOLUME_UNPROTECTED &&
			    volume->mount_id && volume->path[0])
				return fplinux_files_directory_open(
					volume->path, volume->mount_id);
		}
	}
	errno = ENODEV;
	return -1;
}

static int open_entry(int root, const char *path, int flags, mode_t mode)
{
	int root_fd = root_open(root);
	int result, error;

	if (root_fd < 0)
		return -1;
	result = scoped_open(root_fd, path, flags, mode);
	error = errno;
	close(root_fd);
	errno = error;
	return result;
}

static int open_parent(int root, char *path, const char **name)
{
	char *separator = strrchr(path, '/');

	*name = separator ? separator + 1 : path;
	if (fplinux_files_valid_name(*name) < 0)
		return -1;
	if (!separator)
		return root_open(root);
	*separator = '\0';
	return open_entry(root, path, O_RDONLY | O_DIRECTORY, 0);
}

static void throw_io(int error)
{
	KNI_ThrowNew("java/io/IOException", strerror(error));
}

static jlong information(int fd, int field)
{
	struct stat info;
	struct statvfs space;
	jlong block_size;

	if (field >= TOTAL && field <= USED) {
		if (fstatvfs(fd, &space) < 0)
			return -1;
		block_size = space.f_frsize;
		if (field == TOTAL)
			return block_size * space.f_blocks;
		if (field == AVAILABLE)
			return block_size * space.f_bavail;
		return block_size * (space.f_blocks - space.f_bfree);
	}
	if (fstat(fd, &info) < 0 ||
	    (!S_ISREG(info.st_mode) && !S_ISDIR(info.st_mode)))
		return -1;
	if (field == MODE) {
		return (S_ISDIR(info.st_mode) ? DIRECTORY : 0) |
		       (syscall(SYS_faccessat2, fd, "", R_OK,
				AT_EMPTY_PATH | AT_EACCESS) == 0 ?
				READABLE :
				0) |
		       (syscall(SYS_faccessat2, fd, "", W_OK,
				AT_EMPTY_PATH | AT_EACCESS) == 0 ?
				WRITABLE :
				0);
	}
	if (field == SIZE)
		return S_ISREG(info.st_mode) ? info.st_size : -1;
	if (field == MODIFIED)
		return (jlong)info.st_mtime * 1000 +
		       info.st_mtim.tv_nsec / 1000000;
	return -1;
}

KNIEXPORT KNI_RETURNTYPE_BOOLEAN
KNIDECL(com_sun_midp_io_j2me_file_Protocol_rootAvailable)
{
	int root = KNI_GetParameterAsInt(1);
	int fd = root_open(root);

	if (fd >= 0)
		close(fd);
	else if (root == MY_FILES)
		fprintf(stderr, "fplinux-phoneme: My files: %s\n",
			strerror(errno));
	KNI_ReturnBoolean(fd >= 0 ? KNI_TRUE : KNI_FALSE);
}

KNIEXPORT KNI_RETURNTYPE_LONG KNIDECL(com_sun_midp_io_j2me_file_Protocol_query0)
{
	char path[PATH_MAX];
	int root = KNI_GetParameterAsInt(1);
	int field = KNI_GetParameterAsInt(3);
	jlong result = -1;

	KNI_StartHandles(1);
	KNI_DeclareHandle(string);
	KNI_GetParameterAsObject(2, string);
	if (string_path(string, path)) {
		int fd = field >= TOTAL && field <= USED ?
				 root_open(root) :
				 open_entry(root, path, O_PATH | O_NOFOLLOW, 0);

		if (fd >= 0) {
			result = information(fd, field);
			close(fd);
		}
	}
	KNI_EndHandles();
	KNI_ReturnLong(result);
}

static bool own_entry(const char *name)
{
	return strcmp(name, ".") && strcmp(name, "..");
}

static jlong directory_size(int fd, bool recursive)
{
	DIR *directory = fdopendir(fd);
	jlong size = 0;

	if (!directory) {
		int error = errno;

		close(fd);
		errno = error;
		return -1;
	}
	for (;;) {
		struct dirent *entry;
		struct stat info;

		errno = 0;
		entry = readdir(directory);
		if (!entry) {
			if (errno)
				size = -1;
			break;
		}
		if (!own_entry(entry->d_name) ||
		    fstatat(fd, entry->d_name, &info, AT_SYMLINK_NOFOLLOW) < 0)
			continue;
		if (S_ISREG(info.st_mode)) {
			size += info.st_size;
		} else if (recursive && S_ISDIR(info.st_mode)) {
			int child = scoped_open(fd, entry->d_name,
						O_RDONLY | O_DIRECTORY, 0);
			jlong bytes = child < 0 ? -1 :
						  directory_size(child, true);

			if (bytes < 0) {
				size = -1;
				break;
			}
			size += bytes;
		}
	}
	closedir(directory);
	return size;
}

KNIEXPORT KNI_RETURNTYPE_LONG
KNIDECL(com_sun_midp_io_j2me_file_Protocol_directorySize0)
{
	char path[PATH_MAX];
	int root = KNI_GetParameterAsInt(1);
	bool recursive = KNI_GetParameterAsBoolean(3);
	jlong bytes = -1;

	KNI_StartHandles(1);
	KNI_DeclareHandle(string);
	KNI_GetParameterAsObject(2, string);
	if (string_path(string, path)) {
		int fd = open_entry(root, path, O_RDONLY | O_DIRECTORY, 0);

		if (fd >= 0)
			bytes = directory_size(fd, recursive);
		else if (errno == ENOTDIR)
			throw_io(errno);
	}
	KNI_EndHandles();
	KNI_ReturnLong(bytes);
}

static int collect_names(DIR *directory, bool hidden, char **names,
			 size_t *count)
{
	for (;;) {
		struct dirent *entry;
		struct stat info;
		size_t length;

		errno = 0;
		entry = readdir(directory);
		if (!entry)
			return errno ? -1 : 0;
		if (!own_entry(entry->d_name) ||
		    (!hidden && entry->d_name[0] == '.') ||
		    fstatat(dirfd(directory), entry->d_name, &info,
			    AT_SYMLINK_NOFOLLOW) < 0 ||
		    (!S_ISREG(info.st_mode) && !S_ISDIR(info.st_mode)))
			continue;
		if (*count == FPLINUX_FILES_DIRECTORY_LIMIT) {
			errno = EFBIG;
			return -1;
		}
		length = strlen(entry->d_name);
		names[*count] = malloc(length + 2);
		if (!names[*count])
			return -1;
		memcpy(names[*count], entry->d_name, length);
		if (S_ISDIR(info.st_mode))
			names[*count][length++] = '/';
		names[*count][length] = '\0';
		(*count)++;
	}
}

static int read_names(int root, const char *path, bool hidden, char **names,
		      size_t *count)
{
	int fd = open_entry(root, path, O_RDONLY | O_DIRECTORY, 0);
	DIR *directory;
	int result, error;

	if (fd < 0)
		return -1;
	directory = fdopendir(fd);
	if (!directory) {
		error = errno;
		close(fd);
		errno = error;
		return -1;
	}
	result = collect_names(directory, hidden, names, count);
	error = errno;
	closedir(directory);
	errno = error;
	return result;
}

KNIEXPORT KNI_RETURNTYPE_OBJECT KNIDECL(com_sun_midp_io_j2me_file_Protocol_list0)
{
	char path[PATH_MAX];
	int root = KNI_GetParameterAsInt(1);
	bool hidden = KNI_GetParameterAsBoolean(3);
	char **names = NULL;
	size_t count = 0;

	KNI_StartHandles(3);
	KNI_DeclareHandle(string);
	KNI_DeclareHandle(array);
	KNI_DeclareHandle(entry);
	KNI_GetParameterAsObject(2, string);
	if (!string_path(string, path)) {
		throw_io(errno);
	} else {
		names = calloc(FPLINUX_FILES_DIRECTORY_LIMIT, sizeof(*names));
		if (!names ||
		    read_names(root, path, hidden, names, &count) < 0) {
			throw_io(errno);
		} else {
			SNI_NewArray(SNI_STRING_ARRAY, count, array);
			if (KNI_IsNullHandle(array))
				KNI_ThrowNew("java/lang/OutOfMemoryError",
					     NULL);
			for (size_t i = 0;
			     i < count && !KNI_IsNullHandle(array); ++i) {
				jchar utf16[NAME_MAX + 2];
				jsize units;

				if (pcsl_utf8_convert_to_utf16(
					    (const jbyte *)names[i],
					    strlen(names[i]), utf16,
					    NAME_MAX + 2,
					    &units) != PCSL_STRING_OK) {
					throw_io(EILSEQ);
					break;
				}
				KNI_NewString(utf16, units, entry);
				if (KNI_IsNullHandle(entry))
					break;
				KNI_SetObjectArrayElement(array, i, entry);
			}
		}
	}
	for (size_t i = 0; i < count; ++i)
		free(names[i]);
	free(names);
	KNI_EndHandlesAndReturnObject(array);
}

static int change_entry(int parent, const char *name, int operation,
			const char *argument, jlong value)
{
	struct stat info;
	int fd, result, error;

	if (operation == CREATE) {
		fd = scoped_open(parent, name, O_WRONLY | O_CREAT | O_EXCL,
				 0600);
		return fd >= 0 ? close(fd) : -1;
	}
	if (operation == MKDIR)
		return mkdirat(parent, name, 0700);
	if (fstatat(parent, name, &info, AT_SYMLINK_NOFOLLOW) < 0)
		return -1;
	if (!S_ISREG(info.st_mode) && !S_ISDIR(info.st_mode)) {
		errno = EINVAL;
		return -1;
	}
	if (operation == DELETE)
		return unlinkat(parent, name,
				S_ISDIR(info.st_mode) ? AT_REMOVEDIR : 0);
	if (operation == RENAME) {
		if (fplinux_files_valid_name(argument) < 0)
			return -1;
		return (int)syscall(SYS_renameat2, parent, name, parent,
				    argument, RENAME_NOREPLACE);
	}
	if (operation == SET_READABLE || operation == SET_WRITABLE) {
		mode_t bit = operation == SET_READABLE ? S_IRUSR : S_IWUSR;
		mode_t updated = value ? info.st_mode | bit :
					 info.st_mode & ~bit;

		return (int)syscall(SYS_fchmodat2, parent, name, updated & 0777,
				    AT_SYMLINK_NOFOLLOW);
	}
	if (operation != TRUNCATE || !S_ISREG(info.st_mode) || value < 0) {
		errno = EINVAL;
		return -1;
	}
	fd = scoped_open(parent, name, O_WRONLY | O_NONBLOCK, 0);
	if (fd < 0)
		return -1;
	result = fstat(fd, &info);
	if (!result && value < info.st_size)
		result = ftruncate(fd, value);
	error = errno;
	close(fd);
	errno = error;
	return result;
}

KNIEXPORT KNI_RETURNTYPE_VOID KNIDECL(com_sun_midp_io_j2me_file_Protocol_change0)
{
	char path[PATH_MAX], argument[PATH_MAX];
	const char *name;
	int root = KNI_GetParameterAsInt(1);
	int operation = KNI_GetParameterAsInt(3);
	jlong value = KNI_GetParameterAsLong(5);
	int result = -1;

	KNI_StartHandles(2);
	KNI_DeclareHandle(path_handle);
	KNI_DeclareHandle(argument_handle);
	KNI_GetParameterAsObject(2, path_handle);
	KNI_GetParameterAsObject(4, argument_handle);
	if (string_path(path_handle, path) &&
	    (operation != RENAME || string_path(argument_handle, argument))) {
		int parent = open_parent(root, path, &name);

		if (parent >= 0) {
			int error;

			result = change_entry(parent, name, operation, argument,
					      value);
			error = errno;
			close(parent);
			errno = error;
		}
	}
	if (result < 0)
		throw_io(errno);
	KNI_EndHandles();
	KNI_ReturnVoid();
}

static int open_stream(int root, const char *path, bool writable, jlong offset)
{
	struct stat info;
	struct file_stream *stream;
	int flags = writable ? O_WRONLY : O_RDONLY;
	int fd = open_entry(root, path, flags | O_NONBLOCK, 0);
	int error;

	if (fd < 0)
		return -1;
	if (fstat(fd, &info) < 0) {
		error = errno;
		goto failed;
	}
	if (!S_ISREG(info.st_mode) || offset < 0) {
		error = EINVAL;
		goto failed;
	}
	if (writable && lseek(fd, offset < info.st_size ? offset : info.st_size,
			      SEEK_SET) < 0) {
		error = errno;
		goto failed;
	}
	stream = malloc(sizeof(*stream));
	if (!stream) {
		error = ENOMEM;
		goto failed;
	}
	*stream = (struct file_stream){ .next = streams,
					.fd = fd,
					.writable = writable };
	streams = stream;
	return fd;

failed:
	close(fd);
	errno = error;
	return -1;
}

KNIEXPORT KNI_RETURNTYPE_INT KNIDECL(com_sun_midp_io_j2me_file_Protocol_open0)
{
	char path[PATH_MAX];
	int root = KNI_GetParameterAsInt(1);
	bool writable = KNI_GetParameterAsBoolean(3);
	jlong offset = KNI_GetParameterAsLong(4);
	int fd = -1;

	KNI_StartHandles(1);
	KNI_DeclareHandle(string);
	KNI_GetParameterAsObject(2, string);
	if (string_path(string, path))
		fd = open_stream(root, path, writable, offset);
	if (fd < 0)
		throw_io(errno);
	KNI_EndHandles();
	KNI_ReturnInt(fd);
}

static struct file_stream *find_stream(int fd)
{
	for (struct file_stream *stream = streams; stream;
	     stream = stream->next)
		if (stream->fd == fd)
			return stream;
	errno = EBADF;
	return NULL;
}

static unsigned char *buffer_region(jarray data, jint offset, jint length)
{
	jsize size = KNI_IsNullHandle(data) ? 0 : KNI_GetArrayLength(data);

	if (KNI_IsNullHandle(data) || offset < 0 || length < 0 ||
	    offset > size || length > size - offset) {
		errno = EINVAL;
		return NULL;
	}
	return (unsigned char *)SNI_GetRawArrayPointer(data) + offset;
}

KNIEXPORT KNI_RETURNTYPE_INT KNIDECL(com_sun_midp_io_j2me_file_Protocol_read0)
{
	int fd = KNI_GetParameterAsInt(1);
	jint offset = KNI_GetParameterAsInt(3);
	jint length = KNI_GetParameterAsInt(4);
	ssize_t bytes = -1;
	unsigned char *buffer;

	KNI_StartHandles(1);
	KNI_DeclareHandle(data);
	KNI_GetParameterAsObject(2, data);
	if (find_stream(fd) && (buffer = buffer_region(data, offset, length))) {
		do {
			bytes = read(fd, buffer, length);
		} while (bytes < 0 && errno == EINTR);
	}
	if (bytes < 0)
		throw_io(errno);
	KNI_EndHandles();
	KNI_ReturnInt(bytes > 0 ? bytes : -1);
}

KNIEXPORT KNI_RETURNTYPE_LONG
KNIDECL(com_sun_midp_io_j2me_file_Protocol_remaining0)
{
	int fd = KNI_GetParameterAsInt(1);
	jlong skip = KNI_GetParameterAsLong(2);
	struct stat info;
	off_t position;
	jlong result = 0;

	if (!find_stream(fd) || fstat(fd, &info) < 0 ||
	    (position = lseek(fd, 0, SEEK_CUR)) < 0) {
		throw_io(errno);
		KNI_ReturnLong(0);
	}
	if (position < info.st_size)
		result = info.st_size - position;
	if (skip > 0) {
		if (skip < result)
			result = skip;
		if (lseek(fd, result, SEEK_CUR) < 0)
			throw_io(errno);
	}
	KNI_ReturnLong(result);
}

KNIEXPORT KNI_RETURNTYPE_VOID KNIDECL(com_sun_midp_io_j2me_file_Protocol_write0)
{
	int fd = KNI_GetParameterAsInt(1);
	jint offset = KNI_GetParameterAsInt(3);
	jint length = KNI_GetParameterAsInt(4);
	unsigned char *buffer;

	KNI_StartHandles(1);
	KNI_DeclareHandle(data);
	KNI_GetParameterAsObject(2, data);
	buffer = find_stream(fd) ? buffer_region(data, offset, length) : NULL;
	if (!buffer) {
		throw_io(errno);
	} else {
		while (length > 0) {
			ssize_t bytes = write(fd, buffer, length);

			if (bytes < 0 && errno == EINTR)
				continue;
			if (bytes <= 0) {
				throw_io(bytes < 0 ? errno : EIO);
				break;
			}
			length -= bytes;
			buffer += bytes;
		}
	}
	KNI_EndHandles();
	KNI_ReturnVoid();
}

static int flush_stream(int fd)
{
	int result;

	do {
		result = fsync(fd);
	} while (result < 0 && errno == EINTR);
	return result;
}

KNIEXPORT KNI_RETURNTYPE_VOID KNIDECL(com_sun_midp_io_j2me_file_Protocol_flush0)
{
	int fd = KNI_GetParameterAsInt(1);

	if (!find_stream(fd) || flush_stream(fd) < 0)
		throw_io(errno);
	KNI_ReturnVoid();
}

KNIEXPORT KNI_RETURNTYPE_VOID KNIDECL(com_sun_midp_io_j2me_file_Protocol_close0)
{
	int fd = KNI_GetParameterAsInt(1);
	struct file_stream **position = &streams;

	while (*position && (*position)->fd != fd)
		position = &(*position)->next;
	if (*position) {
		struct file_stream *stream = *position;
		int error = stream->writable && flush_stream(fd) < 0 ? errno :
								       0;

		*position = stream->next;
		free(stream);
		if (close(fd) < 0 && !error)
			error = errno;
		if (error)
			throw_io(error);
	}
	KNI_ReturnVoid();
}
