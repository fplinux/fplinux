/* SPDX-License-Identifier: GPL-2.0-only */
#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fplinux-ui-prefs.h"

#define APP "org.example.prefs"

static const char *root;

enum sync_failure {
	SYNC_SUCCEEDS,
	SYNC_FILE_FAILS,
	SYNC_DIRECTORY_FAILS,
	SYNC_SELECTED_DIRECTORY_FAILS,
};

static enum sync_failure sync_failure;
static ino_t sync_directory_inode;

enum io_failure {
	IO_SUCCEEDS,
	IO_MKDIR_FAILS,
	IO_CHMOD_FAILS,
	IO_TEMPORARY_FAILS,
	IO_FLUSH_FAILS,
	IO_CLOSE_FAILS,
	IO_RENAME_FAILS,
	IO_READ_FAILS,
};

static enum io_failure io_failure;
static int failure_error;

int __real_fsync(int fd);
int __real_mkdir(const char *path, mode_t mode);
int __real_fchmod(int fd, mode_t mode);
int __real_mkostemp(char *path, int flags);
int __real_fflush(FILE *file);
int __real_fclose(FILE *file);
int __real_rename(const char *source, const char *destination);
FILE *__real_fopen(const char *path, const char *mode);

int __wrap_mkdir(const char *path, mode_t mode)
{
	if (io_failure == IO_MKDIR_FAILS) {
		errno = failure_error;
		return -1;
	}
	return __real_mkdir(path, mode);
}

int __wrap_fchmod(int fd, mode_t mode)
{
	if (io_failure == IO_CHMOD_FAILS) {
		errno = failure_error;
		return -1;
	}
	return __real_fchmod(fd, mode);
}

int __wrap_mkostemp(char *path, int flags)
{
	if (io_failure == IO_TEMPORARY_FAILS) {
		errno = failure_error;
		return -1;
	}
	return __real_mkostemp(path, flags);
}

int __wrap_fflush(FILE *file)
{
	if (io_failure == IO_FLUSH_FAILS) {
		errno = failure_error;
		return EOF;
	}
	return __real_fflush(file);
}

int __wrap_fclose(FILE *file)
{
	int result = __real_fclose(file);

	if (io_failure == IO_CLOSE_FAILS) {
		errno = failure_error;
		return EOF;
	}
	return result;
}

int __wrap_rename(const char *source, const char *destination)
{
	if (io_failure == IO_RENAME_FAILS) {
		errno = failure_error;
		return -1;
	}
	return __real_rename(source, destination);
}

FILE *__wrap_fopen(const char *path, const char *mode)
{
	if (io_failure == IO_READ_FAILS && mode[0] == 'r') {
		errno = failure_error;
		return NULL;
	}
	return __real_fopen(path, mode);
}

/* The filesystem is real; only a selected synchronization failure is injected. */
int __wrap_fsync(int fd)
{
	struct stat status;

	assert(!fstat(fd, &status));
	if ((sync_failure == SYNC_FILE_FAILS && S_ISREG(status.st_mode)) ||
	    (sync_failure == SYNC_DIRECTORY_FAILS && S_ISDIR(status.st_mode)) ||
	    (sync_failure == SYNC_SELECTED_DIRECTORY_FAILS &&
	     S_ISDIR(status.st_mode) &&
	     status.st_ino == sync_directory_inode)) {
		errno = EIO;
		return -1;
	}
	return __real_fsync(fd);
}

static struct fplinux_ui_prefs *open_preferences(void)
{
	char path[PATH_MAX];
	int written = snprintf(path, sizeof(path), "%s/" APP, root);

	assert(written > 0 && (size_t)written < sizeof(path));
	return fplinux_ui_prefs_open_directory(path);
}

static void assert_durable(struct fplinux_ui_prefs *prefs)
{
	struct fplinux_storage_result result = fplinux_ui_prefs_save(prefs);

	assert(result.state == FPLINUX_STORAGE_DURABLE);
	assert(!result.error);
}

