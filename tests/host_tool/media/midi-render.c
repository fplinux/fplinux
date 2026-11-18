/* SPDX-License-Identifier: GPL-2.0-only */
#ifdef FPLINUX_MIDI_SYNTH_SEMANTICS
#include <assert.h>
#include <fcntl.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

union allocation_header {
	size_t size;
	max_align_t alignment;
};

static size_t current_bytes, peak_bytes, copy_pool_bytes;
static unsigned int allocation_attempt, fail_allocation;

static void *tracked_malloc(size_t size)
{
	union allocation_header *header;

	if (++allocation_attempt == fail_allocation)
		return NULL;
	header = malloc(sizeof(*header) + size);
	if (!header)
		return NULL;
	header->size = size;
	current_bytes += size;
	if (current_bytes > peak_bytes)
		peak_bytes = current_bytes;
	return header + 1;
}

static void tracked_free(void *pointer)
{
	union allocation_header *header;

	if (!pointer)
		return;
	header = (union allocation_header *)pointer - 1;
	current_bytes -= header->size;
	free(header);
}

static void *tracked_realloc(void *pointer, size_t size)
{
	union allocation_header *header;
	size_t old;

	if (!pointer)
		return tracked_malloc(size);
	if (++allocation_attempt == fail_allocation)
		return NULL;
	header = (union allocation_header *)pointer - 1;
	old = header->size;
	header = realloc(header, sizeof(*header) + size);
	if (!header)
		return NULL;
	header->size = size;
	current_bytes = current_bytes - old + size;
	if (current_bytes > peak_bytes)
		peak_bytes = current_bytes;
	return header + 1;
}

#define TSF_MALLOC tracked_malloc
#define TSF_REALLOC tracked_realloc
#define TSF_FREE tracked_free
#define TSF_IMPLEMENTATION
#include <tsf.h>

static struct tsf_voice *first_voice(tsf *synth)
{
	int i;

	for (i = 0; i < synth->voiceNum; i++) {
		if (synth->voices[i].playingPreset != -1)
			return &synth->voices[i];
	}
	assert(!"expected a sounding sample voice");
	return NULL;
}

static void note(tsf *synth, int preset, float velocity)
{
	tsf_reset(synth);
	assert(tsf_channel_set_presetnumber(synth, 0, preset, 0));
	assert(tsf_channel_midi_control(synth, 0, 7, 127));
	assert(tsf_channel_note_on(synth, 0, 69, velocity));
}

static void near(float actual, float expected)
{
	assert(fabsf(actual - expected) < 0.0001f);
}

static void check_routes(tsf *synth)
{
	struct tsf_voice *voice;
	float samples[2048];
	float initial;

	note(synth, 66, 1.0f);
	voice = first_voice(synth);
	assert(voice->region->ccModPitch == -10);
	assert(voice->region->ccVibPitch == -50);
	assert(voice->region->pressureModPitch == 10);
	assert(voice->region->pressureVibPitch == 50);
	assert(tsf_channel_midi_control(synth, 0, 1, 127));
	near(voice->modPitchDepth - voice->region->modLfoToPitch, -9.921875f);
	near(voice->vibPitchDepth - voice->region->vibLfoToPitch, -49.609375f);
	assert(tsf_channel_set_pressure(synth, 0, 127));
	near(voice->modPitchDepth - voice->region->modLfoToPitch, 0);
	near(voice->vibPitchDepth - voice->region->vibLfoToPitch, 0);
	assert(tsf_channel_midi_control(synth, 0, 1, 0));
	near(voice->modPitchDepth - voice->region->modLfoToPitch, 9.921875f);
	near(voice->vibPitchDepth - voice->region->vibLfoToPitch, 49.609375f);
	note(synth, 65, 1.0f);
	voice = first_voice(synth);
	assert(voice->region->ccVibPitch == 0 &&
	       voice->region->pressureVibPitch == 0);
	assert(tsf_channel_midi_control(synth, 0, 1, 127));
	assert(tsf_channel_set_pressure(synth, 0, 127));
	near(voice->vibPitchDepth - voice->region->vibLfoToPitch, 0);
	note(synth, 38, 1.0f);
	voice = first_voice(synth);
	assert(voice->region->ccVibPitch == 50 &&
	       voice->region->pressureVibPitch == 50);
	initial = voice->viblfo.level;
	tsf_render_float(synth, samples, 1024, 0);
	assert(voice->viblfo.level != initial);
	assert(tsf_channel_midi_control(synth, 0, 1, 127));
	assert(tsf_channel_set_pressure(synth, 0, 64));
	near(voice->vibPitchDepth - voice->region->vibLfoToPitch, 74.609375f);
	assert(tsf_channel_midi_control(synth, 0, 121, 0));
	near(voice->vibPitchDepth - voice->region->vibLfoToPitch, 0);
	tsf_reset(synth);
	assert(tsf_channel_set_presetnumber(synth, 9, 0, 1));
	assert(tsf_channel_midi_control(synth, 9, 1, 127));
	assert(tsf_channel_note_on(synth, 9, 38, 1.0f));
	voice = first_voice(synth);
	assert(voice->playingChannel == 9);
	near(voice->vibPitchDepth - voice->region->vibLfoToPitch, 49.609375f);
}

static void check_gain_reset_and_voice_limit(tsf *synth)
{
	struct tsf_voice *voice;
	float full, half, full_volume;
	unsigned int newest = 0;
	float samples[2048];
	int i;

	note(synth, 0, 1.0f);
	voice = first_voice(synth);
	full = voice->noteGainDB;
	near(synth->globalGainDB, -13.979400f);
	note(synth, 0, 0.5f);
	half = first_voice(synth)->noteGainDB;
	near(half - full, -12.041200f);
	full_volume = tsf_channel_get_volume(synth, 0);
	assert(tsf_channel_midi_control(synth, 0, 7, 64));
	near(tsf_channel_get_volume(synth, 0) / full_volume, 0.2539525f);
	assert(tsf_channel_midi_control(synth, 0, 7, 0));
	near(tsf_channel_get_volume(synth, 0), 0);
	tsf_reset(synth);
	assert(tsf_channel_set_presetnumber(synth, 0, 0, 0));
	near(tsf_channel_get_volume(synth, 0), 0.6200012f);
	for (i = 0; i < 100; i++)
		assert(tsf_channel_note_on(synth, 0, 40 + i % 48, 1.0f));
	assert(tsf_active_voice_count(synth) == 32);
	for (i = 0; i < synth->voiceNum; i++) {
		if (synth->voices[i].playIndex == synth->voicePlayIndex - 1)
			newest++;
	}
	assert(newest > 0);
	tsf_channel_sounds_off_all(synth, 0);
	assert(tsf_active_voice_count(synth) == 0);
	tsf_render_float(synth, samples, 1024, 0);
	for (i = 0; i < 2048; i++)
		assert(samples[i] == 0);
}

