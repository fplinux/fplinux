/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "session-bluetooth.h"

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "session-core.h"

#ifndef FPLINUX_BLUETOOTH_RECEIVE_EXECUTABLE
#define FPLINUX_BLUETOOTH_RECEIVE_EXECUTABLE "/usr/bin/fplinux-bluetooth"
#endif

struct fplinux_session_bluetooth {
	int fd;
	pid_t worker;
};

static int open_listener(void)
{
	const struct sockaddr_un address = {
		.sun_family = AF_UNIX,
		.sun_path = FPLINUX_BLUETOOTH_RECEIVE_SOCKET,
	};
	int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK,
			0);
	if (fd < 0)
		return -1;
	if (bind(fd, (const struct sockaddr *)&address, sizeof(address)) < 0) {
		struct stat entry;
		if (errno != EADDRINUSE ||
		    lstat(address.sun_path, &entry) < 0 ||
		    entry.st_uid != geteuid() || !S_ISSOCK(entry.st_mode) ||
		    connect(fd, (const struct sockaddr *)&address,
			    sizeof(address)) == 0 ||
		    errno != ECONNREFUSED || unlink(address.sun_path) < 0 ||
		    bind(fd, (const struct sockaddr *)&address,
			 sizeof(address)) < 0)
			goto fail;
	}
	if (chmod(address.sun_path, 0666) == 0 && listen(fd, 4) == 0)
		return fd;
	unlink(address.sun_path);
fail:
	close(fd);
	return -1;
}

static _Noreturn void
start_worker(const struct fplinux_session_options *options, int control,
	     int listener)
{
	int control_copy = fcntl(control, F_DUPFD_CLOEXEC, 5);
	int listener_copy = fcntl(listener, F_DUPFD_CLOEXEC, 5);
	if (control_copy < 0 || listener_copy < 0 ||
	    dup2(control_copy, FPLINUX_BLUETOOTH_RECEIVE_CONTROL_FD) < 0 ||
	    dup2(listener_copy, FPLINUX_BLUETOOTH_RECEIVE_LISTENER_FD) < 0)
		_exit(126);
	if (geteuid() == 0 &&
	    (setgroups(0, NULL) < 0 || setgid(options->app_gid) < 0 ||
	     setuid(options->app_uid) < 0))
		_exit(126);
	if (geteuid() != options->app_uid || getegid() != options->app_gid)
		_exit(126);
	sigset_t signals;
	sigemptyset(&signals);
	if (sigprocmask(SIG_SETMASK, &signals, NULL) < 0)
		_exit(126);
	umask(0077);
	execl(FPLINUX_BLUETOOTH_RECEIVE_EXECUTABLE,
	      FPLINUX_BLUETOOTH_RECEIVE_EXECUTABLE,
	      FPLINUX_BLUETOOTH_RECEIVE_WORKER, options->home, (char *)NULL);
	_exit(127);
}

struct fplinux_session_bluetooth *
fplinux_session_bluetooth_create(const struct fplinux_session_options *options)
{
	if (access(FPLINUX_BLUETOOTH_RECEIVE_EXECUTABLE, X_OK) < 0)
		return NULL;
	struct fplinux_session_bluetooth *bluetooth =
		malloc(sizeof(*bluetooth));
	if (bluetooth == NULL)
		return NULL;
	int listener = open_listener();
	if (listener < 0)
		goto fail;
	int pair[2];
	if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK,
		       0, pair) < 0)
		goto close_listener;
	pid_t worker = fork();
	if (worker == 0) {
		close(pair[0]);
		start_worker(options, pair[1], listener);
	}
	close(pair[1]);
	if (worker < 0) {
		close(pair[0]);
		goto close_listener;
	}
	close(listener);
	*bluetooth = (struct fplinux_session_bluetooth){ .fd = pair[0],
							 .worker = worker };
	return bluetooth;
close_listener:
	close(listener);
	unlink(FPLINUX_BLUETOOTH_RECEIVE_SOCKET);
fail:
	free(bluetooth);
	return NULL;
}

void fplinux_session_bluetooth_destroy(
	struct fplinux_session_bluetooth *bluetooth)
{
	if (bluetooth == NULL)
		return;
	if (bluetooth->fd >= 0)
		close(bluetooth->fd);
	while (waitpid(bluetooth->worker, NULL, 0) < 0 && errno == EINTR)
		continue;
	unlink(FPLINUX_BLUETOOTH_RECEIVE_SOCKET);
	free(bluetooth);
}

int fplinux_session_bluetooth_fd(
	const struct fplinux_session_bluetooth *bluetooth)
{
	return bluetooth != NULL ? bluetooth->fd : -1;
}

bool fplinux_session_bluetooth_receive(
	struct fplinux_session_bluetooth *bluetooth,
	struct fplinux_session_bluetooth_event *event)
{
	if (bluetooth->fd < 0)
		return false;
	ssize_t size;
	do {
		size = recv(bluetooth->fd, event, sizeof(*event), MSG_TRUNC);
	} while (size < 0 && errno == EINTR);
	if (size == sizeof(*event))
		return true;
	if (size < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
		return false;
	close(bluetooth->fd);
	bluetooth->fd = -1;
	*event = (struct fplinux_session_bluetooth_event){
		.state = FPLINUX_SESSION_BLUETOOTH_UNAVAILABLE,
	};
	return true;
}

bool fplinux_session_bluetooth_decide(
	struct fplinux_session_bluetooth *bluetooth, uint32_t id, bool accept)
{
	struct fplinux_bluetooth_receive_decision decision = {
		.id = id,
		.accept = accept,
	};
	if (bluetooth == NULL || bluetooth->fd < 0 || id == 0)
		return false;
	ssize_t size;
	do {
		size = send(bluetooth->fd, &decision, sizeof(decision),
			    MSG_NOSIGNAL);
	} while (size < 0 && errno == EINTR);
	return size == sizeof(decision);
}
