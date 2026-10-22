/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "terminal-pty-supervisor.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/signalfd.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static void reset_signals(void)
{
	sigset_t signals;
	struct sigaction action = { .sa_handler = SIG_DFL };
	int number;

	sigemptyset(&signals);
	sigprocmask(SIG_SETMASK, &signals, NULL);
	for (number = 1; number < NSIG; ++number)
		sigaction(number, &action, NULL);
}

static void report(int fd, enum fplinux_terminal_pty_report_type type,
		   pid_t shell, int status, int error)
{
	const struct fplinux_terminal_pty_report packet = {
		.type = type,
		.shell = shell,
		.status = status,
		.error = error,
	};

	while (write(fd, &packet, sizeof(packet)) < 0 && errno == EINTR)
		;
}

static void shell_setup_failed(int fd, int error)
{
	while (write(fd, &error, sizeof(error)) < 0 && errno == EINTR)
		;
	_exit(126);
}

static pid_t start_shell(int slave, int owner_fd, int status_fd,
			 const char *shell, const char *startup)
{
	int setup[2];
	int error = 0;
	ssize_t count;
	pid_t child;

	if (pipe2(setup, O_CLOEXEC) < 0)
		return -1;
	child = fork();
	if (!child) {
		close(setup[0]);
		close(owner_fd);
		close(status_fd);
		reset_signals();
		if (setsid() < 0 || ioctl(slave, TIOCSCTTY, 0) < 0 ||
		    dup2(slave, STDIN_FILENO) < 0 ||
		    dup2(slave, STDOUT_FILENO) < 0 ||
		    dup2(slave, STDERR_FILENO) < 0)
			shell_setup_failed(setup[1], errno);
		if (slave > STDERR_FILENO)
			close(slave);
		if (setenv("TERM", "xterm-256color", 1) ||
		    setenv("LANG", "C.UTF-8", 1) || setenv("SHELL", shell, 1))
			shell_setup_failed(setup[1], errno);
		/* READY is safe only after this PID owns the PTY's separate SID. */
		while (write(setup[1], &error, sizeof(error)) < 0 &&
		       errno == EINTR)
			;
		close(setup[1]);
		execl(shell, shell, "--noprofile", "--rcfile", startup, "-i",
		      (char *)NULL);
		_exit(127);
	}
	close(setup[1]);
	if (child < 0) {
		error = errno;
		close(setup[0]);
		errno = error;
		return -1;
	}
	do {
		count = read(setup[0], &error, sizeof(error));
	} while (count < 0 && errno == EINTR);
	close(setup[0]);
	if (count != sizeof(error) || error) {
		int status;

		kill(child, SIGKILL);
		while (waitpid(child, &status, 0) < 0 && errno == EINTR)
			;
		errno = error ? error : EIO;
		return -1;
	}
	return child;
}

static int signal_session(pid_t shell, int number)
{
	DIR *directory = opendir("/proc");
	struct dirent *entry;
	int count = 0;

	if (!directory)
		return -1;
	while ((entry = readdir(directory))) {
		char *end;
		long value = strtol(entry->d_name, &end, 10);
		pid_t pid;

		if (!*entry->d_name || *end || value <= 0 || value > INT_MAX)
			continue;
		pid = (pid_t)value;
		/* Never signal another terminal, the GUI group, or an unrelated PID. */
		if (getsid(pid) != shell)
			continue;
		++count;
		if (number)
			kill(pid, number);
	}
	closedir(directory);
	return count;
}

static void reap_available(pid_t shell, int *shell_status)
{
	pid_t child;
	int status;

	for (;;) {
		child = waitpid(-1, &status, WNOHANG);
		if (child > 0) {
			if (child == shell)
				*shell_status = status;
		} else if (child < 0 && errno == EINTR) {
			continue;
		} else {
			return;
		}
	}
}

