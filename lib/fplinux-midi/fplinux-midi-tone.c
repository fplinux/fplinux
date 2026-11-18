/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "fplinux-midi-internal.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define TONE_SILENCE 0xffU
#define TONE_VERSION 0xfeU
#define TONE_TEMPO 0xfdU
#define TONE_RESOLUTION 0xfcU
#define TONE_BLOCK_START 0xfbU
#define TONE_BLOCK_END 0xfaU
#define TONE_PLAY_BLOCK 0xf9U
#define TONE_SET_VOLUME 0xf8U
#define TONE_REPEAT 0xf7U
#define TONE_BLOCK_COUNT 128U

struct tone_range {
	size_t begin;
	size_t end;
};

struct tone_parser {
	const uint8_t *bytes;
	size_t count;
	struct tone_range blocks[TONE_BLOCK_COUNT];
	struct fplinux_midi_sequence *sequence;
	size_t capacity;
	uint64_t units;
	unsigned int volume;
	unsigned int tempo;
	unsigned int resolution;
	unsigned int commands;
};

static uint32_t note_step(unsigned int note)
{
	double frequency = 440.0 * pow(2.0, ((double)note - 69.0) / 12.0);

	return (uint32_t)(frequency * 4294967296.0 / FPLINUX_MIDI_TONE_RATE);
}

static int validate_note(uint8_t note, uint8_t duration)
{
	if ((note > 127 && note != TONE_SILENCE) || !duration ||
	    duration > 127) {
		errno = EINVAL;
		return -1;
	}
	return 0;
}

static int validate_event(struct tone_parser *parser, size_t *position,
			  size_t end)
{
	uint8_t command, value;

	if (end - *position < 2) {
		errno = EINVAL;
		return -1;
	}
	if (++parser->commands > FPLINUX_MIDI_EVENT_LIMIT) {
		errno = EFBIG;
		return -1;
	}
	command = parser->bytes[*position];
	value = parser->bytes[*position + 1];
	*position += 2;
	if (command <= 127 || command == TONE_SILENCE)
		return validate_note(command, value);
	if (command == TONE_SET_VOLUME && value <= 100)
		return 0;
	if (command == TONE_PLAY_BLOCK && value < TONE_BLOCK_COUNT &&
	    parser->blocks[value].end)
		return 0;
	if (command == TONE_REPEAT && value >= 2 && value <= 127 &&
	    end - *position >= 2) {
		command = parser->bytes[*position];
		value = parser->bytes[*position + 1];
		*position += 2;
		return validate_note(command, value);
	}
	errno = EINVAL;
	return -1;
}

static int append_note(struct tone_parser *parser, uint8_t note,
		       uint8_t duration)
{
	struct fplinux_midi_sequence *sequence = parser->sequence;
	struct fplinux_midi_tone *tones;
	uint64_t numerator, denominator = parser->resolution * parser->tempo;
	size_t capacity;

	if (sequence->count == FPLINUX_MIDI_EVENT_LIMIT) {
		errno = EFBIG;
		return -1;
	}
	parser->units += duration;
	numerator = parser->units * 240000000ULL;
	if (numerator > FPLINUX_MIDI_DURATION_US * denominator) {
		errno = EFBIG;
		return -1;
	}
	if (sequence->count == parser->capacity) {
		capacity = parser->capacity ? parser->capacity * 2 : 128;
		if (capacity > FPLINUX_MIDI_EVENT_LIMIT)
			capacity = FPLINUX_MIDI_EVENT_LIMIT;
		tones = realloc(sequence->tones, capacity * sizeof(*tones));
		if (!tones)
			return -1;
		sequence->tones = tones;
		parser->capacity = capacity;
	}
	sequence->duration_us = (numerator + denominator - 1) / denominator;
	denominator *= 1000000;
	sequence->duration_frames =
		numerator / denominator * FPLINUX_MIDI_TONE_RATE +
		(numerator % denominator * FPLINUX_MIDI_TONE_RATE +
		 denominator - 1) /
			denominator;
	sequence->tones[sequence->count++] = (struct fplinux_midi_tone){
		.end_frame = sequence->duration_frames,
		.step = note == TONE_SILENCE ? 0 : note_step(note),
		.volume = note == TONE_SILENCE ? 0 : parser->volume,
	};
	return 0;
}