static void check_copy_storage_and_preparation_failures(tsf *synth)
{
	tsf *copy, *pool[16];
	size_t retained;
	unsigned int failure, i;
	int16_t samples[2048];

	copy = tsf_copy(synth);
	assert(copy && copy->presets == synth->presets &&
	       copy->fontSamples == synth->fontSamples);
	assert(!copy->voices && !copy->channels);
	assert(tsf_set_max_voices(copy, 32));
	assert(tsf_channel_set_presetnumber(copy, 15, 0, 0));
	assert(copy->voices != synth->voices &&
	       copy->channels != synth->channels);
	assert(tsf_channel_set_presetnumber(copy, 0, 80, 0));
	assert(tsf_channel_note_on(copy, 0, 69, 1.0f));
	tsf_reset(synth);
	assert(tsf_active_voice_count(copy) > 0);
	tsf_render_short(copy, samples, 1024, 0);
	assert(tsf_active_voice_count(copy) > 0);
	tsf_close(copy);
	retained = current_bytes;
	for (i = 0; i < 16; i++) {
		pool[i] = tsf_copy(synth);
		assert(pool[i] && pool[i]->presets == synth->presets &&
		       pool[i]->fontSamples == synth->fontSamples);
		assert(tsf_set_max_voices(pool[i], 160));
		assert(tsf_channel_set_presetnumber(pool[i], 15, 0, 0));
	}
	copy_pool_bytes = current_bytes;
	/* Sixteen prepared contexts retain one bank and bounded private heaps. */
	assert(copy_pool_bytes - retained < 16U * 65536U);
	assert(copy_pool_bytes < 2U * 1024U * 1024U);
	for (i = 0; i < 16; i++)
		tsf_close(pool[i]);
	assert(current_bytes == retained);
	fail_allocation = allocation_attempt + 1;
	assert(!tsf_copy(synth));
	assert(current_bytes == retained);
	fail_allocation = 0;
	for (failure = 0; failure < 2; failure++) {
		copy = tsf_copy(synth);
		assert(copy);
		fail_allocation = allocation_attempt + 1;
		if (!failure)
			assert(!tsf_set_max_voices(copy, 32));
		else
			assert(!tsf_channel_set_presetnumber(copy, 15, 0, 0));
		fail_allocation = 0;
		tsf_close(copy);
		assert(current_bytes == retained);
	}
}

int main(void)
{
	static const int drum_programs[] = { 0, 8, 16, 24, 25, 32, 40, 48 };
	struct stat status;
	tsf *synth;
	void *mapping;
	unsigned int regions = 0;
	size_t ready;
	unsigned int prepared_allocations;
	int fd, i;

	fd = open(FPLINUX_MIDI_SOUNDFONT, O_RDONLY);
	assert(fd >= 0 && fstat(fd, &status) == 0);
	mapping = mmap(NULL, (size_t)status.st_size, PROT_READ, MAP_PRIVATE, fd,
		       0);
	assert(mapping != MAP_FAILED);
	close(fd);
	synth = tsf_load_memory(mapping, (int)status.st_size);
	assert(synth);
	assert((const char *)synth->fontSamples >= (const char *)mapping);
	assert((const char *)synth->fontSamples <
	       (const char *)mapping + status.st_size);
	assert(tsf_set_max_voices(synth, 32));
	tsf_set_output(synth, TSF_STEREO_INTERLEAVED, 22050, 0);
	tsf_set_volume(synth, 0.2f);
	for (i = 0; i < 128; i++)
		assert(tsf_get_presetindex(synth, 0, i) >= 0);
	for (i = 0; i < 8; i++)
		assert(tsf_get_presetindex(synth, 128, drum_programs[i]) >= 0);
	for (i = 0; i < 16; i++)
		assert(tsf_channel_set_presetnumber(synth, i, 0, i == 9));
	assert(tsf_channel_get_preset_bank(synth, 9) == 0);
	assert(synth->presets[tsf_channel_get_preset_index(synth, 9)].bank ==
	       128);
	for (i = 0; i < synth->presetNum; i++)
		regions += (unsigned int)synth->presets[i].regionNum;
	assert(regions == 2063);
	ready = current_bytes;
	prepared_allocations = allocation_attempt;
	assert(ready < 524288 && peak_bytes < 1048576);
	check_routes(synth);
	check_gain_reset_and_voice_limit(synth);
	assert(allocation_attempt == prepared_allocations);
	check_copy_storage_and_preparation_failures(synth);
	tsf_close(synth);
	assert(current_bytes == 0);
	for (fail_allocation = 1; fail_allocation < 256; fail_allocation++) {
		allocation_attempt = 0;
		synth = tsf_load_memory(mapping, (int)status.st_size);
		if (synth) {
			tsf_close(synth);
			assert(current_bytes == 0);
			break;
		}
		assert(current_bytes == 0);
	}
	assert(fail_allocation > 1 && fail_allocation < 256);
	assert(munmap(mapping, (size_t)status.st_size) == 0);
	printf("{\"ready_bytes\":%zu,\"peak_bytes\":%zu,\"copy_pool_bytes\":%zu,\"region_bytes\":%zu,\"voice_bytes\":%zu}\n",
	       ready, peak_bytes, copy_pool_bytes, sizeof(struct tsf_region),
	       sizeof(struct tsf_voice));
	return 0;
}

#else
#include "fplinux-midi-internal.h"

#include <assert.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include <tsf.h>

static unsigned int synth_creations;
static unsigned int synth_deletions;
static unsigned int synth_copies;
static unsigned int live_synths;
static unsigned int fail_preparation;
static unsigned int bank_loads;
static unsigned int bank_unmaps;
static const void *mapped_bank;
static size_t mapped_bytes;

tsf *__real_tsf_load_memory(const void *bytes, int count);
tsf *__real_tsf_copy(tsf *synth);
void __real_tsf_close(tsf *synth);
int __real_tsf_set_max_voices(tsf *synth, int count);
int __real_tsf_channel_set_presetnumber(tsf *synth, int channel, int preset,
					int drums);
void *__real_mmap(void *address, size_t length, int protection, int flags,
		  int fd, off_t offset);
int __real_munmap(void *address, size_t length);

/* Observe the actual borrowed bank lifetime without replacing synthesis. */
tsf *__wrap_tsf_load_memory(const void *bytes, int count)
{
	tsf *synth;

	assert(bytes == mapped_bank && (size_t)count == mapped_bytes);
	synth_creations++;
	synth = __real_tsf_load_memory(bytes, count);
	if (synth)
		live_synths++;
	return synth;
}

tsf *__wrap_tsf_copy(tsf *synth)
{
	tsf *copy;

	if (fail_preparation == 1) {
		fail_preparation = 0;
		return NULL;
	}
	copy = __real_tsf_copy(synth);
	if (copy) {
		synth_copies++;
		live_synths++;
	}
	return copy;
}

