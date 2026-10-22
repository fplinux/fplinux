/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_SESSION_AUDIO_H
#define FPLINUX_SESSION_AUDIO_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#include "fplinux-ui-session.h"

/* Retained application tasks and the single bounded clock helper. */
#define FPLINUX_SESSION_AUDIO_PROCESSES (FPLINUX_UI_SESSION_TASK_MAX + 1)
/*
 * A failed connection is tried again after the backoff, which starts at
 * RETRY_MS and doubles with each failure up to RETRY_MAX_MS; a connection
 * that announced the whole state resets it.
 */
#define FPLINUX_SESSION_AUDIO_RETRY_MS 100U
#define FPLINUX_SESSION_AUDIO_RETRY_MAX_MS 2000U
/* The size of sockaddr_un.sun_path. */
#define FPLINUX_SESSION_AUDIO_PATH_BYTES 108

struct fplinux_session_audio_process {
	/* 0 for a free entry. */
	pid_t pid;
	uid_t uid;
	char app_id[FPLINUX_UI_SESSION_ID_BYTES];
	bool clock;
	bool audible;
	/* APP went out on the current connection, and what AUDIBLE last said. */
	bool announced;
	bool told_audible;
};

/*
 * The manager's client of the audio service's control socket
 * (fplinux-audio-protocol.h). It holds the processes the service must know,
 * with their uid, application ID and whether they are audible, and sends
 * each change as it happens, in the order of the calls.
 *
 * Nothing blocks: the socket is nonblocking, and a send that fails or would
 * block closes the connection instead of dropping the command. The next
 * connection, after the backoff, announces the whole state again: APP and
 * then AUDIBLE for each process. A process that goes while no connection
 * exists is only forgotten, as the service forgets every process when the
 * connection drops. Packets from the service are read and ignored. Each
 * connection, each loss and the first failure after them log one line.
 *
 * Every call takes the current CLOCK_MONOTONIC time in milliseconds.
 */
struct fplinux_session_audio {
	char socket_path[FPLINUX_SESSION_AUDIO_PATH_BYTES];
	bool enabled;
	/* The connection is added to epoll_fd while it exists. */
	int epoll_fd;
	int fd;
	/* When to connect again while fd is -1, and the backoff after that. */
	uint64_t retry_ms;
	unsigned int backoff_ms;
	/* A failure was logged since the last connection. */
	bool quiet;
	struct fplinux_session_audio_process
		processes[FPLINUX_SESSION_AUDIO_PROCESSES];
};

/*
 * A NULL socket_path disables the client: it never connects and ignores
 * every process. epoll_fd may be -1. The first dispatch connects.
 */
void fplinux_session_audio_init(struct fplinux_session_audio *audio,
				const char *socket_path, int epoll_fd);
/*
 * Announces a started process, not audible, or replaces the announcement of
 * the same pid. Returns false when disabled, for an application ID that is
 * not one word, or when every entry is taken.
 */
bool fplinux_session_audio_app(struct fplinux_session_audio *audio, pid_t pid,
			       uid_t uid, const char *app_id, uint64_t now_ms);
/* The manager's actual clock helper; its stream has reserved capacity. */
bool fplinux_session_audio_clock(struct fplinux_session_audio *audio, pid_t pid,
				 uid_t uid, uint64_t now_ms);
/* The process is leaving, closing or was reaped; later calls for it do nothing. */
void fplinux_session_audio_gone(struct fplinux_session_audio *audio, pid_t pid,
				uint64_t now_ms);
/* Sends AUDIBLE when the announced process's state changes. */
void fplinux_session_audio_audible(struct fplinux_session_audio *audio,
				   pid_t pid, bool audible, uint64_t now_ms);
/* Reads what the service sent, notices a hang-up and connects when due. */
void fplinux_session_audio_dispatch(struct fplinux_session_audio *audio,
				    uint64_t now_ms);
/* When dispatch connects again, or 0 while connected or disabled. */
uint64_t
fplinux_session_audio_deadline(const struct fplinux_session_audio *audio);
/* Closes the connection, which makes the service forget every process. */
void fplinux_session_audio_close(struct fplinux_session_audio *audio);

#endif
