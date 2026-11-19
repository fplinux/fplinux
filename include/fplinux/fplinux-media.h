/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef FPLINUX_MEDIA_H
#define FPLINUX_MEDIA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define FPLINUX_MEDIA_BLOCK_FRAMES 1024U
#define FPLINUX_MEDIA_RESOURCE_BYTES (16U * 1024U * 1024U)
#define FPLINUX_MEDIA_FILE_BYTES (512ULL * 1024ULL * 1024ULL)
#define FPLINUX_MEDIA_TEXT_BYTES 256U

enum fplinux_media_codec {
	FPLINUX_MEDIA_WAV,
	FPLINUX_MEDIA_MP3,
	FPLINUX_MEDIA_AMR_NB,
};

struct fplinux_media_info {
	enum fplinux_media_codec codec;
	unsigned int rate;
	unsigned int channels;
	uint64_t position_frames;
	uint64_t duration_ms;
	bool duration_known;
	bool seeking;
	/* Decoder EOF: the consumer still drains its audio transport. */
	bool eof;
	char title[FPLINUX_MEDIA_TEXT_BYTES];
	char artist[FPLINUX_MEDIA_TEXT_BYTES];
	char album[FPLINUX_MEDIA_TEXT_BYTES];
	/* True for RIFF WAVE, including a WAVE-wrapped MP3 stream. */
	bool wave_container;
};

struct fplinux_media;

/*
 * Unprivileged, synchronous decoders. A caller serializes each instance and
 * runs file IO outside its UI or guest thread. open_fd duplicates a regular
 * file descriptor and reads from byte zero without changing its offset.
 * open_bytes copies its input, bounded by RESOURCE_BYTES. Both sniff content.
 * WAV accepts unsigned 8-bit PCM, little-endian signed 16-bit PCM, or
 * 4-bit IMA ADPCM in complete blocks, mono/stereo, 8000..48000 Hz.
 * MP3 may be raw or WAVE-wrapped.
 * AMR-NB accepts mono 8000 Hz files with good-quality speech frames (0..7).
 * Failures return NULL/-1 with errno; unsupported content uses ENOTSUP.
 */
struct fplinux_media *fplinux_media_open_fd(int fd);
struct fplinux_media *fplinux_media_open_bytes(const void *bytes, size_t count);
void fplinux_media_close(struct fplinux_media *media);
const struct fplinux_media_info *
fplinux_media_info(const struct fplinux_media *media);

/*
 * Interleaved signed 16-bit native-endian PCM, at most BLOCK_FRAMES, with
 * capacity*channels sample elements. Positive results must be consumed even
 * when info.eof becomes true. Zero is EOF.
 * A cooperative seek returns -1/EAGAIN until its next bounded decode step;
 * do not treat that as a playback failure. No complete PCM clip is retained.
 */
ssize_t fplinux_media_read(struct fplinux_media *media, int16_t *samples,
			   size_t capacity);
/* Flush the consumer's old transport before seeking. Latest target wins.
 * Returns 0 when ready, 1 while pending, -1 on error. */
int fplinux_media_seek(struct fplinux_media *media, uint64_t position_ms);
/* Local guest Player gain, 0..100; Music leaves this at its unity default. */
int fplinux_media_set_gain(struct fplinux_media *media, unsigned int level);

#endif
