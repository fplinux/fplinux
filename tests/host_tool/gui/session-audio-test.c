// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include "session-audio.h"

#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static void expect(int fd, const char *text)
{
	char packet[256];
	ssize_t count = recv(fd, packet, sizeof(packet), MSG_DONTWAIT);

	assert(count == (ssize_t)strlen(text));
	assert(memcmp(packet, text, (size_t)count) == 0);
}

static void expect_none(int fd)
{
	char packet[256];

	assert(recv(fd, packet, sizeof(packet), MSG_DONTWAIT) < 0 &&
	       errno == EAGAIN);
}

struct resync_owner {
	const char *owner;
	const char *audible;
	bool announced;
	bool told;
};

static void expect_resync(int fd, struct resync_owner *owners, size_t count)
{
	for (size_t received = 0; received < count * 2; ++received) {
		char packet[256];
		ssize_t bytes = recv(fd, packet, sizeof(packet), MSG_DONTWAIT);
		bool matched = false;

		assert(bytes > 0);
		for (size_t index = 0; index < count; ++index) {
			struct resync_owner *owner = &owners[index];

			if (bytes == (ssize_t)strlen(owner->owner) &&
			    !memcmp(packet, owner->owner, (size_t)bytes)) {
				assert(!owner->announced);
				owner->announced = true;
				matched = true;
				break;
			}
			if (bytes == (ssize_t)strlen(owner->audible) &&
			    !memcmp(packet, owner->audible, (size_t)bytes)) {
				assert(owner->announced && !owner->told);
				owner->told = true;
				matched = true;
				break;
			}
		}
		assert(matched);
	}
	for (size_t index = 0; index < count; ++index)
		assert(owners[index].announced && owners[index].told);
	expect_none(fd);
}

static int accept_client(int listener)
{
	int fd = accept4(listener, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);

	assert(fd >= 0);
	return fd;
}

int main(int argc, char **argv)
{
	struct fplinux_session_audio audio;
	struct sockaddr_un address = { .sun_family = AF_UNIX };
	struct epoll_event event;
	int epoll_fd;
	int listener;
	int server;
	struct resync_owner initial[] = {
		{ .owner = "APP 100 1000 org.example.a",
		  .audible = "AUDIBLE 100 1" },
		{ .owner = "APP 200 1000 org.example.b",
		  .audible = "AUDIBLE 200 0" },
		{ .owner = "APP 300 1000 org.example.c",
		  .audible = "AUDIBLE 300 0" },
		{ .owner = "APP 400 1000 org.example.d",
		  .audible = "AUDIBLE 400 0" },
		{ .owner = "CLOCK 500 0", .audible = "AUDIBLE 500 1" },
	};
	struct resync_owner resumed[] = {
		{ .owner = "APP 100 1000 org.example.a",
		  .audible = "AUDIBLE 100 0" },
		{ .owner = "APP 300 1000 org.example.c",
		  .audible = "AUDIBLE 300 0" },
		{ .owner = "APP 400 1000 org.example.d",
		  .audible = "AUDIBLE 400 0" },
		{ .owner = "CLOCK 600 42", .audible = "AUDIBLE 600 1" },
	};

	assert(argc == 2);
	alarm(10);
	assert(strlen(argv[1]) < sizeof(address.sun_path));
	strcpy(address.sun_path, argv[1]);
	listener = socket(AF_UNIX,
			  SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	epoll_fd = epoll_create1(EPOLL_CLOEXEC);
	assert(listener >= 0 && epoll_fd >= 0);
	assert(bind(listener, (struct sockaddr *)&address, sizeof(address)) ==
	       0);
	assert(listen(listener, 4) == 0);
	fplinux_session_audio_init(&audio, NULL, epoll_fd);
	assert(!fplinux_session_audio_clock(&audio, 500, 0, 0));
	fplinux_session_audio_dispatch(&audio, 0);
	assert(audio.fd == -1);
	assert(fplinux_session_audio_deadline(&audio) == 0);
	fplinux_session_audio_init(&audio, argv[1], epoll_fd);
	assert(fplinux_session_audio_app(&audio, 100, 1000, "org.example.a",
					 0));
	assert(fplinux_session_audio_app(&audio, 200, 1000, "org.example.b",
					 0));
	assert(fplinux_session_audio_app(&audio, 300, 1000, "org.example.c",
					 0));
	assert(fplinux_session_audio_app(&audio, 400, 1000, "org.example.d",
					 0));
	assert(fplinux_session_audio_clock(&audio, 500, 0, 0));
	assert(!fplinux_session_audio_clock(&audio, 501, 0, 0));
	fplinux_session_audio_audible(&audio, 100, true, 0);
	fplinux_session_audio_audible(&audio, 500, true, 0);
	fplinux_session_audio_dispatch(&audio, 0);
	server = accept_client(listener);
	expect_resync(server, initial, sizeof(initial) / sizeof(initial[0]));
	/* Hiding the application must not silence the bounded clock helper. */
	fplinux_session_audio_audible(&audio, 100, false, 1);
	expect(server, "AUDIBLE 100 0");
	fplinux_session_audio_audible(&audio, 500, true, 1);
	expect_none(server);
	fplinux_session_audio_gone(&audio, 500, 2);
	expect(server, "GONE 500");
	fplinux_session_audio_gone(&audio, 500, 2);
	expect_none(server);
	/* A failed control connection retains actual state for full resync. */
	close(server);
	assert(epoll_wait(epoll_fd, &event, 1, 2000) == 1);
	fplinux_session_audio_dispatch(&audio, 10);
	assert(audio.fd == -1);
	assert(fplinux_session_audio_deadline(&audio) == 110);
	assert(fplinux_session_audio_clock(&audio, 600, 42, 11));
	fplinux_session_audio_audible(&audio, 600, true, 11);
	fplinux_session_audio_gone(&audio, 200, 12);
	fplinux_session_audio_dispatch(&audio, 109);
	assert(accept4(listener, NULL, NULL, SOCK_CLOEXEC) == -1 &&
	       errno == EAGAIN);
	fplinux_session_audio_dispatch(&audio, 110);
	server = accept_client(listener);
	expect_resync(server, resumed, sizeof(resumed) / sizeof(resumed[0]));
	fplinux_session_audio_close(&audio);
	assert(recv(server, &event, sizeof(event), MSG_DONTWAIT) == 0);
	assert(epoll_wait(epoll_fd, &event, 1, 0) == 0);
	close(server);
	close(listener);
	close(epoll_fd);
	unlink(argv[1]);
	puts("session audio: OK");
	return 0;
}
