/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "fplinux-ui-session.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

static bool interrupt_receive;

ssize_t __real_recvmsg(int fd, struct msghdr *message, int flags);

/* Inject only EINTR; packet bytes and descriptors remain kernel-owned. */
ssize_t __wrap_recvmsg(int fd, struct msghdr *message, int flags)
{
	if (interrupt_receive) {
		interrupt_receive = false;
		errno = EINTR;
		return -1;
	}
	return __real_recvmsg(fd, message, flags);
}

int main(void)
{
	const uint16_t pixels[] = { 0x1234, 0xffff, 0, 0xabcd };
	uint16_t copied[4] = { 0 };
	struct fplinux_ui_session_message sent = {
		.type = FPLINUX_UI_SESSION_LAUNCH,
		.width = 2,
		.height = 2,
		.has_frame = true,
	};
	struct fplinux_ui_session_message received;
	int pair[2];
	int producer;

	assert(socketpair(AF_UNIX,
			  SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0,
			  pair) == 0);
	assert(fplinux_ui_session_receive(pair[1], &received) == 0);
	assert(errno == EAGAIN || errno == EWOULDBLOCK);
	producer = memfd_create("session-interrupted-receive", MFD_CLOEXEC);
	assert(producer >= 0);
	assert(write(producer, pixels, sizeof(pixels)) == sizeof(pixels));
	sent.frame_fd = producer;
	assert(fplinux_ui_session_send(pair[0], &sent));
	assert(close(producer) == 0);
	interrupt_receive = true;
	assert(fplinux_ui_session_receive(pair[1], &received) == 1);
	assert(received.type == FPLINUX_UI_SESSION_LAUNCH);
	assert(received.has_frame && received.frame_fd >= 0);
	assert(received.width == 2 && received.height == 2);
	assert((fcntl(received.frame_fd, F_GETFD) & FD_CLOEXEC) != 0);
	assert(pread(received.frame_fd, copied, sizeof(copied), 0) ==
	       sizeof(copied));
	assert(!memcmp(copied, pixels, sizeof(pixels)));
	assert(close(received.frame_fd) == 0);
	assert(fplinux_ui_session_receive(pair[1], &received) == 0);
	assert(errno == EAGAIN || errno == EWOULDBLOCK);
	assert(close(pair[0]) == 0);
	assert(close(pair[1]) == 0);
	return 0;
}
