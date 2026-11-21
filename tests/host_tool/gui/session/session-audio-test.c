// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include "session-audio.h"

#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

struct packet {
	const char *data;
	size_t length;
};

#define PACKET(bytes) ((struct packet){ (bytes), sizeof(bytes) - 1 })

static void expect(int fd, struct packet expected)
{
	char packet[256];
	ssize_t count =
		recv(fd, packet, sizeof(packet), MSG_DONTWAIT | MSG_TRUNC);

	assert(count == (ssize_t)expected.length);
	assert(memcmp(packet, expected.data, expected.length) == 0);
}

static void expect_none(int fd)
{
	char packet[256];

	assert(recv(fd, packet, sizeof(packet), MSG_DONTWAIT) < 0 &&
	       errno == EAGAIN);
}

static void acknowledge(int fd, struct packet packet)
{
	assert(send(fd, packet.data, packet.length, 0) ==
	       (ssize_t)packet.length);
}

static void output_replay(struct fplinux_session_audio *audio, int server,
			  uint64_t now_ms)
{
	expect(server, PACKET("\x02\x00"));
	expect_none(server);
	acknowledge(server, PACKET("\x09\x00"));
	fplinux_session_audio_dispatch(audio, now_ms);
}

struct resync_owner {
	struct packet owner;
	struct packet audible;
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