/* Reap exited orphans; true once the shell itself is waitable or gone. */
static bool shell_exited(pid_t shell)
{
	for (;;) {
		siginfo_t info = { 0 };
		int status;

		if (waitid(P_ALL, 0, &info, WEXITED | WNOHANG | WNOWAIT) < 0) {
			if (errno == EINTR)
				continue;
			return true;
		}
		if (!info.si_pid)
			return false;
		/* Keep the shell PID reserved until its complete SID has been cleaned. */
		if (info.si_pid == shell)
			return true;
		while (waitpid(info.si_pid, &status, 0) < 0 && errno == EINTR)
			;
	}
}

static void drain_signals(int fd)
{
	struct signalfd_siginfo info;

	while (read(fd, &info, sizeof(info)) == sizeof(info))
		;
}

static int close_session(pid_t shell)
{
	const struct timespec grace = { .tv_nsec = 100000000L };
	const struct timespec retry = { .tv_nsec = 10000000L };
	int status = 126 << 8;

	signal_session(shell, SIGHUP);
	nanosleep(&grace, NULL);
	for (;;) {
		/* Stop forks before the final sweep, including disowned Bash jobs. */
		signal_session(shell, SIGSTOP);
		if (signal_session(shell, SIGKILL) < 0) {
			kill(-shell, SIGKILL);
			kill(shell, SIGKILL);
		}
		reap_available(shell, &status);
		if (signal_session(shell, 0) <= 0)
			break;
		nanosleep(&retry, NULL);
	}
	/* The subreaper owns children orphaned by the shell's termination. */
	reap_available(shell, &status);
	return status;
}

static int parse_fd(const char *text)
{
	char *end;
	long value = strtol(text, &end, 10);

	return *text && !*end && value >= 0 && value <= INT_MAX ? (int)value :
								  -1;
}

int main(int argc, char **argv)
{
	/* The owner pipe's EOF and SIGCHLD are the only wakeups. */
	struct pollfd events[2] = {
		{ .fd = -1, .events = POLLIN },
		{ .fd = -1, .events = POLLIN },
	};
	sigset_t child_signal;
	DIR *directory;
	pid_t shell;
	int status_fd;
	int slave;
	int status;

	if (argc != 6)
		return 126;
	events[0].fd = parse_fd(argv[1]);
	status_fd = parse_fd(argv[2]);
	slave = parse_fd(argv[3]);
	if (events[0].fd < 0 || status_fd < 0 || slave < 0)
		return 126;
	reset_signals();
	signal(SIGPIPE, SIG_IGN);
	/* The shell child's reset_signals() unblocks SIGCHLD before exec. */
	sigemptyset(&child_signal);
	sigaddset(&child_signal, SIGCHLD);
	/* Linux 3.4+; orphaned PTY jobs must be reaped by this package. */
	if (prctl(PR_SET_CHILD_SUBREAPER, 1) < 0 ||
	    sigprocmask(SIG_BLOCK, &child_signal, NULL) < 0 ||
	    (events[1].fd = signalfd(-1, &child_signal,
				     SFD_CLOEXEC | SFD_NONBLOCK)) < 0 ||
	    !(directory = opendir("/proc"))) {
		report(status_fd, FPLINUX_TERMINAL_PTY_ERROR, -1, 0, errno);
		return 126;
	}
	closedir(directory);
	shell = start_shell(slave, events[0].fd, status_fd, argv[4], argv[5]);
	close(slave);
	if (shell < 0) {
		report(status_fd, FPLINUX_TERMINAL_PTY_ERROR, -1, 0, errno);
		return 126;
	}
	report(status_fd, FPLINUX_TERMINAL_PTY_READY, shell, 0, 0);
	/* An exit after this reap leaves SIGCHLD pending, so poll still wakes. */
	while (!shell_exited(shell)) {
		if (poll(events, 2, -1) < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (events[1].revents)
			drain_signals(events[1].fd);
		if (events[0].revents) {
			char byte;

			if (read(events[0].fd, &byte, 1) <= 0)
				break;
		}
	}
	close(events[1].fd);
	status = close_session(shell);
	report(status_fd, FPLINUX_TERMINAL_PTY_FINISHED, shell, status, 0);
	close(events[0].fd);
	close(status_fd);
	return 0;
}
