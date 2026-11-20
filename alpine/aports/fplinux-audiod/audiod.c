// SPDX-License-Identifier: GPL-2.0-only
/*
 * fplinux-audiod mixes the application and clock streams that the session
 * manager allows to play, opens the card on the first audible frame and releases it
 * after a while of silence, so that FM radio and root tools find it free.
 * One thread and one poll loop; the protocol is fplinux-audio-protocol.h.
 */
#define _GNU_SOURCE

#include "audio-device.h"
#include "audio-mixer.h"
#include "audio-streams.h"
#include "fplinux-audio-protocol.h"
#include "fplinux-cli.h"

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define DEVICE "default"
/* 10 ms periods and a 30 ms buffer; sizes move in 10 ms steps. */
#define PERIOD_FRAMES 480U
#define BUFFER_FRAMES 1440U
#define FRAME_STEP 480U
#define PERIOD_FRAMES_MAX 7680U
/*
 * The card is released after this long with nothing audible, paused
 * streams included: FM radio plays only while nothing else plays.
 */
#define IDLE_MS 3000U
/* While something wants to play, a busy card is tried again this often. */
#define RETRY_MS 1000U
#define PRIORITY 5
/* An underrun burst ends after a second of output without one. */
#define NEVER UINT64_MAX

enum { OPTION_DIR, OPTION_DEVICE, OPTION_PERIOD, OPTION_BUFFER, OPTION_STATUS };

enum {
	POLL_SIGNAL,
	POLL_TIMER,
	POLL_CONTROL,
	POLL_MANAGER,
	POLL_STREAM,
	POLL_STATUS,
	POLL_RADIO_LISTEN,
	POLL_RADIO,
	POLL_CONTROLS,
	POLL_CONNECTIONS,
	POLL_DEVICE = POLL_CONNECTIONS + FPLINUX_AUDIO_CONNECTIONS,
	POLL_COUNT = POLL_DEVICE + FPLINUX_AUDIO_DEVICE_FDS,
};

struct options {
	const char *directory;
	unsigned int period;
	unsigned int buffer;
};

struct audio_service {
	char control_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
	char stream_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
	char status_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
	char radio_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
	int signals;
	int timer;
	int control;
	int manager;
	int stream;
	int status;
	int radio_listen;
	int radio;
	bool radio_requested;
	bool radio_announced;
	uint64_t timer_ms;
	struct fplinux_audio_device device;
	struct fplinux_audio_streams streams;
	uint64_t clipped_samples;
	int device_error;
	int32_t *mix;
	int16_t *output;
	/* Frames at the end of output not yet written to the card. */
	uint32_t pending;
	uint64_t audible_ms;
	uint64_t retry_ms;
	uint64_t clean;
	bool underrun;
};

static void close_device(struct audio_service *service);

static uint64_t now_ms(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
}

static int show_status(const char *directory)
{
	struct sockaddr_un address = { .sun_family = AF_UNIX };
	struct pollfd ready;
	char state[512];
	uint64_t deadline = now_ms() + 500;
	ssize_t length;
	int fd, result, error;

	snprintf(address.sun_path, sizeof(address.sun_path), "%s%s", directory,
		 strrchr(FPLINUX_AUDIO_STATUS_SOCKET, '/'));
	fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (fd < 0)
		goto fail;
	if (connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0)
		goto close_failed;
	ready = (struct pollfd){ .fd = fd, .events = POLLIN };
	for (;;) {
		uint64_t now = now_ms();

		if (now >= deadline) {
			errno = ETIMEDOUT;
			goto close_failed;
		}
		result = poll(&ready, 1, (int)(deadline - now));
		if (result < 0 && errno == EINTR)
			continue;
		if (result <= 0) {
			if (!result)
				errno = ETIMEDOUT;
			goto close_failed;
		}
		break;
	}
	length = recv(fd, state, sizeof(state), MSG_DONTWAIT | MSG_TRUNC);
	if (length < 6 || (size_t)length >= sizeof(state) ||
	    memcmp(state, "STATE ", 6) || memchr(state, '\0', (size_t)length)) {
		errno = EPROTO;
		goto close_failed;
	}
	close(fd);
	if (fwrite(state, 1, (size_t)length, stdout) != (size_t)length ||
	    putchar('\n') == EOF)
		goto fail;
	return 0;

close_failed:
	error = errno;
	close(fd);
	errno = error;
fail:
	fprintf(stderr, "fplinux-audiod: status: %s\n", strerror(errno));
	return 1;
}