int __wrap_tsf_set_max_voices(tsf *synth, int count)
{
	if (fail_preparation == 2) {
		fail_preparation = 0;
		return 0;
	}
	return __real_tsf_set_max_voices(synth, count);
}

int __wrap_tsf_channel_set_presetnumber(tsf *synth, int channel, int preset,
					int drums)
{
	if (fail_preparation == 3) {
		fail_preparation = 0;
		return 0;
	}
	return __real_tsf_channel_set_presetnumber(synth, channel, preset,
						   drums);
}

void __wrap_tsf_close(tsf *synth)
{
	synth_deletions++;
	assert(live_synths > 0);
	live_synths--;
	__real_tsf_close(synth);
}

void *__wrap_mmap(void *address, size_t length, int protection, int flags,
		  int fd, off_t offset)
{
	void *mapping =
		__real_mmap(address, length, protection, flags, fd, offset);

	assert(protection == PROT_READ && flags == MAP_PRIVATE && offset == 0);
	if (mapping != MAP_FAILED) {
		bank_loads++;
		mapped_bank = mapping;
		mapped_bytes = length;
	}
	return mapping;
}

int __wrap_munmap(void *address, size_t length)
{
	assert(address == mapped_bank && length == mapped_bytes);
	bank_unmaps++;
	assert(live_synths == 0);
	return __real_munmap(address, length);
}

/* 96 PPQN: one quarter at 120 BPM, then one at 60 BPM = 1.5 seconds. */
static const uint8_t tempo_song[] = {
	'M',  'T',  'h',  'd',	0,    0,   0,	 6,    0,    1,	   0,	 2,
	0,    96,   'M',  'T',	'r',  'k', 0,	 0,    0,    18,   0,	 0xff,
	0x51, 3,    0x07, 0xa1, 0x20, 96,  0xff, 0x51, 3,    0x0f, 0x42, 0x40,
	96,   0xff, 0x2f, 0,	'M',  'T', 'r',	 'k',  0,    0,	   0,	 23,
	0,    0xc0, 0,	  0,	0x90, 69,  100,	 96,   0x80, 69,   0,	 0,
	0x90, 72,   100,  96,	0x80, 72,  0,	 0,    0xff, 0x2f, 0,
};

static const uint8_t fractional_song[] = {
	'M',  'T',  'h',  'd', 0,    0,	 0,   6, 0,    0,    0,	   1,	 0,
	3,    'M',  'T',  'r', 'k',  0,	 0,   0, 15,   0,    0xff, 0x51, 3,
	0x07, 0xa1, 0x21, 0,   0x90, 69, 100, 3, 0xff, 0x2f, 0,
};

/* VERSION 1, 120 BPM, 1/64 notes. A block, rest, volume and repeated notes. */
static const uint8_t tone_sequence[] = {
	0xfe, 1,    0xfd, 30,	0xfc, 64,   0xfb, 0,	69, 16, 0xff,
	8,    0xfa, 0,	  0xf8, 50,   0xf9, 0,	  0xf7, 2,  72, 8,
};

static struct fplinux_midi_info get_info(struct fplinux_midi *midi)
{
	struct fplinux_midi_info info;

	assert(fplinux_midi_get_info(midi, &info) == 0);
	return info;
}

static ssize_t read_block(struct fplinux_midi *midi, int16_t *samples,
			  size_t capacity)
{
	unsigned int attempts;
	ssize_t count;

	for (attempts = 0; attempts < 10000; attempts++) {
		count = fplinux_midi_read(midi, samples, capacity);
		if (count >= 0 || errno != EAGAIN)
			return count;
	}
	assert(!"bounded renderer did not progress");
	return -1;
}

static bool has_signal(const int16_t *samples, size_t frames)
{
	size_t i;

	for (i = 0; i < frames * 2; i++) {
		if (samples[i] > 100 || samples[i] < -100)
			return true;
	}
	return false;
}

static struct fplinux_midi *open_track(const uint8_t *track, size_t count)
{
	uint8_t song[512] = {
		'M', 'T', 'h', 'd', 0,	 0,   0,   6, 0, 0, 0,
		1,   0,	  100, 'M', 'T', 'r', 'k', 0, 0, 0, 0,
	};

	assert(count <= sizeof(song) - 22);
	song[20] = (uint8_t)(count >> 8);
	song[21] = (uint8_t)count;
	memcpy(song + 22, track, count);
	return fplinux_midi_open_bytes(song, count + 22);
}

static void check_layered_string_chord_preserves_individual_notes(void)
{
	static const uint8_t notes[] = {
		48, 52, 55, 59, 60, 64, 67, 71, 72, 76, 79, 83,
	};
	static const uint8_t ending[] = { 0x81, 0x48, 0xff, 0x2f, 0 };
	uint8_t single_track[] = {
		0, 0xc0, 48, 0, 0x90, 48, 48, 0x81, 0x48, 0xff, 0x2f, 0,
	};
	uint8_t chord_track[3 + sizeof(notes) * 4 + sizeof(ending)] = {
		0,
		0xc0,
		48,
	};
	int32_t expected[8][2048] = { 0 };
	int16_t samples[2048];
	struct fplinux_midi *midi;
	unsigned int peak_sum = 0, block;
	int maximum_error = 0;
	size_t note, i;

	for (note = 0; note < sizeof(notes); note++) {
		unsigned int note_peak = 0;

		chord_track[3 + note * 4] = 0;
		chord_track[4 + note * 4] = 0x90;
		chord_track[5 + note * 4] = notes[note];
		chord_track[6 + note * 4] = 48;
		single_track[5] = notes[note];
		midi = open_track(single_track, sizeof(single_track));
		assert(midi && fplinux_midi_start(midi) == 0);
		for (block = 0; block < 8; block++) {
			assert(read_block(midi, samples, 1024) == 1024);
			for (i = 0; i < 2048; i++) {
				unsigned int magnitude =
					(unsigned int)abs(samples[i]);

				expected[block][i] += samples[i];
				if (magnitude > note_peak)
					note_peak = magnitude;
			}
		}
		assert(note_peak > 0);
		peak_sum += note_peak;
		fplinux_midi_close(midi);
	}
	/* Sum of individual peaks bounds every mixed sample below clipping. */
	assert(peak_sum > 100 && peak_sum < 16000);
	memcpy(chord_track + 3 + sizeof(notes) * 4, ending, sizeof(ending));
	midi = open_track(chord_track, sizeof(chord_track));
	assert(midi && fplinux_midi_start(midi) == 0);
	for (block = 0; block < 8; block++) {
		assert(read_block(midi, samples, 1024) == 1024);
		for (i = 0; i < 2048; i++) {
			int error = abs(samples[i] - expected[block][i]);

			if (error > maximum_error)
				maximum_error = error;
		}
	}
	fplinux_midi_close(midi);
	if (maximum_error > 16)
		fprintf(stderr,
			"layered string chord maximum error %d LSB, peak bound %u\n",
			maximum_error, peak_sum);
	/* Allow twelve S16 roundings and a margin for float accumulation. */
	assert(maximum_error <= 16);
}

