/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "session-audio.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

/* Packets read from the service in one dispatch; the rest wait for the next. */
#define FPLINUX_SESSION_AUDIO_READS 32

_Static_assert(FPLINUX_SESSION_AUDIO_PATH_BYTES ==
		       sizeof(((struct sockaddr_un *)0)->sun_path),
	       "the socket path fits sockaddr_un");

void fplinux_session_audio_init(struct fplinux_session_audio *audio,
				const char *socket_path, int epoll_fd)
{
	memset(audio, 0, sizeof(*audio));
	audio->fd = -1;
	audio->epoll_fd = epoll_fd;
	audio->backoff_ms = FPLINUX_SESSION_AUDIO_RETRY_MS;
	for (unsigned int i = 0; i < FPLINUX_SESSION_AUDIO_CLASSES; ++i)
		audio->volume[i].desired = FPLINUX_AUDIO_VOLUME_MAX;
	audio->desired_output = FPLINUX_AUDIO_ROUTE_AUTO;
	if (!socket_path)
		return;
	if (strlen(socket_path) >= sizeof(audio->socket_path)) {
		fprintf(stderr,
			"fplinux-session: audio service path too long: %s\n",
			socket_path);
		return;
	}
	strcpy(audio->socket_path, socket_path);
	audio->enabled = true;
}

static void retry_later(struct fplinux_session_audio *audio, uint64_t now_ms)
{
	audio->retry_ms = now_ms + audio->backoff_ms;
	audio->backoff_ms *= 2;
	if (audio->backoff_ms > FPLINUX_SESSION_AUDIO_RETRY_MAX_MS)
		audio->backoff_ms = FPLINUX_SESSION_AUDIO_RETRY_MAX_MS;
}

static void disconnect(struct fplinux_session_audio *audio)
{
	unsigned int i;

	if (audio->fd < 0)
		return;
	if (audio->epoll_fd >= 0)
		epoll_ctl(audio->epoll_fd, EPOLL_CTL_DEL, audio->fd, NULL);
	close(audio->fd);
	audio->fd = -1;
	for (i = 0; i < FPLINUX_SESSION_AUDIO_CLASSES; ++i) {
		audio->volume[i].known = false;
		audio->volume[i].waiting = false;
	}
	audio->output_known = false;
	audio->output_waiting = false;
	audio->ready = false;
	for (i = 0; i < FPLINUX_SESSION_AUDIO_PROCESSES; ++i) {
		audio->processes[i].announced = false;
		audio->processes[i].playback = FPLINUX_UI_SESSION_PLAYBACK_NONE;
	}
}

/* The service forgets every process now; the next connection repeats them. */
static void lose(struct fplinux_session_audio *audio, const char *reason,
		 uint64_t now_ms)
{
	fprintf(stderr, "fplinux-session: audio service connection lost: %s\n",
		reason);
	audio->quiet = true;
	disconnect(audio);
	retry_later(audio, now_ms);
}

/* A command never waits: one that cannot go now closes the connection. */
static bool send_control(struct fplinux_session_audio *audio,
			 const struct fplinux_audio_control *message,
			 uint64_t now_ms)
{
	if (fplinux_audio_control_send(audio->fd, message) == 0)
		return true;
	lose(audio, strerror(errno), now_ms);
	return false;
}

static bool send_volume(struct fplinux_session_audio *audio,
			enum fplinux_session_audio_class class, uint64_t now_ms)
{
	struct fplinux_session_audio_volume *volume = &audio->volume[class];
	struct fplinux_audio_control command = {
		.type = FPLINUX_AUDIO_SET_VOLUME,
		.volume = {
			.class = class == FPLINUX_SESSION_AUDIO_MEDIA ?
					 FPLINUX_AUDIO_MEDIA : FPLINUX_AUDIO_ALARM,
			.level = volume->desired,
		},
	};

	if (!send_control(audio, &command, now_ms))
		return false;
	volume->sent = volume->desired;
	volume->waiting = true;
	volume->deadline_ms = now_ms + FPLINUX_SESSION_AUDIO_VOLUME_TIMEOUT_MS;
	return true;
}

