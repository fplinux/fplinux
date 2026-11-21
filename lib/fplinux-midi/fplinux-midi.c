/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define _POSIX_C_SOURCE 200809L
#include "fplinux-midi-internal.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <tsf.h>

#ifndef FPLINUX_MIDI_SOUNDFONT
#define FPLINUX_MIDI_SOUNDFONT "/usr/share/fplinux/soundfonts/default.sf2"
#endif

#define MIDI_EVENT_STEPS 256U
#define MIDI_RELEASE_FRAMES (5U * FPLINUX_MIDI_RATE)
#define MIDI_POLYPHONY 160
/* Headroom for the polyphonic sum before S16 conversion. */
#define MIDI_REFERENCE_GAIN 0.2f

struct fplinux_midi {
	struct fplinux_midi_sequence sequence;
	enum fplinux_midi_state state;
	/* Own voices/channels; presets and samples belong to the shared bank. */
	tsf *synth;
	uint64_t position_frames;
	uint64_t seek_frames;
	uint64_t release_frames;
	size_t next_event;
	unsigned int gain;
	unsigned int rate;
	int loop_count;
	int loops_left;
	int error;
	bool seeking;
	bool releasing;
	bool tone;
	bool prefetched;
	uint32_t tone_phase;
};

/* All bank references and per-player synthesis run under this mutex. */
static pthread_mutex_t midi_mutex = PTHREAD_MUTEX_INITIALIZER;
static tsf *shared_synth;
static void *bank_mapping = MAP_FAILED;
static size_t bank_bytes;
static unsigned int synth_references;

static void destroy_shared_synth(void)
{
	if (shared_synth)
		tsf_close(shared_synth);
	shared_synth = NULL;
	if (bank_mapping != MAP_FAILED)
		munmap(bank_mapping, bank_bytes);
	bank_mapping = MAP_FAILED;
	bank_bytes = 0;
}

static void release_active(struct fplinux_midi *midi)
{
	if (midi->state == FPLINUX_MIDI_PLAYING)
		midi->state = FPLINUX_MIDI_PAUSED;
}

static void unprefetch_player(struct fplinux_midi *midi)
{
	release_active(midi);
	if (!midi->prefetched)
		return;
	midi->prefetched = false;
	if (midi->synth)
		tsf_close(midi->synth);
	midi->synth = NULL;
	if (!midi->tone && --synth_references == 0)
		destroy_shared_synth();
}

static int fail_player(struct fplinux_midi *midi, int error)
{
	unprefetch_player(midi);
	midi->state = FPLINUX_MIDI_FAILED;
	midi->error = error;
	midi->seeking = false;
	errno = error;
	return -1;
}

static int initialize_channels(tsf *synth)
{
	unsigned int channel;

	if (!tsf_channel_set_presetnumber(synth, 15, 0, 0))
		return -1;
	for (channel = 0; channel < 16; channel++) {
		if (!tsf_channel_set_presetnumber(synth, (int)channel, 0,
						  channel == 9))
			return -1;
	}
	return 0;
}

