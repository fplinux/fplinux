// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include "audio-streams.h"

#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

/* A flooding connection yields to the others after this many packets. */
#define PACKETS_PER_ROUND 16U

/* Media and Alarm share one Q15 curve: -27 dB through unity in 3 dB steps. */
static const int32_t volume_gain[FPLINUX_AUDIO_VOLUME_MAX + 1] = {
	0,
	1464,
	2068,
	2920,
	4125,
	5827,
	8231,
	11627,
	16423,
	23198,
	FPLINUX_AUDIO_MIXER_UNITY,
};

void fplinux_audio_streams_init(struct fplinux_audio_streams *streams)
{
	unsigned int i;

	*streams = (struct fplinux_audio_streams){ 0 };
	streams->rate = FPLINUX_AUDIO_MIXER_RATE;
	streams->media_volume = FPLINUX_AUDIO_VOLUME_MAX;
	streams->alarm_volume = FPLINUX_AUDIO_VOLUME_MAX;
	for (i = 0; i < FPLINUX_AUDIO_CONNECTIONS; ++i)
		streams->connections[i].fd = -1;
}

static struct fplinux_audio_process *
find_process(struct fplinux_audio_streams *streams, pid_t pid)
{
	unsigned int i;

	for (i = 0; pid > 0 && i < FPLINUX_AUDIO_PROCESSES; ++i)
		if (streams->processes[i].pid == pid)
			return &streams->processes[i];
	return NULL;
}

/* The entry of a process that left, or NULL. */
static pid_t *find_gone(struct fplinux_audio_streams *streams, pid_t pid)
{
	unsigned int i;

	for (i = 0; pid > 0 && i < FPLINUX_AUDIO_GONE; ++i)
		if (streams->gone[i] == pid)
			return &streams->gone[i];
	return NULL;
}

static pid_t stream_pid(const struct fplinux_audio_streams *streams,
			const struct fplinux_audio_stream *stream)
{
	return streams->connections[stream->connection].pid;
}

/* Sends one message, with the ring's descriptor when ring is not -1. */
static bool reply(int fd, const struct fplinux_audio_message *message, int ring)
{
	union {
		struct cmsghdr header;
		char data[CMSG_SPACE(sizeof(int))];
	} control;
	struct iovec iov = { .iov_base = (void *)message,
			     .iov_len = sizeof(*message) };
	struct msghdr header = { .msg_iov = &iov, .msg_iovlen = 1 };

	if (ring >= 0) {
		struct cmsghdr *rights;

		memset(&control, 0, sizeof(control));
		header.msg_control = control.data;
		header.msg_controllen = sizeof(control.data);
		rights = CMSG_FIRSTHDR(&header);
		rights->cmsg_level = SOL_SOCKET;
		rights->cmsg_type = SCM_RIGHTS;
		rights->cmsg_len = CMSG_LEN(sizeof(int));
		memcpy(CMSG_DATA(rights), &ring, sizeof(int));
	}
	return sendmsg(fd, &header, MSG_DONTWAIT | MSG_NOSIGNAL) ==
	       (ssize_t)sizeof(*message);
}

static bool refuse(int fd, int error)
{
	struct fplinux_audio_message message = {
		.type = FPLINUX_AUDIO_ERROR,
		.error = error,
	};

	return reply(fd, &message, -1);
}

static void end_stream(struct fplinux_audio_streams *streams,
		       struct fplinux_audio_stream *stream)
{
	unsigned int channel;

	/* A stream that ends while it plays fades out, like a pause. */
	if (streams->output == FPLINUX_AUDIO_OUTPUT_OPEN)
		for (channel = 0; channel < FPLINUX_AUDIO_MIXER_CHANNELS;
		     ++channel)
			streams->tail[channel] += stream->mixer.last[channel];
	atomic_store(&stream->ring->state, FPLINUX_AUDIO_RING_ENDED);
	munmap(stream->ring, stream->bytes);
	stream->ring = NULL;
}

void fplinux_audio_streams_disconnect(struct fplinux_audio_streams *streams,
				      unsigned int index)
{
	struct fplinux_audio_connection *connection =
		&streams->connections[index];
	struct fplinux_audio_message unread;
	unsigned int i;

	for (i = 0; i < FPLINUX_AUDIO_STREAMS; ++i)
		if (streams->streams[i].ring &&
		    streams->streams[i].connection == index)
			end_stream(streams, &streams->streams[i]);
	/*
	 * Closing with packets unread would reset the connection, and the
	 * peer would lose an ERROR it has not read yet: an OPEN sent with a
	 * refused HELLO is still queued.
	 */
	for (i = 0; i < PACKETS_PER_ROUND; ++i)
		if (recv(connection->fd, &unread, sizeof(unread),
			 MSG_DONTWAIT) <= 0)
			break;
	close(connection->fd);
	*connection = (struct fplinux_audio_connection){ .fd = -1 };
}

