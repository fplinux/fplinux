/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "fplinux-midi-internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

struct midi_reader {
	const uint8_t *bytes;
	size_t count;
	size_t position;
};

struct midi_parser {
	struct fplinux_midi_sequence *sequence;
	size_t capacity;
	uint32_t parsed_events;
	uint32_t last_tick;
};

static uint16_t read_be16(const uint8_t *bytes)
{
	return (uint16_t)((uint16_t)bytes[0] << 8 | bytes[1]);
}

static uint32_t read_be32(const uint8_t *bytes)
{
	return (uint32_t)bytes[0] << 24 | (uint32_t)bytes[1] << 16 |
	       (uint32_t)bytes[2] << 8 | bytes[3];
}

static int take_bytes(struct midi_reader *reader, size_t count,
		      const uint8_t **bytes)
{
	if (count > reader->count - reader->position) {
		errno = EINVAL;
		return -1;
	}
	*bytes = reader->bytes + reader->position;
	reader->position += count;
	return 0;
}

static int read_variable(struct midi_reader *reader, uint32_t *value)
{
	const uint8_t *byte;
	unsigned int i;

	*value = 0;
	for (i = 0; i < 4; i++) {
		if (take_bytes(reader, 1, &byte) < 0)
			return -1;
		*value = (*value << 7) | (*byte & 0x7fU);
		if (!(*byte & 0x80U))
			return 0;
	}
	errno = EINVAL;
	return -1;
}

static int append_event(struct midi_parser *parser,
			const struct fplinux_midi_event *event)
{
	struct fplinux_midi_sequence *sequence = parser->sequence;
	struct fplinux_midi_event *events;
	size_t capacity;

	if (sequence->count == parser->capacity) {
		capacity = parser->capacity ? parser->capacity * 2 : 128;
		if (capacity > FPLINUX_MIDI_EVENT_LIMIT)
			capacity = FPLINUX_MIDI_EVENT_LIMIT;
		events = realloc(sequence->events, capacity * sizeof(*events));
		if (!events)
			return -1;
		sequence->events = events;
		parser->capacity = capacity;
	}
	sequence->events[sequence->count++] = *event;
	return 0;
}

static int read_meta(struct midi_reader *reader,
		     struct fplinux_midi_event *event, bool *ended)
{
	const uint8_t *type, *data;
	uint32_t count;

	if (take_bytes(reader, 1, &type) < 0 ||
	    read_variable(reader, &count) < 0 ||
	    take_bytes(reader, count, &data) < 0)
		return -1;
	if (*type == 0x2f) {
		if (count || reader->position != reader->count) {
			errno = EINVAL;
			return -1;
		}
		*ended = true;
	} else if (*type == 0x51) {
		if (count != 3) {
			errno = EINVAL;
			return -1;
		}
		event->tempo = (uint32_t)data[0] << 16 |
			       (uint32_t)data[1] << 8 | data[2];
		if (!event->tempo) {
			errno = EINVAL;
			return -1;
		}
		event->status = FPLINUX_MIDI_TEMPO;
		return 1;
	}
	return 0;
}

static int read_sysex(struct midi_reader *reader,
		      struct fplinux_midi_event *event)
{
	const uint8_t *data;
	uint32_t count;

	if (read_variable(reader, &count) < 0 ||
	    take_bytes(reader, count, &data) < 0)
		return -1;
	/* Universal non-realtime General MIDI System On. */
	if (event->status == 0xf0 && count == 5 && data[0] == 0x7e &&
	    data[1] < 0x80 && data[2] == 0x09 && data[3] == 0x01 &&
	    data[4] == 0xf7) {
		event->status = FPLINUX_MIDI_RESET;
		return 1;
	}
	return 0;
}

static int read_track(struct midi_parser *parser, struct midi_reader *reader)
{
	const uint8_t *byte, *data;
	uint8_t running_status = 0;
	uint32_t tick = 0, delta;
	bool ended = false;

	while (reader->position < reader->count) {
		struct fplinux_midi_event event = { 0 };
		uint8_t status;
		size_t length;
		int append = 1;

		if (++parser->parsed_events > FPLINUX_MIDI_EVENT_LIMIT) {
			errno = EFBIG;
			return -1;
		}
		if (read_variable(reader, &delta) < 0 ||
		    take_bytes(reader, 1, &byte) < 0)
			return -1;
		if (delta > UINT32_MAX - tick) {
			errno = EFBIG;
			return -1;
		}
		tick += delta;
		event.tick = tick;
		event.order = parser->parsed_events;
		status = *byte;
		if (status < 0x80) {
			if (!running_status) {
				errno = EINVAL;
				return -1;
			}
			status = running_status;
			reader->position--;
		} else if (status < 0xf0) {
			running_status = status;
		} else {
			running_status = 0;
		}
		event.status = status;
		if (status == 0xff) {
			append = read_meta(reader, &event, &ended);
		} else if (status == 0xf0 || status == 0xf7) {
			append = read_sysex(reader, &event);
		} else if (status < 0xf0) {
			length = (status & 0xe0U) == 0xc0 ? 1 : 2;
			if (take_bytes(reader, length, &data) < 0)
				return -1;
			if (data[0] >= 0x80 ||
			    (length == 2 && data[1] >= 0x80)) {
				errno = EINVAL;
				return -1;
			}
			event.first = data[0];
			event.second = length == 2 ? data[1] : 0;
		} else {
			errno = EINVAL;
			return -1;
		}
		if (append < 0 || (append && append_event(parser, &event) < 0))
			return -1;
	}
	if (!ended) {
		errno = EINVAL;
		return -1;
	}
	if (tick > parser->last_tick)
		parser->last_tick = tick;
	return 0;
}

