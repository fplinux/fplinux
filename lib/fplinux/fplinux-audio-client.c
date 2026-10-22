/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define _GNU_SOURCE

#include "fplinux-audio-client-internal.h"
#include "fplinux-audio-protocol.h"

#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

struct fplinux_audio_client {
	int fd;
	/* Answers to timed-out OPENs precede the next OPEN's answer. */
	unsigned int late;
	struct fplinux_audio_stream *streams[FPLINUX_AUDIO_CLIENT_STREAMS];
};

struct fplinux_audio_stream {
	struct fplinux_audio_client *client;
	struct fplinux_audio_ring *ring;
	int16_t *frames;
	size_t bytes;
	uint32_t id;
	/* Private copies of geometry and producer state from the shared ring. */
	uint32_t capacity;
	uint32_t channels;
	uint32_t written;
	bool kicked;
	uint32_t kick_read;
	bool ended;
};

static uint64_t now_ms(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}

static void send_message(int fd, uint32_t type, uint32_t stream)
{
	struct fplinux_audio_message message = { .type = type,
						 .stream = stream };

	(void)send(fd, &message, sizeof(message), MSG_DONTWAIT | MSG_NOSIGNAL);
}

static void hang_up(struct fplinux_audio_client *client)
{
	for (size_t i = 0; i < FPLINUX_AUDIO_CLIENT_STREAMS; ++i)
		if (client->streams[i])
			client->streams[i]->ended = true;
	if (client->fd >= 0)
		close(client->fd);
	client->fd = -1;
	client->late = 0;
}