static bool send_output(struct fplinux_session_audio *audio, uint64_t now_ms)
{
	struct fplinux_audio_control command = {
		.type = FPLINUX_AUDIO_SET_OUTPUT,
		.route = audio->desired_output,
	};

	if (!send_control(audio, &command, now_ms))
		return false;
	audio->sent_output = audio->desired_output;
	audio->output_waiting = true;
	audio->output_error = 0;
	audio->output_deadline_ms =
		now_ms + FPLINUX_SESSION_AUDIO_VOLUME_TIMEOUT_MS;
	return true;
}

static bool tell(struct fplinux_session_audio *audio,
		 struct fplinux_session_audio_process *process, uint64_t now_ms)
{
	struct fplinux_audio_control command = {
		.type = FPLINUX_AUDIO_CONTROL_ELIGIBLE,
		.eligible = {
			.pid = process->pid,
			.enabled = process->eligible,
		},
	};

	if (!send_control(audio, &command, now_ms))
		return false;
	process->told_eligible = process->eligible;
	return true;
}

/* APP and then ELIGIBLE, so that the service never guesses the state. */
static bool announce(struct fplinux_session_audio *audio,
		     struct fplinux_session_audio_process *process,
		     uint64_t now_ms)
{
	struct fplinux_audio_control command;

	if (process->clock)
		command = (struct fplinux_audio_control){
			.type = FPLINUX_AUDIO_CONTROL_CLOCK,
			.clock = {
				.pid = process->pid,
				.uid = process->uid,
			},
		};
	else
		command = (struct fplinux_audio_control){
			.type = FPLINUX_AUDIO_CONTROL_APP,
			.app = {
				.pid = process->pid,
				.uid = process->uid,
				.background = process->background,
			},
		};
	if (!send_control(audio, &command, now_ms))
		return false;
	process->announced = true;
	return tell(audio, process, now_ms);
}

static bool announce_all(struct fplinux_session_audio *audio, uint64_t now_ms)
{
	unsigned int i;

	for (i = 0; i < FPLINUX_SESSION_AUDIO_PROCESSES; ++i)
		if (audio->processes[i].pid &&
		    !announce(audio, &audio->processes[i], now_ms))
			return false;
	return true;
}

static void connect_service(struct fplinux_session_audio *audio,
			    uint64_t now_ms)
{
	struct sockaddr_un address = { .sun_family = AF_UNIX };
	struct epoll_event event = { .events = EPOLLIN };
	int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK,
			0);

	strcpy(address.sun_path, audio->socket_path);
	event.data.fd = fd;
	if (fd < 0 ||
	    connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0 ||
	    (audio->epoll_fd >= 0 &&
	     epoll_ctl(audio->epoll_fd, EPOLL_CTL_ADD, fd, &event) < 0)) {
		if (!audio->quiet)
			fprintf(stderr,
				"fplinux-session: audio service unavailable: %s\n",
				strerror(errno));
		audio->quiet = true;
		if (fd >= 0)
			close(fd);
		retry_later(audio, now_ms);
		return;
	}
	audio->fd = fd;
	audio->quiet = false;
	fprintf(stderr, "fplinux-session: audio service connected\n");
	for (unsigned int i = 0; i < FPLINUX_SESSION_AUDIO_CLASSES; ++i)
		if (!send_volume(audio, i, now_ms))
			return;
}

static struct fplinux_session_audio_process *
find_process(struct fplinux_session_audio *audio, pid_t pid)
{
	unsigned int i;

	for (i = 0; pid > 0 && i < FPLINUX_SESSION_AUDIO_PROCESSES; ++i)
		if (audio->processes[i].pid == pid)
			return &audio->processes[i];
	return NULL;
}