/*
 * Stores the state and the sleeping flag that the policy and the output
 * give a stream. Returns whether audiod should look at its write counter
 * now: a stream that may play while audiod does not poll it every period.
 */
static bool update(struct fplinux_audio_streams *streams,
		   struct fplinux_audio_stream *stream)
{
	struct fplinux_audio_process *process =
		find_process(streams, stream_pid(streams, stream));
	uint32_t reason = FPLINUX_AUDIO_REASON_NONE;
	bool paused;
	uint32_t state = FPLINUX_AUDIO_RING_PLAYING;
	uint32_t sleeping;
	unsigned int level = process && process->clock ? streams->alarm_volume :
							 streams->media_volume;

	if (stream->user_paused)
		reason = FPLINUX_AUDIO_REASON_USER;
	else if (stream->route_paused)
		reason = FPLINUX_AUDIO_REASON_DEVICE;
	else if (!streams->manager_ready || !process ||
		 (!process->eligible && !process->background))
		reason = FPLINUX_AUDIO_REASON_POLICY;
	else if (!process->clock && streams->private_unavailable)
		reason = FPLINUX_AUDIO_REASON_DEVICE;
	else if (!process->clock && streams->clock_active)
		reason = FPLINUX_AUDIO_REASON_CLOCK;
	else if (!process->clock && streams->fm_active)
		reason = FPLINUX_AUDIO_REASON_FM;
	else if (!process->clock && !process->eligible &&
		 streams->foreground_active)
		reason = FPLINUX_AUDIO_REASON_FOREGROUND;
	paused = reason != FPLINUX_AUDIO_REASON_NONE;
	sleeping = paused || streams->output != FPLINUX_AUDIO_OUTPUT_OPEN;

	if (paused)
		state = FPLINUX_AUDIO_RING_PAUSED;
	else if (streams->output == FPLINUX_AUDIO_OUTPUT_UNAVAILABLE) {
		state = FPLINUX_AUDIO_RING_UNAVAILABLE;
		reason = FPLINUX_AUDIO_REASON_DEVICE;
	}
	stream->mixer.paused = paused;
	stream->mixer.gain.target = volume_gain[level];
	atomic_store(&stream->ring->reason, reason);
	if (stream->state != state) {
		atomic_store(&stream->ring->state, state);
		stream->state = state;
	}
	if (stream->sleeping != sleeping) {
		atomic_store(&stream->ring->sleeping, sleeping);
		stream->sleeping = sleeping;
	}
	return !paused && sleeping;
}

