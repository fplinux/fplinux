/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef FPLINUX_AUDIO_PROTOCOL_H
#define FPLINUX_AUDIO_PROTOCOL_H

#include "fplinux-audio-route.h"

/*
 * The private protocol of the system audio service, fplinux-audiod. It is
 * compiled into the service, audio client and session manager. It is
 * neither installed nor part of the application ABI.
 *
 * audiod runs as root and mixes managed application and clock streams.
 * It listens on Unix SOCK_SEQPACKET sockets in FPLINUX_AUDIO_DIR, which
 * its service creates as root:fplinux 2750 so that both sockets get group
 * fplinux:
 *
 * - FPLINUX_AUDIO_CONTROL_SOCKET, mode 0600: the session manager's policy,
 *   in fplinux-audio-control.h. audiod accepts one connection at a time
 *   and only from a peer with its own uid, root on the phone.
 * - FPLINUX_AUDIO_STREAM_SOCKET, mode 0660: the applications' streams, in
 *   fixed-size binary messages (struct fplinux_audio_message). The frames of
 *   each stream go through a shared ring (struct fplinux_audio_ring) whose
 *   memfd audiod creates and sends with OPENED; it never accepts a
 *   descriptor from an application.
 * - FPLINUX_AUDIO_STATUS_SOCKET, mode 0660: one read-only snapshot at
 *   connection, then EOF. See fplinux-audio-status.h.
 * - FPLINUX_AUDIO_RADIO_SOCKET, mode 0600: one root FM frontend lease.
 *   See fplinux-audio-radio.h.
 *
 * In every poll round audiod serves the control socket before the stream
 * socket, so a process the manager announced before it could connect is
 * known when its HELLO arrives.
 */

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#define FPLINUX_AUDIO_DIR "/run/fplinux-audio"
#define FPLINUX_AUDIO_CONTROL_SOCKET "/run/fplinux-audio/control.sock"
#define FPLINUX_AUDIO_STREAM_SOCKET "/run/fplinux-audio/stream.sock"
#define FPLINUX_AUDIO_STATUS_SOCKET "/run/fplinux-audio/status.sock"
#define FPLINUX_AUDIO_RADIO_SOCKET "/run/fplinux-audio/radio.sock"
/*
 * Overrides FPLINUX_AUDIO_STREAM_SOCKET in the audio client when set and not
 * empty, for tests; the manager does not set it.
 */
#define FPLINUX_AUDIO_STREAM_SOCKET_ENV "FPLINUX_AUDIO_STREAM_SOCKET"

#define FPLINUX_AUDIO_PROTOCOL_VERSION 1U

/* Formats and limits of the shared audio client. */
#define FPLINUX_AUDIO_RATE_MIN 8000U
#define FPLINUX_AUDIO_RATE_MAX 48000U
#define FPLINUX_AUDIO_CHANNELS_MAX 2U
#define FPLINUX_AUDIO_CLIENT_STREAMS 4U
#define FPLINUX_AUDIO_APP_STREAMS 8U
/* Only the manager-announced clock process may use this one slot. */
#define FPLINUX_AUDIO_CLOCK_STREAM_INDEX FPLINUX_AUDIO_APP_STREAMS
#define FPLINUX_AUDIO_STREAMS (FPLINUX_AUDIO_APP_STREAMS + 1U)

#define FPLINUX_AUDIO_VOLUME_MAX 10U
#define FPLINUX_AUDIO_RESYNC_MS 1000

/*
 * Stream messages, application to audiod and back. Each packet holds one
 * struct fplinux_audio_message; fields a type does not use are 0. audiod
 * closes a connection that sends a packet of another size, an unknown type
 * or a nonzero unused field; it reads application packets without a
 * control buffer, so descriptors sent to it are discarded.
 *
 * HELLO (version): the first message of a connection. audiod checks the
 *   peer's SO_PEERCRED pid and uid against the processes the manager
 *   announced; a pid not announced yet is given until
 *   FPLINUX_AUDIO_HELLO_DEADLINE_MS after the connection. An accepted HELLO
 *   has no answer. A refused one is answered with ERROR, EAGAIN at once
 *   for a process that left (GONE), EACCES for a process the manager did
 *   not announce or EPROTONOSUPPORT for another version, and audiod closes
 *   the connection.
 * OPEN (rate, channels, flags 0): asks for a stream. Answered with OPENED or
 *   ERROR: EINVAL for a format out of range or nonzero flags, EMFILE with
 *   FPLINUX_AUDIO_CLIENT_STREAMS streams of this process open, EBUSY with
 *   FPLINUX_AUDIO_APP_STREAMS application streams open in all, ENOMEM.
 *   The clock helper gets EMFILE when its one reserved stream is open.
 * OPENED (stream, rate, channels): the new stream's nonzero ID, unique on
 *   the connection, with exactly one descriptor in SCM_RIGHTS: a sealed
 *   memfd of FPLINUX_AUDIO_RING_BYTES(capacity, channels) bytes holding the
 *   ring, which the application maps shared, readable and writable.
 * ERROR (error): a positive errno answering HELLO or OPEN.
 * CLOSE (stream): ends the stream; no answer. An unknown ID is ignored.
 * KICK (stream): frames arrived in a ring marked sleeping; no answer.
 * PAUSE (stream, flags 0|1): user resume or pause, independently of focus.
 *   Pausing preserves queued frames; CLOSE discards them. No answer.
 *
 * audiod sends only OPENED and ERROR, each answering one HELLO
 * or OPEN in order. The audio client sends HELLO and OPEN together and waits
 * at most FPLINUX_AUDIO_OPEN_TIMEOUT_MS for the answer. A hang-up ends every
 * stream of the connection; audiod hangs up when it ends a process's
 * streams on GONE, and when it stops.
 */
