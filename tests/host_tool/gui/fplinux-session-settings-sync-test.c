/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "session-settings.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fail_kind;

int __real_fsync(int fd);
int __wrap_fsync(int fd)
{
	struct stat status;

	if (fstat(fd, &status) < 0)
		abort();
	if ((fail_kind == 1 && S_ISREG(status.st_mode)) ||
	    (fail_kind == 2 && S_ISDIR(status.st_mode))) {
		errno = EIO;
		return -1;
	}
	return __real_fsync(fd);
}

int main(int argc, char **argv)
{
	struct fplinux_session_settings settings;
	char directory[] = "/tmp/fplinux-session-settings-sync-XXXXXX";
	char path[sizeof(directory) + 16];
	char text[256] = { 0 };
	struct dirent *entry;
	unsigned int files = 0;
	int error;
	bool saved;
	FILE *file;
	DIR *dir;

	if (argc != 2 || !mkdtemp(directory))
		return 2;
	fail_kind = atoi(argv[1]);
	snprintf(path, sizeof(path), "%s/session.conf", directory);
	file = fopen(path, "w");
	if (!file || fputs("old\n", file) < 0 || fclose(file) < 0)
		abort();
	fplinux_session_settings_defaults(&settings, NULL);
	saved = fplinux_session_settings_save(&settings, path);
	error = errno;
	file = fopen(path, "r");
	if (!file || !fgets(text, sizeof(text), file) || fclose(file) < 0)
		abort();
	dir = opendir(directory);
	if (!dir)
		abort();
	while ((entry = readdir(dir)))
		files += strcmp(entry->d_name, ".") &&
			 strcmp(entry->d_name, "..");
	closedir(dir);
	unlink(path);
	rmdir(directory);
	if (files != 1 ||
	    (fail_kind == 0 && (!saved || !strcmp(text, "old\n"))) ||
	    (fail_kind == 1 &&
	     (saved || error != EIO || strcmp(text, "old\n"))) ||
	    (fail_kind == 2 &&
	     (saved || error != EIO || !strcmp(text, "old\n")))) {
		fprintf(stderr,
			"FAIL: fsync kind=%d saved=%d errno=%d files=%u first=%s",
			fail_kind, saved, error, files, text);
		return 1;
	}
	return 0;
}