			if (bytes == (ssize_t)owner->owner.length &&
			    !memcmp(packet, owner->owner.data, (size_t)bytes)) {
				assert(!owner->announced);
				owner->announced = true;
				matched = true;
				break;
			}
			if (bytes == (ssize_t)owner->audible.length &&
			    !memcmp(packet, owner->audible.data,
				    (size_t)bytes)) {
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
	expect(fd, PACKET("\x07"));
	expect_none(fd);
}

static int accept_client(int listener)
{
	int fd = accept4(listener, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);

	assert(fd >= 0);
	return fd;
}

static uint64_t now_ms(void)
{
	struct timespec now;

	assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
	return (uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static void wait_applied(struct fplinux_session_audio *audio)
{
	uint64_t deadline = now_ms() + 3000;

	while (!audio->ready || audio->output_waiting ||
	       fplinux_session_audio_volume_pending(
		       audio, FPLINUX_SESSION_AUDIO_MEDIA) ||
	       fplinux_session_audio_volume_pending(
		       audio, FPLINUX_SESSION_AUDIO_ALARM)) {
		uint64_t now = now_ms();

		assert(now < deadline);
		fplinux_session_audio_dispatch(audio, now);
		usleep(1000);
	}
}

static void assert_volume(const struct fplinux_session_audio *audio,
			  enum fplinux_session_audio_class class,
			  unsigned int expected)
{
	unsigned int applied;

	assert(fplinux_session_audio_volume_applied(audio, class, &applied));
	assert(applied == expected);
}

static void exercise_live_service(const char *path, pid_t service)
{
	struct fplinux_session_audio audio;
	const int queue_bytes = 1024;
	pid_t pid = getpid();
	uint64_t now;

	fplinux_session_audio_init(&audio, path, -1);
	assert(fplinux_session_audio_app(&audio, pid, geteuid(),
					 "org.example.live", true, now_ms()));
	wait_applied(&audio);
	now = now_ms();
	assert(fplinux_session_audio_set_volume(
		&audio, FPLINUX_SESSION_AUDIO_MEDIA, 3, now));
	assert(fplinux_session_audio_set_volume(
		&audio, FPLINUX_SESSION_AUDIO_MEDIA, 0, now));
	assert(fplinux_session_audio_set_volume(
		&audio, FPLINUX_SESSION_AUDIO_ALARM, 7, now));
	wait_applied(&audio);
	assert_volume(&audio, FPLINUX_SESSION_AUDIO_MEDIA, 0);
	assert_volume(&audio, FPLINUX_SESSION_AUDIO_ALARM, 7);

	assert(fplinux_session_audio_set_output(
		&audio, FPLINUX_AUDIO_ROUTE_SPEAKER, now_ms()));
	wait_applied(&audio);
	assert(audio.output_error == ENOTSUP);
	assert(audio.applied_output == FPLINUX_AUDIO_ROUTE_AUTO);
	assert(fplinux_session_audio_set_output(
		&audio, FPLINUX_AUDIO_ROUTE_AUTO, now_ms()));

	assert(setsockopt(audio.fd, SOL_SOCKET, SO_SNDBUF, &queue_bytes,
			  sizeof(queue_bytes)) == 0);
	assert(kill(service, SIGSTOP) == 0);
	for (unsigned int i = 0; audio.fd >= 0 && i < 10000; ++i)
		fplinux_session_audio_eligible(&audio, pid, !(i & 1), now_ms());
	assert(kill(service, SIGCONT) == 0);
	assert(audio.fd < 0);

	now = now_ms();
	assert(fplinux_session_audio_set_volume(
		&audio, FPLINUX_SESSION_AUDIO_MEDIA, 4, now));
	assert(fplinux_session_audio_set_volume(
		&audio, FPLINUX_SESSION_AUDIO_ALARM, 0, now));
	wait_applied(&audio);
	assert_volume(&audio, FPLINUX_SESSION_AUDIO_MEDIA, 4);
	assert_volume(&audio, FPLINUX_SESSION_AUDIO_ALARM, 0);
	assert(audio.applied_output == FPLINUX_AUDIO_ROUTE_AUTO);
	fplinux_session_audio_close(&audio);
}

static void exercise_independent_peer(const char *path)
{
	struct fplinux_session_audio audio;
	struct sockaddr_un address = { .sun_family = AF_UNIX };
	struct epoll_event event;
	int epoll_fd;
	int listener;
	int server;
	unsigned int level;
	struct resync_owner initial[] = {
		{ .owner = PACKET("\x03\x64\x00\x00\x00\xe8\x03\x00\x00\x01"),
		  .audible = PACKET("\x06\x64\x00\x00\x00\x01") },
		{ .owner = PACKET("\x03\xc8\x00\x00\x00\xe8\x03\x00\x00\x00"),
		  .audible = PACKET("\x06\xc8\x00\x00\x00\x00") },
		{ .owner = PACKET("\x03\x2c\x01\x00\x00\xe8\x03\x00\x00\x00"),
		  .audible = PACKET("\x06\x2c\x01\x00\x00\x00") },
		{ .owner = PACKET("\x03\x90\x01\x00\x00\xe8\x03\x00\x00\x00"),
		  .audible = PACKET("\x06\x90\x01\x00\x00\x00") },
		{ .owner = PACKET("\x04\xf4\x01\x00\x00\x00\x00\x00\x00"),
		  .audible = PACKET("\x06\xf4\x01\x00\x00\x01") },
	};
	struct resync_owner resumed[] = {
		{ .owner = PACKET("\x03\x64\x00\x00\x00\xe8\x03\x00\x00\x00"),
		  .audible = PACKET("\x06\x64\x00\x00\x00\x00") },
		{ .owner = PACKET("\x03\x2c\x01\x00\x00\xe8\x03\x00\x00\x01"),
		  .audible = PACKET("\x06\x2c\x01\x00\x00\x00") },
		{ .owner = PACKET("\x03\x90\x01\x00\x00\xe8\x03\x00\x00\x00"),
		  .audible = PACKET("\x06\x90\x01\x00\x00\x00") },
		{ .owner = PACKET("\x04\x58\x02\x00\x00\x2a\x00\x00\x00"),
		  .audible = PACKET("\x06\x58\x02\x00\x00\x01") },
	};

	assert(strlen(path) < sizeof(address.sun_path));
	strcpy(address.sun_path, path);
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
	fplinux_session_audio_init(&audio, path, epoll_fd);
	assert(!fplinux_session_audio_set_volume(
		&audio, FPLINUX_SESSION_AUDIO_MEDIA, 11, 0));
	assert(fplinux_session_audio_set_volume(
		&audio, FPLINUX_SESSION_AUDIO_MEDIA, 3, 0));
	assert(fplinux_session_audio_set_volume(
		&audio, FPLINUX_SESSION_AUDIO_ALARM, 2, 0));
	assert(fplinux_session_audio_app(&audio, 100, 1000, "org.example.a",
					 true, 0));
	assert(fplinux_session_audio_app(&audio, 200, 1000, "org.example.b",
					 false, 0));
	assert(fplinux_session_audio_app(&audio, 300, 1000, "org.example.c",
					 false, 0));
	assert(fplinux_session_audio_app(&audio, 400, 1000, "org.example.d",
					 false, 0));
	assert(fplinux_session_audio_clock(&audio, 500, 0, 0));
	assert(!fplinux_session_audio_clock(&audio, 501, 0, 0));
	fplinux_session_audio_eligible(&audio, 100, true, 0);
	fplinux_session_audio_eligible(&audio, 500, true, 0);
	fplinux_session_audio_dispatch(&audio, 0);
	server = accept_client(listener);
	expect(server, PACKET("\x01\x01\x03"));
	expect(server, PACKET("\x01\x02\x02"));
	expect_none(server);
	assert(fplinux_session_audio_volume_pending(
		&audio, FPLINUX_SESSION_AUDIO_MEDIA));
	assert(!fplinux_session_audio_volume_applied(
		&audio, FPLINUX_SESSION_AUDIO_MEDIA, &level));
	assert(!audio.volume[FPLINUX_SESSION_AUDIO_MEDIA].confirmed);
	assert(fplinux_session_audio_set_volume(
		&audio, FPLINUX_SESSION_AUDIO_MEDIA, 8, 1));
	assert(fplinux_session_audio_set_volume(
		&audio, FPLINUX_SESSION_AUDIO_MEDIA, 4, 1));
	acknowledge(server, PACKET("\x08\x01\x03"));
	fplinux_session_audio_dispatch(&audio, 1);
	assert(fplinux_session_audio_volume_applied(
		       &audio, FPLINUX_SESSION_AUDIO_MEDIA, &level) &&
	       level == 3);
	assert(fplinux_session_audio_volume_pending(
		&audio, FPLINUX_SESSION_AUDIO_MEDIA));
	expect(server, PACKET("\x01\x01\x04"));
	expect_none(server);
	acknowledge(server, PACKET("\x08\x01\x04"));
	fplinux_session_audio_dispatch(&audio, 2);
	assert(fplinux_session_audio_volume_applied(
		       &audio, FPLINUX_SESSION_AUDIO_MEDIA, &level) &&
	       level == 4);
	assert(!fplinux_session_audio_volume_pending(
		&audio, FPLINUX_SESSION_AUDIO_MEDIA));
	expect_none(server);
	acknowledge(server, PACKET("\x08\x02\x02"));
	fplinux_session_audio_dispatch(&audio, 3);
	assert(fplinux_session_audio_volume_applied(
		       &audio, FPLINUX_SESSION_AUDIO_ALARM, &level) &&
	       level == 2);
	output_replay(&audio, server, 4);
	expect_resync(server, initial, sizeof(initial) / sizeof(initial[0]));
	assert(fplinux_session_audio_playback(&audio, 100, 1000) ==
	       FPLINUX_UI_SESSION_PLAYBACK_NONE);
	acknowledge(server, PACKET("\x0b\x64\x00\x00\x00\xe8\x03\x00\x00\x04"));
	acknowledge(server, PACKET("\x0b\xc8\x00\x00\x00\xe9\x03\x00\x00\x04"));
	acknowledge(server, PACKET("\x0b\xf4\x01\x00\x00\x00\x00\x00\x00\x04"));
	fplinux_session_audio_dispatch(&audio, 4);
	assert(audio.fd >= 0);
	assert(fplinux_session_audio_playback(&audio, 100, 1000) ==
	       FPLINUX_UI_SESSION_PLAYBACK_PLAYING);
	assert(fplinux_session_audio_playback(&audio, 200, 1000) ==
	       FPLINUX_UI_SESSION_PLAYBACK_NONE);
	assert(fplinux_session_audio_playback(&audio, 500, 0) ==
	       FPLINUX_UI_SESSION_PLAYBACK_NONE);
	acknowledge(server, PACKET("\x0b\x64\x00\x00\x00\xe8\x03\x00\x00\x01"));
	fplinux_session_audio_dispatch(&audio, 4);
	assert(fplinux_session_audio_playback(&audio, 100, 1000) ==
	       FPLINUX_UI_SESSION_PLAYBACK_PAUSED);
	/* Hiding the application must not silence the bounded clock helper. */
	fplinux_session_audio_eligible(&audio, 100, false, 1);
	expect(server, PACKET("\x06\x64\x00\x00\x00\x00"));
	fplinux_session_audio_eligible(&audio, 500, true, 1);
	expect_none(server);
	fplinux_session_audio_gone(&audio, 500, 2);
	expect(server, PACKET("\x05\xf4\x01\x00\x00"));
	fplinux_session_audio_gone(&audio, 500, 2);
	expect_none(server);
	/* A failed control connection retains actual state for full resync. */
	close(server);
	assert(epoll_wait(epoll_fd, &event, 1, 2000) == 1);
	fplinux_session_audio_dispatch(&audio, 10);
	assert(audio.fd == -1);
	assert(fplinux_session_audio_playback(&audio, 100, 1000) ==
	       FPLINUX_UI_SESSION_PLAYBACK_NONE);
	assert(!fplinux_session_audio_volume_applied(
		&audio, FPLINUX_SESSION_AUDIO_MEDIA, &level));
	assert(audio.volume[FPLINUX_SESSION_AUDIO_MEDIA].confirmed &&
	       audio.volume[FPLINUX_SESSION_AUDIO_MEDIA].applied == 4);
	assert(fplinux_session_audio_deadline(&audio) == 110);
	assert(fplinux_session_audio_clock(&audio, 600, 42, 11));
	fplinux_session_audio_eligible(&audio, 600, true, 11);
	fplinux_session_audio_gone(&audio, 200, 12);
	/* Reconnect uses the current grant and class intent, not the lost snapshot. */
	assert(fplinux_session_audio_app(&audio, 300, 1000, "org.example.c",
					 true, 12));
	assert(fplinux_session_audio_set_volume(
		&audio, FPLINUX_SESSION_AUDIO_ALARM, 7, 12));
	fplinux_session_audio_dispatch(&audio, 109);
	assert(accept4(listener, NULL, NULL, SOCK_CLOEXEC) == -1 &&
	       errno == EAGAIN);
	fplinux_session_audio_dispatch(&audio, 110);
	server = accept_client(listener);
	expect(server, PACKET("\x01\x01\x04"));
	expect(server, PACKET("\x01\x02\x07"));
	expect_none(server);
	acknowledge(server, PACKET("\x08\x01\x04"));
	acknowledge(server, PACKET("\x08\x02\x07"));
	fplinux_session_audio_dispatch(&audio, 111);
	output_replay(&audio, server, 112);
	expect_resync(server, resumed, sizeof(resumed) / sizeof(resumed[0]));
	assert(fplinux_session_audio_playback(&audio, 100, 1000) ==
	       FPLINUX_UI_SESSION_PLAYBACK_NONE);
	acknowledge(server, PACKET("\x0b\xc8\x00\x00\x00\xe8\x03\x00\x00\x04"));
	acknowledge(server, PACKET("\x0b\x64\x00\x00\x00\xe8\x03\x00\x00\x02"));
	fplinux_session_audio_dispatch(&audio, 112);
	assert(audio.fd >= 0);
	assert(fplinux_session_audio_playback(&audio, 200, 1000) ==
	       FPLINUX_UI_SESSION_PLAYBACK_NONE);
	assert(fplinux_session_audio_playback(&audio, 100, 1000) ==
	       FPLINUX_UI_SESSION_PLAYBACK_HELD);
	assert(fplinux_session_audio_set_volume(
		&audio, FPLINUX_SESSION_AUDIO_MEDIA, 0, 120));
	expect(server, PACKET("\x01\x01\x00"));
	assert(fplinux_session_audio_volume_pending(
		&audio, FPLINUX_SESSION_AUDIO_MEDIA));
	assert(fplinux_session_audio_deadline(&audio) == 1120);
	fplinux_session_audio_dispatch(&audio, 1120);
	assert(audio.fd == -1);
	assert(!fplinux_session_audio_volume_applied(
		&audio, FPLINUX_SESSION_AUDIO_MEDIA, &level));
	close(server);
	fplinux_session_audio_dispatch(&audio, 1220);
	server = accept_client(listener);
	expect(server, PACKET("\x01\x01\x00"));
	expect(server, PACKET("\x01\x02\x07"));
	acknowledge(server, PACKET("\x08\x01\x00"));
	acknowledge(server, PACKET("\x08\x02\x07"));
	fplinux_session_audio_dispatch(&audio, 1221);
	assert(fplinux_session_audio_volume_applied(
		       &audio, FPLINUX_SESSION_AUDIO_MEDIA, &level) &&
	       level == 0);
	assert(!fplinux_session_audio_volume_pending(
		&audio, FPLINUX_SESSION_AUDIO_MEDIA));
	for (size_t i = 0; i < sizeof(resumed) / sizeof(resumed[0]); ++i) {
		resumed[i].announced = false;
		resumed[i].told = false;
	}
	output_replay(&audio, server, 1222);
	expect_resync(server, resumed, sizeof(resumed) / sizeof(resumed[0]));
	fplinux_session_audio_close(&audio);
	assert(recv(server, &event, sizeof(event), MSG_DONTWAIT) == 0);
	assert(epoll_wait(epoll_fd, &event, 1, 0) == 0);
	close(server);
	close(listener);
	close(epoll_fd);
	unlink(path);
}

int main(int argc, char **argv)
{
	alarm(10);
	if (argc == 4 && !strcmp(argv[1], "--live")) {
		char *end;
		long service = strtol(argv[3], &end, 10);

		assert(!*end && service > 0 && (pid_t)service == service);
		exercise_live_service(argv[2], (pid_t)service);
	} else {
		assert(argc == 2);
		exercise_independent_peer(argv[1]);
	}
	puts("session audio: OK");
	return 0;
}
