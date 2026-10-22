// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include "fplinux-audio-client-internal.h"
#include "fplinux-audio-protocol.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

/* Only the service boundary is replaced; transport and mappings are real. */
struct peer {
	int listener;
	int fd;
	const char *scenario;
	unsigned int opened;
	unsigned int closed;
	unsigned int kicked;
	struct fplinux_audio_ring *rings[4];
	size_t bytes[4];
	pthread_mutex_t mutex;
};

static void answer_open(struct peer *peer,
			const struct fplinux_audio_message *request)
{
	struct fplinux_audio_message answer = { .type = FPLINUX_AUDIO_OPENED,
						.stream = peer->opened + 1U,
						.rate = request->rate,
						.channels = request->channels };
	uint32_t capacity = fplinux_audio_ring_capacity(request->rate);
	size_t bytes = FPLINUX_AUDIO_RING_BYTES(capacity, request->channels);
	int fd = memfd_create("audio-client-peer",
			      MFD_CLOEXEC | MFD_ALLOW_SEALING);
	struct fplinux_audio_ring *ring;
	union {
		struct cmsghdr alignment;
		char bytes[CMSG_SPACE(sizeof(fd))];
	} control = { 0 };
	struct iovec vector = { .iov_base = &answer,
				.iov_len = sizeof(answer) };
	struct msghdr header = { .msg_iov = &vector,
				 .msg_iovlen = 1,
				 .msg_control = control.bytes,
				 .msg_controllen = sizeof(control.bytes) };
	struct cmsghdr *rights = CMSG_FIRSTHDR(&header);

	assert(fd >= 0 && ftruncate(fd, (off_t)bytes) == 0);
	ring = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	assert(ring != MAP_FAILED);
	ring->magic = FPLINUX_AUDIO_RING_MAGIC;
	ring->rate = request->rate;
	ring->channels = request->channels;
	ring->capacity = capacity;
	atomic_store(&ring->sleeping, 1);
	if (!strcmp(peer->scenario, "ring")) {
		atomic_store(&ring->write, UINT32_MAX - 31U);
		atomic_store(&ring->read, UINT32_MAX - 31U);
		atomic_store(&ring->heard, UINT32_MAX - 31U);
	}
	if (!strcmp(peer->scenario, "malformed"))
		ring->magic = 0;
	assert(fcntl(fd, F_ADD_SEALS,
		     F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL) == 0);
	pthread_mutex_lock(&peer->mutex);
	assert(peer->opened < 4);
	peer->rings[peer->opened] = ring;
	peer->bytes[peer->opened++] = bytes;
	pthread_mutex_unlock(&peer->mutex);
	rights->cmsg_level = SOL_SOCKET;
	rights->cmsg_type = SCM_RIGHTS;
	rights->cmsg_len = CMSG_LEN(sizeof(fd));
	memcpy(CMSG_DATA(rights), &fd, sizeof(fd));
	assert(sendmsg(peer->fd, &header, MSG_NOSIGNAL) ==
	       (ssize_t)sizeof(answer));
	close(fd);
}