static void check_player_pcm_isolation_and_close_orders(void)
{
	static const uint8_t tracks[][36] = {
		{ 0,   0xc0, 0,	   0,	 0xb0, 10, 0, 0,    0x90, 69,
		  100, 0x83, 0x10, 0x80, 69,   0,  0, 0xff, 0x2f, 0 },
		{ 0,   0xc0, 80,   0,	 0xb0, 10,   127, 0,	0xb0, 11,   96,
		  0,   0xb0, 64,   127,	 0,    0xe0, 0,	  96,	0,    0x90, 76,
		  100, 0x83, 0x10, 0x90, 76,   0,    0,	  0xff, 0x2f, 0 },
	};
	static const size_t lengths[] = { 20, 32 };
	int16_t reference[2][10][1024], samples[1024];
	struct fplinux_midi *players[2], *single;
	unsigned int i, block, close_first;

	for (i = 0; i < 2; i++) {
		single = open_track(tracks[i], lengths[i]);
		assert(single && fplinux_midi_start(single) == 0);
		for (block = 0; block < 10; block++)
			assert(read_block(single, reference[i][block], 512) ==
			       512);
		assert(has_signal(reference[i][0], 512));
		fplinux_midi_close(single);
	}
	assert(memcmp(reference[0][0], reference[1][0], sizeof(samples)) != 0);
	for (close_first = 0; close_first < 2; close_first++) {
		for (i = 0; i < 2; i++) {
			players[i] = open_track(tracks[i], lengths[i]);
			assert(players[i] &&
			       fplinux_midi_start(players[i]) == 0);
		}
		for (block = 0; block < 10; block++) {
			for (i = 0; i < 2; i++) {
				if (i == close_first &&
				    (block == 3 || block >= 7))
					continue;
				assert(read_block(players[i], samples, 512) ==
				       512);
				if (i != close_first || block < 2)
					assert(!memcmp(samples,
						       reference[i][block],
						       sizeof(samples)));
				if (i == close_first && block == 2)
					assert(!has_signal(samples, 512));
			}
			if (block == 1)
				assert(fplinux_midi_set_gain(
					       players[close_first], 0) == 0);
			if (block == 2)
				assert(fplinux_midi_stop(
					       players[close_first]) == 0);
			if (block == 3) {
				assert(fplinux_midi_seek(players[close_first],
							 20) == 1);
				assert(fplinux_midi_start(
					       players[close_first]) == 0);
			}
			if (block == 6)
				fplinux_midi_close(players[close_first]);
		}
		fplinux_midi_close(players[1 - close_first]);
		assert(live_synths == 0);
	}
}

static void check_preparation_failure_preserves_other_player(void)
{
	struct fplinux_midi *playing, *failed;
	int16_t expected[2048], actual[2048];
	unsigned int stage;

	for (stage = 1; stage <= 3; stage++) {
		failed =
			fplinux_midi_open_bytes(tempo_song, sizeof(tempo_song));
		assert(failed);
		fail_preparation = stage;
		assert(fplinux_midi_prefetch(failed) == -1 && errno == ENOMEM);
		assert(get_info(failed).state == FPLINUX_MIDI_FAILED);
		assert(live_synths == 0);
		fplinux_midi_close(failed);
		playing =
			fplinux_midi_open_bytes(tempo_song, sizeof(tempo_song));
		assert(playing && fplinux_midi_start(playing) == 0);
		assert(read_block(playing, expected, 1024) == 1024);
		assert(fplinux_midi_stop(playing) == 0);
		assert(fplinux_midi_start(playing) == 0);
		failed =
			fplinux_midi_open_bytes(tempo_song, sizeof(tempo_song));
		assert(failed);
		fail_preparation = stage;
		assert(fplinux_midi_prefetch(failed) == -1 && errno == ENOMEM);
		assert(get_info(playing).state == FPLINUX_MIDI_PLAYING);
		assert(live_synths == 2);
		assert(read_block(playing, actual, 1024) == 1024);
		assert(!memcmp(expected, actual, sizeof(expected)));
		fplinux_midi_close(failed);
		fplinux_midi_close(playing);
		assert(live_synths == 0);
	}
}

static void check_literal_tempo_frames_and_track_order(void)
{
	static const uint8_t song[] = {
		'M',  'T',  'h',  'd',	0,    0,    0,	  6,	0,    1,
		0,    2,    1,	  0xe0, 'M',  'T',  'r',  'k',	0,    0,
		0,    28,   0,	  0xff, 0x51, 3,    0x07, 0xa1, 0x20, 0x83,
		0x60, 0xff, 0x51, 3,	0x0f, 0x42, 0x40, 0x83, 0x60, 0xff,
		0x51, 3,    0x03, 0xd0, 0x90, 0x83, 0x60, 0xff, 0x2f, 0,
		'M',  'T',  'r',  'k',	0,    0,    0,	  26,	0,    0xc0,
		80,   0,    0x90, 69,	100,  0x83, 0x60, 0xb0, 7,    0,
		0x83, 0x60, 0xb0, 7,	127,  0x83, 0x60, 0x90, 69,   0,
		0,    0xff, 0x2f, 0,
	};
	static const uint64_t frames[] = { 0,	  0,	 0,	11025,
					   11025, 33075, 33075, 38587 };
	static const uint8_t statuses[] = { 0xff, 0xc0, 0x90, 0xff,
					    0xb0, 0xff, 0xb0, 0x90 };
	struct fplinux_midi_sequence sequence;
	struct fplinux_midi *midi;
	int16_t samples[2048];
	uint64_t position = 0;
	size_t i;
	ssize_t count;
	bool sounded_before = false, sounded_after = false;

	assert(fplinux_midi_parse_smf(song, sizeof(song), &sequence) == 0);
	assert(sequence.count == 8 && sequence.duration_us == 1750000);
	assert(sequence.duration_frames == 38588);
	for (i = 0; i < 8; i++) {
		assert(sequence.events[i].frame == frames[i]);
		assert(sequence.events[i].status == statuses[i]);
	}
	free(sequence.events);
	midi = fplinux_midi_open_bytes(song, sizeof(song));
	assert(midi && fplinux_midi_start(midi) == 0);
	while (position < 38587) {
		count = read_block(midi, samples, 1024);
		assert(count > 0);
		for (i = 0; i < (size_t)count; i++) {
			uint64_t frame = position + i;

			if (frame >= 11025 && frame < 33075)
				assert(samples[i * 2] == 0 &&
				       samples[i * 2 + 1] == 0);
			if (frame < 11025 &&
			    (samples[i * 2] > 100 || samples[i * 2] < -100))
				sounded_before = true;
			if (frame >= 33075 &&
			    (samples[i * 2] > 100 || samples[i * 2] < -100))
				sounded_after = true;
		}
		position += (uint64_t)count;
	}
	assert(sounded_before && sounded_after);
	fplinux_midi_close(midi);
}

