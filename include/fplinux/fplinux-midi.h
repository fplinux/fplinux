/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef FPLINUX_MIDI_H
#define FPLINUX_MIDI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define FPLINUX_MIDI_RATE 22050U
#define FPLINUX_MIDI_TONE_RATE 44100U
#define FPLINUX_MIDI_CHANNELS 2U
#define FPLINUX_MIDI_BLOCK_FRAMES 1024U
#define FPLINUX_MIDI_RESOURCE_BYTES (16U * 1024U * 1024U)
#define FPLINUX_MIDI_EVENT_LIMIT 65536U
#define FPLINUX_MIDI_TRACK_LIMIT 64U
#define FPLINUX_MIDI_PREFETCH_LIMIT 16U

enum fplinux_midi_state {
	FPLINUX_MIDI_READY,
	FPLINUX_MIDI_PLAYING,
	FPLINUX_MIDI_PAUSED,
	FPLINUX_MIDI_ENDED,
	FPLINUX_MIDI_FAILED,
};

struct fplinux_midi_info {
	enum fplinux_midi_state state;
	uint64_t position_us;
	uint64_t duration_us;
	uint64_t position_frames;
	unsigned int rate;
	unsigned int channels;
	unsigned int gain;
	bool seeking;
	/* Renderer EOF: the caller must still drain accepted PCM. */
	bool eof;
};

struct fplinux_midi;

/*
 * Synchronous, unprivileged SMF type 0/1 with PPQN timing and General MIDI.
 * Type 2 and SMPTE timing return ENOTSUP. Input is parsed into bounded events;
 * no input bytes are retained. Invalid content returns EINVAL, a resource
 * limit EFBIG; sequences are capped at 24 hours. Unknown manufacturer SysEx and
 * descriptive metadata are ignored. File playback keeps 16 independent
 * polyphonic parts; channel-mode 124..127 releases the addressed part's notes.
 * Calls may block on file IO or synthesis and belong outside UI/guest threads.
 * The library serializes all instances and owns one read-only mapped bank.
 * Per-key pressure, sostenuto, chorus and reverb are outside this subset.
 * A caller prevents any call from racing with close on the same handle.
 */
struct fplinux_midi *fplinux_midi_open_bytes(const void *bytes, size_t count);
/* Simple tones use full-range S16 at 100% note and player volume. */
struct fplinux_midi *fplinux_midi_open_tone(unsigned int note,
					    uint64_t duration_ms);
/* JSR-135 version 1 ToneControl bytes, including blocks/repeat/volume. */
struct fplinux_midi *fplinux_midi_open_tone_sequence(const void *bytes,
						     size_t count);
void fplinux_midi_close(struct fplinux_midi *midi);
int fplinux_midi_get_info(struct fplinux_midi *midi,
			  struct fplinux_midi_info *info);

/*
 * prefetch prepares this handle's synth voices/channels and retains the shared
 * read-only bank. Programs, controllers and timelines are independent.
 * At most PREFETCH_LIMIT MIDI handles retain references (otherwise ENOSPC).
 * Repeated prefetch/unprefetch calls are idempotent. Simple tones need no bank.
 * unprefetch pauses and drops this handle's synth/reference; the last reference
 * destroys the shared resources. close also unprefetches.
 */
int fplinux_midi_prefetch(struct fplinux_midi *midi);
int fplinux_midi_unprefetch(struct fplinux_midi *midi);
/*
 * start implicitly prefetches. Restoring an unprefetched timeline schedules
 * bounded sample catch-up; it does not synchronously replay. pause and
 * release_active freeze only this handle's samples and retain its synth/time.
 * The caller drops future transport PCM and seeks to its heard time separately.
 * stop resets time/loop count and returns to READY;
 * its prefetched reference remains until unprefetch/close.
 * start after ENDED restarts at zero. Simple tones use a lightweight oscillator
 * at TONE_RATE without a sample bank. No callbacks or transport IO.
 */
int fplinux_midi_start(struct fplinux_midi *midi);
int fplinux_midi_pause(struct fplinux_midi *midi);
int fplinux_midi_release_active(struct fplinux_midi *midi);
int fplinux_midi_stop(struct fplinux_midi *midi);
/* Player gain 0..100, default 100: scales rendered PCM, independently of system
 * volume and MIDI channel/instrument dynamics. Zero mutes; 100 is unity.
 */
int fplinux_midi_set_gain(struct fplinux_midi *midi, unsigned int level);
/* Total plays: 1 once, -1 forever. Set while READY or PAUSED. */
int fplinux_midi_set_loop_count(struct fplinux_midi *midi, int count);

/*
 * Flush the old consumer transport before seek. Latest target wins, clamped
 * to duration. Returns 0 ready, 1 pending, -1 error. read advances a pending
 * seek in bounded silent rendering steps even when READY/PAUSED, returning
 * -1/EAGAIN until ready. Position reports the requested target while pending.
 */
int fplinux_midi_seek(struct fplinux_midi *midi, uint64_t position_ms);
/*
 * Interleaved native-endian S16 stereo, capacity frames <= BLOCK_FRAMES.
 * Positive results always belong to the caller, including the final block.
 * Zero means EOF; -1/EAGAIN means paused, ready or a bounded pending step.
 * Nominal media time excludes the note-release tail at final EOF. MIDI sends
 * pedal-off/all-notes-off and drains active voices for at most five seconds.
 */
ssize_t fplinux_midi_read(struct fplinux_midi *midi, int16_t *samples,
			  size_t capacity);

#endif