static const char *parse_option(size_t option, const char *value, void *data)
{
	struct options *options = data;

	if (option == OPTION_DIR && strlen(value) > 64)
		return "--dir must be at most 64 bytes";
	if (option == OPTION_PERIOD &&
	    (!fplinux_cli_unsigned(value, FRAME_STEP, PERIOD_FRAMES_MAX,
				   &options->period) ||
	     options->period % FRAME_STEP))
		return "--period-frames must be a multiple of 480 up to 7680";
	if (option == OPTION_BUFFER &&
	    (!fplinux_cli_unsigned(value, 2 * FRAME_STEP, UINT_MAX,
				   &options->buffer) ||
	     options->buffer % FRAME_STEP))
		return "--buffer-frames must be a multiple of 480";
	return NULL;
}

static int listen_at(const char *path, mode_t mode)
{
	struct sockaddr_un address = { .sun_family = AF_UNIX };
	struct stat current;
	int fd, error;

	if (lstat(path, &current) == 0) {
		if (!S_ISSOCK(current.st_mode)) {
			errno = EEXIST;
			return -1;
		}
		if (unlink(path) < 0)
			return -1;
	} else if (errno != ENOENT) {
		return -1;
	}
	fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (fd < 0)
		return -1;
	strcpy(address.sun_path, path);
	if (bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0 ||
	    chmod(path, mode) < 0 ||
	    listen(fd, FPLINUX_AUDIO_CONNECTIONS) < 0) {
		error = errno;
		close(fd);
		errno = error;
		return -1;
	}
	return fd;
}

static bool peer(int fd, struct ucred *credentials)
{
	socklen_t length = sizeof(*credentials);

	return !getsockopt(fd, SOL_SOCKET, SO_PEERCRED, credentials, &length) &&
	       length == sizeof(*credentials);
}

static void drop_manager(struct audio_service *service)
{
	close(service->manager);
	service->manager = -1;
	fplinux_audio_streams_manager(&service->streams, false, 0);
	if (service->device.pcm)
		close_device(service);
	printf("fplinux-audiod: manager disconnected; streams paused\n");
}

/* A second peer cannot replace the trusted manager. */
static void accept_manager(struct audio_service *service, uint64_t now)
{
	struct ucred credentials;
	int fd;

	while ((fd = accept4(service->control, NULL, NULL,
			     SOCK_CLOEXEC | SOCK_NONBLOCK)) >= 0) {
		if (!peer(fd, &credentials) || credentials.uid != geteuid()) {
			close(fd);
			continue;
		}
		if (service->manager >= 0) {
			close(fd);
			continue;
		}
		service->manager = fd;
		fplinux_audio_streams_manager(&service->streams, true, now);
		printf("fplinux-audiod: manager connected\n");
	}
}

static bool parse_volume(const char *value, bool *alarm, unsigned int *level)
{
	if (!strncmp(value, "MEDIA ", 6)) {
		*alarm = false;
	} else if (!strncmp(value, "ALARM ", 6)) {
		*alarm = true;
	} else {
		return false;
	}
	value += 6;
	if (*value < '0' || *value > '9' || (*value == '0' && value[1]))
		return false;
	return fplinux_cli_unsigned(value, 0, FPLINUX_AUDIO_VOLUME_MAX, level);
}

static void read_manager(struct audio_service *service)
{
	char command[FPLINUX_AUDIO_CONTROL_BYTES];

	for (;;) {
		ssize_t length = recv(service->manager, command,
				      sizeof(command),
				      MSG_DONTWAIT | MSG_TRUNC);

		if (length < 0 && (errno == EAGAIN || errno == EINTR))
			return;
		if (length <= 0) {
			drop_manager(service);
			return;
		}
		if ((size_t)length >= sizeof(command) ||
		    memchr(command, '\0', (size_t)length)) {
			fprintf(stderr,
				"fplinux-audiod: ignoring a malformed control command\n");
			continue;
		}
		command[length] = '\0';
		if (!strncmp(command, FPLINUX_AUDIO_CONTROL_VOLUME " ",
			     sizeof(FPLINUX_AUDIO_CONTROL_VOLUME))) {
			const char *value =
				command + sizeof(FPLINUX_AUDIO_CONTROL_VOLUME);
			unsigned int level;
			bool alarm;
			ssize_t sent;

			if (!parse_volume(value, &alarm, &level))
				continue;
			if (!alarm && service->device.fm_enabled) {
				int error = fplinux_audio_device_fm(
					&service->device, true, level);

				if (error < 0) {
					service->device_error = error;
					drop_manager(service);
					return;
				}
			}
			fplinux_audio_streams_volume(&service->streams, alarm,
						     level);
			do {
				sent = send(service->manager, command,
					    (size_t)length,
					    MSG_DONTWAIT | MSG_NOSIGNAL);
			} while (sent < 0 && errno == EINTR);
			if (sent != length) {
				drop_manager(service);
				return;
			}
			continue;
		}
		if (!strncmp(command, "OUTPUT ", 7)) {
			const char *names[] = { "AUTO", "SPEAKER",
						"HEADPHONES" };
			unsigned int route;
			char reply[48];
			int error;

			for (route = 0;
			     route < sizeof(names) / sizeof(names[0]); ++route)
				if (!strcmp(command + 7, names[route]))
					break;
			if (route == sizeof(names) / sizeof(names[0]))
				continue;
			error = fplinux_audio_device_route(
				&service->device,
				(enum fplinux_audio_route)route);
			service->device_error = error;
			if (error < 0)
				snprintf(reply, sizeof(reply),
					 "OUTPUT ERROR %d", -error);
			else
				snprintf(reply, sizeof(reply), "OUTPUT %s",
					 names[route]);
			if (send(service->manager, reply, strlen(reply),
				 MSG_DONTWAIT | MSG_NOSIGNAL) !=
			    (ssize_t)strlen(reply)) {
				drop_manager(service);
				return;
			}
			continue;
		}
		fplinux_audio_streams_command(&service->streams, command);
	}
}