static bool open_stream(struct fplinux_audio_streams *streams,
			unsigned int index,
			const struct fplinux_audio_message *request)
{
	struct fplinux_audio_connection *connection =
		&streams->connections[index];
	struct fplinux_audio_message answer = {
		.type = FPLINUX_AUDIO_OPENED,
		.rate = request->rate,
		.channels = request->channels,
	};
	struct fplinux_audio_stream *stream = NULL;
	struct fplinux_audio_ring *ring = MAP_FAILED;
	unsigned int first =
		connection->clock ? FPLINUX_AUDIO_CLOCK_STREAM_INDEX : 0;
	unsigned int limit = connection->clock ? FPLINUX_AUDIO_STREAMS :
						 FPLINUX_AUDIO_APP_STREAMS;
	unsigned int i, mine = 0;
	uint32_t capacity;
	size_t bytes;
	bool sent;
	int fd;

	if (request->rate < FPLINUX_AUDIO_RATE_MIN ||
	    request->rate > FPLINUX_AUDIO_RATE_MAX || !request->channels ||
	    request->channels > FPLINUX_AUDIO_CHANNELS_MAX || request->flags)
		return refuse(connection->fd, EINVAL);
	for (i = first; i < limit; ++i) {
		if (!streams->streams[i].ring) {
			if (!stream)
				stream = &streams->streams[i];
		} else if (stream_pid(streams, &streams->streams[i]) ==
			   connection->pid) {
			++mine;
		}
	}
	if (mine >= (connection->clock ? 1U : FPLINUX_AUDIO_CLIENT_STREAMS))
		return refuse(connection->fd, EMFILE);
	if (!stream)
		return refuse(connection->fd, EBUSY);
	capacity = fplinux_audio_ring_capacity(request->rate);
	bytes = FPLINUX_AUDIO_RING_BYTES(capacity, request->channels);
	/* Sealed so that the application cannot shrink it under audiod. */
	fd = memfd_create("fplinux-audio", MFD_CLOEXEC | MFD_ALLOW_SEALING);
	if (fd >= 0 && ftruncate(fd, (off_t)bytes) == 0 &&
	    fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL) ==
		    0)
		ring = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
			    0);
	if (ring == MAP_FAILED) {
		if (fd >= 0)
			close(fd);
		return refuse(connection->fd, ENOMEM);
	}
	ring->magic = FPLINUX_AUDIO_RING_MAGIC;
	ring->rate = request->rate;
	ring->channels = request->channels;
	ring->capacity = capacity;
	/* Stream IDs are nonzero and never reused on a connection. */
	if (!++connection->last_id)
		++connection->last_id;
	*stream = (struct fplinux_audio_stream){
		.ring = ring,
		.bytes = bytes,
		.connection = index,
		.id = connection->last_id,
	};
	fplinux_audio_mixer_stream_init(&stream->mixer,
					fplinux_audio_ring_frames(ring),
					capacity, request->rate,
					request->channels, streams->mixed);
	stream->mixer.step = ((uint64_t)request->rate << 32) / streams->rate;
	update(streams, stream);
	stream->mixer.gain.current = stream->mixer.gain.target;
	answer.stream = stream->id;
	sent = reply(connection->fd, &answer, fd);
	close(fd);
	if (!sent) {
		munmap(ring, bytes);
		stream->ring = NULL;
	}
	return sent;
}

static struct fplinux_audio_stream *
find_stream(struct fplinux_audio_streams *streams, unsigned int index,
	    uint32_t id)
{
	unsigned int i;

	for (i = 0; i < FPLINUX_AUDIO_STREAMS; ++i)
		if (streams->streams[i].ring &&
		    streams->streams[i].connection == index &&
		    streams->streams[i].id == id)
			return &streams->streams[i];
	return NULL;
}

/* Handles one packet; false closes the connection. */
static bool handle(struct fplinux_audio_streams *streams, unsigned int index,
		   const struct fplinux_audio_message *message)
{
	struct fplinux_audio_connection *connection =
		&streams->connections[index];
	struct fplinux_audio_process *process;
	struct fplinux_audio_stream *stream;

	if (message->error || message->reserved)
		return false;
	switch (message->type) {
	case FPLINUX_AUDIO_HELLO:
		if (connection->state != FPLINUX_AUDIO_CONNECTION_NEW ||
		    message->stream || message->rate || message->channels ||
		    message->flags)
			return false;
		if (message->version != FPLINUX_AUDIO_PROTOCOL_VERSION) {
			refuse(connection->fd, EPROTONOSUPPORT);
			return false;
		}
		process = find_process(streams, connection->pid);
		if (!process && find_gone(streams, connection->pid)) {
			refuse(connection->fd, EAGAIN);
			return false;
		}
		if (!process) {
			connection->state = FPLINUX_AUDIO_CONNECTION_WAITING;
			return true;
		}
		if (process->uid != connection->uid) {
			refuse(connection->fd, EACCES);
			return false;
		}
		connection->state = FPLINUX_AUDIO_CONNECTION_READY;
		connection->clock = process->clock;
		return true;
	case FPLINUX_AUDIO_OPEN:
		if (connection->state != FPLINUX_AUDIO_CONNECTION_READY ||
		    message->version || message->stream)
			return false;
		return open_stream(streams, index, message);
	case FPLINUX_AUDIO_CLOSE:
	case FPLINUX_AUDIO_KICK:
	case FPLINUX_AUDIO_PAUSE:
		if (connection->state != FPLINUX_AUDIO_CONNECTION_READY ||
		    message->version || message->rate || message->channels ||
		    (message->type == FPLINUX_AUDIO_PAUSE ?
			     message->flags > 1 :
			     message->flags != 0))
			return false;
		/* A KICK only wakes the poll loop, which looks at the ring. */
		stream = find_stream(streams, index, message->stream);
		if (stream && message->type == FPLINUX_AUDIO_CLOSE)
			end_stream(streams, stream);
		if (stream && message->type == FPLINUX_AUDIO_PAUSE) {
			stream->user_paused = message->flags != 0;
			if (!message->flags)
				stream->route_paused = false;
		}
		return true;
	default:
		return false;
	}
}