enum fplinux_audio_message_type {
	FPLINUX_AUDIO_HELLO = 1,
	FPLINUX_AUDIO_OPEN = 2,
	FPLINUX_AUDIO_OPENED = 3,
	FPLINUX_AUDIO_ERROR = 4,
	FPLINUX_AUDIO_CLOSE = 5,
	FPLINUX_AUDIO_KICK = 6,
	FPLINUX_AUDIO_PAUSE = 7,
};

struct fplinux_audio_message {
	uint32_t type;
	uint32_t version;
	uint32_t stream;
	uint32_t rate;
	uint32_t channels;
	uint32_t flags;
	int32_t error;
	uint32_t reserved;
};

#define FPLINUX_AUDIO_HELLO_DEADLINE_MS 1000
#define FPLINUX_AUDIO_OPEN_TIMEOUT_MS 500

/*
 * The ring of one stream: this header, then capacity frames of channels
 * int16_t samples at FPLINUX_AUDIO_RING_HEADER_BYTES. Counters are frames of
 * the stream modulo 2^32; frame n is at index n & (capacity - 1).
 *
 * capacity is the smallest power of two of at least a quarter of the rate,
 * at least 250 ms: 2048 frames at 8000 Hz, 8192 at 22050 Hz, 16384 at
 * 44100 and 48000 Hz.
 *
 * - write, stored only by the application: the frames it has written. It
 *   copies new frames to [write, write + n), then stores write + n, never
 *   more than capacity frames ahead of read.
 * - read, stored only by audiod: the first frame it may still read. Frames
 *   before it may be overwritten.
 * - heard, stored only by audiod: the frames that have left the output,
 *   published once per period. It never falls and never passes read, so
 *   write - heard, floored at 0, is the frames queued, which includes what
 *   audiod and the output still hold.
 * - state: enum fplinux_audio_ring_state, stored only by audiod.
 * - sleeping, stored by audiod: nonzero while audiod does not look at the
 *   ring every period, because the output is closed or the stream is
 *   paused. The application's next write that adds frames sends one KICK
 *   for the stream; audiod clears sleeping when it looks at the ring again.
 *   Both sides use sequentially consistent operations for this hand-off:
 *   the application stores write and then loads sleeping, and audiod
 *   stores sleeping and then loads write again, so at least one of them
 *   sees the other.
 * - reason, stored only by audiod: the current pause or availability cause,
 *   independent of the client's own pause request.
 *
 * audiod trusts nothing in the mapping: it keeps its own rate, channels,
 * capacity and read position, loads write once per period, and ends a
 * stream whose write is more than capacity frames ahead of its read
 * position. A corrupt ring spoils only that stream.
 */
#define FPLINUX_AUDIO_RING_MAGIC 0x31415046U /* "FPA1" in memory order */
#define FPLINUX_AUDIO_RING_HEADER_BYTES 64U
#define FPLINUX_AUDIO_RING_FRAMES_MAX 16384U
#define FPLINUX_AUDIO_RING_BYTES(capacity, channels) \
	(FPLINUX_AUDIO_RING_HEADER_BYTES +           \
	 (size_t)(capacity) * (channels) * sizeof(int16_t))

enum fplinux_audio_ring_state {
	/* The stream plays, or would as soon as it has frames. */
	FPLINUX_AUDIO_RING_PLAYING = 0,
	/* The policy holds the stream: its application is not audible. */
	FPLINUX_AUDIO_RING_PAUSED = 1,
	/* The output is busy, for example with FM radio, or failed to open. */
	FPLINUX_AUDIO_RING_UNAVAILABLE = 2,
	/* audiod ended the stream; writes fail with EPIPE. */
	FPLINUX_AUDIO_RING_ENDED = 3,
};

enum fplinux_audio_ring_reason {
	FPLINUX_AUDIO_REASON_NONE = 0,
	FPLINUX_AUDIO_REASON_USER = 1,
	FPLINUX_AUDIO_REASON_POLICY = 2,
	FPLINUX_AUDIO_REASON_FOREGROUND = 3,
	FPLINUX_AUDIO_REASON_CLOCK = 4,
	FPLINUX_AUDIO_REASON_DEVICE = 5,
	FPLINUX_AUDIO_REASON_FM = 6,
};

struct fplinux_audio_ring {
	/* Set by audiod before it sends the descriptor, constant afterwards. */
	uint32_t magic;
	uint32_t rate;
	uint32_t channels;
	uint32_t capacity;
	_Atomic uint32_t write;
	_Atomic uint32_t read;
	_Atomic uint32_t heard;
	_Atomic uint32_t state;
	_Atomic uint32_t sleeping;
	_Atomic uint32_t reason;
	uint32_t reserved[6];
};

_Static_assert(sizeof(struct fplinux_audio_ring) ==
		       FPLINUX_AUDIO_RING_HEADER_BYTES,
	       "the ring header has a fixed size");
_Static_assert(sizeof(struct fplinux_audio_message) == 32,
	       "stream messages have a fixed size");

/* The frames that follow the header of a mapped ring. */
static inline int16_t *
fplinux_audio_ring_frames(struct fplinux_audio_ring *ring)
{
	return (int16_t *)((unsigned char *)ring +
			   FPLINUX_AUDIO_RING_HEADER_BYTES);
}

/* The ring capacity for a stream rate in range, as described above. */
static inline uint32_t fplinux_audio_ring_capacity(uint32_t rate)
{
	uint32_t capacity = 1;

	while (capacity < (rate + 3) / 4)
		capacity <<= 1;
	return capacity;
}

#endif