static void *serve(void *data)
{
	struct peer *peer = data;
	struct fplinux_audio_message request;
	int fd = accept4(peer->listener, NULL, NULL, SOCK_CLOEXEC);

	assert(fd >= 0);
	pthread_mutex_lock(&peer->mutex);
	peer->fd = fd;
	pthread_mutex_unlock(&peer->mutex);
	assert(recv(fd, &request, sizeof(request), 0) ==
	       (ssize_t)sizeof(request));
	assert(request.type == FPLINUX_AUDIO_HELLO && request.version == 1);
	while (recv(fd, &request, sizeof(request), 0) ==
	       (ssize_t)sizeof(request)) {
		switch (request.type) {
		case FPLINUX_AUDIO_OPEN:
			if (!strcmp(peer->scenario, "error")) {
				struct fplinux_audio_message answer = {
					.type = FPLINUX_AUDIO_ERROR,
					.error = EBUSY
				};

				assert(send(fd, &answer, sizeof(answer),
					    MSG_NOSIGNAL) ==
				       (ssize_t)sizeof(answer));
				break;
			}
			if (!strcmp(peer->scenario, "late-answer") &&
			    !peer->opened) {
				struct timespec pause = { .tv_nsec =
								  700000000L };

				nanosleep(&pause, NULL);
			}
			answer_open(peer, &request);
			break;
		case FPLINUX_AUDIO_CLOSE:
			pthread_mutex_lock(&peer->mutex);
			++peer->closed;
			pthread_mutex_unlock(&peer->mutex);
			break;
		case FPLINUX_AUDIO_KICK:
			pthread_mutex_lock(&peer->mutex);
			++peer->kicked;
			pthread_mutex_unlock(&peer->mutex);
			break;
		default:
			assert(false);
		}
	}
	close(fd);
	return NULL;
}

static struct fplinux_audio_ring *ring_at(struct peer *peer, unsigned int index)
{
	struct fplinux_audio_ring *ring;

	pthread_mutex_lock(&peer->mutex);
	assert(index < peer->opened);
	ring = peer->rings[index];
	pthread_mutex_unlock(&peer->mutex);
	return ring;
}

static void test_ring(struct fplinux_audio_client *client, struct peer *peer)
{
	struct fplinux_audio_stream *stream =
		fplinux_audio_stream_open(client, 8000, 2);
	struct fplinux_audio_ring *ring;
	int16_t frames[2300 * 2];
	int16_t *mapped;
	uint32_t start = UINT32_MAX - 31U;

	assert(stream);
	ring = ring_at(peer, 0);
	mapped = fplinux_audio_ring_frames(ring);
	for (size_t i = 0; i < 2300 * 2; ++i)
		frames[i] = (int16_t)i;
	assert(fplinux_audio_stream_write(stream, frames, 2300) == 2048);
	assert(fplinux_audio_stream_queued(stream) == 2048);
	assert(fplinux_audio_stream_write(stream, frames, 1) == 0);
	for (size_t i = 0; i < 2048; ++i) {
		size_t position = ((start + (uint32_t)i) & 2047U) * 2U;

		assert(mapped[position] == (int16_t)(i * 2U));
		assert(mapped[position + 1U] == (int16_t)(i * 2U + 1U));
	}
	atomic_store(&ring->read, start + 2000U);
	atomic_store(&ring->heard, start + 1900U);
	assert(fplinux_audio_stream_queued(stream) == 148);
	assert(fplinux_audio_stream_write(stream, frames, 2300) == 2000);
	assert(fplinux_audio_stream_queued(stream) == 2148);
	assert(fplinux_audio_stream_write(stream, NULL, 1) == -1 &&
	       errno == EINVAL);
	assert(fplinux_audio_stream_write(stream, NULL, 0) == 0);
	atomic_store(&ring->state, FPLINUX_AUDIO_RING_ENDED);
	assert(fplinux_audio_stream_write(stream, frames, 1) == -1 &&
	       errno == EPIPE);
	assert(fplinux_audio_stream_queued(stream) == -1 && errno == EPIPE);
	fplinux_audio_stream_close(stream);
}

static void test_limits(struct fplinux_audio_client *client)
{
	struct fplinux_audio_stream *streams[4];

	assert(!fplinux_audio_stream_open(client, 7999, 1) && errno == EINVAL);
	assert(!fplinux_audio_stream_open(client, 48001, 1) && errno == EINVAL);
	assert(!fplinux_audio_stream_open(client, 8000, 3) && errno == EINVAL);
	for (size_t i = 0; i < 4; ++i) {
		streams[i] = fplinux_audio_stream_open(client, 8000, 1);
		assert(streams[i]);
	}
	assert(!fplinux_audio_stream_open(client, 8000, 1) && errno == EMFILE);
	/* Client teardown owns all four still-open mappings and sends CLOSE. */
}

