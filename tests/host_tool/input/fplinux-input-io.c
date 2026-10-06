// SPDX-License-Identifier: GPL-2.0-only
/* Script only device I/O and elapsed waits; the bridge loop remains real. */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int __real_open(const char *path, int flags, ...);
ssize_t __real_read(int fd, void *buffer, size_t size);
int __wrap_open(const char *path, int flags, ...);
int __wrap_ioctl(int fd, unsigned long request, ...);
int __wrap_poll(struct pollfd *fds, nfds_t count, int timeout);
ssize_t __wrap_read(int fd, void *buffer, size_t size);
unsigned int __wrap_sleep(unsigned int seconds);

static unsigned int attempt;
static int channel = -1;
static int input_device = -1;
static bool delivered;

int __wrap_open(const char *path, int flags, ...)
{
	if (!strcmp(path, "/dev/uinput")) {
		input_device = __real_open("/dev/null", O_WRONLY);
		return input_device;
	}
	if (strcmp(path, "/dev/ttyGS0"))
		return __real_open(path, flags);
	++attempt;
	if (attempt == 10)
		exit(0);
	/* Three identical failures, recovery, a new failure, then poll errors. */
	if (attempt <= 3 || attempt == 5 || attempt == 6) {
		errno = attempt <= 3 ? ENOENT : ENODEV;
		return -1;
	}
	channel = __real_open("/dev/null", O_RDONLY);
	delivered = false;
	return channel;
}

int __wrap_ioctl(int fd, unsigned long request, ...)
{
	(void)request;
	if (fd == input_device)
		return 0;
	errno = ENOTTY;
	return -1;
}

int __wrap_poll(struct pollfd *fds, nfds_t count, int timeout)
{
	(void)timeout;
	if (count != 1 || fds[0].fd != channel)
		abort();
	if (attempt == 7 || attempt == 8) {
		errno = EIO;
		return -1;
	}
	fds[0].revents = POLLIN;
	return 1;
}

ssize_t __wrap_read(int fd, void *buffer, size_t size)
{
	if (fd != channel)
		return __real_read(fd, buffer, size);
	if (delivered)
		return 0;
	if (!size)
		abort();
	((char *)buffer)[0] = 'x';
	delivered = true;
	return 1;
}

unsigned int __wrap_sleep(unsigned int seconds)
{
	(void)seconds;
	return 0;
}
