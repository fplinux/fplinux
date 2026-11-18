/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef FPLINUX_MIDI_INTERNAL_H
#define FPLINUX_MIDI_INTERNAL_H

#include "fplinux-midi.h"

#define FPLINUX_MIDI_DURATION_US (24ULL * 60ULL * 60ULL * 1000000ULL)
#define FPLINUX_MIDI_RESET 0xf0U
#define FPLINUX_MIDI_TEMPO 0xffU

struct fplinux_midi_event {
	uint64_t frame;
	uint32_t tick;
	uint32_t order;
	uint32_t tempo;
	uint8_t status;
	uint8_t first;
	uint8_t second;
};

struct fplinux_midi_sequence {
	struct fplinux_midi_event *events;
	struct fplinux_midi_tone *tones;
	size_t count;
	uint64_t duration_us;
	uint64_t duration_frames;
};

struct fplinux_midi_tone {
	uint64_t end_frame;
	uint32_t step;
	unsigned int volume;
};

int fplinux_midi_parse_smf(const void *bytes, size_t count,
			   struct fplinux_midi_sequence *sequence);
int fplinux_midi_make_tone(unsigned int note, uint64_t duration_ms,
			   struct fplinux_midi_sequence *sequence);
int fplinux_midi_parse_tones(const void *bytes, size_t count,
			     struct fplinux_midi_sequence *sequence);

#endif
