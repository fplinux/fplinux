// SPDX-License-Identifier: GPL-2.0-only
/* Script only device I/O and elapsed waits; the bridge loop remains real. */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int __real_open(const char *path, int flags, ...);
ssize_t __real_read(int fd, void *buffer, size_t size);
ssize_t __real_write(int fd, const void *buffer, size_t size);
int __wrap_open(const char *path, int flags, ...);
int __wrap_ioctl(int fd, unsigned long request, ...);
int __wrap_poll(struct pollfd *fds, nfds_t count, int timeout);
ssize_t __wrap_read(int fd, void *buffer, size_t size);
ssize_t __wrap_write(int fd, const void *buffer, size_t size);
unsigned int __wrap_sleep(unsigned int seconds);

static unsigned int attempt;
static int channel = -1;
static int input_device = -1;
static bool delivered;
static bool lease_expired;
static size_t received;

static const char before_lease[] =
	"1 31 1 tail\n3 0 1\n1 0 1\n1 768 1\n1 32 -1\n1 32 3\n"
	"0 1 0\n0 0 1\n65537 30 1\n1 18446744073709551616 1\n"
	"1 30 1\0hidden\n"
	"1 30 1                                                                tail\n"
	"1 30 1\r\n0 0 0\nreset\0hidden\nreset\r\n1 31 1\n0 0 0\nx";
static const char after_lease[] = "1 34 1\n1 32 1\n0 0 0\n1 34 1";

static bool sends_events(void)
{
	return getenv("FPLINUX_TEST_INPUT_EVENTS") != NULL;
}

int __wrap_open(const char *path, int flags, ...)
{
	if (!strcmp(path, "/dev/uinput")) {
		input_device = __real_open("/dev/null", O_WRONLY);
		return input_device;
	}
	if (strcmp(path, "/dev/ttyGS0"))
		return __real_open(path, flags);
	++attempt;
	if (sends_events()) {
		if (attempt > 1)
			exit(0);
		channel = __real_open("/dev/null", O_RDONLY);
		return channel;
	}
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
	if (sends_events() && !lease_expired &&
	    received == sizeof(before_lease) - 1) {
		if (timeout != 1000)
			abort();
		lease_expired = true;
		received = 0;
		return 0;
	}
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
	if (!size)
		abort();
	if (sends_events()) {
		const char *input = lease_expired ? after_lease : before_lease;
		size_t length = lease_expired ? sizeof(after_lease) - 1 :
						sizeof(before_lease) - 1;

		if (received == length)
			return 0;
		((char *)buffer)[0] = input[received++];
		return 1;
	}
	if (delivered)
		return 0;
	((char *)buffer)[0] = 'x';
	delivered = true;
	return 1;
}

ssize_t __wrap_write(int fd, const void *buffer, size_t size)
{
	const struct input_event *event = buffer;

	if (fd != input_device)
		return __real_write(fd, buffer, size);
	if (size != sizeof(*event))
		abort();
	printf("%u %u %d\n", event->type, event->code, event->value);
	return size;
}

unsigned int __wrap_sleep(unsigned int seconds)
{
	(void)seconds;
	return 0;
}
