// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

extern char **environ;

int fplinux_quake_program_main(int argc, char **argv);
int __real_open(const char *path, int flags, ...);
int __real_mkdir(const char *path, mode_t mode);
int __real_stat(const char *path, struct stat *status);
int __real_access(const char *path, int mode);
int __real_symlink(const char *target, const char *path);
FILE *__real_fopen(const char *path, const char *mode);

static void fixture_path(char *path, size_t size, const char *name)
{
	const char *root = getenv("FPLINUX_QUAKE_TEST_ROOT");

	if (!root || snprintf(path, size, "%s/%s", root, name) >= (int)size)
		_exit(125);
}

static void record_resource_access(void)
{
	char path[4096];
	int descriptor;

	fixture_path(path, sizeof(path), "resources-opened");
	descriptor = __real_open(path, O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
	if (descriptor < 0 || close(descriptor) < 0)
		_exit(125);
}

static bool is_card_pak(const char *path)
{
	return !strncmp(path, "/mnt/card/fplinux/quake/id1/pak",
			sizeof("/mnt/card/fplinux/quake/id1/pak") - 1);
}

static void card_pak_path(char *path, size_t size, const char *original)
{
	const char *name = strrchr(original, '/');

	fixture_path(path, size, name + 1);
}

int __wrap_open(const char *path, int flags, ...)
{
	char replacement[4096];
	mode_t mode = 0;
	int descriptor;

	if (flags & O_CREAT) {
		va_list arguments;

		va_start(arguments, flags);
		mode = va_arg(arguments, mode_t);
		va_end(arguments);
	}
	if (strcmp(path, "/tmp/fplinux-quake.lock"))
		return __real_open(path, flags, mode);
	record_resource_access();
	fixture_path(replacement, sizeof(replacement), "session.lock");
	descriptor = __real_open(replacement, flags, mode);
	if (descriptor >= 0) {
		char value[32];

		snprintf(value, sizeof(value), "%d", descriptor);
		if (setenv("FPLINUX_QUAKE_TEST_LOCK_FD", value, 1) < 0)
			_exit(125);
	}
	return descriptor;
}

int __wrap_mkdir(const char *path, mode_t mode)
{
	if (!strcmp(path, "/mnt/card")) {
		record_resource_access();
		return 0;
	}
	return __real_mkdir(path, mode);
}

FILE *__wrap_fopen(const char *path, const char *mode)
{
	static char mountinfo[] = "1 0 0:1 / /mnt/card ro - vfat fixture ro\n";

	if (!strcmp(path, "/proc/self/mountinfo")) {
		record_resource_access();
		return fmemopen(mountinfo, sizeof(mountinfo) - 1, "r");
	}
	return __real_fopen(path, mode);
}

int __wrap_stat(const char *path, struct stat *status)
{
	char replacement[4096];

	if (!is_card_pak(path))
		return __real_stat(path, status);
	record_resource_access();
	card_pak_path(replacement, sizeof(replacement), path);
	return __real_stat(replacement, status);
}

int __wrap_access(const char *path, int mode)
{
	char replacement[4096];

	if (!strcmp(path, "/usr/bin/tyr-glquake")) {
		fixture_path(replacement, sizeof(replacement), "gpu-engine");
		return __real_access(replacement, mode);
	}
	if (!is_card_pak(path))
		return __real_access(path, mode);
	record_resource_access();
	card_pak_path(replacement, sizeof(replacement), path);
	return __real_access(replacement, mode);
}

int __wrap_symlink(const char *target, const char *path)
{
	char replacement[4096];

	if (!is_card_pak(target))
		return __real_symlink(target, path);
	card_pak_path(replacement, sizeof(replacement), target);
	return __real_symlink(replacement, path);
}

int __wrap_execv(const char *path, char *const arguments[])
{
	char replacement[4096];

	if (strcmp(path, "/usr/bin/tyr-quake") &&
	    strcmp(path, "/usr/bin/tyr-glquake")) {
		errno = ENOENT;
		return -1;
	}
	if (!strcmp(path, "/usr/bin/tyr-glquake")) {
		fixture_path(replacement, sizeof(replacement), "gpu-engine");
		if (__real_access(replacement, X_OK) < 0)
			return -1;
	}
	if (setenv("FPLINUX_QUAKE_TEST_EXEC_PATH", path, 1) < 0)
		return -1;
	return execve("/proc/self/exe", arguments, environ);
}

static int run_engine_peer(int argc, char **argv)
{
	static const int signals[] = { SIGHUP, SIGINT, SIGQUIT, SIGTERM };
	const char *lock_value = getenv("FPLINUX_QUAKE_TEST_LOCK_FD");
	const char *executable = getenv("FPLINUX_QUAKE_TEST_EXEC_PATH");
	const char *home = getenv("HOME");
	bool signals_default = true;
	bool lock_closed;
	char path[4096];
	FILE *report;
	size_t index;
	int argument;

	if (!lock_value || !executable || !home)
		return 125;
	lock_closed = fcntl(atoi(lock_value), F_GETFD) < 0 && errno == EBADF;
	for (index = 0; index < sizeof(signals) / sizeof(signals[0]); ++index) {
		struct sigaction action;

		if (sigaction(signals[index], NULL, &action) < 0 ||
		    action.sa_handler != SIG_DFL)
			signals_default = false;
	}
	fixture_path(path, sizeof(path), "engine-report");
	report = __real_fopen(path, "w");
	if (!report)
		return 125;
	fprintf(report, "executable=%s\nhome=%s\npid=%ld\nparent=%ld\n",
		executable, home, (long)getpid(), (long)getppid());
	fprintf(report, "lock_closed=%d\nsignals_default=%d\n", lock_closed,
		signals_default);
	for (argument = 0; argument < argc; ++argument)
		fprintf(report, "arg=%s\n", argv[argument]);
	fputs("ready=1\n", report);
	if (fclose(report) < 0)
		return 125;
	if (getenv("FPLINUX_QUAKE_TEST_WAIT"))
		for (;;)
			pause();
	return 23;
}

int main(int argc, char **argv)
{
	if (!strcmp(argv[0], "/usr/bin/tyr-quake") ||
	    !strcmp(argv[0], "/usr/bin/tyr-glquake"))
		return run_engine_peer(argc, argv);
	return fplinux_quake_program_main(argc, argv);
}