static void assert_save_error(struct fplinux_ui_prefs *prefs,
			      enum fplinux_storage_state state, int error)
{
	struct fplinux_storage_result result;

	errno = 0;
	result = fplinux_ui_prefs_save(prefs);
	assert(result.state == state);
	assert(result.error == error);
	assert(errno == error);
}

static void file_path(char *path, size_t size, const char *name)
{
	int written = snprintf(path, size, "%s/" APP "/%s", root, name);

	assert(written > 0 && (size_t)written < size);
}

/* Reads the whole preferences file; returns its length or -1 if missing. */
static long read_file(char *buffer, size_t size)
{
	char path[PATH_MAX];
	FILE *file;
	size_t length;

	file_path(path, sizeof(path), "preferences");
	file = fopen(path, "rb");
	if (!file)
		return -1;
	length = fread(buffer, 1, size - 1, file);
	assert(!ferror(file));
	fclose(file);
	buffer[length] = '\0';
	return (long)length;
}

static void write_file(const char *contents, size_t length)
{
	char path[PATH_MAX];
	FILE *file;

	file_path(path, sizeof(path), "preferences");
	file = fopen(path, "wb");
	assert(file);
	assert(fwrite(contents, 1, length, file) == length);
	assert(!fclose(file));
}

static ino_t file_inode(void)
{
	char path[PATH_MAX];
	struct stat status;

	file_path(path, sizeof(path), "preferences");
	assert(!stat(path, &status));
	return status.st_ino;
}

/* Only "preferences" may remain: a temporary file means a failed cleanup. */
static void assert_no_temporary(void)
{
	char path[PATH_MAX];
	struct dirent *entry;
	DIR *directory;

	file_path(path, sizeof(path), "");
	directory = opendir(path);
	assert(directory);
	while ((entry = readdir(directory)))
		assert(!strcmp(entry->d_name, ".") ||
		       !strcmp(entry->d_name, "..") ||
		       !strcmp(entry->d_name, "preferences"));
	closedir(directory);
}

static void remove_file(void)
{
	char path[PATH_MAX];

	file_path(path, sizeof(path), "preferences");
	assert(!unlink(path) || errno == ENOENT);
}

static void test_missing_file(void)
{
	struct fplinux_ui_prefs *prefs;
	char buffer[16];
	char path[PATH_MAX];
	struct stat status;

	remove_file();
	file_path(path, sizeof(path), "");
	assert(!mkdir(path, 0700));
	prefs = open_preferences();
	assert(prefs);
	file_path(path, sizeof(path), "");
	assert(!stat(path, &status));
	assert((status.st_mode & 0777) == 0700);
	assert(!strcmp(fplinux_ui_prefs_get(prefs, "absent", "x"), "x"));
	assert(!fplinux_ui_prefs_get(prefs, "absent", NULL));
	assert(fplinux_ui_prefs_get_int(prefs, "absent", 42) == 42);
	/* Nothing changed: no file is written. */
	assert_durable(prefs);
	assert(read_file(buffer, sizeof(buffer)) == -1);
	fplinux_ui_prefs_close(prefs);
	fplinux_ui_prefs_close(NULL);
	assert(!strcmp(fplinux_ui_prefs_get(NULL, "absent", "y"), "y"));
	assert(!fplinux_ui_prefs_open_directory(""));
	assert(errno == EINVAL);
}

static void test_existing_directory_is_private(void)
{
	char path[PATH_MAX];
	struct stat status;
	struct fplinux_ui_prefs *prefs;

	file_path(path, sizeof(path), "");
	assert(!chmod(path, 0755));
	errno = 0;
	prefs = open_preferences();
	assert(!prefs);
	assert(errno == EACCES);
	assert(!stat(path, &status));
	assert((status.st_mode & 0777) == 0755);
	assert(!chmod(path, 0500));
	prefs = open_preferences();
	assert(prefs);
	assert(!stat(path, &status));
	assert((status.st_mode & 0777) == 0500);
	assert(fplinux_ui_prefs_set(prefs, "private", "yes"));
	assert_durable(prefs);
	assert(!stat(path, &status));
	assert((status.st_mode & 0777) == 0700);
	fplinux_ui_prefs_close(prefs);
}