static void test_disconnect(struct fplinux_audio_client *client,
			    struct peer *peer)
{
	struct fplinux_audio_stream *stream =
		fplinux_audio_stream_open(client, 8000, 1);
	struct pollfd descriptor = { .fd = fplinux_audio_client_fd(client),
				     .events = POLLIN };
	int16_t frame = 1;

	assert(stream);
	pthread_mutex_lock(&peer->mutex);
	assert(shutdown(peer->fd, SHUT_RDWR) == 0);
	pthread_mutex_unlock(&peer->mutex);
	assert(poll(&descriptor, 1, 2000) == 1);
	fplinux_audio_client_dispatch(client, descriptor.revents);
	assert(fplinux_audio_client_fd(client) == -1);
	assert(fplinux_audio_stream_write(stream, &frame, 1) == -1 &&
	       errno == EPIPE);
	assert(fplinux_audio_stream_queued(stream) == -1 && errno == EPIPE);
	fplinux_audio_stream_close(stream);
}

int main(int argc, char **argv)
{
	struct sockaddr_un address = { .sun_family = AF_UNIX };
	struct peer peer = { .fd = -1, .mutex = PTHREAD_MUTEX_INITIALIZER };
	struct fplinux_audio_client *client;
	pthread_t thread;

	assert(argc == 3);
	alarm(10);
	peer.scenario = argv[2];
	assert(strlen(argv[1]) < sizeof(address.sun_path));
	strcpy(address.sun_path, argv[1]);
	assert(setenv(FPLINUX_AUDIO_STREAM_SOCKET_ENV, argv[1], 1) == 0);
	peer.listener = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
	assert(peer.listener >= 0);
	assert(bind(peer.listener, (struct sockaddr *)&address,
		    sizeof(address)) == 0);
	assert(listen(peer.listener, 1) == 0);
	assert(pthread_create(&thread, NULL, serve, &peer) == 0);
	client = fplinux_audio_client_create();
	assert(client && fplinux_audio_client_fd(client) == -1);
	if (!strcmp(peer.scenario, "ring"))
		test_ring(client, &peer);
	else if (!strcmp(peer.scenario, "limits"))
		test_limits(client);
	else if (!strcmp(peer.scenario, "disconnect"))
		test_disconnect(client, &peer);
	else if (!strcmp(peer.scenario, "error"))
		assert(!fplinux_audio_stream_open(client, 8000, 1) &&
		       errno == EBUSY);
	else if (!strcmp(peer.scenario, "malformed"))
		assert(!fplinux_audio_stream_open(client, 8000, 1) &&
		       errno == EPROTO);
	else if (!strcmp(peer.scenario, "late-answer")) {
		struct fplinux_audio_stream *stream;

		assert(!fplinux_audio_stream_open(client, 8000, 1) &&
		       errno == ETIMEDOUT);
		stream = fplinux_audio_stream_open(client, 8000, 1);
		assert(stream && ring_at(&peer, 1));
		fplinux_audio_stream_close(stream);
	} else
		assert(false);
	fplinux_audio_client_close(client);
	assert(pthread_join(thread, NULL) == 0);
	if (!strcmp(peer.scenario, "limits"))
		assert(peer.opened == 4 && peer.closed == 4);
	if (!strcmp(peer.scenario, "late-answer"))
		assert(peer.opened == 2 && peer.closed == 2);
	if (!strcmp(peer.scenario, "malformed"))
		assert(peer.opened == 1 && peer.closed == 1);
	if (!strcmp(peer.scenario, "ring"))
		assert(peer.kicked >= 1);
	for (size_t i = 0; i < peer.opened; ++i)
		assert(munmap(peer.rings[i], peer.bytes[i]) == 0);
	close(peer.listener);
	unlink(argv[1]);
	pthread_mutex_destroy(&peer.mutex);
	puts("audio client: OK");
	return 0;
}