static double estimate_period(const int16_t samples[6615])
{
	double best_error = 1e100, best_period = 0;
	unsigned int step, i;

	for (step = 200; step <= 600; step++) {
		double period = step / 10.0, error = 0;
		unsigned int lag = step / 10;
		double fraction = period - lag;

		for (i = 0; i < 6550; i++) {
			double delayed = samples[i + lag] * (1 - fraction) +
					 samples[i + lag + 1] * fraction;
			double difference = samples[i] - delayed;

			error += difference * difference;
		}
		if (error < best_error) {
			best_error = error;
			best_period = period;
		}
	}
	return best_period;
}

static void check_pitch_rpn_and_controller_pcm(void)
{
	static const uint8_t pitch_track[] = {
		0,    0xc0, 80, 0,   0x90, 69,	100, 100, 0xe0, 127,  127, 100,
		0xb0, 101,  0,	0,   0xb0, 100, 0,   0,	  0xb0, 6,    12,  100,
		0xb0, 121,  0,	100, 0x90, 69,	0,   0,	  0xff, 0x2f, 0,
	};
	/* 22050 / Hz for A4, A4+2 semitones, A4+12 semitones, reset A4. */
	static const double expected_periods[] = { 50.113636, 44.647727,
						   25.056818, 50.113636 };
	struct fplinux_midi *midi =
		open_track(pitch_track, sizeof(pitch_track));
	int16_t windows[4][6615], samples[2048];
	uint64_t position = 0;
	ssize_t count;
	size_t i;

	assert(midi && fplinux_midi_start(midi) == 0);
	while (position < 44100) {
		count = read_block(midi, samples, 1024);
		assert(count > 0);
		for (i = 0; i < (size_t)count; i++) {
			uint64_t frame = position + i;
			unsigned int window = (unsigned int)(frame / 11025);
			uint64_t age = frame % 11025;

			/* Measure 0.3 seconds after each transition settles. */
			if (window < 4 && age >= 2205 && age < 8820)
				windows[window][age - 2205] = samples[i * 2];
		}
		position += (uint64_t)count;
	}
	for (i = 0; i < 4; i++) {
		double period = estimate_period(windows[i]);

		assert(fabs(period - expected_periods[i]) < 0.2);
	}
	fplinux_midi_close(midi);
}

static void check_pan_expression_program_and_drum_pcm(void)
{
	static const uint8_t pan_track[] = {
		0,    0xc0, 80,	 0,   0xb0, 10, 0,   0,	  0x90, 69,   100, 100,
		0xb0, 10,   127, 0,   0xb0, 42, 127, 100, 0xb0, 11,   0,   100,
		0xb0, 11,   127, 100, 0x90, 69, 0,   0,	  0xff, 0x2f, 0,
	};
	uint8_t drum_track[] = {
		0,    0xc9, 0, 0, 0x99, 38,   100, 100,
		0x89, 38,   0, 0, 0xff, 0x2f, 0,
	};
	struct fplinux_midi *midi = open_track(pan_track, sizeof(pan_track));
	int16_t samples[2048], drums[2048];
	uint64_t energy[4][2] = { 0 };
	uint64_t position = 0;
	ssize_t count;
	size_t i;

	assert(midi && fplinux_midi_start(midi) == 0);
	while (position < 44100) {
		count = read_block(midi, samples, 1024);
		assert(count > 0);
		for (i = 0; i < (size_t)count; i++) {
			uint64_t frame = position + i;
			unsigned int window = (unsigned int)(frame / 11025);

			if (window < 4) {
				energy[window][0] +=
					(uint64_t)abs(samples[i * 2]);
				energy[window][1] +=
					(uint64_t)abs(samples[i * 2 + 1]);
			}
		}
		position += (uint64_t)count;
	}
	assert(energy[0][0] > energy[0][1] * 2 + 10000);
	assert(energy[1][1] > energy[1][0] * 2 + 10000);
	assert(energy[2][0] == 0 && energy[2][1] == 0);
	assert(energy[3][1] > energy[3][0] * 2 + 10000);
	fplinux_midi_close(midi);
	midi = open_track(drum_track, sizeof(drum_track));
	assert(midi && fplinux_midi_start(midi) == 0);
	assert(read_block(midi, drums, 1024) == 1024 &&
	       has_signal(drums, 1024));
	fplinux_midi_close(midi);
	drum_track[1] = 0xc0;
	drum_track[4] = 0x90;
	drum_track[8] = 0x80;
	midi = open_track(drum_track, sizeof(drum_track));
	assert(midi && fplinux_midi_start(midi) == 0);
	assert(read_block(midi, samples, 1024) == 1024 &&
	       has_signal(samples, 1024));
	assert(memcmp(samples, drums, sizeof(drums)) != 0);
	fplinux_midi_close(midi);
	memcpy(drums, samples, sizeof(drums));
	drum_track[2] = 80;
	midi = open_track(drum_track, sizeof(drum_track));
	assert(midi && fplinux_midi_start(midi) == 0);
	assert(read_block(midi, samples, 1024) == 1024 &&
	       has_signal(samples, 1024));
	assert(memcmp(samples, drums, sizeof(drums)) != 0);
	fplinux_midi_close(midi);
}

static void check_sustain_and_velocity_zero_release(void)
{
	uint8_t track[] = {
		0,    0xc0, 80, 0,   0xb0, 64, 127, 0,	 0x90, 69,   100, 50,
		0x90, 69,   0,	100, 0xb0, 64, 0,   100, 0xff, 0x2f, 0,
	};
	struct fplinux_midi *held = open_track(track, sizeof(track));
	struct fplinux_midi *released, *note_off;
	int16_t holding[2048], releasing[2048], off[2048];
	uint64_t held_energy = 0, released_energy = 0, position = 0;
	ssize_t count, other_count;
	size_t i;

	track[12] = 0x80;
	note_off = open_track(track, sizeof(track));
	track[6] = 0;
	released = open_track(track, sizeof(track));
	assert(held && released && note_off);
	assert(fplinux_midi_start(held) == 0);
	assert(fplinux_midi_start(released) == 0);
	assert(fplinux_midi_start(note_off) == 0);
	while ((count = read_block(held, holding, 1024)) > 0) {
		assert(read_block(note_off, off, 1024) == count);
		assert(!memcmp(holding, off, (size_t)count * 4));
		other_count = read_block(released, releasing, 1024);
		assert(other_count >= 0);
		for (i = 0; i < (size_t)count; i++) {
			if (position + i >= 11025 && position + i < 15435) {
				held_energy += (uint64_t)abs(holding[i * 2]);
				if (i < (size_t)other_count)
					released_energy +=
						(uint64_t)abs(releasing[i * 2]);
			}
		}
		position += (uint64_t)count;
		assert(position < 143325);
	}
	assert(count == 0 && read_block(note_off, off, 1024) == 0);
	assert(held_energy > released_energy * 20 + 10000);
	assert(get_info(held).position_us == 1250000);
	fplinux_midi_close(held);
	fplinux_midi_close(released);
	fplinux_midi_close(note_off);
}