static void test_round_trip(void)
{
	static const char expected[] = "text=line one\\nline two\\r\\nend\n"
				       "path=C:\\\\dir\\\\\n"
				       "name=Привет, мир\n"
				       "equation=a=b\n"
				       "empty=\n"
				       "count=-17\n";
	char path[PATH_MAX];
	char buffer[512];
	struct fplinux_ui_prefs *prefs;
	struct stat status;
	const char *before;

	remove_file();
	prefs = open_preferences();
	assert(prefs);
	assert(fplinux_ui_prefs_set(prefs, "text",
				    "line one\nline two\r\nend"));
	assert(fplinux_ui_prefs_set(prefs, "path", "C:\\dir\\"));
	assert(fplinux_ui_prefs_set(prefs, "name", "Привет, мир"));
	assert(fplinux_ui_prefs_set(prefs, "equation", "a=b"));
	assert(fplinux_ui_prefs_set(prefs, "empty", ""));
	assert(fplinux_ui_prefs_set_int(prefs, "count", -17));
	/* Setting another key keeps an earlier returned value valid. */
	before = fplinux_ui_prefs_get(prefs, "name", NULL);
	assert(fplinux_ui_prefs_set(prefs, "later", "1"));
	assert(fplinux_ui_prefs_remove(prefs, "later"));
	assert(!strcmp(before, "Привет, мир"));
	assert_durable(prefs);
	fplinux_ui_prefs_close(prefs);

	assert(read_file(buffer, sizeof(buffer)) == (long)strlen(expected));
	assert(!strcmp(buffer, expected));
	file_path(path, sizeof(path), "preferences");
	assert(!stat(path, &status));
	assert((status.st_mode & 0777) == 0600);
	assert_no_temporary();

	prefs = open_preferences();
	assert(prefs);
	assert(!strcmp(fplinux_ui_prefs_get(prefs, "text", NULL),
		       "line one\nline two\r\nend"));
	assert(!strcmp(fplinux_ui_prefs_get(prefs, "path", NULL), "C:\\dir\\"));
	assert(!strcmp(fplinux_ui_prefs_get(prefs, "name", NULL),
		       "Привет, мир"));
	assert(!strcmp(fplinux_ui_prefs_get(prefs, "equation", NULL), "a=b"));
	assert(!strcmp(fplinux_ui_prefs_get(prefs, "empty", "x"), ""));
	assert(fplinux_ui_prefs_get_int(prefs, "count", 0) == -17);
	assert(!fplinux_ui_prefs_get(prefs, "later", NULL));
	fplinux_ui_prefs_close(prefs);
}

static void test_keys(void)
{
	struct fplinux_ui_prefs *prefs = open_preferences();
	static const char *const invalid[] = {
		"",
		"with space",
		"slash/key",
		"equals=key",
		"ключ",
		"new\nline",
		"0123456789012345678901234567890123456789012345678901234567890123",
	};
	const char *longest =
		"012345678901234567890123456789012345678901234567890123456789012";

	assert(prefs);
	for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
		errno = 0;
		assert(!fplinux_ui_prefs_set(prefs, invalid[i], "v"));
		assert(errno == EINVAL);
		assert(!fplinux_ui_prefs_remove(prefs, invalid[i]));
		assert(!strcmp(fplinux_ui_prefs_get(prefs, invalid[i], "f"),
			       "f"));
	}
	assert(!fplinux_ui_prefs_set(prefs, NULL, "v"));
	assert(!fplinux_ui_prefs_set(prefs, "key", NULL));
	assert(errno == EINVAL);
	assert(strlen(longest) == 63);
	assert(fplinux_ui_prefs_set(prefs, longest, "v"));
	assert(fplinux_ui_prefs_set(prefs, "Az09._-", "v"));
	/* A key that is not there is removed already. */
	assert(fplinux_ui_prefs_remove(prefs, "never-set"));
	fplinux_ui_prefs_close(prefs);
}

