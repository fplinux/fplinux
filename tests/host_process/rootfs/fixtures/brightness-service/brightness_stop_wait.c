// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

int __real_poll(struct pollfd *fds, nfds_t count, int timeout);
int __real_ppoll(struct pollfd *fds, nfds_t count,
		 const struct timespec *timeout, const sigset_t *mask);
ssize_t __real_send(int fd, const void *data, size_t length, int flags);

static bool sent_stop;
static bool replied_after_request;

static void fixture_failed(const char *message)
{
	fprintf(stderr, "brightness stop fixture: %s\n", message);
	_exit(125);
}

/* Only choose the signal interleaving; every wait still runs in the kernel. */
static void signal_before_wait(struct pollfd *fds, nfds_t count)
{
	const char *request = getenv("FPLINUX_TEST_STOP_REQUEST");
	const char *notice = getenv("FPLINUX_TEST_STOP_NOTICE");
	FILE *file;
	int signum, ready;

	if (!request || !notice || sent_stop || !replied_after_request)
		return;
	file = fopen(request, "r");
	if (!file) {
		if (errno != ENOENT)
			fixture_failed("cannot read stop request");
		return;
	}
	if (fscanf(file, "%d", &signum) != 1 ||
	    (signum != SIGTERM && signum != SIGINT))
		fixture_failed("invalid stop request");
	if (fclose(file) != 0)
		fixture_failed("cannot close stop request");
	ready = __real_poll(fds, count, 0);
	if (ready < 0)
		fixture_failed("cannot check descriptor readiness");
	if (ready)
		return;
	file = fopen(notice, "w");
	if (!file || fprintf(file, "%d\n", signum) < 0 || fclose(file) != 0)
		fixture_failed("cannot publish signal notice");
	sent_stop = true;
	if (kill(getpid(), signum) < 0)
		fixture_failed("cannot send stop signal");
}

int __wrap_poll(struct pollfd *fds, nfds_t count, int timeout)
{
	int error = errno;

	signal_before_wait(fds, count);
	errno = error;
	return __real_poll(fds, count, timeout);
}

int __wrap_ppoll(struct pollfd *fds, nfds_t count,
		 const struct timespec *timeout, const sigset_t *mask)
{
	int error = errno;

	signal_before_wait(fds, count);
	errno = error;
	return __real_ppoll(fds, count, timeout, mask);
}

ssize_t __wrap_send(int fd, const void *data, size_t length, int flags)
{
	const char *request = getenv("FPLINUX_TEST_STOP_REQUEST");
	ssize_t result = __real_send(fd, data, length, flags);
	int error = errno;

	/* Deliver the wake-up request's real reply before injecting the signal. */
	if (result >= 0 && request && access(request, F_OK) == 0)
		replied_after_request = true;
	errno = error;
	return result;
}