/* A command word holds printable ASCII without spaces. */
static bool is_word(const char *text, size_t capacity)
{
	size_t length = strlen(text);
	size_t i;

	if (!length || length >= capacity)
		return false;
	for (i = 0; i < length; ++i)
		if (text[i] <= ' ' || text[i] > '~')
			return false;
	return true;
}

bool fplinux_session_audio_app(struct fplinux_session_audio *audio, pid_t pid,
			       uid_t uid, const char *app_id, bool background,
			       uint64_t now_ms)
{
	struct fplinux_session_audio_process *process;
	unsigned int i;

	if (!audio->enabled || pid <= 0 || uid == (uid_t)-1 ||
	    !is_word(app_id, FPLINUX_UI_SESSION_ID_BYTES))
		return false;
	process = find_process(audio, pid);
	for (i = 0; !process && i < FPLINUX_SESSION_AUDIO_PROCESSES; ++i)
		if (!audio->processes[i].pid)
			process = &audio->processes[i];
	if (!process) {
		fprintf(stderr,
			"fplinux-session: audio service: no room to announce %s\n",
			app_id);
		return false;
	}
	*process = (struct fplinux_session_audio_process){
		.pid = pid,
		.uid = uid,
		.background = background,
	};
	for (i = 0; background && i < FPLINUX_SESSION_AUDIO_PROCESSES; ++i)
		if (&audio->processes[i] != process)
			audio->processes[i].background = false;
	if (audio->fd >= 0 && audio->ready)
		announce(audio, process, now_ms);
	return true;
}

bool fplinux_session_audio_clock(struct fplinux_session_audio *audio, pid_t pid,
				 uid_t uid, uint64_t now_ms)
{
	struct fplinux_session_audio_process *process =
		find_process(audio, pid);
	unsigned int i;

	if (!audio->enabled || pid <= 0 || uid == (uid_t)-1)
		return false;
	for (i = 0; i < FPLINUX_SESSION_AUDIO_PROCESSES; ++i) {
		if (audio->processes[i].clock && audio->processes[i].pid != pid)
			return false;
		if (!process && !audio->processes[i].pid)
			process = &audio->processes[i];
	}
	if (!process)
		return false;
	*process = (struct fplinux_session_audio_process){
		.pid = pid,
		.uid = uid,
		.clock = true,
	};
	if (audio->fd >= 0 && audio->ready)
		announce(audio, process, now_ms);
	return true;
}

void fplinux_session_audio_gone(struct fplinux_session_audio *audio, pid_t pid,
				uint64_t now_ms)
{
	struct fplinux_session_audio_process *process =
		find_process(audio, pid);
	struct fplinux_audio_control command = {
		.type = FPLINUX_AUDIO_CONTROL_GONE,
		.gone = pid,
	};

	if (!process)
		return;
	if (audio->fd >= 0 && process->announced)
		send_control(audio, &command, now_ms);
	memset(process, 0, sizeof(*process));
}

void fplinux_session_audio_eligible(struct fplinux_session_audio *audio,
				    pid_t pid, bool eligible, uint64_t now_ms)
{
	struct fplinux_session_audio_process *process =
		find_process(audio, pid);

	if (!process)
		return;
	process->eligible = eligible;
	if (audio->fd >= 0 && process->announced &&
	    process->told_eligible != eligible)
		tell(audio, process, now_ms);
}

bool fplinux_session_audio_set_volume(struct fplinux_session_audio *audio,
				      enum fplinux_session_audio_class class,
				      unsigned int level, uint64_t now_ms)
{
	struct fplinux_session_audio_volume *volume;

	if ((unsigned int)class >= FPLINUX_SESSION_AUDIO_CLASSES ||
	    level > FPLINUX_AUDIO_VOLUME_MAX)
		return false;
	volume = &audio->volume[class];
	volume->desired = level;
	if (audio->fd >= 0 && !volume->waiting &&
	    (!volume->known || volume->applied != level))
		send_volume(audio, class, now_ms);
	return true;
}

