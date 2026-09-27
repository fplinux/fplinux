/* SPDX-License-Identifier: GPL-2.0-only */
/* Real guardian processes with a file-backed substitute for VT ioctls. */
#define _GNU_SOURCE
#include "fplinux-drm-session.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/kd.h>
#include <linux/vt.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

static char console_path[256];
static int failure_signal;

int __real_open(const char *path, int flags, ...);

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
		if (failure_signal)
			raise(failure_signal);
		errno = ENODEV;
		return -1;
	}
	return __real_open(path, flags, mode);
}

int __wrap_ioctl(int fd, unsigned long request, ...)
{
	va_list arguments;
	int active;

	va_start(arguments, request);
	switch (request) {
	case VT_GETSTATE: {
		struct vt_stat *state = va_arg(arguments, struct vt_stat *);

		if (pread(fd, &active, sizeof(active), 0) != sizeof(active))
			abort();
		memset(state, 0, sizeof(*state));
		state->v_active = active;
		break;
	}
	case VT_OPENQRY:
		*va_arg(arguments, int *) = 5;
		break;
	case VT_ACTIVATE:
		active = va_arg(arguments, int);
		if (pwrite(fd, &active, sizeof(active), 0) != sizeof(active))
			abort();
		break;
	case VT_SETMODE:
	case VT_WAITACTIVE:
	case VT_RELDISP:
	case VT_DISALLOCATE:
	case KDSETMODE:
		break;
	default:
		va_end(arguments);
		errno = ENOTTY;
		return -1;
	}
	va_end(arguments);
	return 0;
}

int main(int argc, char **argv)
{
	char directory[] = "/tmp/fplinux-drm-vt-XXXXXX";
	struct rlimit no_core = { 0, 0 };
	int active = 1;
	int console = -1;
	int inherited[2] = { -1, -1 };
	int status;
	pid_t app;
	bool ok = false;

	if (argc != 2 || !mkdtemp(directory))
		return EXIT_FAILURE;
	if (prctl(PR_SET_CHILD_SUBREAPER, 1) < 0)
		goto cleanup;
	if (!strcmp(argv[1], "segv"))
		failure_signal = SIGSEGV;
	else if (!strcmp(argv[1], "kill"))
		failure_signal = SIGKILL;
	else if (strcmp(argv[1], "normal"))
		goto cleanup;
	snprintf(console_path, sizeof(console_path), "%s/console", directory);
	console = open(console_path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
	if (console < 0 ||
	    write(console, &active, sizeof(active)) != sizeof(active) ||
	    pipe2(inherited, O_CLOEXEC) < 0)
		goto cleanup;
	app = fork();
	if (app < 0)
		goto cleanup;
	if (app == 0) {
		struct fplinux_drm_session session;
		char error[160];

		close(inherited[0]);
		setrlimit(RLIMIT_CORE, &no_core);
		if (fplinux_drm_session_open(&session, "test-drm", console_path,
					     DRM_FORMAT_RGB565, error,
					     sizeof(error)))
			_exit(2);
		_exit(errno == ENODEV ? 0 : 3);
	}
	close(inherited[1]);
	inherited[1] = -1;
	if (waitpid(app, &status, 0) != app)
		goto cleanup;
	if (failure_signal ? (!WIFSIGNALED(status) ||
			      WTERMSIG(status) != failure_signal) :
			     (!WIFEXITED(status) || WEXITSTATUS(status) != 0))
		goto cleanup;
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
	if (ok && failure_signal)
		while (waitpid(-1, NULL, 0) < 0 && errno == EINTR)
			;
	if (console >= 0)
		close(console);
	if (inherited[0] >= 0)
		close(inherited[0]);
	if (inherited[1] >= 0)
		close(inherited[1]);
	unlink(console_path);
	rmdir(directory);
	return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