enum playback_activity {
	ACTIVITY_NONE,
	ACTIVITY_PAUSED,
	ACTIVITY_HELD,
	ACTIVITY_UNAVAILABLE,
	ACTIVITY_PLAYING,
};

static enum playback_activity
process_activity(const struct audio_service *service,
		 const struct fplinux_audio_process *process)
{
	enum playback_activity activity = ACTIVITY_NONE;

	for (unsigned int i = 0; i < FPLINUX_AUDIO_APP_STREAMS; ++i) {
		const struct fplinux_audio_stream *stream =
			&service->streams.streams[i];
		const struct fplinux_audio_connection *connection;
		enum playback_activity current;

		if (!stream->ring)
			continue;
		connection = &service->streams.connections[stream->connection];
		if (connection->pid != process->pid ||
		    connection->uid != process->uid || connection->clock)
			continue;
		if (stream->user_paused)
			current = ACTIVITY_PAUSED;
		else if (atomic_load(&stream->ring->write) ==
			 stream->mixer.heard)
			continue;
		else if (stream->route_paused ||
			 service->streams.private_unavailable ||
			 stream->state == FPLINUX_AUDIO_RING_UNAVAILABLE)
			current = ACTIVITY_UNAVAILABLE;
		else if (stream->mixer.paused)
			current = ACTIVITY_HELD;
		else if (service->streams.output == FPLINUX_AUDIO_OUTPUT_OPEN)
			current = ACTIVITY_PLAYING;
		else
			continue;
		if (current > activity)
			activity = current;
	}
	return activity;
}

static void announce_activity(struct audio_service *service)
{
	static const char *const names[] = {
		"none", "paused", "held", "unavailable", "playing",
	};

	if (service->manager < 0 || !service->streams.manager_ready)
		return;
	for (unsigned int i = 0; i < FPLINUX_AUDIO_PROCESSES; ++i) {
		struct fplinux_audio_process *process =
			&service->streams.processes[i];
		enum playback_activity activity;
		char packet[FPLINUX_AUDIO_CONTROL_BYTES];
		ssize_t sent;
		size_t length;

		if (!process->pid || process->clock)
			continue;
		activity = process_activity(service, process);
		if (process->activity_known && process->activity == activity)
			continue;
		snprintf(packet, sizeof(packet),
			 FPLINUX_AUDIO_CONTROL_ACTIVITY " %ld %lu %s",
			 (long)process->pid, (unsigned long)process->uid,
			 names[activity]);
		length = strlen(packet);
		do {
			sent = send(service->manager, packet, length,
				    MSG_DONTWAIT | MSG_NOSIGNAL);
		} while (sent < 0 && errno == EINTR);
		if (sent != (ssize_t)length) {
			drop_manager(service);
			return;
		}
		process->activity = activity;
		process->activity_known = true;
	}
}

static void accept_streams(struct audio_service *service, uint64_t now)
{
	struct ucred credentials;
	int fd;

	while ((fd = accept4(service->stream, NULL, NULL,
			     SOCK_CLOEXEC | SOCK_NONBLOCK)) >= 0)
		if (!peer(fd, &credentials) ||
		    !fplinux_audio_streams_accept(&service->streams, fd,
						  credentials.pid,
						  credentials.uid, now))
			close(fd);
}