bool fplinux_session_audio_volume_applied(
	const struct fplinux_session_audio *audio,
	enum fplinux_session_audio_class class, unsigned int *level)
{
	if ((unsigned int)class >= FPLINUX_SESSION_AUDIO_CLASSES ||
	    !audio->volume[class].known)
		return false;
	if (level)
		*level = audio->volume[class].applied;
	return true;
}

bool fplinux_session_audio_volume_pending(
	const struct fplinux_session_audio *audio,
	enum fplinux_session_audio_class class)
{
	const struct fplinux_session_audio_volume *volume;

	if ((unsigned int)class >= FPLINUX_SESSION_AUDIO_CLASSES)
		return true;
	volume = &audio->volume[class];
	return !volume->known || volume->waiting ||
	       volume->applied != volume->desired;
}

bool fplinux_session_audio_set_output(struct fplinux_session_audio *audio,
				      enum fplinux_audio_route output,
				      uint64_t now_ms)
{
	if (!fplinux_audio_route_name(output))
		return false;
	audio->desired_output = output;
	audio->output_error = 0;
	if (audio->fd >= 0 && audio->ready && !audio->output_waiting &&
	    (!audio->output_known || audio->applied_output != output))
		send_output(audio, now_ms);
	return true;
}

static bool complete_replay(struct fplinux_session_audio *audio,
			    uint64_t now_ms)
{
	for (unsigned int i = 0; i < FPLINUX_SESSION_AUDIO_CLASSES; ++i)
		if (fplinux_session_audio_volume_pending(audio, i))
			return true;
	if (!audio->output_known) {
		if (!audio->output_waiting)
			return send_output(audio, now_ms);
		return true;
	}
	if (!audio->ready) {
		const struct fplinux_audio_control sync = {
			.type = FPLINUX_AUDIO_CONTROL_SYNC,
		};

		audio->ready = true;
		if (!announce_all(audio, now_ms))
			return false;
		if (!send_control(audio, &sync, now_ms))
			return false;
		audio->backoff_ms = FPLINUX_SESSION_AUDIO_RETRY_MS;
	}
	return true;
}

static bool acknowledge(struct fplinux_session_audio *audio,
			const struct fplinux_audio_control *message,
			uint64_t now_ms)
{
	if (message->type == FPLINUX_AUDIO_VOLUME_APPLIED) {
		enum fplinux_session_audio_class class =
			message->volume.class == FPLINUX_AUDIO_MEDIA ?
				FPLINUX_SESSION_AUDIO_MEDIA :
				FPLINUX_SESSION_AUDIO_ALARM;
		struct fplinux_session_audio_volume *volume =
			&audio->volume[class];

		if (!volume->waiting || volume->sent != message->volume.level)
			goto invalid;
		volume->waiting = false;
		volume->known = true;
		volume->applied = message->volume.level;
		volume->confirmed = true;
		if (volume->desired != volume->applied &&
		    !send_volume(audio, class, now_ms))
			return false;
		return complete_replay(audio, now_ms);
	}
	if (!audio->output_waiting)
		goto invalid;
	if (message->type == FPLINUX_AUDIO_OUTPUT_APPLIED) {
		if (message->route != audio->sent_output)
			goto invalid;
		audio->output_waiting = false;
		audio->output_known = true;
		audio->applied_output = message->route;
		if (audio->desired_output != audio->applied_output)
			return send_output(audio, now_ms);
		return complete_replay(audio, now_ms);
	}
	if (message->type == FPLINUX_AUDIO_OUTPUT_ERROR) {
		audio->output_waiting = false;
		audio->output_error = message->error;
		if (audio->ready)
			return true;
		/* A rejected preference cannot authorize replay. */
		if (audio->sent_output == FPLINUX_AUDIO_ROUTE_AUTO) {
			lose(audio, "output replay rejected", now_ms);
			return false;
		}
		audio->desired_output = FPLINUX_AUDIO_ROUTE_AUTO;
		return send_output(audio, now_ms);
	}
invalid:
	lose(audio, "invalid acknowledgement", now_ms);
	return false;
}