static void check_modulation_and_channel_pressure_pcm(void)
{
	uint8_t track[] = {
		0,  0xc0, 66,	0,    0xb0, 1,	0, 0, 0xd0, 0,	  0, 0x90,
		69, 100,  0x83, 0x10, 0x80, 69, 0, 0, 0xff, 0x2f, 0,
	};
	struct fplinux_midi *players[4];
	int16_t reference[2048], actual[2048];
	bool changed[2] = { false, false };
	unsigned int i, block;

	for (i = 0; i < 4; i++) {
		track[6] = i & 1 ? 127 : 0;
		track[9] = i & 2 ? 127 : 0;
		players[i] = open_track(track, sizeof(track));
		assert(players[i] && fplinux_midi_start(players[i]) == 0);
	}
	for (block = 0; block < 32; block++) {
		assert(read_block(players[0], reference, 1024) == 1024);
		for (i = 1; i < 4; i++) {
			assert(read_block(players[i], actual, 1024) == 1024);
			if (i < 3)
				changed[i - 1] |= memcmp(reference, actual,
							 sizeof(actual)) != 0;
			else
				/* This program's equal, opposite routes cancel. */
				assert(!memcmp(reference, actual,
					       sizeof(actual)));
		}
	}
	assert(changed[0] && changed[1]);
	for (i = 0; i < 4; i++)
		fplinux_midi_close(players[i]);
}

static void check_shared_prefetch_pool_and_player_lifetime(void)
{
	struct fplinux_midi *players[17], *tone;
	unsigned int created = synth_creations, deleted = synth_deletions;
	unsigned int copied = synth_copies;
	unsigned int loaded = bank_loads, i;
	int16_t samples[2048];

	for (i = 0; i < 17; i++) {
		players[i] =
			fplinux_midi_open_bytes(tempo_song, sizeof(tempo_song));
		assert(players[i]);
		if (i < 16) {
			assert(fplinux_midi_prefetch(players[i]) == 0);
			assert(fplinux_midi_prefetch(players[i]) == 0);
			assert(get_info(players[i]).state ==
			       FPLINUX_MIDI_READY);
		}
	}
	assert(synth_creations == created + 1 && bank_loads == loaded + 1);
	assert(synth_copies == copied + 16 && live_synths == 17);
	assert(synth_deletions == deleted);
	assert(fplinux_midi_prefetch(players[16]) == -1 && errno == ENOSPC);
	assert(get_info(players[16]).state == FPLINUX_MIDI_READY);
	tone = fplinux_midi_open_tone(69, 100);
	assert(tone && fplinux_midi_prefetch(tone) == 0);
	assert(fplinux_midi_start(tone) == 0);
	assert(read_block(tone, samples, 1024) == 1024 &&
	       has_signal(samples, 1024));
	assert(synth_creations == created + 1 && bank_loads == loaded + 1);
	fplinux_midi_close(tone);
	assert(fplinux_midi_start(players[0]) == 0);
	assert(read_block(players[0], samples, 512) == 512);
	assert(fplinux_midi_start(players[1]) == 0);
	assert(read_block(players[1], samples, 512) == 512);
	assert(get_info(players[0]).state == FPLINUX_MIDI_PLAYING);
	assert(read_block(players[0], samples, 512) == 512);
	assert(get_info(players[0]).position_frames == 1024);
	assert(fplinux_midi_release_active(players[0]) == 0);
	assert(get_info(players[0]).state == FPLINUX_MIDI_PAUSED);
	assert(get_info(players[0]).position_frames == 1024);
	assert(read_block(players[1], samples, 512) == 512);
	assert(get_info(players[1]).position_frames == 1024);
	assert(fplinux_midi_seek(players[0], 10) == 1);
	assert(fplinux_midi_start(players[0]) == 0);
	assert(get_info(players[0]).seeking);
	assert(fplinux_midi_read(players[0], samples, 1024) == -1 &&
	       errno == EAGAIN);
	assert(!get_info(players[0]).seeking);
	assert(get_info(players[0]).position_frames == 220);
	assert(read_block(players[0], samples, 512) == 512 &&
	       has_signal(samples, 512));
	assert(synth_creations == created + 1 && bank_loads == loaded + 1);
	assert(fplinux_midi_unprefetch(players[0]) == 0);
	assert(fplinux_midi_unprefetch(players[0]) == 0);
	assert(get_info(players[1]).state == FPLINUX_MIDI_PLAYING);
	assert(read_block(players[1], samples, 512) == 512);
	assert(fplinux_midi_prefetch(players[16]) == 0);
	assert(synth_creations == created + 1 && synth_copies == copied + 17);
	for (i = 0; i < 15; i++)
		fplinux_midi_close(players[i]);
	assert(synth_deletions == deleted + 15 && live_synths == 3);
	assert(fplinux_midi_unprefetch(players[15]) == 0);
	assert(live_synths == 2);
	assert(fplinux_midi_unprefetch(players[15]) == 0);
	assert(fplinux_midi_start(players[16]) == 0);
	assert(read_block(players[16], samples, 512) == 512 &&
	       has_signal(samples, 512));
	fplinux_midi_close(players[15]);
	fplinux_midi_close(players[16]);
	assert(live_synths == 0 && synth_deletions == deleted + 18);
	assert(bank_loads == loaded + 1);
}

