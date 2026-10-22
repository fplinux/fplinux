/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "terminal-pty.h"
#include "terminal-pty-supervisor.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

static bool read_report(int fd, struct fplinux_terminal_pty_report *report)
{
	ssize_t count;

	do {
		count = read(fd, report, sizeof(*report));
	} while (count < 0 && errno == EINTR);
	return count == sizeof(*report);
}

bool fplinux_terminal_pty_open(struct fplinux_terminal_pty *pty,
			       unsigned int columns, unsigned int rows,
			       const char *shell, const char *startup)
{
	struct winsize size = { 0 };
	struct fplinux_terminal_pty_report report;
	int owner[2] = { -1, -1 };
	int status[2] = { -1, -1 };
	char path[128];
	int slave = -1;
	int error;

	*pty = (struct fplinux_terminal_pty){
		.fd = -1,
		.shell = -1,
		.supervisor = -1,
		.owner_fd = -1,
		.status_fd = -1,
	};
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
	if (slave < 0 || ioctl(slave, TIOCSWINSZ, &size) < 0 ||
	    pipe2(owner, O_CLOEXEC) < 0 || pipe2(status, O_CLOEXEC) < 0)
		goto fail;
	pty->supervisor = fork();
	if (pty->supervisor < 0)
		goto fail;
	if (!pty->supervisor) {
		char owner_text[24];
		char status_text[24];
		char slave_text[24];

		/* The manager may SIGKILL the GUI's entire process group. */
		if (setsid() < 0)
			_exit(126);
		close(owner[1]);
		close(status[0]);
		close(pty->fd);
		if (fcntl(owner[0], F_SETFD, 0) < 0 ||
		    fcntl(status[1], F_SETFD, 0) < 0 ||
		    fcntl(slave, F_SETFD, 0) < 0)
			_exit(126);
		snprintf(owner_text, sizeof(owner_text), "%d", owner[0]);
		snprintf(status_text, sizeof(status_text), "%d", status[1]);
		snprintf(slave_text, sizeof(slave_text), "%d", slave);
		/* Exec discards LVGL buffers and mappings; the guardian is a tiny ELF. */
		execl(FPLINUX_TERMINAL_PTY_HELPER_PATH,
		      FPLINUX_TERMINAL_PTY_HELPER_PATH, owner_text, status_text,
		      slave_text, shell, startup, (char *)NULL);
		_exit(127);
	}
	close(slave);
	slave = -1;
	close(owner[0]);
	owner[0] = -1;
	close(status[1]);
	status[1] = -1;
	pty->owner_fd = owner[1];
	owner[1] = -1;
	pty->status_fd = status[0];
	status[0] = -1;
	if (!read_report(pty->status_fd, &report)) {
		errno = EIO;
		goto fail;
	}
	if (report.type != FPLINUX_TERMINAL_PTY_READY || report.shell <= 0) {
		errno = report.error ? report.error : EIO;
		goto fail;
	}
	pty->shell = report.shell;
	if (fcntl(pty->status_fd, F_SETFL, O_NONBLOCK) < 0)
		goto fail;
	return true;

fail:
	error = errno;
	if (slave >= 0)
		close(slave);
	if (owner[0] >= 0)
		close(owner[0]);
	if (owner[1] >= 0)
		close(owner[1]);
	if (status[0] >= 0)
		close(status[0]);
	if (status[1] >= 0)
		close(status[1]);
	fplinux_terminal_pty_close(pty);
	errno = error;
	return false;
}

bool fplinux_terminal_pty_finished(struct fplinux_terminal_pty *pty,
				   int *status)
{
	struct fplinux_terminal_pty_report report;
	pid_t result;
	int supervisor_status;

	if (pty->supervisor <= 0) {
		*status = pty->shell_status;
		return true;
	}
	if (!pty->status_received && read_report(pty->status_fd, &report) &&
	    report.type == FPLINUX_TERMINAL_PTY_FINISHED) {
		pty->shell_status = report.status;
		pty->status_received = true;
	}
	result = waitpid(pty->supervisor, &supervisor_status, WNOHANG);
	if (result == pty->supervisor || (result < 0 && errno == ECHILD)) {
		pty->supervisor = -1;
		pty->shell = -1;
		/* FINISHED can arrive between the first nonblocking read and waitpid. */
		if (!pty->status_received &&
		    read_report(pty->status_fd, &report) &&
		    report.type == FPLINUX_TERMINAL_PTY_FINISHED) {
			pty->shell_status = report.status;
			pty->status_received = true;
		}
		if (!pty->status_received)
			pty->shell_status = 126 << 8;
		*status = pty->shell_status;
		return true;
	}
	return false;
}

void fplinux_terminal_pty_close(struct fplinux_terminal_pty *pty)
{
	int status;

	if (pty->fd >= 0)
		close(pty->fd);
	pty->fd = -1;
	if (pty->owner_fd >= 0)
		close(pty->owner_fd);
	pty->owner_fd = -1;
	/* The guardian exits only after the PTY session is empty and reaped. */
	if (pty->supervisor > 0)
		while (waitpid(pty->supervisor, &status, 0) < 0 &&
		       errno == EINTR)
			;
	pty->supervisor = -1;
	pty->shell = -1;
	if (pty->status_fd >= 0)
		close(pty->status_fd);
	pty->status_fd = -1;
}