static void diagnostic(struct audio_service *service)
{
	struct fplinux_audio_device *device = &service->device;
	char state[512];
	int fd;

	snprintf(
		state, sizeof(state),
		"STATE media=%u alarm=%u rate=%u output=%u applied=%u speaker=%u headphones=%u jack=%u plugged=%u fm=%u fm_active=%u clock=%u foreground=%u pcm=%u clipped=%llu error=%d",
		service->streams.media_volume, service->streams.alarm_volume,
		device->rate, device->route, device->applied_route,
		device->speaker_id != 0 || device->fixed_speaker,
		device->headphone_id != 0, device->jack_id != 0,
		device->jack_present, fplinux_audio_device_fm_available(device),
		device->fm_enabled, service->streams.clock_active,
		service->streams.foreground_active, device->pcm != NULL,
		(unsigned long long)service->clipped_samples,
		service->device_error < 0 ? -service->device_error : 0);
	while ((fd = accept4(service->status, NULL, NULL,
			     SOCK_CLOEXEC | SOCK_NONBLOCK)) >= 0) {
		/* This endpoint never reads commands and never changes policy. */
		send(fd, state, strlen(state), MSG_DONTWAIT | MSG_NOSIGNAL);
		close(fd);
	}
}

static void drop_radio(struct audio_service *service)
{
	int error;

	if (service->radio >= 0)
		close(service->radio);
	service->radio = -1;
	service->radio_requested = false;
	service->radio_announced = false;
	service->streams.fm_active = false;
	error = fplinux_audio_device_radio_close(&service->device);
	if (error < 0)
		service->device_error = error;
}

static void accept_radio(struct audio_service *service)
{
	struct ucred credentials;
	int fd;

	while ((fd = accept4(service->radio_listen, NULL, NULL,
			     SOCK_CLOEXEC | SOCK_NONBLOCK)) >= 0) {
		if (!peer(fd, &credentials) || credentials.uid != geteuid() ||
		    service->radio >= 0) {
			close(fd);
			continue;
		}
		service->radio = fd;
	}
}

static bool private_output_missing(const struct audio_service *service)
{
	bool headphones =
		service->device.route == FPLINUX_AUDIO_ROUTE_HEADPHONES ||
		(service->device.route == FPLINUX_AUDIO_ROUTE_AUTO &&
		 !service->device.speaker_id && !service->device.fixed_speaker);

	return headphones && service->device.jack_id &&
	       !service->device.jack_present;
}

static void read_radio(struct audio_service *service)
{
	char command[32];
	unsigned int frequency;
	ssize_t length = recv(service->radio, command, sizeof(command),
			      MSG_DONTWAIT | MSG_TRUNC);
	int error;

	if (length < 0 && (errno == EAGAIN || errno == EINTR))
		return;
	if (length <= 0 || (size_t)length >= sizeof(command) ||
	    memchr(command, '\0', (size_t)length) || service->radio_announced) {
		drop_radio(service);
		return;
	}
	command[length] = '\0';
	if (!strcmp(command, "SCAN")) {
		service->radio_announced = true;
		if (send(service->radio, "READY", 5,
			 MSG_DONTWAIT | MSG_NOSIGNAL) != 5)
			drop_radio(service);
		return;
	}
	if (strncmp(command, "PLAY ", 5) ||
	    !fplinux_cli_unsigned(command + 5, 875, 1080, &frequency)) {
		drop_radio(service);
		return;
	}
	error = private_output_missing(service) ?
			-ENODEV :
			fplinux_audio_device_tune(&service->device, frequency);
	if (error < 0) {
		char reply[32];

		snprintf(reply, sizeof(reply), "ERROR %d", -error);
		send(service->radio, reply, strlen(reply),
		     MSG_DONTWAIT | MSG_NOSIGNAL);
		drop_radio(service);
		return;
	}
	service->radio_requested = true;
}

static void close_device(struct audio_service *service)
{
	fplinux_audio_device_close(&service->device);
	/* What the card held and what was not written yet is dropped. */
	service->device.written = service->streams.mixed;
	service->pending = 0;
	memset(service->streams.tail, 0, sizeof(service->streams.tail));
	fplinux_audio_streams_publish(&service->streams,
				      service->device.written);
	service->streams.output = FPLINUX_AUDIO_OUTPUT_CLOSED;
}

static void make_unavailable(struct audio_service *service, uint64_t now)
{
	service->streams.output = FPLINUX_AUDIO_OUTPUT_UNAVAILABLE;
	service->retry_ms = now + RETRY_MS;
}