static void check_midi_time_pause_seek_eof(void)
{
	struct fplinux_midi *midi =
		fplinux_midi_open_bytes(tempo_song, sizeof(tempo_song));
	struct fplinux_midi_info info;
	int16_t samples[2048];
	uint64_t total = 0, paused_us;
	ssize_t count;
	unsigned int steps;
	bool sounded = false;

	assert(midi);
	info = get_info(midi);
	assert(info.state == FPLINUX_MIDI_READY);
	assert(info.duration_us == 1500000);
	assert(info.rate == 22050 && info.channels == 2);
	assert(fplinux_midi_start(midi) == 0);
	count = read_block(midi, samples, 1024);
	assert(count == 1024 && has_signal(samples, (size_t)count));
	assert(get_info(midi).position_us == 46439);
	assert(fplinux_midi_pause(midi) == 0);
	paused_us = get_info(midi).position_us;
	assert(fplinux_midi_read(midi, samples, 1024) == -1 && errno == EAGAIN);
	assert(get_info(midi).position_us == paused_us);
	assert(fplinux_midi_unprefetch(midi) == 0);
	assert(get_info(midi).position_us == paused_us);
	assert(fplinux_midi_start(midi) == 0 && get_info(midi).seeking);
	assert(read_block(midi, samples, 1024) == 1024 &&
	       has_signal(samples, 1024));
	assert(get_info(midi).position_frames == 2048);
	assert(fplinux_midi_pause(midi) == 0);
	assert(fplinux_midi_seek(midi, 1300) == 1);
	assert(get_info(midi).position_us == 1300000);
	assert(fplinux_midi_seek(midi, 250) == 1);
	for (steps = 0; get_info(midi).seeking; steps++) {
		assert(steps < 10);
		assert(fplinux_midi_read(midi, samples, 1024) == -1);
		assert(errno == EAGAIN);
	}
	assert(steps == 6);
	info = get_info(midi);
	assert(info.state == FPLINUX_MIDI_PAUSED);
	assert(info.position_frames == 5512 && info.position_us == 249977);
	assert(fplinux_midi_start(midi) == 0);
	count = read_block(midi, samples, 1024);
	assert(count > 0 && has_signal(samples, (size_t)count));
	assert(fplinux_midi_stop(midi) == 0);
	assert(get_info(midi).state == FPLINUX_MIDI_READY);
	assert(get_info(midi).position_us == 0);
	assert(fplinux_midi_start(midi) == 0);
	while ((count = read_block(midi, samples, 1024)) > 0) {
		total += (uint64_t)count;
		sounded |= has_signal(samples, (size_t)count);
		assert(total <= 143325);
	}
	assert(count == 0 && sounded && total >= 33075);
	info = get_info(midi);
	assert(info.eof && info.state == FPLINUX_MIDI_ENDED);
	assert(info.position_frames == 33075 && info.position_us == 1500000);
	assert(fplinux_midi_start(midi) == 0);
	assert(get_info(midi).position_frames == 0);
	fplinux_midi_close(midi);
}

static void check_fractional_tempo_and_repeated_midi(void)
{
	struct fplinux_midi *midi = fplinux_midi_open_bytes(
		fractional_song, sizeof(fractional_song));
	int16_t samples[2048], first_pass[2048];
	uint64_t total = 0;
	size_t remaining;
	ssize_t count;

	assert(midi && get_info(midi).duration_us == 500001);
	assert(fplinux_midi_set_loop_count(midi, 2) == 0);
	assert(fplinux_midi_start(midi) == 0);
	assert(read_block(midi, first_pass, 1024) == 1024);
	total = 1024;
	while (total < 11026) {
		remaining = (size_t)(11026 - total);
		if (remaining > 1024)
			remaining = 1024;
		assert(read_block(midi, samples, remaining) ==
		       (ssize_t)remaining);
		total += remaining;
	}
	assert(read_block(midi, samples, 1024) == 1024);
	assert(!memcmp(first_pass, samples, sizeof(samples)));
	total += 1024;
	while ((count = read_block(midi, samples, 1024)) > 0) {
		total += (uint64_t)count;
		assert(total <= 132302);
	}
	assert(count == 0 && total >= 22052);
	assert(get_info(midi).duration_us == 500001);
	assert(get_info(midi).position_frames == 11026);
	fplinux_midi_close(midi);
}

static void check_channel_modes_preserve_file_parts_and_completion(void)
{
	uint8_t song[] = {
		'M', 'T',  'h', 'd', 0,	  0,	0,  6, 0,  0,	 0,    1,   0,
		96,  'M',  'T', 'r', 'k', 0,	0,  0, 16, 0,	 0xb1, 127, 0,
		0,   0x91, 69,	100, 96,  0x81, 69, 0, 0,  0xff, 0x2f, 0,
	};
	unsigned int variant;
	int16_t samples[2048];

	for (variant = 0; variant < 2; variant++) {
		struct fplinux_midi *midi;
		uint64_t total = 0;
		ssize_t count;
		bool sounded = false;

		if (variant) {
			song[23] = 0xb0;
			song[24] = 124;
		}
		midi = fplinux_midi_open_bytes(song, sizeof(song));
		assert(midi && fplinux_midi_start(midi) == 0);
		while ((count = read_block(midi, samples, 1024)) > 0) {
			sounded |= has_signal(samples, (size_t)count);
			total += (uint64_t)count;
			assert(total <= 121275);
		}
		assert(count == 0 && sounded && total >= 11025);
		assert(get_info(midi).state == FPLINUX_MIDI_ENDED);
		fplinux_midi_close(midi);
	}
}

struct start_attempt {
	struct fplinux_midi *midi;
	int result;
	int error;
};

static void *start_thread(void *context)
{
	struct start_attempt *attempt = context;

	attempt->result = fplinux_midi_start(attempt->midi);
	attempt->error = errno;
	return NULL;
}

static void check_concurrent_midi_start_and_independent_tone(void)
{
	struct start_attempt attempts[2] = { 0 };
	struct fplinux_midi *tone;
	pthread_t threads[2];
	int16_t samples[2048];
	unsigned int i;

	for (i = 0; i < 2; i++) {
		attempts[i].midi =
			fplinux_midi_open_bytes(tempo_song, sizeof(tempo_song));
		assert(attempts[i].midi);
		assert(pthread_create(&threads[i], NULL, start_thread,
				      &attempts[i]) == 0);
	}
	for (i = 0; i < 2; i++)
		assert(pthread_join(threads[i], NULL) == 0);
	for (i = 0; i < 2; i++)
		assert(attempts[i].result == 0);
	tone = fplinux_midi_open_tone(127, 100);
	assert(tone && fplinux_midi_start(tone) == 0);
	assert(get_info(tone).rate == 44100);
	assert(read_block(tone, samples, 1024) == 1024);
	assert(has_signal(samples, 1024));
	for (i = 0; i < 2; i++)
		assert(read_block(attempts[i].midi, samples, 1024) == 1024 &&
		       has_signal(samples, 1024));
	fplinux_midi_close(tone);
	for (i = 0; i < 2; i++)
		fplinux_midi_close(attempts[i].midi);
}

