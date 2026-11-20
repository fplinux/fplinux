/* SPDX-License-Identifier: 0BSD */
#ifndef FPLINUX_MRE_AUDIO_H
#define FPLINUX_MRE_AUDIO_H

#include <stdint.h>

#define MRE_AUDIO_HANDLES 9U
#define MRE_AUDIO_BYTES (8U * 1024U * 1024U)
#define MRE_AUDIO_CHUNK_BYTES 4096U

enum mre_audio_operation {
	MRE_AUDIO_BEGIN = 1,
	MRE_AUDIO_WRITE,
	MRE_AUDIO_PLAY,
	MRE_AUDIO_PAUSE,
	MRE_AUDIO_RESUME,
	MRE_AUDIO_STOP,
	MRE_AUDIO_STOP_MIDI,
	MRE_AUDIO_VOLUME,
};

enum mre_audio_kind {
	MRE_AUDIO_MIDI = 1,
	MRE_AUDIO_GENERIC,
};

/* Format numbers, callback values and the 0..6 volume scale belong to the
 * guest ABI. Output routing remains the system's foreground media policy. */
enum mre_audio_format {
	MRE_AUDIO_MP3 = 5,
	MRE_AUDIO_SMF = 17,
};

struct mre_audio_request {
	uint32_t sequence;
	uint32_t operation;
	uint32_t handle;
	uint32_t kind;
	uint32_t format;
	uint32_t total_bytes;
	uint32_t offset;
	uint32_t bytes;
	uint32_t start_ms;
	int32_t repeat;
	int32_t volume;
	unsigned char data[MRE_AUDIO_CHUNK_BYTES];
};

struct mre_audio_response {
	uint32_t sequence;
	int32_t result;
};

#endif