static int load_shared_synth(void)
{
	const char *soundfont = getenv("FPLINUX_MIDI_SOUNDFONT_PATH");
	struct stat status;
	int fd;
	int error = EIO;

	if (!soundfont || !*soundfont)
		soundfont = FPLINUX_MIDI_SOUNDFONT;
	fd = open(soundfont, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		errno = EIO;
		return -1;
	}
	if (fstat(fd, &status) < 0 || !S_ISREG(status.st_mode) ||
	    status.st_size <= 0 || status.st_size > INT_MAX) {
		close(fd);
		errno = EIO;
		return -1;
	}
	bank_bytes = (size_t)status.st_size;
	bank_mapping = mmap(NULL, bank_bytes, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (bank_mapping == MAP_FAILED)
		goto failed;
	shared_synth = tsf_load_memory(bank_mapping, (int)bank_bytes);
	if (!shared_synth)
		goto failed;
	tsf_set_output(shared_synth, TSF_STEREO_INTERLEAVED, FPLINUX_MIDI_RATE,
		       0);
	tsf_set_volume(shared_synth, MIDI_REFERENCE_GAIN);
	return 0;

failed:
	destroy_shared_synth();
	errno = error;
	return -1;
}

static int prefetch_player(struct fplinux_midi *midi)
{
	uint64_t target;

	if (midi->prefetched)
		return 0;
	if (!midi->tone) {
		if (synth_references == FPLINUX_MIDI_PREFETCH_LIMIT) {
			errno = ENOSPC;
			return -1;
		}
		if (!shared_synth && load_shared_synth() < 0)
			return fail_player(midi, errno);
		midi->synth = tsf_copy(shared_synth);
		if (!midi->synth ||
		    !tsf_set_max_voices(midi->synth, MIDI_POLYPHONY) ||
		    initialize_channels(midi->synth) < 0) {
			if (midi->synth)
				tsf_close(midi->synth);
			midi->synth = NULL;
			if (!synth_references)
				destroy_shared_synth();
			return fail_player(midi, ENOMEM);
		}
		synth_references++;
		/* A released reference needs bounded reconstruction of its time. */
		target = midi->seeking ? midi->seek_frames :
					 midi->position_frames;
		midi->position_frames = 0;
		midi->next_event = 0;
		midi->release_frames = 0;
		midi->releasing = false;
		midi->seek_frames = target;
		midi->seeking = target != 0;
	}
	midi->prefetched = true;
	return 0;
}

static int reset_synth(struct fplinux_midi *midi)
{
	if (!midi->synth)
		return 0;
	tsf_reset(midi->synth);
	if (initialize_channels(midi->synth) < 0)
		return fail_player(midi, EIO);
	return 0;
}

static int rewind_player(struct fplinux_midi *midi)
{
	if (reset_synth(midi) < 0)
		return -1;
	midi->position_frames = 0;
	midi->next_event = 0;
	midi->release_frames = 0;
	midi->releasing = false;
	midi->tone_phase = 0;
	return 0;
}

static int send_event(struct fplinux_midi *midi,
		      const struct fplinux_midi_event *event)
{
	unsigned int channel = event->status & 0x0fU;
	int result = 1;

	switch (event->status & 0xf0U) {
	case 0x80:
		/* A release without a currently sounding note is valid MIDI. */
		tsf_channel_note_off(midi->synth, (int)channel, event->first);
		break;
	case 0x90:
		if (event->second)
			result = tsf_channel_note_on(midi->synth, (int)channel,
						     event->first,
						     event->second / 127.0f);
		else
			tsf_channel_note_off(midi->synth, (int)channel,
					     event->first);
		break;
	case 0xa0:
		/* Per-key pressure is outside the supported file playback subset. */
		break;
	case 0xb0:
		/* File playback keeps independent parts instead of live input groups. */
		if (event->first >= 124)
			tsf_channel_note_off_all(midi->synth, (int)channel);
		else
			result = tsf_channel_midi_control(midi->synth,
							  (int)channel,
							  event->first,
							  event->second);
		break;
	case 0xc0:
		result = tsf_channel_set_presetnumber(
			midi->synth, (int)channel, event->first, channel == 9);
		break;
	case 0xd0:
		result = tsf_channel_set_pressure(midi->synth, (int)channel,
						  event->first);
		break;
	case 0xe0:
		result = tsf_channel_set_pitchwheel(midi->synth, (int)channel,
						    event->first |
							    event->second << 7);
		break;
	case 0xf0:
		if (event->status == FPLINUX_MIDI_RESET)
			return reset_synth(midi);
		break;
	}
	if (!result)
		return fail_player(midi, EIO);
	return 0;
}

static void apply_gain(const struct fplinux_midi *midi, int16_t *samples,
		       size_t frames)
{
	size_t i;

	if (midi->gain == 100)
		return;
	for (i = 0; i < frames * FPLINUX_MIDI_CHANNELS; i++)
		samples[i] = (int16_t)((int32_t)samples[i] *
				       (int32_t)midi->gain / 100);
}

static void render_frames(struct fplinux_midi *midi, int16_t *samples,
			  size_t count)
{
	tsf_render_short(midi->synth, samples, (int)count, 0);
}

static int release_notes(struct fplinux_midi *midi)
{
	unsigned int channel;

	for (channel = 0; channel < 16; channel++) {
		if (!tsf_channel_set_sustain(midi->synth, (int)channel, 0))
			return fail_player(midi, EIO);
		tsf_channel_note_off_all(midi->synth, (int)channel);
	}
	midi->releasing = true;
	return 0;
}

static void end_player(struct fplinux_midi *midi)
{
	release_active(midi);
	midi->state = FPLINUX_MIDI_ENDED;
	midi->seeking = false;
}

static int finish_pass(struct fplinux_midi *midi)
{
	unsigned int channel;

	if (midi->sequence.duration_frames &&
	    (midi->loops_left == -1 || midi->loops_left > 1)) {
		if (midi->loops_left > 1)
			midi->loops_left--;
		return rewind_player(midi);
	}
	if (!midi->releasing && release_notes(midi) < 0)
		return -1;
	if (midi->release_frames >= MIDI_RELEASE_FRAMES) {
		for (channel = 0; channel < 16; channel++)
			tsf_channel_sounds_off_all(midi->synth, (int)channel);
	}
	if (!tsf_active_voice_count(midi->synth) ||
	    midi->release_frames >= MIDI_RELEASE_FRAMES)
		end_player(midi);
	return 0;
}

static ssize_t read_midi(struct fplinux_midi *midi, int16_t *samples,
			 size_t capacity)
{
	size_t produced = 0, events = 0;

	while (produced < capacity && midi->state == FPLINUX_MIDI_PLAYING) {
		uint64_t boundary = midi->sequence.duration_frames;
		size_t count;

		while (midi->next_event < midi->sequence.count &&
		       midi->sequence.events[midi->next_event].frame <=
			       midi->position_frames) {
			if (events++ == MIDI_EVENT_STEPS)
				goto bounded;
			if (send_event(
				    midi,
				    &midi->sequence.events[midi->next_event]) <
			    0)
				return -1;
			midi->next_event++;
		}
		if (midi->position_frames >= boundary) {
			if (finish_pass(midi) < 0)
				return -1;
			if (midi->state == FPLINUX_MIDI_ENDED)
				break;
			if (!midi->releasing)
				continue;
			count = capacity - produced;
			if (count > MIDI_RELEASE_FRAMES - midi->release_frames)
				count = MIDI_RELEASE_FRAMES -
					midi->release_frames;
			render_frames(midi, samples + produced * 2, count);
			midi->release_frames += count;
			produced += count;
			if (finish_pass(midi) < 0)
				return -1;
			continue;
		}
		if (midi->next_event < midi->sequence.count &&
		    midi->sequence.events[midi->next_event].frame < boundary)
			boundary =
				midi->sequence.events[midi->next_event].frame;
		count = capacity - produced;
		if ((uint64_t)count > boundary - midi->position_frames)
			count = (size_t)(boundary - midi->position_frames);
		render_frames(midi, samples + produced * 2, count);
		midi->position_frames += count;
		produced += count;
	}

bounded:
	if (!produced && midi->state != FPLINUX_MIDI_ENDED) {
		errno = EAGAIN;
		return -1;
	}
	apply_gain(midi, samples, produced);
	return (ssize_t)produced;
}

static int seek_step(struct fplinux_midi *midi)
{
	int16_t discarded[FPLINUX_MIDI_BLOCK_FRAMES * 2];
	size_t events = 0, rendered = 0;

	if (prefetch_player(midi) < 0)
		return -1;
	while (midi->position_frames < midi->seek_frames) {
		uint64_t boundary = midi->seek_frames;
		size_t count;

		while (midi->next_event < midi->sequence.count &&
		       midi->sequence.events[midi->next_event].frame <=
			       midi->position_frames) {
			if (events++ == MIDI_EVENT_STEPS)
				return 1;
			if (send_event(
				    midi,
				    &midi->sequence.events[midi->next_event]) <
			    0)
				return -1;
			midi->next_event++;
		}
		if (rendered == FPLINUX_MIDI_BLOCK_FRAMES)
			return 1;
		if (midi->next_event < midi->sequence.count &&
		    midi->sequence.events[midi->next_event].frame < boundary)
			boundary =
				midi->sequence.events[midi->next_event].frame;
		count = FPLINUX_MIDI_BLOCK_FRAMES - rendered;
		if ((uint64_t)count > boundary - midi->position_frames)
			count = (size_t)(boundary - midi->position_frames);
		render_frames(midi, discarded, count);
		midi->position_frames += count;
		rendered += count;
	}
	midi->seeking = false;
	return 0;
}

static int16_t tone_sample(uint32_t phase)
{
	int64_t half = (phase >> 16) & 0x7fff;
	int64_t product = half * (32768 - half);
	int64_t value =
		16 * product * INT16_MAX / (5LL * 32768 * 32768 - 4 * product);

	return (int16_t)(phase & 0x80000000U ? -value : value);
}

static ssize_t read_tone(struct fplinux_midi *midi, int16_t *samples,
			 size_t capacity)
{
	size_t produced = 0;

	while (produced < capacity) {
		const struct fplinux_midi_tone *tone;
		uint64_t begin, age, remaining, ramp;
		int16_t sample;

		if (midi->position_frames == midi->sequence.duration_frames) {
			if (midi->sequence.duration_frames &&
			    (midi->loops_left == -1 || midi->loops_left > 1)) {
				if (midi->loops_left > 1)
					midi->loops_left--;
				midi->position_frames = 0;
				midi->tone_phase = 0;
				midi->next_event = 0;
			} else {
				end_player(midi);
				break;
			}
		}
		while (midi->position_frames >=
		       midi->sequence.tones[midi->next_event].end_frame) {
			midi->next_event++;
			midi->tone_phase = 0;
		}
		tone = &midi->sequence.tones[midi->next_event];
		begin = midi->next_event ?
				midi->sequence.tones[midi->next_event - 1]
					.end_frame :
				0;
		age = midi->position_frames - begin;
		remaining = tone->end_frame - midi->position_frames;
		ramp = age < remaining ? age : remaining;
		if (ramp > midi->rate / 200)
			ramp = midi->rate / 200;
		sample = (int16_t)((int64_t)tone_sample(midi->tone_phase) *
				   tone->volume * (int64_t)ramp /
				   (100 * (midi->rate / 200)));
		samples[produced * 2] = sample;
		samples[produced * 2 + 1] = sample;
		midi->tone_phase += tone->step;
		midi->position_frames++;
		produced++;
	}
	apply_gain(midi, samples, produced);
	return (ssize_t)produced;
}

static struct fplinux_midi *new_player(void)
{
	struct fplinux_midi *midi = calloc(1, sizeof(*midi));

	if (!midi)
		return NULL;
	midi->rate = FPLINUX_MIDI_RATE;
	midi->gain = 100;
	midi->loop_count = 1;
	midi->loops_left = 1;
	return midi;
}

struct fplinux_midi *fplinux_midi_open_bytes(const void *bytes, size_t count)
{
	struct fplinux_midi *midi = new_player();
	int saved_errno;

	if (!midi)
		return NULL;
	if (fplinux_midi_parse_smf(bytes, count, &midi->sequence) < 0) {
		saved_errno = errno;
		free(midi);
		errno = saved_errno;
		return NULL;
	}
	return midi;
}

struct fplinux_midi *fplinux_midi_open_tone(unsigned int note,
					    uint64_t duration_ms)
{
	struct fplinux_midi *midi = new_player();
	int saved_errno;

	if (!midi)
		return NULL;
	if (fplinux_midi_make_tone(note, duration_ms, &midi->sequence) < 0) {
		saved_errno = errno;
		free(midi);
		errno = saved_errno;
		return NULL;
	}
	midi->tone = true;
	midi->rate = FPLINUX_MIDI_TONE_RATE;
	return midi;
}

struct fplinux_midi *fplinux_midi_open_tone_sequence(const void *bytes,
						     size_t count)
{
	struct fplinux_midi *midi = new_player();
	int saved_errno;

	if (!midi)
		return NULL;
	if (fplinux_midi_parse_tones(bytes, count, &midi->sequence) < 0) {
		saved_errno = errno;
		free(midi);
		errno = saved_errno;
		return NULL;
	}
	midi->tone = true;
	midi->rate = FPLINUX_MIDI_TONE_RATE;
	return midi;
}

void fplinux_midi_close(struct fplinux_midi *midi)
{
	if (!midi)
		return;
	pthread_mutex_lock(&midi_mutex);
	unprefetch_player(midi);
	pthread_mutex_unlock(&midi_mutex);
	free(midi->sequence.events);
	free(midi->sequence.tones);
	free(midi);
}

int fplinux_midi_get_info(struct fplinux_midi *midi,
			  struct fplinux_midi_info *info)
{
	uint64_t frames;

	if (!midi || !info) {
		errno = EINVAL;
		return -1;
	}
	pthread_mutex_lock(&midi_mutex);
	frames = midi->seeking ? midi->seek_frames : midi->position_frames;
	*info = (struct fplinux_midi_info){
		.state = midi->state,
		.position_us = frames == midi->sequence.duration_frames ?
				       midi->sequence.duration_us :
				       frames * 1000000 / midi->rate,
		.duration_us = midi->sequence.duration_us,
		.position_frames = frames,
		.rate = midi->rate,
		.channels = FPLINUX_MIDI_CHANNELS,
		.gain = midi->gain,
		.seeking = midi->seeking,
		.eof = midi->state == FPLINUX_MIDI_ENDED,
	};
	pthread_mutex_unlock(&midi_mutex);
	return 0;
}

int fplinux_midi_prefetch(struct fplinux_midi *midi)
{
	int result;

	if (!midi) {
		errno = EINVAL;
		return -1;
	}
	pthread_mutex_lock(&midi_mutex);
	if (midi->state == FPLINUX_MIDI_FAILED) {
		errno = midi->error;
		result = -1;
	} else {
		result = prefetch_player(midi);
	}
	pthread_mutex_unlock(&midi_mutex);
	return result;
}

int fplinux_midi_release_active(struct fplinux_midi *midi)
{
	if (!midi) {
		errno = EINVAL;
		return -1;
	}
	pthread_mutex_lock(&midi_mutex);
	release_active(midi);
	pthread_mutex_unlock(&midi_mutex);
	return 0;
}

int fplinux_midi_unprefetch(struct fplinux_midi *midi)
{
	if (!midi) {
		errno = EINVAL;
		return -1;
	}
	pthread_mutex_lock(&midi_mutex);
	unprefetch_player(midi);
	pthread_mutex_unlock(&midi_mutex);
	return 0;
}

int fplinux_midi_start(struct fplinux_midi *midi)
{
	int result = 0;

	if (!midi) {
		errno = EINVAL;
		return -1;
	}
	pthread_mutex_lock(&midi_mutex);
	if (midi->state == FPLINUX_MIDI_FAILED) {
		errno = midi->error;
		result = -1;
	} else {
		if (midi->state == FPLINUX_MIDI_ENDED) {
			result = rewind_player(midi);
			midi->loops_left = midi->loop_count;
			midi->seeking = false;
		}
		if (!result)
			result = prefetch_player(midi);
		if (!result)
			midi->state = FPLINUX_MIDI_PLAYING;
	}
	pthread_mutex_unlock(&midi_mutex);
	return result;
}

int fplinux_midi_pause(struct fplinux_midi *midi)
{
	if (!midi) {
		errno = EINVAL;
		return -1;
	}
	pthread_mutex_lock(&midi_mutex);
	if (midi->state == FPLINUX_MIDI_PLAYING)
		midi->state = FPLINUX_MIDI_PAUSED;
	pthread_mutex_unlock(&midi_mutex);
	return 0;
}

int fplinux_midi_stop(struct fplinux_midi *midi)
{
	if (!midi) {
		errno = EINVAL;
		return -1;
	}
	pthread_mutex_lock(&midi_mutex);
	release_active(midi);
	midi->state = FPLINUX_MIDI_READY;
	midi->error = 0;
	midi->seeking = false;
	midi->loops_left = midi->loop_count;
	rewind_player(midi);
	pthread_mutex_unlock(&midi_mutex);
	return 0;
}

int fplinux_midi_set_gain(struct fplinux_midi *midi, unsigned int level)
{
	if (!midi || level > 100) {
		errno = EINVAL;
		return -1;
	}
	pthread_mutex_lock(&midi_mutex);
	midi->gain = level;
	pthread_mutex_unlock(&midi_mutex);
	return 0;
}

int fplinux_midi_set_loop_count(struct fplinux_midi *midi, int count)
{
	int result = 0;

	if (!midi || (count < 1 && count != -1)) {
		errno = EINVAL;
		return -1;
	}
	pthread_mutex_lock(&midi_mutex);
	if (midi->state != FPLINUX_MIDI_READY &&
	    midi->state != FPLINUX_MIDI_PAUSED) {
		errno = EBUSY;
		result = -1;
	} else {
		midi->loop_count = count;
		midi->loops_left = count;
	}
	pthread_mutex_unlock(&midi_mutex);
	return result;
}

int fplinux_midi_seek(struct fplinux_midi *midi, uint64_t position_ms)
{
	uint64_t frames;
	int result = 1;

	if (!midi) {
		errno = EINVAL;
		return -1;
	}
	pthread_mutex_lock(&midi_mutex);
	if (midi->state == FPLINUX_MIDI_FAILED) {
		errno = midi->error;
		result = -1;
		goto out;
	}
	if (position_ms >= (midi->sequence.duration_us + 999) / 1000)
		frames = midi->sequence.duration_frames;
	else
		frames = position_ms * midi->rate / 1000;
	if (rewind_player(midi) < 0) {
		result = -1;
		goto out;
	}
	if (midi->state == FPLINUX_MIDI_ENDED)
		midi->state = FPLINUX_MIDI_READY;
	midi->seek_frames = frames;
	midi->seeking = frames != 0;
	if (midi->tone) {
		size_t low = 0, high = midi->sequence.count;
		uint64_t begin;

		midi->position_frames = frames;
		while (low < high) {
			size_t middle = low + (high - low) / 2;

			if (midi->sequence.tones[middle].end_frame <= frames)
				low = middle + 1;
			else
				high = middle;
		}
		midi->next_event = low;
		begin = low ? midi->sequence.tones[low - 1].end_frame : 0;
		midi->tone_phase =
			low < midi->sequence.count ?
				(uint32_t)(frames - begin) *
					midi->sequence.tones[low].step :
				0;
		midi->seeking = false;
	}
	if (!midi->seeking)
		result = 0;
out:
	pthread_mutex_unlock(&midi_mutex);
	return result;
}

ssize_t fplinux_midi_read(struct fplinux_midi *midi, int16_t *samples,
			  size_t capacity)
{
	ssize_t result;

	if (!midi || !samples || !capacity ||
	    capacity > FPLINUX_MIDI_BLOCK_FRAMES) {
		errno = EINVAL;
		return -1;
	}
	pthread_mutex_lock(&midi_mutex);
	if (midi->state == FPLINUX_MIDI_FAILED) {
		errno = midi->error;
		result = -1;
	} else if (midi->seeking) {
		result = seek_step(midi);
		if (result >= 0) {
			errno = EAGAIN;
			result = -1;
		}
	} else if (midi->state == FPLINUX_MIDI_ENDED) {
		result = 0;
	} else if (midi->state != FPLINUX_MIDI_PLAYING) {
		errno = EAGAIN;
		result = -1;
	} else if (midi->tone) {
		result = read_tone(midi, samples, capacity);
	} else {
		result = read_midi(midi, samples, capacity);
	}
	pthread_mutex_unlock(&midi_mutex);
	return result;
}