void fplinux_audio_streams_receive(struct fplinux_audio_streams *streams,
				   unsigned int index)
{
	struct fplinux_audio_connection *connection =
		&streams->connections[index];
	unsigned int packet;

	for (packet = 0; packet < PACKETS_PER_ROUND; ++packet) {
		struct fplinux_audio_message message;
		ssize_t length;

		/* Packets after a HELLO that waits stay queued. */
		if (connection->state == FPLINUX_AUDIO_CONNECTION_FREE ||
		    connection->state == FPLINUX_AUDIO_CONNECTION_WAITING)
			return;
		/* No control buffer: descriptors sent to audiod are discarded. */
		length = recv(connection->fd, &message, sizeof(message),
			      MSG_DONTWAIT | MSG_TRUNC);
		if (length < 0 && (errno == EAGAIN || errno == EINTR))
			return;
		if (length != (ssize_t)sizeof(message) ||
		    !handle(streams, index, &message)) {
			fplinux_audio_streams_disconnect(streams, index);
			return;
		}
	}
}

bool fplinux_audio_streams_accept(struct fplinux_audio_streams *streams, int fd,
				  pid_t pid, uid_t uid, uint64_t now_ms)
{
	unsigned int i;

	for (i = 0; i < FPLINUX_AUDIO_CONNECTIONS; ++i) {
		if (streams->connections[i].state !=
		    FPLINUX_AUDIO_CONNECTION_FREE)
			continue;
		streams->connections[i] = (struct fplinux_audio_connection){
			.state = FPLINUX_AUDIO_CONNECTION_NEW,
			.fd = fd,
			.pid = pid,
			.uid = uid,
			.deadline_ms = now_ms + FPLINUX_AUDIO_HELLO_DEADLINE_MS,
		};
		return true;
	}
	return false;
}

static void announce(struct fplinux_audio_streams *streams, pid_t pid,
		     uid_t uid, bool clock, bool background)
{
	struct fplinux_audio_process *process = find_process(streams, pid);
	pid_t *gone = find_gone(streams, pid);
	unsigned int i;

	if (clock)
		for (i = 0; i < FPLINUX_AUDIO_PROCESSES; ++i)
			if (streams->processes[i].pid &&
			    streams->processes[i].pid != pid &&
			    streams->processes[i].clock) {
				fprintf(stderr,
					"fplinux-audiod: clock helper already announced\n");
				return;
			}
	if (gone)
		*gone = 0;
	for (i = 0; !process && i < FPLINUX_AUDIO_PROCESSES; ++i)
		if (!streams->processes[i].pid)
			process = &streams->processes[i];
	if (!process) {
		printf("fplinux-audiod: too many processes; ignoring process %d\n",
		       (int)pid);
		return;
	}
	if (background)
		for (i = 0; i < FPLINUX_AUDIO_PROCESSES; ++i)
			streams->processes[i].background = false;
	/* Foreground eligibility is replayed separately by the manager. */
	*process = (struct fplinux_audio_process){
		.pid = pid,
		.uid = uid,
		.clock = clock,
		.background = background,
	};
	for (i = 0; i < FPLINUX_AUDIO_CONNECTIONS; ++i) {
		struct fplinux_audio_connection *connection =
			&streams->connections[i];

		if (connection->pid != pid ||
		    connection->state == FPLINUX_AUDIO_CONNECTION_FREE ||
		    connection->state == FPLINUX_AUDIO_CONNECTION_NEW)
			continue;
		if (connection->uid == uid &&
		    (connection->state == FPLINUX_AUDIO_CONNECTION_WAITING ||
		     connection->clock == clock)) {
			connection->state = FPLINUX_AUDIO_CONNECTION_READY;
			connection->clock = clock;
			continue;
		}
		if (connection->state == FPLINUX_AUDIO_CONNECTION_WAITING)
			refuse(connection->fd, EACCES);
		fplinux_audio_streams_disconnect(streams, i);
	}
}