/* Prepares the card after an underrun; false when it failed and closed. */
static bool recover(struct audio_service *service, long error, uint64_t now)
{
	if (error == -EPIPE || error == -ESTRPIPE) {
		if (!service->underrun)
			fprintf(stderr,
				"fplinux-audiod: card %s underrun; prepared again\n",
				service->device.name);
		service->underrun = true;
		service->clean = 0;
		error = fplinux_audio_device_recover(&service->device);
		if (!error)
			return true;
	}
	fprintf(stderr, "fplinux-audiod: card %s failed: %s; streams wait\n",
		service->device.name, fplinux_audio_device_error((int)error));
	close_device(service);
	make_unavailable(service, now);
	return false;
}

/* Mixes and writes whole periods while the card has room for them. */
static void fill(struct audio_service *service, uint64_t now)
{
	struct fplinux_audio_device *device = &service->device;
	long result;

	for (;;) {
		if (service->pending) {
			result = fplinux_audio_device_write(
				device,
				service->output +
					2 * (device->period - service->pending),
				service->pending);
			if (result == -EAGAIN)
				break;
			if (result < 0) {
				if (!recover(service, result, now))
					return;
				break;
			}
			service->pending -= (uint32_t)result;
			service->clean += (uint64_t)result;
			if (service->clean >= device->rate)
				service->underrun = false;
			if (service->pending)
				break;
		}
		result = fplinux_audio_device_avail(device);
		if (result < 0) {
			if (!recover(service, result, now))
				return;
			break;
		}
		if (result < (long)device->period)
			break;
		if (fplinux_audio_streams_mix(&service->streams, service->mix,
					      device->period))
			service->audible_ms = now;
		for (uint32_t i = 0; i < device->period * 2; ++i)
			if (service->mix[i] > 32767 || service->mix[i] < -32768)
				++service->clipped_samples;
		fplinux_audio_mixer_output(service->mix, service->output,
					   device->period);
		service->pending = device->period;
	}
	fplinux_audio_streams_publish(&service->streams,
				      fplinux_audio_device_played(device));
}

static void open_device(struct audio_service *service, uint64_t now)
{
	int result = fplinux_audio_device_open(&service->device);

	if (result < 0) {
		if (service->streams.output != FPLINUX_AUDIO_OUTPUT_UNAVAILABLE)
			printf("fplinux-audiod: card %s unavailable: %s; streams wait\n",
			       service->device.name,
			       fplinux_audio_device_error(result));
		make_unavailable(service, now);
		return;
	}
	printf("fplinux-audiod: card %s open\n", service->device.name);
	service->streams.output = FPLINUX_AUDIO_OUTPUT_OPEN;
	service->audible_ms = now;
	service->underrun = false;
}

/*
 * The policy first, so that eligibility changes precede the next period;
 * then the card: write, release after silence, open for audible frames.
 * Returns whether a stream waits for the card.
 */
static bool advance(struct audio_service *service, uint64_t now,
		    bool card_ready)
{
	enum fplinux_audio_output *output = &service->streams.output;
	bool wanted;
	int error;

	service->streams.private_unavailable = private_output_missing(service);
	if (service->streams.private_unavailable && service->radio_requested)
		drop_radio(service);
	fplinux_audio_streams_refresh(&service->streams);
	if (service->streams.private_unavailable &&
	    !service->device.alarm_active && service->device.pcm)
		close_device(service);
	error = fplinux_audio_device_alarm(&service->device,
					   service->streams.clock_active);
	if (error < 0) {
		service->device_error = error;
		if (service->device.pcm)
			close_device(service);
		make_unavailable(service, now);
		return fplinux_audio_streams_refresh(&service->streams);
	}
	if (service->radio_requested) {
		bool enable = !service->streams.clock_active;

		if (enable && service->device.pcm)
			close_device(service);
		error = fplinux_audio_device_fm(&service->device, enable,
						service->streams.media_volume);
		if (error < 0) {
			char reply[32];

			service->device_error = error;
			snprintf(reply, sizeof(reply), "ERROR %d", -error);
			send(service->radio, reply, strlen(reply),
			     MSG_DONTWAIT | MSG_NOSIGNAL);
			drop_radio(service);
		} else {
			service->streams.fm_active = enable;
			if (!service->radio_announced) {
				service->radio_announced = true;
				if (send(service->radio, "READY", 5,
					 MSG_DONTWAIT | MSG_NOSIGNAL) != 5)
					drop_radio(service);
			}
		}
		fplinux_audio_streams_refresh(&service->streams);
	}
	if (*output == FPLINUX_AUDIO_OUTPUT_OPEN && card_ready)
		fill(service, now);
	if (*output == FPLINUX_AUDIO_OUTPUT_OPEN &&
	    now - service->audible_ms >= IDLE_MS) {
		close_device(service);
		printf("fplinux-audiod: card %s closed after %u ms of silence\n",
		       service->device.name, IDLE_MS);
	}
	/* Rings sleep while the card is not open; look at them again. */
	wanted = fplinux_audio_streams_refresh(&service->streams);
	if (!wanted || *output == FPLINUX_AUDIO_OUTPUT_OPEN ||
	    (*output == FPLINUX_AUDIO_OUTPUT_UNAVAILABLE &&
	     now < service->retry_ms))
		return wanted;
	open_device(service, now);
	wanted = fplinux_audio_streams_refresh(&service->streams);
	if (*output == FPLINUX_AUDIO_OUTPUT_OPEN)
		fill(service, now);
	return wanted;
}

