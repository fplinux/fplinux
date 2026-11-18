// SPDX-License-Identifier: GPL-2.0-only
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <time.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>

/* A service lease and a silent V4L2 seek boundary, with independent checks. */
static unsigned int frequency;
static bool handshake;

int socket(int domain, int type, int protocol)
{
	(void)protocol;
	if (domain != AF_UNIX || (type & ~SOCK_CLOEXEC) != SOCK_SEQPACKET) {
		errno = EINVAL;
		return -1;
	}
	return 18;
}

int connect(int fd, const struct sockaddr *address, socklen_t length)
{
	const struct sockaddr_un *unix_address = (const void *)address;

	(void)length;
	if (fd != 18 ||
	    strcmp(unix_address->sun_path, "/run/fplinux-audio/radio.sock")) {
		errno = EINVAL;
		return -1;
	}
	fputs("SERVICE_CONNECT\n", stderr);
	return 0;
}

ssize_t send(int fd, const void *data, size_t bytes, int flags)
{
	(void)flags;
	if (fd != 18 || (bytes != 4 && bytes != 8)) {
		errno = EINVAL;
		return -1;
	}
	if (getenv("FM_TEST_EXPECT_983") &&
	    (bytes != 8 || memcmp(data, "PLAY 983", 8))) {
		errno = EINVAL;
		return -1;
	}
	fprintf(stderr, "REQUEST %.*s\n", (int)bytes, (const char *)data);
	return (ssize_t)bytes;
}

ssize_t recv(int fd, void *data, size_t bytes, int flags)
{
	const char *reply = getenv("FM_TEST_SERVICE_ERROR") ? "ERROR 16" :
							      "READY";

	(void)fd;
	(void)flags;
	if (bytes < strlen(reply))
		return -1;
	memcpy(data, reply, strlen(reply));
	handshake = true;
	return (ssize_t)strlen(reply);
}

int poll(struct pollfd *fds, nfds_t count, int timeout)
{
	struct timespec delay = { .tv_sec = timeout / 1000 };

	(void)count;
	if (!handshake) {
		fds[0].revents = POLLIN;
		return 1;
	}
	if (getenv("FM_TEST_SIGNAL")) {
		raise(SIGTERM);
		errno = EINTR;
		return -1;
	}
	if (getenv("FM_TEST_SERVICE_GONE")) {
		fds[0].revents = POLLHUP;
		return 1;
	}
	nanosleep(&delay, NULL);
	return 0;
}

int open(const char *path, int flags, ...)
{
	(void)flags;
	if (strcmp(path, "/dev/radio0")) {
		errno = ENOENT;
		return -1;
	}
	fputs("RADIO_OPEN\n", stderr);
	return 17;
}

int close(int fd)
{
	if (fd == 17)
		fputs("RADIO_CLOSE\n", stderr);
	if (fd == 18)
		fputs("LEASE_CLOSE\n", stderr);
	return 0;
}

int ioctl(int fd, unsigned long request, ...)
{
	va_list arguments;
	void *data;

	(void)fd;
	va_start(arguments, request);
	data = va_arg(arguments, void *);
	va_end(arguments);
	if (request == VIDIOC_QUERYCAP) {
		((struct v4l2_capability *)data)->capabilities =
			V4L2_CAP_RADIO | V4L2_CAP_HW_FREQ_SEEK;
	} else if (request == VIDIOC_G_TUNER) {
		struct v4l2_tuner *tuner = data;

		tuner->type = V4L2_TUNER_RADIO;
		tuner->capability = V4L2_TUNER_CAP_LOW |
				    V4L2_TUNER_CAP_HWSEEK_BOUNDED;
	} else if (request == VIDIOC_S_FREQUENCY) {
		frequency = ((struct v4l2_frequency *)data)->frequency;
	} else if (request == VIDIOC_S_HW_FREQ_SEEK) {
		const struct v4l2_hw_freq_seek *seek = data;

		if (seek->type != V4L2_TUNER_RADIO || !seek->seek_upward ||
		    seek->wrap_around || seek->spacing != 100000 ||
		    seek->rangelow != 1400000 || seek->rangehigh != 1728000) {
			errno = EINVAL;
			return -1;
		}
		if (getenv("FM_TEST_NO_CHANNEL")) {
			errno = ENODATA;
			return -1;
		}
		if (frequency <= 1400000)
			frequency = 1400000;
		else if (frequency <= 1409600)
			frequency = 1409600;
		else if (frequency <= 1494400)
			frequency = 1494400;
		else if (frequency <= 1728000)
			frequency = 1728000;
		else {
			errno = ENODATA;
			return -1;
		}
	} else if (request == VIDIOC_G_FREQUENCY) {
		((struct v4l2_frequency *)data)->frequency = frequency;
	} else {
		/* The frontend may seek, but must never change audio controls. */
		errno = EPERM;
		return -1;
	}
	return 0;
}
