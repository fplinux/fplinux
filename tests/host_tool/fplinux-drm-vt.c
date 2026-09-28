/* SPDX-License-Identifier: GPL-2.0-only */
/* Real guardian processes with a file-backed substitute for VT ioctls. */
#define _GNU_SOURCE
#include "fplinux-drm-session.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

static char console_path[256];
static int failure_signal;
static int ready_fd = -1;
static bool killall_mode;
static bool exec_failure;
static const char *guardian_path;

static bool child_pid(pid_t parent, pid_t *child)
{
	char path[80];
	FILE *file;
	bool found;

	snprintf(path, sizeof(path), "/proc/%ld/task/%ld/children",
		 (long)parent, (long)parent);
	file = fopen(path, "r");
	if (!file)
		return false;
	found = fscanf(file, "%d", child) == 1;
	fclose(file);
	return found;
}

static bool process_name(pid_t process, char *name, size_t size)
{
	char path[64];
	FILE *file;
	bool found;

	snprintf(path, sizeof(path), "/proc/%ld/comm", (long)process);
	file = fopen(path, "r");
	if (!file)
		return false;
	found = fgets(name, size, file) != NULL;
	fclose(file);
	if (found)
		name[strcspn(name, "\n")] = '\0';
	return found;
}

static const char *base_name(const char *path)
{
	const char *slash = strrchr(path, '/');

	return slash ? slash + 1 : path;
}

static bool process_matches(pid_t process, const char *name, bool *matches)
{
	char path[64];
	char comm[32];
	char command[PATH_MAX];
	char executable[PATH_MAX];
	FILE *file;
	size_t count;
	ssize_t length;

	if (!process_name(process, comm, sizeof(comm)))
		return false;
	snprintf(path, sizeof(path), "/proc/%ld/cmdline", (long)process);
	file = fopen(path, "r");
	if (!file)
		return false;
	count = fread(command, 1, sizeof(command) - 1, file);
	fclose(file);
	if (!count)
		return false;
	command[count] = '\0';
	snprintf(path, sizeof(path), "/proc/%ld/exe", (long)process);
	length = readlink(path, executable, sizeof(executable) - 1);
	if (length < 0)
		return false;
	executable[length] = '\0';
	*matches = !strcmp(comm, name) || !strcmp(base_name(command), name) ||
		   !strcmp(base_name(executable), name);
	return true;
}

int __real_open(const char *path, int flags, ...);
int __real_execv(const char *path, char *const argv[]);

int __wrap_execv(const char *path, char *const argv[])
{
	(void)path;
	if (exec_failure) {
		errno = ENOENT;
		return -1;
	}
	return __real_execv(guardian_path, argv);
}

int __wrap_open(const char *path, int flags, ...)
{
	mode_t mode = 0;

	if (flags & O_CREAT) {
		va_list arguments;

		va_start(arguments, flags);
		mode = va_arg(arguments, unsigned int);
		va_end(arguments);
	}
	if (!strcmp(path, "/dev/tty5"))
		return __real_open(console_path, flags, mode);
	if (!strcmp(path, "test-drm")) {
		if (killall_mode) {
			if (write(ready_fd, "R", 1) != 1)
				_exit(4);
			for (;;)
				pause();
		}
		if (failure_signal)
			raise(failure_signal);
		errno = ENODEV;
		return -1;
	}
	return __real_open(path, flags, mode);
}

int main(int argc, char **argv)
{
	char directory[] = "/tmp/fplinux-drm-vt-XXXXXX";
	struct rlimit no_core = { 0, 0 };
	int active = 1;
	int console = -1;
	int inherited[2] = { -1, -1 };
	int ready[2] = { -1, -1 };
	int status;
	pid_t app = -1;
	pid_t guardian = -1;
	bool ok = false;

	if (argc != 3 || !mkdtemp(directory))
		return EXIT_FAILURE;
	guardian_path = argv[2];
	if (prctl(PR_SET_CHILD_SUBREAPER, 1) < 0)
		goto cleanup;
	if (!strcmp(argv[1], "segv"))
		failure_signal = SIGSEGV;
	else if (!strcmp(argv[1], "kill"))
		failure_signal = SIGKILL;
	else if (!strcmp(argv[1], "killall"))
		killall_mode = true;
	else if (!strcmp(argv[1], "exec-fail"))
		exec_failure = true;
	else if (strcmp(argv[1], "normal"))
		goto cleanup;
	snprintf(console_path, sizeof(console_path), "%s/console", directory);
	console = open(console_path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
	if (console < 0 ||
	    write(console, &active, sizeof(active)) != sizeof(active) ||
	    pipe2(inherited, O_CLOEXEC) < 0 || pipe2(ready, O_CLOEXEC) < 0)
		goto cleanup;
	app = fork();
	if (app < 0)
		goto cleanup;
	if (app == 0) {
		struct fplinux_drm_session session;
		char error[160];

		close(inherited[0]);
		close(ready[0]);
		ready_fd = ready[1];
		if (killall_mode &&
		    prctl(PR_SET_NAME, "fplinux-vttest", 0, 0, 0) < 0)
			_exit(5);
		setrlimit(RLIMIT_CORE, &no_core);
		if (fplinux_drm_session_open(&session, "test-drm", console_path,
					     DRM_FORMAT_RGB565, error,
					     sizeof(error)))
			_exit(2);
		_exit(errno == (exec_failure ? EIO : ENODEV) ? 0 : 3);
	}
	close(inherited[1]);
	inherited[1] = -1;
	close(ready[1]);
	ready[1] = -1;
	if (killall_mode) {
		char signal_ready;
		bool app_matches;
		bool guardian_matches;

		if (read(ready[0], &signal_ready, 1) != 1 ||
		    signal_ready != 'R' || !child_pid(app, &guardian) ||
		    !process_matches(app, "fplinux-vttest", &app_matches) ||
		    !process_matches(guardian, "fplinux-vttest",
				     &guardian_matches) ||
		    !app_matches)
			goto cleanup;
		if (guardian_matches)
			kill(guardian, SIGKILL);
		if (kill(app, SIGKILL) < 0)
			goto cleanup;
	}
	if (waitpid(app, &status, 0) != app)
		goto cleanup;
	app = -1;
	if (failure_signal || killall_mode) {
		int expected = killall_mode ? SIGKILL : failure_signal;

		if (!WIFSIGNALED(status) || WTERMSIG(status) != expected)
			goto cleanup;
	} else if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		goto cleanup;
	}
	for (int attempt = 0; attempt < 300; ++attempt) {
		struct pollfd lifetime = { .fd = inherited[0],
					   .events = POLLIN };

		if (pread(console, &active, sizeof(active), 0) !=
		    sizeof(active))
			goto cleanup;
		if (active == 1 && poll(&lifetime, 1, 0) == 1 &&
		    (lifetime.revents & POLLHUP)) {
			ok = true;
			break;
		}
		usleep(10000);
	}
cleanup:
	if (ok && (failure_signal || killall_mode))
		while (waitpid(-1, NULL, 0) < 0 && errno == EINTR)
			;
	if (!ok && killall_mode) {
		if (app > 0) {
			kill(app, SIGKILL);
			waitpid(app, NULL, 0);
		}
		if (guardian > 0)
			kill(guardian, SIGKILL);
	}
	if (console >= 0)
		close(console);
	if (inherited[0] >= 0)
		close(inherited[0]);
	if (inherited[1] >= 0)
		close(inherited[1]);
	if (ready[0] >= 0)
		close(ready[0]);
	if (ready[1] >= 0)
		close(ready[1]);
	unlink(console_path);
	rmdir(directory);
	return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