static void test_values(void)
{
	struct fplinux_ui_prefs *prefs = open_preferences();
	static const char *const invalid[] = {
		"\xff", /* not a lead byte */
		"\xc0\xaf", /* overlong '/' */
		"\xe0\x80\xaf", /* overlong '/' */
		"\xed\xa0\x80", /* surrogate */
		"\xf4\x90\x80\x80", /* past U+10FFFF */
		"abc\xe2\x82", /* truncated */
		"\x80", /* lone continuation */
	};
	char *large = malloc(65537);

	assert(prefs && large);
	for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
		errno = 0;
		assert(!fplinux_ui_prefs_set(prefs, "bad", invalid[i]));
		assert(errno == EILSEQ);
	}
	assert(fplinux_ui_prefs_set(prefs, "emoji", "\xf0\x9f\x98\x80"));
	memset(large, 'x', 65536);
	large[65536] = '\0';
	errno = 0;
	assert(!fplinux_ui_prefs_set(prefs, "large", large));
	assert(errno == E2BIG);
	/* The largest value, escaped to twice its size, survives a save. */
	memset(large, '\n', 65535);
	large[65535] = '\0';
	assert(fplinux_ui_prefs_set(prefs, "large", large));
	assert_durable(prefs);
	fplinux_ui_prefs_close(prefs);
	prefs = open_preferences();
	assert(prefs);
	assert(!strcmp(fplinux_ui_prefs_get(prefs, "large", ""), large));
	assert(!fplinux_ui_prefs_get(prefs, "bad", NULL));
	fplinux_ui_prefs_close(prefs);
	free(large);
}

static void test_integers(void)
{
	static const struct {
		const char *text;
		long long value;
	} cases[] = {
		{ "42", 42 },
		{ "-7", -7 },
		{ "+5", 5 },
		{ "0", 0 },
		{ "9223372036854775807", 9223372036854775807LL },
		{ "-9223372036854775808", -9223372036854775807LL - 1 },
		{ "9223372036854775808", 99 },
		{ "-9223372036854775809", 99 },
		{ "12abc", 99 },
		{ "", 99 },
		{ " 5", 99 },
		{ "5 ", 99 },
		{ "0x10", 99 },
		{ "-", 99 },
	};
	struct fplinux_ui_prefs *prefs = open_preferences();

	assert(prefs);
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
		assert(fplinux_ui_prefs_set(prefs, "number", cases[i].text));
		errno = 1234;
		assert(fplinux_ui_prefs_get_int(prefs, "number", 99) ==
		       cases[i].value);
		assert(errno == 1234);
	}
	assert(fplinux_ui_prefs_set_int(prefs, "number",
					-9223372036854775807LL - 1));
	assert(!strcmp(fplinux_ui_prefs_get(prefs, "number", NULL),
		       "-9223372036854775808"));
	fplinux_ui_prefs_close(prefs);
}