static int connect_service(void)
{
	const char *path = getenv(FPLINUX_AUDIO_STREAM_SOCKET_ENV);
	struct sockaddr_un address = { .sun_family = AF_UNIX };
	int fd;
	int error;

	if (!path || !path[0])
		path = FPLINUX_AUDIO_STREAM_SOCKET;
	if (strlen(path) >= sizeof(address.sun_path)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	strcpy(address.sun_path, path);
	fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (fd < 0)
		return -1;
	if (connect(fd, (const struct sockaddr *)&address, sizeof(address)) ==
	    0)
		return fd;
	error = errno;
	close(fd);
	if (error == ENOENT)
		error = ECONNREFUSED;
	else if (error == EAGAIN)
		error = ETIMEDOUT;
	errno = error;
	return -1;
}

/* The caller owns the sole received descriptor, including rejected answers. */
static int receive_message(int socket_fd, struct fplinux_audio_message *message,
			   int *fd)
{
	union {
		char bytes[CMSG_SPACE(2 * sizeof(int))];
		struct cmsghdr alignment;
	} control;
	struct iovec vector = { .iov_base = message,
				.iov_len = sizeof(*message) };
	struct msghdr header = {
		.msg_iov = &vector,
		.msg_iovlen = 1,
		.msg_control = control.bytes,
		.msg_controllen = sizeof(control.bytes),
	};
	struct cmsghdr *item;
	ssize_t received;
	bool extra = false;
	bool reset = false;

	*fd = -1;
	for (;;) {
		received = recvmsg(socket_fd, &header,
				   MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
		if (received >= 0)
			break;
		if (errno == EINTR)
			continue;
		/* A closing peer's final ERROR can follow ECONNRESET on Linux. */
		if (errno == ECONNRESET && !reset) {
			reset = true;
			continue;
		}
		if (errno != EAGAIN && errno != EWOULDBLOCK)
			return -1;
		if (!reset)
			return 0;
		errno = ECONNRESET;
		return -1;
	}
	item = CMSG_FIRSTHDR(&header);
	if (item && item->cmsg_level == SOL_SOCKET &&
	    item->cmsg_type == SCM_RIGHTS) {
		size_t count = (item->cmsg_len - CMSG_LEN(0)) / sizeof(int);

		for (size_t i = 0; i < count; ++i) {
			int received_fd;

			memcpy(&received_fd, CMSG_DATA(item) + i * sizeof(int),
			       sizeof(received_fd));
			if (*fd < 0)
				*fd = received_fd;
			else {
				close(received_fd);
				extra = true;
			}
		}
	}
	if (received == 0)
		errno = ECONNRESET;
	else if ((size_t)received != sizeof(*message) || extra ||
		 (header.msg_flags & (MSG_TRUNC | MSG_CTRUNC)))
		errno = EPROTO;
	else
		return 1;
	if (*fd >= 0)
		close(*fd);
	*fd = -1;
	return -1;
}

static void drop_answer(struct fplinux_audio_client *client,
			const struct fplinux_audio_message *message, int fd)
{
	if (message->type == FPLINUX_AUDIO_OPENED && message->stream)
		send_message(client->fd, FPLINUX_AUDIO_CLOSE, message->stream);
	if (fd >= 0)
		close(fd);
}

static struct fplinux_audio_stream *
map_stream(const struct fplinux_audio_message *message, int fd, uint32_t rate,
	   uint32_t channels)
{
	uint32_t capacity = fplinux_audio_ring_capacity(rate);
	size_t bytes = FPLINUX_AUDIO_RING_BYTES(capacity, channels);
	struct fplinux_audio_stream *stream;
	struct fplinux_audio_ring *ring;
	struct stat status;

	if (fd < 0 || !message->stream || message->rate != rate ||
	    message->channels != channels || fstat(fd, &status) < 0 ||
	    status.st_size < 0 || (size_t)status.st_size != bytes) {
		errno = EPROTO;
		return NULL;
	}
	ring = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (ring == MAP_FAILED)
		return NULL;
	if (ring->magic != FPLINUX_AUDIO_RING_MAGIC || ring->rate != rate ||
	    ring->channels != channels || ring->capacity != capacity) {
		munmap(ring, bytes);
		errno = EPROTO;
		return NULL;
	}
	stream = calloc(1, sizeof(*stream));
	if (!stream) {
		munmap(ring, bytes);
		return NULL;
	}
	stream->ring = ring;
	stream->frames = fplinux_audio_ring_frames(ring);
	stream->bytes = bytes;
	stream->id = message->stream;
	stream->capacity = capacity;
	stream->channels = channels;
	stream->written =
		atomic_load_explicit(&ring->write, memory_order_relaxed);
	return stream;
}

static struct fplinux_audio_stream *
request_stream(struct fplinux_audio_client *client, uint32_t rate,
	       uint32_t channels, uint64_t deadline_ms, bool *retry)
{
	struct fplinux_audio_message hello = {
		.type = FPLINUX_AUDIO_HELLO,
		.version = FPLINUX_AUDIO_PROTOCOL_VERSION,
	};
	struct fplinux_audio_message request = {
		.type = FPLINUX_AUDIO_OPEN,
		.rate = rate,
		.channels = channels,
	};
	bool fresh = client->fd < 0;
	int error;

	*retry = false;
	if (fresh) {
		client->fd = connect_service();
		if (client->fd < 0)
			return NULL;
		if (send(client->fd, &hello, sizeof(hello),
			 MSG_DONTWAIT | MSG_NOSIGNAL) < 0)
			goto broken;
	}
	if (send(client->fd, &request, sizeof(request),
		 MSG_DONTWAIT | MSG_NOSIGNAL) < 0) {
		struct fplinux_audio_message answer;
		int fd;

		error = errno;
		if (fresh && receive_message(client->fd, &answer, &fd) > 0) {
			if (fd >= 0)
				close(fd);
			if (answer.type == FPLINUX_AUDIO_ERROR &&
			    answer.error > 0) {
				hang_up(client);
				errno = answer.error;
				return NULL;
			}
		}
		*retry = !fresh;
		errno = error;
		goto broken;
	}
	for (;;) {
		struct pollfd descriptor = { .fd = client->fd,
					     .events = POLLIN };
		struct fplinux_audio_message answer;
		uint64_t now = now_ms();
		int fd;
		int received = receive_message(client->fd, &answer, &fd);

		if (received < 0)
			goto broken;
		if (received > 0 && client->late) {
			--client->late;
			drop_answer(client, &answer, fd);
			continue;
		}
		if (received > 0 && answer.type == FPLINUX_AUDIO_OPENED) {
			struct fplinux_audio_stream *stream =
				map_stream(&answer, fd, rate, channels);

			error = errno;
			if (fd >= 0)
				close(fd);
			if (!stream && answer.stream)
				send_message(client->fd, FPLINUX_AUDIO_CLOSE,
					     answer.stream);
			errno = error;
			return stream;
		}
		if (received > 0) {
			if (fd >= 0)
				close(fd);
			if (answer.type != FPLINUX_AUDIO_ERROR ||
			    answer.error <= 0) {
				errno = EPROTO;
				goto broken;
			}
			if (fresh)
				hang_up(client);
			errno = answer.error;
			return NULL;
		}
		if (now >= deadline_ms) {
			++client->late;
			errno = ETIMEDOUT;
			return NULL;
		}
		if (poll(&descriptor, 1, (int)(deadline_ms - now)) < 0 &&
		    errno != EINTR)
			goto broken;
		if ((descriptor.revents & (POLLHUP | POLLERR | POLLNVAL)) &&
		    !(descriptor.revents & POLLIN)) {
			errno = ECONNRESET;
			goto broken;
		}
	}
broken:
	error = errno;
	hang_up(client);
	errno = error == EPIPE || error == ENOTCONN ? ECONNRESET : error;
	return NULL;
}

bool fplinux_audio_format_valid(unsigned int rate, unsigned int channels)
{
	return rate >= FPLINUX_AUDIO_RATE_MIN &&
	       rate <= FPLINUX_AUDIO_RATE_MAX && channels >= 1 &&
	       channels <= FPLINUX_AUDIO_CHANNELS_MAX;
}

struct fplinux_audio_client *fplinux_audio_client_create(void)
{
	struct fplinux_audio_client *client = calloc(1, sizeof(*client));

	if (client)
		client->fd = -1;
	return client;
}

struct fplinux_audio_stream *
fplinux_audio_stream_open(struct fplinux_audio_client *client,
			  unsigned int rate, unsigned int channels)
{
	uint64_t deadline_ms = now_ms() + FPLINUX_AUDIO_OPEN_TIMEOUT_MS;
	struct fplinux_audio_stream *stream;
	size_t slot;
	bool retry;

	if (!client || !fplinux_audio_format_valid(rate, channels)) {
		errno = EINVAL;
		return NULL;
	}
	for (slot = 0; slot < FPLINUX_AUDIO_CLIENT_STREAMS; ++slot)
		if (!client->streams[slot])
			break;
	if (slot == FPLINUX_AUDIO_CLIENT_STREAMS) {
		errno = EMFILE;
		return NULL;
	}
	stream = request_stream(client, rate, channels, deadline_ms, &retry);
	if (!stream && retry)
		stream = request_stream(client, rate, channels, deadline_ms,
					&retry);
	if (!stream)
		return NULL;
	stream->client = client;
	client->streams[slot] = stream;
	return stream;
}

long fplinux_audio_stream_write(struct fplinux_audio_stream *stream,
				const int16_t *frames, size_t count)
{
	struct fplinux_audio_ring *ring;
	uint32_t read;
	uint32_t used;
	uint32_t start;
	size_t taken;
	size_t first;

	if (!stream || (!frames && count)) {
		errno = EINVAL;
		return -1;
	}
	ring = stream->ring;
	if (stream->ended ||
	    atomic_load_explicit(&ring->state, memory_order_acquire) ==
		    FPLINUX_AUDIO_RING_ENDED) {
		errno = EPIPE;
		return -1;
	}
	read = atomic_load_explicit(&ring->read, memory_order_acquire);
	used = stream->written - read;
	taken = used >= stream->capacity ? 0 : stream->capacity - used;
	if (taken > count)
		taken = count;
	if (!taken)
		return 0;
	start = stream->written & (stream->capacity - 1);
	first = stream->capacity - start;
	if (first > taken)
		first = taken;
	memcpy(stream->frames + (size_t)start * stream->channels, frames,
	       first * stream->channels * sizeof(*frames));
	memcpy(stream->frames, frames + first * stream->channels,
	       (taken - first) * stream->channels * sizeof(*frames));
	stream->written += (uint32_t)taken;
	/* Publish frames before checking the service's sleep hand-off. */
	atomic_store(&ring->write, stream->written);
	if (!atomic_load(&ring->sleeping))
		stream->kicked = false;
	else if (!stream->kicked || read != stream->kick_read) {
		struct fplinux_audio_message kick = {
			.type = FPLINUX_AUDIO_KICK,
			.stream = stream->id,
		};

		if (send(stream->client->fd, &kick, sizeof(kick),
			 MSG_DONTWAIT | MSG_NOSIGNAL) ==
		    (ssize_t)sizeof(kick)) {
			stream->kicked = true;
			stream->kick_read = read;
		}
	}
	return (long)taken;
}

long fplinux_audio_stream_queued(const struct fplinux_audio_stream *stream)
{
	int32_t queued;

	if (!stream) {
		errno = EINVAL;
		return -1;
	}
	if (stream->ended ||
	    atomic_load_explicit(&stream->ring->state, memory_order_acquire) ==
		    FPLINUX_AUDIO_RING_ENDED) {
		errno = EPIPE;
		return -1;
	}
	queued = (int32_t)(stream->written -
			   atomic_load_explicit(&stream->ring->heard,
						memory_order_acquire));
	return queued > 0 ? (long)queued : 0;
}

void fplinux_audio_stream_close(struct fplinux_audio_stream *stream)
{
	struct fplinux_audio_client *client;

	if (!stream)
		return;
	client = stream->client;
	if (!stream->ended)
		send_message(client->fd, FPLINUX_AUDIO_CLOSE, stream->id);
	for (size_t i = 0; i < FPLINUX_AUDIO_CLIENT_STREAMS; ++i)
		if (client->streams[i] == stream)
			client->streams[i] = NULL;
	munmap(stream->ring, stream->bytes);
	free(stream);
}

int fplinux_audio_client_fd(const struct fplinux_audio_client *client)
{
	return client ? client->fd : -1;
}

void fplinux_audio_client_dispatch(struct fplinux_audio_client *client,
				   short revents)
{
	struct fplinux_audio_message message;
	int fd;
	int received;

	if (!client || client->fd < 0)
		return;
	if (revents & POLLIN) {
		while ((received = receive_message(client->fd, &message, &fd)) >
		       0) {
			if (client->late)
				--client->late;
			drop_answer(client, &message, fd);
		}
		if (received < 0) {
			hang_up(client);
			return;
		}
	}
	if (revents & (POLLHUP | POLLERR | POLLNVAL))
		hang_up(client);
}

void fplinux_audio_client_close(struct fplinux_audio_client *client)
{
	if (!client)
		return;
	for (size_t i = 0; i < FPLINUX_AUDIO_CLIENT_STREAMS; ++i)
		fplinux_audio_stream_close(client->streams[i]);
	if (client->fd >= 0)
		close(client->fd);
	free(client);
}