static int compare_events(const void *left, const void *right)
{
	const struct fplinux_midi_event *a = left, *b = right;

	if (a->tick != b->tick)
		return a->tick < b->tick ? -1 : 1;
	return a->order < b->order ? -1 : a->order > b->order;
}

static uint64_t time_frames(uint64_t numerator, uint64_t denominator)
{
	return numerator / denominator * FPLINUX_MIDI_RATE +
	       numerator % denominator * FPLINUX_MIDI_RATE / denominator;
}

static int set_event_times(struct midi_parser *parser, unsigned int division)
{
	struct fplinux_midi_sequence *sequence = parser->sequence;
	uint64_t numerator = 0, denominator = (uint64_t)division * 1000000;
	uint64_t limit = FPLINUX_MIDI_DURATION_US * division;
	uint32_t tick = 0, tempo = 500000;
	size_t i;

	if (sequence->count > 1)
		qsort(sequence->events, sequence->count,
		      sizeof(*sequence->events), compare_events);
	for (i = 0; i <= sequence->count; i++) {
		struct fplinux_midi_event *event =
			i < sequence->count ? &sequence->events[i] : NULL;
		uint32_t next_tick = event ? event->tick : parser->last_tick;
		uint64_t delta = (uint64_t)(next_tick - tick) * tempo;

		if (delta > limit - numerator) {
			errno = EFBIG;
			return -1;
		}
		numerator += delta;
		tick = next_tick;
		if (event) {
			event->frame = time_frames(numerator, denominator);
			if (event->status == FPLINUX_MIDI_TEMPO)
				tempo = event->tempo;
		}
	}
	sequence->duration_us = (numerator + division - 1) / division;
	sequence->duration_frames = time_frames(numerator, denominator);
	if (numerator % denominator * FPLINUX_MIDI_RATE % denominator)
		sequence->duration_frames++;
	return 0;
}

int fplinux_midi_parse_smf(const void *bytes, size_t count,
			   struct fplinux_midi_sequence *sequence)
{
	struct midi_reader reader = { .bytes = bytes, .count = count };
	struct midi_parser parser = { .sequence = sequence };
	const uint8_t *header, *track;
	uint32_t header_bytes;
	unsigned int format, tracks, division, i;
	int saved_errno;

	memset(sequence, 0, sizeof(*sequence));
	if (!bytes || count < 14) {
		errno = EINVAL;
		return -1;
	}
	if (count > FPLINUX_MIDI_RESOURCE_BYTES) {
		errno = EFBIG;
		return -1;
	}
	if (take_bytes(&reader, 8, &header) < 0)
		return -1;
	header_bytes = read_be32(header + 4);
	if (memcmp(header, "MThd", 4) || header_bytes < 6 ||
	    take_bytes(&reader, header_bytes, &header) < 0) {
		errno = EINVAL;
		return -1;
	}
	format = read_be16(header);
	tracks = read_be16(header + 2);
	division = read_be16(header + 4);
	if (format > 1 || (division & 0x8000U)) {
		errno = ENOTSUP;
		return -1;
	}
	if (!division || !tracks || (format == 0 && tracks != 1)) {
		errno = EINVAL;
		return -1;
	}
	if (tracks > FPLINUX_MIDI_TRACK_LIMIT) {
		errno = EFBIG;
		return -1;
	}
	for (i = 0; i < tracks; i++) {
		struct midi_reader track_reader = { 0 };

		if (take_bytes(&reader, 8, &header) < 0)
			goto failed;
		if (memcmp(header, "MTrk", 4)) {
			errno = EINVAL;
			goto failed;
		}
		track_reader.count = read_be32(header + 4);
		if (take_bytes(&reader, track_reader.count, &track) < 0)
			goto failed;
		track_reader.bytes = track;
		if (read_track(&parser, &track_reader) < 0)
			goto failed;
	}
	if (reader.position != reader.count) {
		errno = EINVAL;
		goto failed;
	}
	if (set_event_times(&parser, division) < 0)
		goto failed;
	return 0;

failed:
	saved_errno = errno;
	free(sequence->events);
	memset(sequence, 0, sizeof(*sequence));
	errno = saved_errno;
	return -1;
}