/* An unchanged save keeps the file; every real save replaces it. */
static void test_unchanged_and_atomic(void)
{
	char path[PATH_MAX];
	struct fplinux_ui_prefs *prefs;
	ino_t inode;

	remove_file();
	prefs = open_preferences();
	assert(prefs);
	assert(fplinux_ui_prefs_set(prefs, "a", "1"));
	assert_durable(prefs);
	inode = file_inode();
	assert_durable(prefs);
	assert(fplinux_ui_prefs_set(prefs, "a", "1"));
	assert(fplinux_ui_prefs_set_int(prefs, "a", 1));
	assert(fplinux_ui_prefs_remove(prefs, "absent"));
	assert_durable(prefs);
	assert(file_inode() == inode);
	assert(fplinux_ui_prefs_set(prefs, "a", "2"));
	assert_durable(prefs);
	assert(file_inode() != inode);
	assert_no_temporary();

	/*
	 * A failed rename leaves no temporary file and keeps the change for a
	 * later save. A directory in the file's place makes rename fail even
	 * for root.
	 */
	assert(fplinux_ui_prefs_set(prefs, "a", "3"));
	remove_file();
	file_path(path, sizeof(path), "preferences");
	assert(!mkdir(path, 0700));
	errno = 0;
	assert_save_error(prefs, FPLINUX_STORAGE_UNPUBLISHED, EISDIR);
	assert(errno == EISDIR);
	assert_no_temporary();
	assert(!rmdir(path));
	assert_durable(prefs);
	fplinux_ui_prefs_close(prefs);
	prefs = open_preferences();
	assert(prefs);
	assert(fplinux_ui_prefs_get_int(prefs, "a", 0) == 3);
	fplinux_ui_prefs_close(prefs);
}

static void test_malformed_lines(void)
{
	static const char contents[] = "good=1\n"
				       "no equals sign\n"
				       "bad key=2\n"
				       "=empty key\n"
				       "escape=\\q\n"
				       "trailing=abc\\\n"
				       "utf=\xff\n"
				       "\n"
				       "dup=1\n"
				       "dup=2\n"
				       "nul=a\0b\n"
				       "last=x";
	struct fplinux_ui_prefs *prefs;
	char buffer[256];

	write_file(contents, sizeof(contents) - 1);
	prefs = open_preferences();
	assert(prefs);
	assert(!strcmp(fplinux_ui_prefs_get(prefs, "good", ""), "1"));
	assert(!strcmp(fplinux_ui_prefs_get(prefs, "dup", ""), "2"));
	assert(!strcmp(fplinux_ui_prefs_get(prefs, "last", ""), "x"));
	assert(!fplinux_ui_prefs_get(prefs, "escape", NULL));
	assert(!fplinux_ui_prefs_get(prefs, "trailing", NULL));
	assert(!fplinux_ui_prefs_get(prefs, "utf", NULL));
	assert(!fplinux_ui_prefs_get(prefs, "nul", NULL));
	/* Loading is not a change; the next real save drops the bad lines. */
	assert_durable(prefs);
	assert(read_file(buffer, sizeof(buffer)) ==
	       (long)(sizeof(contents) - 1));
	assert(fplinux_ui_prefs_set(prefs, "good", "3"));
	assert_durable(prefs);
	fplinux_ui_prefs_close(prefs);
	assert(read_file(buffer, sizeof(buffer)) > 0);
	assert(!strcmp(buffer, "good=3\ndup=2\nlast=x\n"));
}

/* Failed synchronization is reported and retains the change for a retry. */
static void test_sync_failure(void)
{
	char buffer[32];
	struct fplinux_ui_prefs *prefs;

	remove_file();
	prefs = open_preferences();
	assert(prefs);
	assert(fplinux_ui_prefs_set(prefs, "value", "old"));
	assert_durable(prefs);
	assert(fplinux_ui_prefs_set(prefs, "value", "new"));
	sync_failure = SYNC_FILE_FAILS;
	errno = 0;
	assert_save_error(prefs, FPLINUX_STORAGE_UNPUBLISHED, EIO);
	assert(errno == EIO);
	assert(read_file(buffer, sizeof(buffer)) > 0);
	assert(!strcmp(buffer, "value=old\n"));
	assert_no_temporary();
	sync_failure = SYNC_DIRECTORY_FAILS;
	errno = 0;
	assert_save_error(prefs, FPLINUX_STORAGE_PUBLISHED_UNSYNCED, EIO);
	assert(errno == EIO);
	assert(read_file(buffer, sizeof(buffer)) > 0);
	assert(!strcmp(buffer, "value=new\n"));
	assert_no_temporary();
	/* A failed directory sync must still be retried after publication. */
	errno = 0;
	assert_save_error(prefs, FPLINUX_STORAGE_PUBLISHED_UNSYNCED, EIO);
	assert(errno == EIO);
	sync_failure = SYNC_SUCCEEDS;
	assert_durable(prefs);
	fplinux_ui_prefs_close(prefs);
	prefs = open_preferences();
	assert(prefs);
	assert(!strcmp(fplinux_ui_prefs_get(prefs, "value", ""), "new"));
	fplinux_ui_prefs_close(prefs);
}