static void arm(struct audio_service *service, uint64_t deadline)
{
	struct itimerspec value = { 0 };

	if (deadline == service->timer_ms)
		return;
	service->timer_ms = deadline;
	if (deadline != NEVER) {
		value.it_value.tv_sec = (time_t)(deadline / 1000);
		value.it_value.tv_nsec = (long)(deadline % 1000) * 1000000;
	}
	timerfd_settime(service->timer, TFD_TIMER_ABSTIME, &value, NULL);
}

static void watch(struct audio_service *service, struct pollfd *fds)
{
	unsigned int i;

	fds[POLL_SIGNAL] = (struct pollfd){ service->signals, POLLIN, 0 };
	fds[POLL_TIMER] = (struct pollfd){ service->timer, POLLIN, 0 };
	fds[POLL_CONTROL] = (struct pollfd){ service->control, POLLIN, 0 };
	fds[POLL_MANAGER] = (struct pollfd){ service->manager, POLLIN, 0 };
	fds[POLL_STREAM] = (struct pollfd){ service->stream, POLLIN, 0 };
	fds[POLL_STATUS] = (struct pollfd){ service->status, POLLIN, 0 };
	fds[POLL_RADIO_LISTEN] =
		(struct pollfd){ service->radio_listen, POLLIN, 0 };
	fds[POLL_RADIO] = (struct pollfd){ service->radio, POLLIN, 0 };
	fds[POLL_CONTROLS] =
		(struct pollfd){ service->device.control_fd, POLLIN, 0 };
	for (i = 0; i < FPLINUX_AUDIO_CONNECTIONS; ++i) {
		const struct fplinux_audio_connection *connection =
			&service->streams.connections[i];
		struct pollfd *watched = &fds[POLL_CONNECTIONS + i];

		*watched = (struct pollfd){ .fd = -1 };
		if (connection->state == FPLINUX_AUDIO_CONNECTION_FREE)
			continue;
		watched->fd = connection->fd;
		/* The packets behind a HELLO that waits stay queued. */
		if (connection->state != FPLINUX_AUDIO_CONNECTION_WAITING)
			watched->events = POLLIN;
	}
	for (i = 0; i < FPLINUX_AUDIO_DEVICE_FDS; ++i)
		fds[POLL_DEVICE + i] = (struct pollfd){ .fd = -1 };
	for (i = 0; i < service->device.fd_count; ++i)
		fds[POLL_DEVICE + i] = service->device.fds[i];
}