static int expand_sequence(struct tone_parser *parser, size_t begin)
{
	struct tone_range stack[TONE_BLOCK_COUNT + 1] = {
		{ .begin = begin, .end = parser->count },
	};
	size_t depth = 0;

	parser->commands = 0;
	for (;;) {
		struct tone_range *range = &stack[depth];
		uint8_t command, value;
		unsigned int repeats = 1;

		if (range->begin == range->end) {
			if (!depth)
				return 0;
			depth--;
			continue;
		}
		if (++parser->commands > FPLINUX_MIDI_EVENT_LIMIT) {
			errno = EFBIG;
			return -1;
		}
		command = parser->bytes[range->begin];
		value = parser->bytes[range->begin + 1];
		range->begin += 2;
		if (command == TONE_SET_VOLUME) {
			parser->volume = value;
			continue;
		}
		if (command == TONE_PLAY_BLOCK) {
			if (depth == TONE_BLOCK_COUNT) {
				errno = EFBIG;
				return -1;
			}
			stack[++depth] = parser->blocks[value];
			continue;
		}
		if (command == TONE_REPEAT) {
			repeats = value;
			command = parser->bytes[range->begin];
			value = parser->bytes[range->begin + 1];
			range->begin += 2;
		}
		while (repeats--) {
			if (append_note(parser, command, value) < 0)
				return -1;
		}
	}
}

int fplinux_midi_parse_tones(const void *bytes, size_t count,
			     struct fplinux_midi_sequence *sequence)
{
	struct tone_parser parser = {
		.bytes = bytes,
		.count = count,
		.sequence = sequence,
		.volume = 100,
		.tempo = 120,
		.resolution = 64,
	};
	size_t position = 2, begin;
	int saved_errno;

	memset(sequence, 0, sizeof(*sequence));
	if (!bytes || count < 4 || count % 2) {
		errno = EINVAL;
		return -1;
	}
	if (count > FPLINUX_MIDI_RESOURCE_BYTES) {
		errno = EFBIG;
		return -1;
	}
	if (parser.bytes[0] != TONE_VERSION || parser.bytes[1] != 1) {
		errno = EINVAL;
		return -1;
	}
	if (position < count && parser.bytes[position] == TONE_TEMPO) {
		unsigned int modifier = parser.bytes[position + 1];

		if (modifier < 5 || modifier > 127) {
			errno = EINVAL;
			return -1;
		}
		parser.tempo = modifier * 4;
		position += 2;
	}
	if (position < count && parser.bytes[position] == TONE_RESOLUTION) {
		parser.resolution = parser.bytes[position + 1];
		if (!parser.resolution || parser.resolution > 127) {
			errno = EINVAL;
			return -1;
		}
		position += 2;
	}
	while (position < count && parser.bytes[position] == TONE_BLOCK_START) {
		unsigned int block = parser.bytes[position + 1];
		struct tone_range range;

		if (block >= TONE_BLOCK_COUNT || parser.blocks[block].end) {
			errno = EINVAL;
			return -1;
		}
		position += 2;
		range.begin = position;
		while (position < count &&
		       parser.bytes[position] != TONE_BLOCK_END) {
			if (validate_event(&parser, &position, count) < 0)
				return -1;
		}
		if (position == count || range.begin == position ||
		    parser.bytes[position + 1] != block) {
			errno = EINVAL;
			return -1;
		}
		range.end = position;
		parser.blocks[block] = range;
		position += 2;
	}
	begin = position;
	if (begin == count) {
		errno = EINVAL;
		return -1;
	}
	while (position < count) {
		if (validate_event(&parser, &position, count) < 0)
			return -1;
	}
	if (expand_sequence(&parser, begin) == 0)
		return 0;
	saved_errno = errno;
	free(sequence->tones);
	memset(sequence, 0, sizeof(*sequence));
	errno = saved_errno;
	return -1;
}

int fplinux_midi_make_tone(unsigned int note, uint64_t duration_ms,
			   struct fplinux_midi_sequence *sequence)
{
	if (note > 127 || !duration_ms ||
	    duration_ms > FPLINUX_MIDI_DURATION_US / 1000) {
		errno = EINVAL;
		return -1;
	}
	memset(sequence, 0, sizeof(*sequence));
	sequence->tones = calloc(1, sizeof(*sequence->tones));
	if (!sequence->tones)
		return -1;
	sequence->count = 1;
	sequence->duration_us = duration_ms * 1000;
	sequence->duration_frames =
		(duration_ms * FPLINUX_MIDI_TONE_RATE + 999) / 1000;
	sequence->tones[0] = (struct fplinux_midi_tone){
		.end_frame = sequence->duration_frames,
		.step = note_step(note),
		.volume = 100,
	};
	return 0;
}