static void test_unreadable_file(void)
{
	char path[PATH_MAX];

	remove_file();
	file_path(path, sizeof(path), "preferences");
	assert(!mkdir(path, 0700));
	errno = 0;
	assert(!open_preferences());
	assert(errno == EISDIR);
	assert(!rmdir(path));
}

static struct fplinux_ui_prefs *prepare_saved_preferences(void)
{
	struct fplinux_ui_prefs *prefs = open_preferences();

	assert(prefs);
	assert(fplinux_ui_prefs_set(prefs, "value", "old"));
	assert_durable(prefs);
	return prefs;
}

static void test_read_without_metadata_writes(enum io_failure failure)
{
	struct fplinux_ui_prefs *prefs = prepare_saved_preferences();
	char buffer[32];
	char path[PATH_MAX];
	struct stat status;

	fplinux_ui_prefs_close(prefs);
	file_path(path, sizeof(path), "");
	assert(!chmod(path, 0500));
	io_failure = failure;
	failure_error = EROFS;
	prefs = open_preferences();
	assert(prefs);
	assert(!strcmp(fplinux_ui_prefs_get(prefs, "value", ""), "old"));
	assert_durable(prefs);
	fplinux_ui_prefs_close(prefs);
	io_failure = IO_SUCCEEDS;
	assert(!stat(path, &status));
	assert((status.st_mode & 0777) == 0500);
	assert(read_file(buffer, sizeof(buffer)) > 0);
	assert(!strcmp(buffer, "value=old\n"));
	assert(!chmod(path, 0700));
}

static void test_missing_directory_is_lazy(void)
{
	struct fplinux_ui_prefs *prefs;
	char directory[PATH_MAX];
	struct stat status;
	int written;

	written = snprintf(directory, sizeof(directory), "%s/new/state/" APP,
			   root);
	assert(written > 0 && (size_t)written < sizeof(directory));
	prefs = fplinux_ui_prefs_open_directory(directory);
	assert(prefs);
	assert(stat(directory, &status) < 0 && errno == ENOENT);
	assert(!strcmp(fplinux_ui_prefs_get(prefs, "value", "default"),
		       "default"));
	assert(fplinux_ui_prefs_set(prefs, "value", "draft"));
	io_failure = IO_MKDIR_FAILS;
	failure_error = ENOSPC;
	errno = 0;
	assert_save_error(prefs, FPLINUX_STORAGE_UNPUBLISHED, ENOSPC);
	assert(errno == ENOSPC);
	assert(!strcmp(fplinux_ui_prefs_get(prefs, "value", ""), "draft"));
	assert(stat(directory, &status) < 0 && errno == ENOENT);
	io_failure = IO_SUCCEEDS;
	assert_durable(prefs);
	assert(!stat(directory, &status));
	assert((status.st_mode & 0777) == 0700);
	assert(!stat(root, &status));
	assert((status.st_mode & 0777) == 0755);
	fplinux_ui_prefs_close(prefs);
	prefs = fplinux_ui_prefs_open_directory(directory);
	assert(prefs);
	assert(!strcmp(fplinux_ui_prefs_get(prefs, "value", ""), "draft"));
	fplinux_ui_prefs_close(prefs);
}

static void test_read_error_preserves_file(int error)
{
	struct fplinux_ui_prefs *prefs = prepare_saved_preferences();
	char buffer[32];

	fplinux_ui_prefs_close(prefs);
	io_failure = IO_READ_FAILS;
	failure_error = error;
	errno = 0;
	assert(!open_preferences());
	assert(errno == error);
	io_failure = IO_SUCCEEDS;
	assert(read_file(buffer, sizeof(buffer)) > 0);
	assert(!strcmp(buffer, "value=old\n"));
	assert_no_temporary();
}