enum fplinux_ui_session_playback
fplinux_session_audio_playback(const struct fplinux_session_audio *audio,
			       pid_t pid, uid_t uid)
{
	if (audio->ready)
		for (unsigned int i = 0; i < FPLINUX_SESSION_AUDIO_PROCESSES;
		     ++i) {
			const struct fplinux_session_audio_process *process =
				&audio->processes[i];

			if (process->pid == pid && process->uid == uid &&
			    process->announced && !process->clock)
				return process->playback;
		}
	return FPLINUX_UI_SESSION_PLAYBACK_NONE;
}

static void receive_activity(struct fplinux_session_audio *audio,
			     const struct fplinux_audio_control *message)
{
	static const enum fplinux_ui_session_playback states[] = {
		[FPLINUX_AUDIO_ACTIVITY_NONE] =
			FPLINUX_UI_SESSION_PLAYBACK_NONE,
		[FPLINUX_AUDIO_ACTIVITY_PAUSED] =
			FPLINUX_UI_SESSION_PLAYBACK_PAUSED,
		[FPLINUX_AUDIO_ACTIVITY_HELD] =
			FPLINUX_UI_SESSION_PLAYBACK_HELD,
		[FPLINUX_AUDIO_ACTIVITY_UNAVAILABLE] =
			FPLINUX_UI_SESSION_PLAYBACK_UNAVAILABLE,
		[FPLINUX_AUDIO_ACTIVITY_PLAYING] =
			FPLINUX_UI_SESSION_PLAYBACK_PLAYING,
	};
	struct fplinux_session_audio_process *process =
		find_process(audio, message->activity.pid);

	/* GONE or a replaced process can leave an older notification in flight. */
	if (audio->ready && process && process->announced && !process->clock &&
	    process->uid == message->activity.uid)
		process->playback = states[message->activity.state];
}

void fplinux_session_audio_dispatch(struct fplinux_session_audio *audio,
				    uint64_t now_ms)
{
	unsigned int count;

	if (!audio->enabled)
		return;
	if (audio->fd < 0) {
		if (now_ms >= audio->retry_ms)
			connect_service(audio, now_ms);
		return;
	}
	for (unsigned int i = 0; i < FPLINUX_SESSION_AUDIO_CLASSES; ++i)
		if (audio->volume[i].waiting &&
		    now_ms >= audio->volume[i].deadline_ms) {
			lose(audio, "volume acknowledgement timed out", now_ms);
			return;
		}
	if (audio->output_waiting && now_ms >= audio->output_deadline_ms) {
		lose(audio, "output acknowledgement timed out", now_ms);
		return;
	}
	for (count = 0; count < FPLINUX_SESSION_AUDIO_READS; ++count) {
		struct fplinux_audio_control message;
		int result = fplinux_audio_control_receive(
			audio->fd, FPLINUX_AUDIO_TO_MANAGER, &message);

		if (result < 0 && errno == EINTR)
			continue;
		if (result > 0) {
			if (message.type == FPLINUX_AUDIO_CONTROL_ACTIVITY)
				receive_activity(audio, &message);
			else if (!acknowledge(audio, &message, now_ms))
				return;
			continue;
		}
		if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return;
		lose(audio, result ? strerror(errno) : "closed by the service",
		     now_ms);
		return;
	}
}

uint64_t
fplinux_session_audio_deadline(const struct fplinux_session_audio *audio)
{
	uint64_t deadline = audio->output_waiting ? audio->output_deadline_ms :
						    0;

	if (!audio->enabled)
		return 0;
	if (audio->fd < 0)
		return audio->retry_ms;
	for (unsigned int i = 0; i < FPLINUX_SESSION_AUDIO_CLASSES; ++i)
		if (audio->volume[i].waiting &&
		    (!deadline || audio->volume[i].deadline_ms < deadline))
			deadline = audio->volume[i].deadline_ms;
	return deadline;
}

void fplinux_session_audio_close(struct fplinux_session_audio *audio)
{
	disconnect(audio);
}