static void forget(struct fplinux_audio_streams *streams, pid_t pid)
{
	struct fplinux_audio_process *process = find_process(streams, pid);
	unsigned int i;

	for (i = 0; i < FPLINUX_AUDIO_CONNECTIONS; ++i) {
		struct fplinux_audio_connection *connection =
			&streams->connections[i];

		if (connection->state == FPLINUX_AUDIO_CONNECTION_FREE ||
		    connection->pid != pid)
			continue;
		/* A HELLO not answered yet learns that the process left. */
		if (connection->state != FPLINUX_AUDIO_CONNECTION_READY)
			refuse(connection->fd, EAGAIN);
		fplinux_audio_streams_disconnect(streams, i);
	}
	if (process)
		*process = (struct fplinux_audio_process){ 0 };
	if (!find_gone(streams, pid)) {
		streams->last_gone =
			(streams->last_gone + 1) % FPLINUX_AUDIO_GONE;
		streams->gone[streams->last_gone] = pid;
	}
}

void fplinux_audio_streams_command(struct fplinux_audio_streams *streams,
				   const struct fplinux_audio_control *command)
{
	struct fplinux_audio_process *process;

	switch (command->type) {
	case FPLINUX_AUDIO_CONTROL_SYNC:
		streams->manager_ready = true;
		break;
	case FPLINUX_AUDIO_CONTROL_APP:
		announce(streams, command->app.pid, command->app.uid, false,
			 command->app.background);
		break;
	case FPLINUX_AUDIO_CONTROL_CLOCK:
		announce(streams, command->clock.pid, command->clock.uid, true,
			 false);
		break;
	case FPLINUX_AUDIO_CONTROL_GONE:
		forget(streams, command->gone);
		break;
	case FPLINUX_AUDIO_CONTROL_ELIGIBLE:
		process = find_process(streams, command->eligible.pid);
		if (process)
			process->eligible = command->eligible.enabled;
		break;
	default:
		break;
	}
}

void fplinux_audio_streams_volume(struct fplinux_audio_streams *streams,
				  enum fplinux_audio_class class,
				  unsigned int level)
{
	bool alarm = class == FPLINUX_AUDIO_ALARM;
	unsigned int i;

	if (alarm)
		streams->alarm_volume = level;
	else
		streams->media_volume = level;
	fplinux_audio_streams_refresh(streams);
	for (i = 0; i < FPLINUX_AUDIO_STREAMS; ++i) {
		struct fplinux_audio_stream *stream = &streams->streams[i];
		struct fplinux_audio_process *process;

		if (!stream->ring)
			continue;
		process = find_process(streams, stream_pid(streams, stream));
		if (!process || process->clock != alarm)
			continue;
		/* A silent or held stream has no previous level to fade from. */
		if (stream->mixer.paused ||
		    streams->output != FPLINUX_AUDIO_OUTPUT_OPEN ||
		    (!stream->mixer.last[0] && !stream->mixer.last[1]))
			stream->mixer.gain.current = stream->mixer.gain.target;
	}
}

void fplinux_audio_streams_private_output_lost(
	struct fplinux_audio_streams *streams)
{
	unsigned int i;

	for (i = 0; i < FPLINUX_AUDIO_APP_STREAMS; ++i)
		if (streams->streams[i].ring)
			streams->streams[i].route_paused = true;
	fplinux_audio_streams_refresh(streams);
}

void fplinux_audio_streams_manager(struct fplinux_audio_streams *streams,
				   bool connected, uint64_t now_ms)
{
	if (connected) {
		streams->manager_ready = false;
		streams->resync_ms = now_ms + FPLINUX_AUDIO_RESYNC_MS;
		return;
	}
	/* Streams pause: none of their processes is audible any more. */
	memset(streams->processes, 0, sizeof(streams->processes));
	streams->clock_active = false;
	streams->foreground_active = false;
	streams->manager_ready = false;
	/* The resynchronization rules apply to every process now. */
	memset(streams->gone, 0, sizeof(streams->gone));
	streams->resync_ms = 0;
}

uint64_t fplinux_audio_streams_expire(struct fplinux_audio_streams *streams,
				      uint64_t now_ms)
{
	uint64_t next = UINT64_MAX;
	unsigned int i, closed = 0;

	if (streams->resync_ms && now_ms >= streams->resync_ms) {
		streams->resync_ms = 0;
		for (i = 0; i < FPLINUX_AUDIO_CONNECTIONS; ++i) {
			struct fplinux_audio_connection *connection =
				&streams->connections[i];
			struct fplinux_audio_process *process;

			if (connection->state != FPLINUX_AUDIO_CONNECTION_READY)
				continue;
			process = find_process(streams, connection->pid);
			if (process && process->uid == connection->uid &&
			    process->clock == connection->clock)
				continue;
			fplinux_audio_streams_disconnect(streams, i);
			++closed;
		}
		if (closed)
			printf("fplinux-audiod: ended the streams of %u connections not announced again\n",
			       closed);
	}
	if (streams->resync_ms)
		next = streams->resync_ms;
	for (i = 0; i < FPLINUX_AUDIO_CONNECTIONS; ++i) {
		struct fplinux_audio_connection *connection =
			&streams->connections[i];

		if (connection->state != FPLINUX_AUDIO_CONNECTION_NEW &&
		    connection->state != FPLINUX_AUDIO_CONNECTION_WAITING)
			continue;
		if (now_ms < connection->deadline_ms) {
			if (connection->deadline_ms < next)
				next = connection->deadline_ms;
			continue;
		}
		/* A connection without a HELLO in time just closes. */
		if (connection->state == FPLINUX_AUDIO_CONNECTION_WAITING)
			refuse(connection->fd, EACCES);
		fplinux_audio_streams_disconnect(streams, i);
	}
	return next;
}

