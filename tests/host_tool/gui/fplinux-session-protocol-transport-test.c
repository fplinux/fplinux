/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
/* Run the existing JSON packet contract with a one-packet memory transport. */
#include "fplinux-ui-session.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static char packet[FPLINUX_UI_SESSION_PACKET_BYTES * 2];
static size_t packet_bytes;
static int packet_fds[253];
static size_t packet_fd_count;
static int receive_fault;

/* Faults that a real Linux socket cannot emit, exercised by the shared test. */
void fplinux_protocol_transport_fault(int fault)
{
	receive_fault = fault;
}

int __wrap_socketpair(int domain, int type, int protocol, int pair[2])
{
	(void)domain;
	(void)type;
	(void)protocol;
	pair[0] = 1001;
	pair[1] = 1002;
	return 0;
}

ssize_t __wrap_send(int fd, const void *data, size_t length, int flags)
{
	(void)fd;
	(void)flags;
	if (packet_bytes || length > sizeof(packet))
		abort();
	memcpy(packet, data, length);
	packet_bytes = length;
	return (ssize_t)length;
}

ssize_t __wrap_sendmsg(int fd, const struct msghdr *message, int flags)
{
	ssize_t sent;

	if (message->msg_iovlen != 1 || packet_fd_count)
		abort();
	sent = __wrap_send(fd, message->msg_iov[0].iov_base,
			   message->msg_iov[0].iov_len, flags);
	for (struct cmsghdr *item = CMSG_FIRSTHDR(message); item;
	     item = CMSG_NXTHDR((struct msghdr *)message, item)) {
		if (item->cmsg_level != SOL_SOCKET ||
		    item->cmsg_type != SCM_RIGHTS)
			abort();
		for (size_t i = 0;
		     i < (item->cmsg_len - CMSG_LEN(0)) / sizeof(int); ++i) {
			int original;

			if (packet_fd_count == 253)
				abort();
			memcpy(&original,
			       (char *)CMSG_DATA(item) + i * sizeof(int),
			       sizeof(original));
			packet_fds[packet_fd_count] = dup(original);
			if (packet_fds[packet_fd_count++] < 0)
				abort();
		}
	}
	return sent;
}

ssize_t __wrap_recvmsg(int fd, struct msghdr *message, int flags)
{
	size_t length = packet_bytes;
	size_t capacity = message->msg_iov[0].iov_len;
	size_t control_capacity = message->msg_controllen;
	size_t count = packet_fd_count;

	(void)fd;
	if (receive_fault == 4 || receive_fault == 5) {
		errno = receive_fault == 4 ? EINTR : EAGAIN;
		receive_fault = 0;
		return -1;
	}
	if (!length) {
		errno = EAGAIN;
		return -1;
	}
	memcpy(message->msg_iov[0].iov_base, packet,
	       length < capacity ? length : capacity);
	packet_bytes = 0;
	message->msg_flags = length > capacity ? MSG_TRUNC : 0;
	if (count) {
		struct cmsghdr *rights;

		if (control_capacity < CMSG_SPACE(count * sizeof(int)))
			abort();
		rights = CMSG_FIRSTHDR(message);
		rights->cmsg_level = SOL_SOCKET;
		rights->cmsg_type = SCM_RIGHTS;
		rights->cmsg_len = CMSG_LEN(count * sizeof(int));
		for (size_t i = 0; i < count; ++i)
			if ((flags & MSG_CMSG_CLOEXEC) && receive_fault != 3 &&
			    fcntl(packet_fds[i], F_SETFD, FD_CLOEXEC) < 0)
				abort();
		memcpy(CMSG_DATA(rights), packet_fds, count * sizeof(int));
		message->msg_controllen = CMSG_SPACE(count * sizeof(int));
		if (receive_fault == 1) {
			/*
			 * A valid rights header followed by credentials, as
			 * the kernel sends them: with their struct ucred. musl's
			 * CMSG_NXTHDR() never returns a last header without
			 * data, which the kernel does not send either.
			 */
			struct ucred credentials = { .pid = getpid() };
			struct cmsghdr *unexpected =
				(struct cmsghdr *)((char *)rights +
						   message->msg_controllen);

			if (control_capacity <
			    message->msg_controllen +
				    CMSG_SPACE(sizeof(credentials)))
				abort();
			unexpected->cmsg_level = SOL_SOCKET;
			unexpected->cmsg_type = SCM_CREDENTIALS;
			unexpected->cmsg_len = CMSG_LEN(sizeof(credentials));
			memcpy(CMSG_DATA(unexpected), &credentials,
			       sizeof(credentials));
			message->msg_controllen +=
				CMSG_SPACE(sizeof(credentials));
		}
	} else {
		message->msg_controllen = 0;
	}
	if (receive_fault == 2)
		message->msg_flags |= MSG_CTRUNC;
	receive_fault = 0;
	packet_fd_count = 0;
	return (ssize_t)((flags & MSG_TRUNC) || length < capacity ? length :
								    capacity);
}

int __real_close(int fd);
int __wrap_close(int fd)
{
	return fd == 1001 || fd == 1002 ? 0 : __real_close(fd);
}