static void check_tone_frequency_gain_loop_and_sequence(void)
{
	struct fplinux_midi *midi = fplinux_midi_open_tone(69, 100);
	struct fplinux_midi_info info;
	int16_t samples[2048], reference[2048];
	uint64_t total = 0;
	unsigned int crossings = 0;
	unsigned int peak = 0;
	int16_t previous = 0;
	size_t i;
	ssize_t count;

	assert(midi && fplinux_midi_start(midi) == 0);
	assert(read_block(midi, reference, 1024) == 1024);
	for (i = 0; i < 2048; i++) {
		unsigned int magnitude = (unsigned int)abs(reference[i]);

		if (magnitude > peak)
			peak = magnitude;
	}
	/* A 100% tone uses the S16 range before system volume is applied. */
	assert(peak >= 32700 && peak <= 32767);
	assert(fplinux_midi_stop(midi) == 0);
	assert(fplinux_midi_set_gain(midi, 25) == 0);
	assert(fplinux_midi_start(midi) == 0);
	assert(read_block(midi, samples, 1024) == 1024);
	for (i = 0; i < 2048; i++)
		assert(samples[i] == reference[i] / 4);
	assert(fplinux_midi_stop(midi) == 0);
	assert(fplinux_midi_set_gain(midi, 0) == 0);
	assert(fplinux_midi_start(midi) == 0);
	assert(read_block(midi, samples, 1024) == 1024);
	assert(!has_signal(samples, 1024));
	assert(fplinux_midi_stop(midi) == 0);
	assert(fplinux_midi_set_gain(midi, 100) == 0);
	assert(fplinux_midi_set_loop_count(midi, 2) == 0);
	assert(fplinux_midi_start(midi) == 0);
	while ((count = read_block(midi, samples, 1024)) > 0) {
		for (i = 0; i < (size_t)count; i++) {
			assert(samples[i * 2] == samples[i * 2 + 1]);
			if (previous <= 0 && samples[i * 2] > 0)
				crossings++;
			previous = samples[i * 2];
		}
		total += (uint64_t)count;
	}
	assert(count == 0 && total == 8820);
	assert(crossings >= 87 && crossings <= 89);
	assert(get_info(midi).position_us == 100000);
	fplinux_midi_close(midi);
	midi = fplinux_midi_open_tone_sequence(tone_sequence,
					       sizeof(tone_sequence));
	assert(midi);
	info = get_info(midi);
	assert(info.duration_us == 1250000 && info.rate == 44100);
	assert(fplinux_midi_seek(midi, 625) == 0);
	assert(fplinux_midi_start(midi) == 0);
	assert(read_block(midi, samples, 1024) == 1024);
	for (i = 0; i < 2048; i++)
		assert(samples[i] == 0);
	assert(fplinux_midi_stop(midi) == 0);
	assert(fplinux_midi_start(midi) == 0);
	total = 0;
	peak = 0;
	while ((count = read_block(midi, samples, 1024)) > 0) {
		for (i = 0; i < (size_t)count; i++) {
			uint64_t position = total + i;
			unsigned int magnitude =
				(unsigned int)abs(samples[i * 2]);

			if (magnitude > peak)
				peak = magnitude;
			assert(magnitude <= 16384);
			if (position >= 22050 && position < 33075)
				assert(samples[i * 2] == 0);
		}
		total += (uint64_t)count;
	}
	assert(count == 0 && total == 55125);
	assert(peak >= 16300 && peak <= 16384);
	fplinux_midi_close(midi);
}

static void check_rejected_smf_resources(void)
{
	uint8_t song[sizeof(fractional_song)];
	uint8_t *large;
	size_t i, length = 22 + 65536 * 4 + 4;

	memcpy(song, fractional_song, sizeof(song));
	song[9] = 2;
	assert(!fplinux_midi_open_bytes(song, sizeof(song)) &&
	       errno == ENOTSUP);
	song[9] = 0;
	song[12] = 0xe7;
	assert(!fplinux_midi_open_bytes(song, sizeof(song)) &&
	       errno == ENOTSUP);
	song[12] = 0;
	song[13] = 0;
	assert(!fplinux_midi_open_bytes(song, sizeof(song)) && errno == EINVAL);
	memcpy(song, fractional_song, sizeof(song));
	assert(!fplinux_midi_open_bytes(song, sizeof(song) - 1) &&
	       errno == EINVAL);
	memset(song + 26, 0, 3);
	assert(!fplinux_midi_open_bytes(song, sizeof(song)) && errno == EINVAL);
	large = calloc(1, length);
	assert(large);
	memcpy(large, fractional_song, 22);
	large[13] = 96;
	large[18] = 0;
	large[19] = 4;
	large[20] = 0;
	large[21] = 4;
	for (i = 0; i < 65536; i++) {
		large[22 + i * 4 + 1] = 0x80;
		large[22 + i * 4 + 2] = 69;
	}
	large[length - 3] = 0xff;
	large[length - 2] = 0x2f;
	assert(!fplinux_midi_open_bytes(large, length) && errno == EFBIG);
	free(large);
}

static void check_rejected_tone_sequences(void)
{
	static const uint8_t invalid[][8] = {
		{ 0xfe, 2, 69, 16 },	      { 0xfe, 1, 0xfd, 4, 69, 16 },
		{ 0xfe, 1, 0xfc, 0, 69, 16 }, { 0xfe, 1, 0xf9, 0 },
		{ 0xfe, 1, 0xf7, 1, 69, 16 }, { 0xfe, 1, 0xf8, 101 },
	};
	static const size_t sizes[] = { 4, 6, 6, 4, 6, 4 };
	uint8_t expansion[1060];
	size_t i, position = 0;
	unsigned int block, repeat;

	for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
		assert(!fplinux_midi_open_tone_sequence(invalid[i], sizes[i]) &&
		       errno == EINVAL);
	expansion[position++] = 0xfe;
	expansion[position++] = 1;
	for (block = 0; block < 3; block++) {
		expansion[position++] = 0xfb;
		expansion[position++] = (uint8_t)block;
		for (repeat = 0; repeat < (block ? 127U : 1U); repeat++) {
			expansion[position++] = block ? 0xf9 : 69;
			expansion[position++] = block ? (uint8_t)(block - 1) :
							1;
		}
		expansion[position++] = 0xfa;
		expansion[position++] = (uint8_t)block;
	}
	for (repeat = 0; repeat < 5; repeat++) {
		expansion[position++] = 0xf9;
		expansion[position++] = 2;
	}
	assert(position <= sizeof(expansion));
	assert(!fplinux_midi_open_tone_sequence(expansion, position) &&
	       errno == EFBIG);
}

int main(int argc, char **argv)
{
	if (argc == 2 && !strcmp(argv[1], "missing-bank")) {
		struct fplinux_midi *midi =
			fplinux_midi_open_bytes(tempo_song, sizeof(tempo_song));
		int16_t samples[2048];

		assert(midi && fplinux_midi_start(midi) == -1 && errno == EIO);
		assert(get_info(midi).state == FPLINUX_MIDI_FAILED);
		assert(fplinux_midi_read(midi, samples, 1024) == -1 &&
		       errno == EIO);
		fplinux_midi_close(midi);
		puts("midi missing bank: ok");
		return 0;
	}
	assert(argc == 1);
	check_rejected_smf_resources();
	check_rejected_tone_sequences();
	check_shared_prefetch_pool_and_player_lifetime();
	check_layered_string_chord_preserves_individual_notes();
	check_player_pcm_isolation_and_close_orders();
	check_preparation_failure_preserves_other_player();
	check_literal_tempo_frames_and_track_order();
	check_pitch_rpn_and_controller_pcm();
	check_pan_expression_program_and_drum_pcm();
	check_sustain_and_velocity_zero_release();
	check_modulation_and_channel_pressure_pcm();
	check_midi_time_pause_seek_eof();
	check_fractional_tempo_and_repeated_midi();
	check_channel_modes_preserve_file_parts_and_completion();
	check_concurrent_midi_start_and_independent_tone();
	check_tone_frequency_gain_loop_and_sequence();
	puts("midi render: ok");
	return 0;
}
#endif
