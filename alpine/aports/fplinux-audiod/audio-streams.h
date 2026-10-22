/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_AUDIO_STREAMS_H
#define FPLINUX_AUDIO_STREAMS_H

#include "audio-mixer.h"
#include "fplinux-audio-protocol.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/*
 * The processes the session manager announced, the applications' stream
 * connections and their rings, as fplinux-audio-protocol.h describes them.
 * The service owns the sockets and the card; this module owns the policy:
 * which stream plays, waits, sleeps or ends.
 */
#define FPLINUX_AUDIO_PROCESSES 5U
#define FPLINUX_AUDIO_CONNECTIONS 16U
/* Processes remembered as gone, so that their HELLO fails at once. */
#define FPLINUX_AUDIO_GONE 5U

struct fplinux_audio_process {
	/* 0 for a free entry. */
	pid_t pid;
	uid_t uid;
	bool audible;
	bool clock;
};

enum fplinux_audio_connection_state {
	FPLINUX_AUDIO_CONNECTION_FREE,
	/* Connected; HELLO is due before its deadline. */
	FPLINUX_AUDIO_CONNECTION_NEW,
	/* HELLO from a process not announced yet; its later packets wait. */
	FPLINUX_AUDIO_CONNECTION_WAITING,
	FPLINUX_AUDIO_CONNECTION_READY,
};

struct fplinux_audio_connection {
	enum fplinux_audio_connection_state state;
	int fd;
	pid_t pid;
	uid_t uid;
	/* The role authenticated when HELLO was accepted. */
	bool clock;
	uint64_t deadline_ms;
	uint32_t last_id;
};

struct fplinux_audio_stream {
	/* NULL for a free slot. */
	struct fplinux_audio_ring *ring;
	size_t bytes;
	unsigned int connection;
	uint32_t id;
	/* The values last stored in the ring. */
	uint32_t state;
	uint32_t sleeping;
	struct fplinux_audio_mixer_stream mixer;
};

enum fplinux_audio_output {
	FPLINUX_AUDIO_OUTPUT_CLOSED,
	FPLINUX_AUDIO_OUTPUT_OPEN,
	/* The card is busy or failed to open; streams wait. */
	FPLINUX_AUDIO_OUTPUT_UNAVAILABLE,
};

struct fplinux_audio_streams {
	struct fplinux_audio_process processes[FPLINUX_AUDIO_PROCESSES];
	pid_t gone[FPLINUX_AUDIO_GONE];
	unsigned int last_gone;
	struct fplinux_audio_connection connections[FPLINUX_AUDIO_CONNECTIONS];
	struct fplinux_audio_stream streams[FPLINUX_AUDIO_STREAMS];
	enum fplinux_audio_output output;
	/* Output frames mixed since the service started. */
	uint64_t mixed;
	/* When streams of processes not announced again end; 0 for none. */
	uint64_t resync_ms;
	/* The last samples of streams that ended while they played. */
	int32_t tail[FPLINUX_AUDIO_MIXER_CHANNELS];
};

void fplinux_audio_streams_init(struct fplinux_audio_streams *streams);

/* Applies one control command, a string without a newline. */
void fplinux_audio_streams_command(struct fplinux_audio_streams *streams,
				   char *command);

/*
 * The manager's connection dropped, which forgets every process, or a new
 * one arrived at now_ms, which starts the resynchronization deadline.
 */
void fplinux_audio_streams_manager(struct fplinux_audio_streams *streams,
				   bool connected, uint64_t now_ms);

/*
 * Takes a new nonblocking stream connection with its peer's credentials;
 * false when every slot is in use, and the caller closes it.
 */
bool fplinux_audio_streams_accept(struct fplinux_audio_streams *streams, int fd,
				  pid_t pid, uid_t uid, uint64_t now_ms);

/* Reads the packets waiting on a connection, or closes it on hang-up. */
void fplinux_audio_streams_receive(struct fplinux_audio_streams *streams,
				   unsigned int index);

/* Ends a connection's streams and closes it. */
void fplinux_audio_streams_disconnect(struct fplinux_audio_streams *streams,
				      unsigned int index);

/*
 * Refuses HELLOs past their deadline and ends the streams of processes not
 * announced again; returns the next deadline, or UINT64_MAX.
 */
uint64_t fplinux_audio_streams_expire(struct fplinux_audio_streams *streams,
				      uint64_t now_ms);

/*
 * Stores each stream's state and sleeping flag after a policy or output
 * change and ends a stream with a hostile write counter. Returns whether a
 * stream that may play has frames, which needs the card.
 */
bool fplinux_audio_streams_refresh(struct fplinux_audio_streams *streams);

/* Mixes the next period; returns whether a stream played frames. */
bool fplinux_audio_streams_mix(struct fplinux_audio_streams *streams,
			       int32_t *mix, uint32_t frames);

/* Publishes each ring's read and heard positions. */
void fplinux_audio_streams_publish(struct fplinux_audio_streams *streams,
				   uint64_t played);

/* Ends every stream and closes every connection. */
void fplinux_audio_streams_stop(struct fplinux_audio_streams *streams);

#endif
