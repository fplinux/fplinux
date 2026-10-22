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

#include "fplinux-audio-protocol.h"

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
	for (i = 0; i < FPLINUX_SESSION_AUDIO_PROCESSES; ++i)
		audio->processes[i].announced = false;
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
static bool send_text(struct fplinux_session_audio *audio, const char *text,
		      uint64_t now_ms)
{
	size_t length = strlen(text);
	ssize_t sent;

	do {
		sent = send(audio->fd, text, length,
			    MSG_DONTWAIT | MSG_NOSIGNAL);
	} while (sent < 0 && errno == EINTR);
	if (sent == (ssize_t)length)
		return true;
	lose(audio, sent < 0 ? strerror(errno) : "short send", now_ms);
	return false;
}

static bool tell(struct fplinux_session_audio *audio,
		 struct fplinux_session_audio_process *process, uint64_t now_ms)
{
	char text[FPLINUX_AUDIO_CONTROL_BYTES];

	snprintf(text, sizeof(text), FPLINUX_AUDIO_CONTROL_AUDIBLE " %ld %d",
		 (long)process->pid, process->audible);
	if (!send_text(audio, text, now_ms))
		return false;
	process->told_audible = process->audible;
	return true;
}

/* APP and then AUDIBLE, so that the service never guesses the state. */
static bool announce(struct fplinux_session_audio *audio,
		     struct fplinux_session_audio_process *process,
		     uint64_t now_ms)
{
	char text[FPLINUX_AUDIO_CONTROL_BYTES];

	if (process->clock)
		snprintf(text, sizeof(text),
			 FPLINUX_AUDIO_CONTROL_CLOCK " %ld %lu",
			 (long)process->pid, (unsigned long)process->uid);
	else
		snprintf(text, sizeof(text),
			 FPLINUX_AUDIO_CONTROL_APP " %ld %lu %s",
			 (long)process->pid, (unsigned long)process->uid,
			 process->app_id);
	if (!send_text(audio, text, now_ms))
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
	if (announce_all(audio, now_ms))
		audio->backoff_ms = FPLINUX_SESSION_AUDIO_RETRY_MS;
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
			       uid_t uid, const char *app_id, uint64_t now_ms)
{
	struct fplinux_session_audio_process *process;
	unsigned int i;

	if (!audio->enabled || pid <= 0 ||
	    !is_word(app_id, sizeof(process->app_id)))
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
	};
	strcpy(process->app_id, app_id);
	if (audio->fd >= 0)
		announce(audio, process, now_ms);
	return true;
}

bool fplinux_session_audio_clock(struct fplinux_session_audio *audio, pid_t pid,
				 uid_t uid, uint64_t now_ms)
{
	struct fplinux_session_audio_process *process =
		find_process(audio, pid);
	unsigned int i;

	if (!audio->enabled || pid <= 0)
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
	if (audio->fd >= 0)
		announce(audio, process, now_ms);
	return true;
}

void fplinux_session_audio_gone(struct fplinux_session_audio *audio, pid_t pid,
				uint64_t now_ms)
{
	struct fplinux_session_audio_process *process =
		find_process(audio, pid);
	char text[FPLINUX_AUDIO_CONTROL_BYTES];

	if (!process)
		return;
	if (audio->fd >= 0 && process->announced) {
		snprintf(text, sizeof(text), FPLINUX_AUDIO_CONTROL_GONE " %ld",
			 (long)pid);
		send_text(audio, text, now_ms);
	}
	memset(process, 0, sizeof(*process));
}

void fplinux_session_audio_audible(struct fplinux_session_audio *audio,
				   pid_t pid, bool audible, uint64_t now_ms)
{
	struct fplinux_session_audio_process *process =
		find_process(audio, pid);

	if (!process)
		return;
	process->audible = audible;
	if (audio->fd >= 0 && process->announced &&
	    process->told_audible != audible)
		tell(audio, process, now_ms);
}

void fplinux_session_audio_dispatch(struct fplinux_session_audio *audio,
				    uint64_t now_ms)
{
	char packet[FPLINUX_AUDIO_CONTROL_BYTES];
	unsigned int count;

	if (!audio->enabled)
		return;
	if (audio->fd < 0) {
		if (now_ms >= audio->retry_ms)
			connect_service(audio, now_ms);
		return;
	}
	/* The control connection carries no replies. */
	for (count = 0; count < FPLINUX_SESSION_AUDIO_READS; ++count) {
		ssize_t bytes =
			recv(audio->fd, packet, sizeof(packet), MSG_DONTWAIT);

		if (bytes > 0 || (bytes < 0 && errno == EINTR))
			continue;
		if (bytes < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return;
		lose(audio, bytes ? strerror(errno) : "closed by the service",
		     now_ms);
		return;
	}
}

uint64_t
fplinux_session_audio_deadline(const struct fplinux_session_audio *audio)
{
	return audio->enabled && audio->fd < 0 ? audio->retry_ms : 0;
}

void fplinux_session_audio_close(struct fplinux_session_audio *audio)
{
	disconnect(audio);
}
