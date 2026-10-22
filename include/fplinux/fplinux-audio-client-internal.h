/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef FPLINUX_AUDIO_CLIENT_INTERNAL_H
#define FPLINUX_AUDIO_CLIENT_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct fplinux_audio_client;
struct fplinux_audio_stream;

/* Shared transport for the UI runtime and the session's clock alert. */
#define FPLINUX_AUDIO_INTERNAL __attribute__((visibility("hidden")))

FPLINUX_AUDIO_INTERNAL bool fplinux_audio_format_valid(unsigned int rate,
						       unsigned int channels);
FPLINUX_AUDIO_INTERNAL struct fplinux_audio_client *
fplinux_audio_client_create(void);
/* Connects lazily and waits at most 500 ms for a stream, returning errno. */
FPLINUX_AUDIO_INTERNAL struct fplinux_audio_stream *
fplinux_audio_stream_open(struct fplinux_audio_client *client,
			  unsigned int rate, unsigned int channels);
FPLINUX_AUDIO_INTERNAL long
fplinux_audio_stream_write(struct fplinux_audio_stream *stream,
			   const int16_t *frames, size_t count);
FPLINUX_AUDIO_INTERNAL long
fplinux_audio_stream_queued(const struct fplinux_audio_stream *stream);
FPLINUX_AUDIO_INTERNAL void
fplinux_audio_stream_close(struct fplinux_audio_stream *stream);
FPLINUX_AUDIO_INTERNAL int
fplinux_audio_client_fd(const struct fplinux_audio_client *client);
/* Polling the connection detects a service crash even while rings are idle. */
FPLINUX_AUDIO_INTERNAL void
fplinux_audio_client_dispatch(struct fplinux_audio_client *client,
			      short revents);
/* Ends and frees every stream still owned by the client. */
FPLINUX_AUDIO_INTERNAL void
fplinux_audio_client_close(struct fplinux_audio_client *client);

#endif