static void test_unpublished_failure(enum io_failure failure, int error,
				     enum sync_failure sync)
{
	struct fplinux_ui_prefs *prefs = prepare_saved_preferences();
	char buffer[32];

	assert(fplinux_ui_prefs_set(prefs, "value", "new"));
	io_failure = failure;
	failure_error = error;
	sync_failure = sync;
	errno = 0;
	assert_save_error(prefs, FPLINUX_STORAGE_UNPUBLISHED, error);
	assert(errno == error);
	io_failure = IO_SUCCEEDS;
	sync_failure = SYNC_SUCCEEDS;
	assert(read_file(buffer, sizeof(buffer)) > 0);
	assert(!strcmp(buffer, "value=old\n"));
	assert(!strcmp(fplinux_ui_prefs_get(prefs, "value", ""), "new"));
	assert_no_temporary();
	assert_durable(prefs);
	assert(read_file(buffer, sizeof(buffer)) > 0);
	assert(!strcmp(buffer, "value=new\n"));
	assert_no_temporary();
	fplinux_ui_prefs_close(prefs);
}

static void test_directory_sync_retry_keeps_publication(void)
{
	struct fplinux_ui_prefs *prefs = prepare_saved_preferences();
	char buffer[32];
	ino_t published;

	assert(fplinux_ui_prefs_set(prefs, "value", "new"));
	sync_failure = SYNC_DIRECTORY_FAILS;
	errno = 0;
	assert_save_error(prefs, FPLINUX_STORAGE_PUBLISHED_UNSYNCED, EIO);
	assert(errno == EIO);
	assert(read_file(buffer, sizeof(buffer)) > 0);
	assert(!strcmp(buffer, "value=new\n"));
	published = file_inode();
	errno = 0;
	assert_save_error(prefs, FPLINUX_STORAGE_PUBLISHED_UNSYNCED, EIO);
	assert(errno == EIO);
	assert(file_inode() == published);
	sync_failure = SYNC_SUCCEEDS;
	assert_durable(prefs);
	assert(file_inode() == published);
	assert_no_temporary();
	fplinux_ui_prefs_close(prefs);
}

static void test_new_directory_sync_retains_parent_retry(void)
{
	const char *original_root = root;
	struct fplinux_ui_prefs *prefs;
	struct stat status;
	char state[PATH_MAX];
	char buffer[32];
	ino_t published;
	int written;

	assert(!stat(root, &status));
	sync_directory_inode = status.st_ino;
	written = snprintf(state, sizeof(state), "%s/new/state", root);
	assert(written > 0 && (size_t)written < sizeof(state));
	root = state;
	prefs = open_preferences();
	assert(prefs);
	assert(fplinux_ui_prefs_set(prefs, "value", "new"));
	sync_failure = SYNC_SELECTED_DIRECTORY_FAILS;
	assert_save_error(prefs, FPLINUX_STORAGE_PUBLISHED_UNSYNCED, EIO);
	assert(read_file(buffer, sizeof(buffer)) > 0);
	assert(!strcmp(buffer, "value=new\n"));
	published = file_inode();
	assert_save_error(prefs, FPLINUX_STORAGE_PUBLISHED_UNSYNCED, EIO);
	assert(file_inode() == published);
	sync_failure = SYNC_SUCCEEDS;
	assert_durable(prefs);
	assert(file_inode() == published);
	assert_no_temporary();
	fplinux_ui_prefs_close(prefs);
	root = original_root;
}

