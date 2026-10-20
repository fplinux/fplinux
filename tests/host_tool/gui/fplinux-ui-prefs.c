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
};

static enum sync_failure sync_failure;

int __real_fsync(int fd);

/* The filesystem is real; only a selected synchronization failure is injected. */
int __wrap_fsync(int fd)
{
	struct stat status;

	assert(!fstat(fd, &status));
	if ((sync_failure == SYNC_FILE_FAILS && S_ISREG(status.st_mode)) ||
	    (sync_failure == SYNC_DIRECTORY_FAILS && S_ISDIR(status.st_mode))) {
		errno = EIO;
		return -1;
	}
	return __real_fsync(fd);
}

/*
 * The runtime's fplinux_ui_state_dir() lives with the Wayland and LVGL code;
 * this stand-in gives every application a directory under the test root.
 */
bool fplinux_ui_state_dir(const char *app_id, char *path, size_t capacity)
{
	if (!app_id || !app_id[0] || strchr(app_id, '/')) {
		errno = EINVAL;
		return false;
	}
	if (snprintf(path, capacity, "%s/%s", root, app_id) >= (int)capacity) {
		errno = ENAMETOOLONG;
		return false;
	}
	if (mkdir(path, 0700) < 0 && errno != EEXIST)
		return false;
	return true;
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

	remove_file();
	prefs = fplinux_ui_prefs_open(APP);
	assert(prefs);
	assert(!strcmp(fplinux_ui_prefs_get(prefs, "absent", "x"), "x"));
	assert(!fplinux_ui_prefs_get(prefs, "absent", NULL));
	assert(fplinux_ui_prefs_get_int(prefs, "absent", 42) == 42);
	/* Nothing changed: no file is written. */
	assert(fplinux_ui_prefs_save(prefs));
	assert(read_file(buffer, sizeof(buffer)) == -1);
	fplinux_ui_prefs_close(prefs);
	fplinux_ui_prefs_close(NULL);
	assert(!strcmp(fplinux_ui_prefs_get(NULL, "absent", "y"), "y"));
	assert(!fplinux_ui_prefs_open(""));
	assert(errno == EINVAL);
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
	prefs = fplinux_ui_prefs_open(APP);
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
	assert(fplinux_ui_prefs_save(prefs));
	fplinux_ui_prefs_close(prefs);

	assert(read_file(buffer, sizeof(buffer)) == (long)strlen(expected));
	assert(!strcmp(buffer, expected));
	file_path(path, sizeof(path), "preferences");
	assert(!stat(path, &status));
	assert((status.st_mode & 0777) == 0600);
	assert_no_temporary();

	prefs = fplinux_ui_prefs_open(APP);
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
	struct fplinux_ui_prefs *prefs = fplinux_ui_prefs_open(APP);
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
	struct fplinux_ui_prefs *prefs = fplinux_ui_prefs_open(APP);
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
	assert(fplinux_ui_prefs_save(prefs));
	fplinux_ui_prefs_close(prefs);
	prefs = fplinux_ui_prefs_open(APP);
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
	struct fplinux_ui_prefs *prefs = fplinux_ui_prefs_open(APP);

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
	prefs = fplinux_ui_prefs_open(APP);
	assert(prefs);
	assert(fplinux_ui_prefs_set(prefs, "a", "1"));
	assert(fplinux_ui_prefs_save(prefs));
	inode = file_inode();
	assert(fplinux_ui_prefs_save(prefs));
	assert(fplinux_ui_prefs_set(prefs, "a", "1"));
	assert(fplinux_ui_prefs_set_int(prefs, "a", 1));
	assert(fplinux_ui_prefs_remove(prefs, "absent"));
	assert(fplinux_ui_prefs_save(prefs));
	assert(file_inode() == inode);
	assert(fplinux_ui_prefs_set(prefs, "a", "2"));
	assert(fplinux_ui_prefs_save(prefs));
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
	assert(!fplinux_ui_prefs_save(prefs));
	assert(errno == EISDIR);
	assert_no_temporary();
	assert(!rmdir(path));
	assert(fplinux_ui_prefs_save(prefs));
	fplinux_ui_prefs_close(prefs);
	prefs = fplinux_ui_prefs_open(APP);
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
	prefs = fplinux_ui_prefs_open(APP);
	assert(prefs);
	assert(!strcmp(fplinux_ui_prefs_get(prefs, "good", ""), "1"));
	assert(!strcmp(fplinux_ui_prefs_get(prefs, "dup", ""), "2"));
	assert(!strcmp(fplinux_ui_prefs_get(prefs, "last", ""), "x"));
	assert(!fplinux_ui_prefs_get(prefs, "escape", NULL));
	assert(!fplinux_ui_prefs_get(prefs, "trailing", NULL));
	assert(!fplinux_ui_prefs_get(prefs, "utf", NULL));
	assert(!fplinux_ui_prefs_get(prefs, "nul", NULL));
	/* Loading is not a change; the next real save drops the bad lines. */
	assert(fplinux_ui_prefs_save(prefs));
	assert(read_file(buffer, sizeof(buffer)) ==
	       (long)(sizeof(contents) - 1));
	assert(fplinux_ui_prefs_set(prefs, "good", "3"));
	assert(fplinux_ui_prefs_save(prefs));
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
	prefs = fplinux_ui_prefs_open(APP);
	assert(prefs);
	assert(fplinux_ui_prefs_set(prefs, "value", "old"));
	assert(fplinux_ui_prefs_save(prefs));
	assert(fplinux_ui_prefs_set(prefs, "value", "new"));
	sync_failure = SYNC_FILE_FAILS;
	errno = 0;
	assert(!fplinux_ui_prefs_save(prefs));
	assert(errno == EIO);
	assert(read_file(buffer, sizeof(buffer)) > 0);
	assert(!strcmp(buffer, "value=old\n"));
	assert_no_temporary();
	sync_failure = SYNC_DIRECTORY_FAILS;
	errno = 0;
	assert(!fplinux_ui_prefs_save(prefs));
	assert(errno == EIO);
	assert(read_file(buffer, sizeof(buffer)) > 0);
	assert(!strcmp(buffer, "value=new\n"));
	assert_no_temporary();
	/* A failed directory sync must still be retried after publication. */
	errno = 0;
	assert(!fplinux_ui_prefs_save(prefs));
	assert(errno == EIO);
	sync_failure = SYNC_SUCCEEDS;
	assert(fplinux_ui_prefs_save(prefs));
	fplinux_ui_prefs_close(prefs);
	prefs = fplinux_ui_prefs_open(APP);
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
	assert(!fplinux_ui_prefs_open(APP));
	assert(errno == EISDIR);
	assert(!rmdir(path));
}

int main(int argc, char **argv)
{
	assert(argc == 2);
	root = argv[1];
	test_missing_file();
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
