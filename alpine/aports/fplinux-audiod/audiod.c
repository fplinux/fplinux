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
#define CLEAN_FRAMES FPLINUX_AUDIO_MIXER_RATE
#define NEVER UINT64_MAX

enum { OPTION_DIR, OPTION_DEVICE, OPTION_PERIOD, OPTION_BUFFER };

enum {
	POLL_SIGNAL,
	POLL_TIMER,
	POLL_CONTROL,
	POLL_MANAGER,
	POLL_STREAM,
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
	int signals;
	int timer;
	int control;
	int manager;
	int stream;
	uint64_t timer_ms;
	struct fplinux_audio_device device;
	struct fplinux_audio_streams streams;
	int32_t *mix;
	int16_t *output;
	/* Frames at the end of output not yet written to the card. */
	uint32_t pending;
	uint64_t audible_ms;
	uint64_t retry_ms;
	uint64_t clean;
	bool underrun;
};

static uint64_t now_ms(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
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
	printf("fplinux-audiod: manager disconnected; streams paused\n");
}

/* One manager at a time, with audiod's own user; a new one replaces it. */
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
		if (service->manager >= 0)
			drop_manager(service);
		service->manager = fd;
		fplinux_audio_streams_manager(&service->streams, true, now);
		printf("fplinux-audiod: manager connected\n");
	}
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
		fplinux_audio_streams_command(&service->streams, command);
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
			if (service->clean >= CLEAN_FRAMES)
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
 * The policy first, so that AUDIBLE 0 takes effect before the next period;
 * then the card: write, release after silence, open for audible frames.
 * Returns whether a stream waits for the card.
 */
static bool advance(struct audio_service *service, uint64_t now,
		    bool card_ready)
{
	enum fplinux_audio_output *output = &service->streams.output;
	bool wanted;

	fplinux_audio_streams_refresh(&service->streams);
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
	sigset_t stops;

	snprintf(service->control_path, sizeof(service->control_path), "%s%s",
		 directory, control);
	snprintf(service->stream_path, sizeof(service->stream_path), "%s%s",
		 directory, stream);
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
	return service->stream < 0 ? -1 : 0;
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
		.timer_ms = NEVER,
	};
	enum fplinux_cli_result parsed = fplinux_cli_parse(&cli, argc, argv);
	int result = 1;

	if (parsed != FPLINUX_CLI_READY)
		return parsed;
	if (arguments[OPTION_DIR].count)
		options.directory = arguments[OPTION_DIR].value;
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
	if (service.device.pcm)
		fplinux_audio_device_close(&service.device);
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
	free(service.mix);
	free(service.output);
	return result;
}