bool fplinux_audio_streams_refresh(struct fplinux_audio_streams *streams)
{
	bool wanted = false;
	unsigned int i;

	streams->clock_active = false;
	streams->foreground_active = false;
	for (i = 0; i < FPLINUX_AUDIO_STREAMS; ++i) {
		struct fplinux_audio_stream *stream = &streams->streams[i];
		struct fplinux_audio_process *process;
		uint32_t write;

		if (!stream->ring || stream->user_paused ||
		    stream->route_paused)
			continue;
		write = atomic_load(&stream->ring->write);
		if (write - stream->mixer.read > stream->mixer.mask + 1) {
			end_stream(streams, stream);
			continue;
		}
		process = find_process(streams, stream_pid(streams, stream));
		if (!streams->manager_ready || !process || !process->eligible ||
		    (!process->clock && streams->private_unavailable) ||
		    write == stream->mixer.heard)
			continue;
		if (process->clock)
			streams->clock_active = true;
		else
			streams->foreground_active = true;
	}

	for (i = 0; i < FPLINUX_AUDIO_STREAMS; ++i) {
		struct fplinux_audio_stream *stream = &streams->streams[i];
		uint32_t available;

		if (!stream->ring || !update(streams, stream))
			continue;
		/* sleeping is stored: load write again, as the protocol says. */
		available =
			atomic_load(&stream->ring->write) - stream->mixer.read;
		if (available > stream->mixer.mask + 1)
			end_stream(streams, stream);
		else if (available)
			wanted = true;
	}
	return wanted;
}

bool fplinux_audio_streams_mix(struct fplinux_audio_streams *streams,
			       int32_t *mix, uint32_t frames)
{
	bool played = false;
	unsigned int i;

	memset(mix, 0, sizeof(*mix) * FPLINUX_AUDIO_MIXER_CHANNELS * frames);
	fplinux_audio_mixer_ramp_down(mix, frames, streams->tail);
	memset(streams->tail, 0, sizeof(streams->tail));
	streams->mixed += frames;
	for (i = 0; i < FPLINUX_AUDIO_STREAMS; ++i) {
		struct fplinux_audio_stream *stream = &streams->streams[i];
		uint32_t write = 0;
		long made;

		if (!stream->ring)
			continue;
		if (!stream->mixer.paused)
			write = atomic_load_explicit(&stream->ring->write,
						     memory_order_acquire);
		made = fplinux_audio_mixer_mix(&stream->mixer, write, mix,
					       frames, streams->mixed);
		if (made < 0)
			end_stream(streams, stream);
		else if (made)
			played = true;
	}
	return played;
}

void fplinux_audio_streams_publish(struct fplinux_audio_streams *streams,
				   uint64_t played)
{
	unsigned int i;

	for (i = 0; i < FPLINUX_AUDIO_STREAMS; ++i) {
		struct fplinux_audio_stream *stream = &streams->streams[i];
		uint32_t heard;

		if (!stream->ring)
			continue;
		heard = fplinux_audio_mixer_heard(&stream->mixer, played);
		atomic_store_explicit(&stream->ring->read, stream->mixer.read,
				      memory_order_release);
		atomic_store_explicit(&stream->ring->heard, heard,
				      memory_order_release);
	}
}

void fplinux_audio_streams_stop(struct fplinux_audio_streams *streams)
{
	unsigned int i;

	for (i = 0; i < FPLINUX_AUDIO_CONNECTIONS; ++i)
		if (streams->connections[i].state !=
		    FPLINUX_AUDIO_CONNECTION_FREE)
			fplinux_audio_streams_disconnect(streams, i);
}
