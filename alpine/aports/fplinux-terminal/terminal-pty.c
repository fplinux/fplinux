/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "terminal-pty.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

bool fplinux_terminal_pty_open(struct fplinux_terminal_pty *pty,
			       unsigned int columns, unsigned int rows,
			       const char *shell, const char *startup,
			       const char *shell_token)
{
	struct winsize size = { 0 };
	char path[128];
	int slave = -1;
	int error;

	pty->fd = -1;
	pty->shell = -1;
	if (!columns || columns > USHRT_MAX || !rows || rows > USHRT_MAX) {
		errno = EINVAL;
		return false;
	}
	size.ws_col = (unsigned short)columns;
	size.ws_row = (unsigned short)rows;
	pty->fd = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC | O_NONBLOCK);
	if (pty->fd < 0)
		return false;
	if (grantpt(pty->fd) || unlockpt(pty->fd))
		goto fail;
	error = ptsname_r(pty->fd, path, sizeof(path));
	if (error) {
		errno = error;
		goto fail;
	}
	slave = open(path, O_RDWR | O_NOCTTY | O_CLOEXEC);
	if (slave < 0 || ioctl(slave, TIOCSWINSZ, &size) < 0)
		goto fail;
	pty->shell = fork();
	if (pty->shell < 0)
		goto fail;
	if (!pty->shell) {
		sigset_t signals;
		struct sigaction action = { .sa_handler = SIG_DFL };
		int number;

		sigemptyset(&signals);
		sigprocmask(SIG_SETMASK, &signals, NULL);
		for (number = 1; number < NSIG; ++number)
			sigaction(number, &action, NULL);
		if (setsid() < 0 || ioctl(slave, TIOCSCTTY, 0) < 0 ||
		    dup2(slave, STDIN_FILENO) < 0 ||
		    dup2(slave, STDOUT_FILENO) < 0 ||
		    dup2(slave, STDERR_FILENO) < 0)
			_exit(126);
		if (slave > STDERR_FILENO)
			close(slave);
		close(pty->fd);
		if (setenv("TERM", "xterm-256color", 1) ||
		    setenv("LANG", "C.UTF-8", 1) || setenv("SHELL", shell, 1) ||
		    setenv("FPLINUX_TERMINAL_PROMPT_TOKEN", shell_token, 1))
			_exit(126);
		execl(shell, shell, "--noprofile", "--rcfile", startup, "-i",
		      (char *)NULL);
		_exit(127);
	}
	close(slave);
	return true;
fail:
	error = errno;
	if (slave >= 0)
		close(slave);
	fplinux_terminal_pty_close(pty);
	errno = error;
	return false;
}

bool fplinux_terminal_pty_shell_foreground(
	const struct fplinux_terminal_pty *pty)
{
	return pty->shell > 0 && tcgetpgrp(pty->fd) == pty->shell;
}

bool fplinux_terminal_pty_finished(struct fplinux_terminal_pty *pty,
				   int *status)
{
	pid_t result;

	if (pty->shell <= 0)
		return true;
	result = waitpid(pty->shell, status, WNOHANG);
	if (result == pty->shell || (result < 0 && errno == ECHILD)) {
		pty->shell = -1;
		return true;
	}
	return false;
}

void fplinux_terminal_pty_close(struct fplinux_terminal_pty *pty)
{
	const struct timespec delay = { .tv_nsec = 10000000L };
	unsigned int attempt;
	int status;

	if (pty->fd >= 0)
		close(pty->fd);
	pty->fd = -1;
	if (pty->shell <= 0)
		return;
	kill(pty->shell, SIGHUP);
	for (attempt = 0; attempt < 100; ++attempt) {
		if (fplinux_terminal_pty_finished(pty, &status))
			return;
		nanosleep(&delay, NULL);
	}
	kill(pty->shell, SIGKILL);
	while (waitpid(pty->shell, &status, 0) < 0 && errno == EINTR)
		;
	pty->shell = -1;
}