static int serve(struct audio_service *service)
{
	struct pollfd fds[POLL_COUNT];

	for (;;) {
		bool card_ready = false, wanted;
		uint64_t now, next, expirations;
		unsigned int i;

		watch(service, fds);
		if (poll(fds, POLL_COUNT, -1) < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (fds[POLL_SIGNAL].revents)
			return 0;
		now = now_ms();
		if (fds[POLL_TIMER].revents &&
		    read(service->timer, &expirations, sizeof(expirations)) >=
			    0)
			service->timer_ms = NEVER;
		/* The manager before the applications, as the protocol says. */
		if (service->manager >= 0 &&
		    (fds[POLL_MANAGER].revents & POLLIN))
			read_manager(service);
		else if (service->manager >= 0 && fds[POLL_MANAGER].revents)
			drop_manager(service);
		if (fds[POLL_CONTROL].revents & POLLIN)
			accept_manager(service, now);
		if (fds[POLL_STATUS].revents & POLLIN)
			diagnostic(service);
		if (fds[POLL_RADIO].revents) {
			if (fds[POLL_RADIO].revents & POLLIN)
				read_radio(service);
			else
				drop_radio(service);
		}
		if (fds[POLL_RADIO_LISTEN].revents & POLLIN)
			accept_radio(service);
		if (fds[POLL_CONTROLS].revents) {
			bool private_output = service->device.applied_route ==
					      FPLINUX_AUDIO_ROUTE_HEADPHONES;
			int changed =
				fplinux_audio_device_events(&service->device);

			service->device_error = changed < 0 ? changed : 0;
			if (changed > 0) {
				if ((changed &
				     FPLINUX_AUDIO_DEVICE_PRIVATE_LOST) &&
				    private_output) {
					fplinux_audio_streams_private_output_lost(
						&service->streams);
					if (service->device.pcm)
						close_device(service);
					if (service->radio_requested)
						drop_radio(service);
				}
				if (service->device.route ==
					    FPLINUX_AUDIO_ROUTE_AUTO &&
				    !service->device.fm_enabled)
					service->device_error =
						fplinux_audio_device_route(
							&service->device,
							service->device.route);
			}
			if (service->device_error < 0)
				fprintf(stderr,
					"fplinux-audiod: route update: %s\n",
					fplinux_audio_device_error(
						service->device_error));
		}
		for (i = 0; i < FPLINUX_AUDIO_CONNECTIONS; ++i) {
			short revents = fds[POLL_CONNECTIONS + i].revents;

			if (!revents || service->streams.connections[i].state ==
						FPLINUX_AUDIO_CONNECTION_FREE)
				continue;
			if (revents & POLLIN)
				fplinux_audio_streams_receive(&service->streams,
							      i);
			else
				fplinux_audio_streams_disconnect(
					&service->streams, i);
		}
		if (fds[POLL_STREAM].revents & POLLIN)
			accept_streams(service, now);
		for (i = 0; i < service->device.fd_count; ++i)
			if (fds[POLL_DEVICE + i].revents)
				card_ready = true;
		if (card_ready)
			card_ready = fplinux_audio_device_ready(
				&service->device, &fds[POLL_DEVICE]);
		next = fplinux_audio_streams_expire(&service->streams, now);
		wanted = advance(service, now, card_ready);
		announce_activity(service);
		if (service->streams.output == FPLINUX_AUDIO_OUTPUT_OPEN &&
		    service->audible_ms + IDLE_MS < next)
			next = service->audible_ms + IDLE_MS;
		if (service->streams.output ==
			    FPLINUX_AUDIO_OUTPUT_UNAVAILABLE &&
		    wanted && service->retry_ms < next)
			next = service->retry_ms;
		arm(service, next);
	}
}

/* musl's sched_setscheduler() always fails; the thread call works. */
static void raise_priority(void)
{
	struct sched_param parameter = { .sched_priority = PRIORITY };
	int error =
		pthread_setschedparam(pthread_self(), SCHED_FIFO, &parameter);

	if (error)
		fprintf(stderr,
			"fplinux-audiod: real-time priority unavailable: %s; running at normal priority\n",
			strerror(error));
}

static int start(struct audio_service *service, const char *directory)
{
	const char *control = strrchr(FPLINUX_AUDIO_CONTROL_SOCKET, '/');
	const char *stream = strrchr(FPLINUX_AUDIO_STREAM_SOCKET, '/');
	const char *status = strrchr(FPLINUX_AUDIO_STATUS_SOCKET, '/');
	const char *radio = strrchr(FPLINUX_AUDIO_RADIO_SOCKET, '/');
	sigset_t stops;

	snprintf(service->control_path, sizeof(service->control_path), "%s%s",
		 directory, control);
	snprintf(service->stream_path, sizeof(service->stream_path), "%s%s",
		 directory, stream);
	snprintf(service->status_path, sizeof(service->status_path), "%s%s",
		 directory, status);
	snprintf(service->radio_path, sizeof(service->radio_path), "%s%s",
		 directory, radio);
	sigemptyset(&stops);
	sigaddset(&stops, SIGTERM);
	sigaddset(&stops, SIGINT);
	if (sigprocmask(SIG_BLOCK, &stops, NULL) < 0)
		return -1;
	service->signals = signalfd(-1, &stops, SFD_CLOEXEC | SFD_NONBLOCK);
	service->timer =
		timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
	service->mix = calloc((size_t)service->device.period * 2,
			      sizeof(*service->mix));
	service->output = calloc((size_t)service->device.period * 2,
				 sizeof(*service->output));
	if (service->signals < 0 || service->timer < 0 || !service->mix ||
	    !service->output)
		return -1;
	service->control = listen_at(service->control_path, 0600);
	if (service->control < 0)
		return -1;
	service->stream = listen_at(service->stream_path, 0660);
	service->status = listen_at(service->status_path, 0660);
	service->radio_listen = listen_at(service->radio_path, 0600);
	return service->stream < 0 || service->status < 0 ||
			       service->radio_listen < 0 ?
		       -1 :
		       0;
}

int main(int argc, char **argv)
{
	struct fplinux_cli_option arguments[] = {
		[OPTION_DIR] = { .name = "dir",
				 .metavar = "DIR",
				 .help = "Create the sockets in DIR." },
		[OPTION_DEVICE] = { .name = "device",
				    .metavar = "PCM",
				    .help = "Play on this ALSA device." },
		[OPTION_PERIOD] = { .name = "period-frames",
				    .metavar = "N",
				    .help = "Write N frames per period." },
		[OPTION_BUFFER] = { .name = "buffer-frames",
				    .metavar = "N",
				    .help = "Keep N frames in the card." },
		[OPTION_STATUS] = { .name = "status",
				    .help = "Print the running service's read-only state." },
	};
	struct options options = {
		.directory = FPLINUX_AUDIO_DIR,
		.period = PERIOD_FRAMES,
		.buffer = BUFFER_FRAMES,
	};
	struct fplinux_cli cli = {
		.program = "fplinux-audiod",
		.description = "Mix the applications' sound and play it.",
		.options = arguments,
		.option_count = sizeof(arguments) / sizeof(arguments[0]),
		.parse_option = parse_option,
		.data = &options,
	};
	struct audio_service service = {
		.signals = -1,
		.timer = -1,
		.control = -1,
		.manager = -1,
		.stream = -1,
		.status = -1,
		.radio_listen = -1,
		.radio = -1,
		.timer_ms = NEVER,
	};
	enum fplinux_cli_result parsed = fplinux_cli_parse(&cli, argc, argv);
	int result = 1, device_error;

	if (parsed != FPLINUX_CLI_READY)
		return parsed;
	if (arguments[OPTION_DIR].count)
		options.directory = arguments[OPTION_DIR].value;
	if (arguments[OPTION_STATUS].count) {
		if (arguments[OPTION_DEVICE].count ||
		    arguments[OPTION_PERIOD].count ||
		    arguments[OPTION_BUFFER].count)
			return fplinux_cli_error(&cli,
						 "--status accepts only --dir");
		return show_status(options.directory);
	}
	if (options.buffer < 2 * options.period ||
	    options.buffer > (FPLINUX_AUDIO_MIXER_HISTORY - 2) * options.period)
		return fplinux_cli_error(
			&cli, "--buffer-frames must hold 2 to 14 periods");
	service.device = (struct fplinux_audio_device){
		.name = arguments[OPTION_DEVICE].count ?
				arguments[OPTION_DEVICE].value :
				DEVICE,
		.period = options.period,
		.buffer = options.buffer,
	};
	/* The service logger reads standard output through a pipe. */
	setvbuf(stdout, NULL, _IOLBF, 0);
	signal(SIGPIPE, SIG_IGN);
	umask(077);
	fplinux_audio_streams_init(&service.streams);
	device_error = fplinux_audio_device_init(&service.device);
	if (device_error < 0) {
		fprintf(stderr,
			"fplinux-audiod: initialize the sound card: %s\n",
			fplinux_audio_device_error(device_error));
		goto done;
	}
	service.streams.rate = service.device.rate;
	if (service.device.rate == 44100) {
		service.device.period = options.period / 480 * 441;
		service.device.buffer = options.buffer / 480 * 441;
	}
	raise_priority();
	if (start(&service, options.directory) < 0) {
		fprintf(stderr, "fplinux-audiod: startup in %s: %s\n",
			options.directory, strerror(errno));
		goto done;
	}
	printf("fplinux-audiod: serving %s for %s, %u frames per period, %u buffered\n",
	       options.directory, service.device.name, service.device.period,
	       service.device.buffer);
	result = serve(&service) < 0;
	if (result)
		fprintf(stderr, "fplinux-audiod: poll: %s\n", strerror(errno));

done:
	/* Ending the streams hangs up on every application. */
	fplinux_audio_streams_stop(&service.streams);
	drop_radio(&service);
	fplinux_audio_device_cleanup(&service.device);
	if (service.manager >= 0)
		close(service.manager);
	if (service.timer >= 0)
		close(service.timer);
	if (service.signals >= 0)
		close(service.signals);
	if (service.control >= 0) {
		close(service.control);
		unlink(service.control_path);
	}
	if (service.stream >= 0) {
		close(service.stream);
		unlink(service.stream_path);
	}
	if (service.status >= 0) {
		close(service.status);
		unlink(service.status_path);
	}
	if (service.radio_listen >= 0) {
		close(service.radio_listen);
		unlink(service.radio_path);
	}
	free(service.mix);
	free(service.output);
	return result;
}