static void test_pending_sync_saves_latest_values(void)
{
	struct fplinux_ui_prefs *prefs = prepare_saved_preferences();
	char buffer[32];
	ino_t published;

	assert(fplinux_ui_prefs_set(prefs, "value", "published"));
	sync_failure = SYNC_DIRECTORY_FAILS;
	assert_save_error(prefs, FPLINUX_STORAGE_PUBLISHED_UNSYNCED, EIO);
	published = file_inode();
	assert(fplinux_ui_prefs_set(prefs, "value", "published"));
	assert_save_error(prefs, FPLINUX_STORAGE_PUBLISHED_UNSYNCED, EIO);
	assert(file_inode() == published);
	assert(fplinux_ui_prefs_set(prefs, "value", "latest"));
	io_failure = IO_RENAME_FAILS;
	failure_error = EACCES;
	assert_save_error(prefs, FPLINUX_STORAGE_UNPUBLISHED, EACCES);
	io_failure = IO_SUCCEEDS;
	assert(file_inode() == published);
	assert(read_file(buffer, sizeof(buffer)) > 0);
	assert(!strcmp(buffer, "value=published\n"));
	assert(!strcmp(fplinux_ui_prefs_get(prefs, "value", ""), "latest"));
	assert_save_error(prefs, FPLINUX_STORAGE_PUBLISHED_UNSYNCED, EIO);
	assert(read_file(buffer, sizeof(buffer)) > 0);
	assert(!strcmp(buffer, "value=latest\n"));
	assert(file_inode() != published);
	published = file_inode();
	sync_failure = SYNC_SUCCEEDS;
	assert_durable(prefs);
	assert(file_inode() == published);
	assert_no_temporary();
	fplinux_ui_prefs_close(prefs);
}

static bool run_scenario(const char *scenario)
{
	if (!strcmp(scenario, "read-only-mkdir"))
		test_read_without_metadata_writes(IO_MKDIR_FAILS);
	else if (!strcmp(scenario, "read-only-fchmod"))
		test_read_without_metadata_writes(IO_CHMOD_FAILS);
	else if (!strcmp(scenario, "missing-directory"))
		test_missing_directory_is_lazy();
	else if (!strcmp(scenario, "read-error-access"))
		test_read_error_preserves_file(EACCES);
	else if (!strcmp(scenario, "read-error-io"))
		test_read_error_preserves_file(EIO);
	else if (!strcmp(scenario, "save-error-temporary"))
		test_unpublished_failure(IO_TEMPORARY_FAILS, ENOSPC,
					 SYNC_SUCCEEDS);
	else if (!strcmp(scenario, "save-error-flush"))
		test_unpublished_failure(IO_FLUSH_FAILS, EDQUOT, SYNC_SUCCEEDS);
	else if (!strcmp(scenario, "save-error-file-sync"))
		test_unpublished_failure(IO_SUCCEEDS, EIO, SYNC_FILE_FAILS);
	else if (!strcmp(scenario, "save-error-close"))
		test_unpublished_failure(IO_CLOSE_FAILS, EIO, SYNC_SUCCEEDS);
	else if (!strcmp(scenario, "save-error-rename"))
		test_unpublished_failure(IO_RENAME_FAILS, EACCES,
					 SYNC_SUCCEEDS);
	else if (!strcmp(scenario, "directory-sync-retry"))
		test_directory_sync_retry_keeps_publication();
	else if (!strcmp(scenario, "new-directory-sync-retry"))
		test_new_directory_sync_retains_parent_retry();
	else if (!strcmp(scenario, "pending-sync-latest"))
		test_pending_sync_saves_latest_values();
	else
		return false;
	return true;
}

int main(int argc, char **argv)
{
	assert(argc == 2 || argc == 3);
	root = argv[1];
	assert(!chmod(root, 0755));
	if (argc == 3) {
		assert(run_scenario(argv[2]));
		puts("fplinux-ui-prefs: OK");
		return 0;
	}
	test_missing_file();
	test_existing_directory_is_private();
	test_round_trip();
	test_keys();
	test_values();
	test_integers();
	test_unchanged_and_atomic();
	test_sync_failure();
	test_malformed_lines();
	test_unreadable_file();
	puts("fplinux-ui-prefs: OK");
	return 0;
}
