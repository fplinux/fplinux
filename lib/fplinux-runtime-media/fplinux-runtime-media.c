/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define _GNU_SOURCE
#include "fplinux-runtime-media.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include <fplinux/fplinux-media.h>
#include <fplinux/fplinux-midi.h>
#include <fplinux/fplinux-ui.h>
#include <fplinux/fplinux-ui-audio.h>

struct media_slot {
	uint32_t handle;
	bool destroyed;
	bool pending;
	bool prepared;
	bool requested;
	bool result_ready;
	bool rendering;
	bool allowed;
	bool eof;
	bool ended;
	bool rollback_start;
	bool rollback_done;
	int error;
	enum fplinux_runtime_media_command command;
	uint64_t value;
	struct fplinux_runtime_media_info info;
	void *bytes;
	size_t byte_count;
	unsigned int note;
	uint64_t tone_ms;
	int16_t samples[FPLINUX_MEDIA_BLOCK_FRAMES * 2];
	size_t frames;
	size_t consumed;
	/* Only the worker touches decoder objects and deferred seek state. */
	struct fplinux_media *decoder;
	struct fplinux_midi *midi;
	enum fplinux_runtime_media_format decoded_format;
	bool seek_pending;
	bool resume_seek;
	uint64_t resume_us;
	uint64_t seek_position_us;
	/* Only the UI owner touches the transport and heard-frame accounting. */
	struct fplinux_audio_stream *stream;
	uint64_t base_us;
	uint64_t submitted;
};

struct fplinux_runtime_media {
	struct fplinux_ui *ui;
	fplinux_runtime_media_event_fn callback;
	void *context;
	pthread_t worker;
	pthread_mutex_t lock;
	pthread_cond_t changed;
	bool stopping;
	bool visible;
	bool timer_active;
	int notice;
	int timer;
	uint32_t serial;
	size_t source_bytes;
	struct media_slot slots[FPLINUX_RUNTIME_MEDIA_HANDLES];
};

static void notify(struct fplinux_runtime_media *media)
{
	uint64_t one = 1;
	ssize_t result;

	do {
		result = write(media->notice, &one, sizeof(one));
	} while (result < 0 && errno == EINTR);
}

static struct media_slot *find_slot(struct fplinux_runtime_media *media,
				    uint32_t handle)
{
	unsigned int i;

	for (i = 0; i < FPLINUX_RUNTIME_MEDIA_HANDLES; i++)
		if (media->slots[i].handle == handle &&
		    !media->slots[i].destroyed)
			return &media->slots[i];
	return NULL;
}

static void decoder_close(struct media_slot *slot)
{
	fplinux_media_close(slot->decoder);
	fplinux_midi_close(slot->midi);
	slot->decoder = NULL;
	slot->midi = NULL;
	free(slot->bytes);
	slot->bytes = NULL;
}

static void decoder_info(struct media_slot *slot,
			 struct fplinux_runtime_media_info *info)
{
	if (slot->midi) {
		struct fplinux_midi_info midi;

		if (fplinux_midi_get_info(slot->midi, &midi) < 0)
			return;
		info->rate = midi.rate;
		info->format = slot->decoded_format;
		info->channels = midi.channels;
		info->duration_us = midi.duration_us;
		info->duration_known = true;
	} else if (slot->decoder) {
		const struct fplinux_media_info *decoded =
			fplinux_media_info(slot->decoder);

		info->format = decoded->codec == FPLINUX_MEDIA_WAV ?
				       FPLINUX_RUNTIME_MEDIA_WAV :
				       FPLINUX_RUNTIME_MEDIA_MP3;
		info->rate = decoded->rate;
		info->channels = decoded->channels;
		info->duration_us = decoded->duration_ms * 1000;
		info->duration_known = decoded->duration_known;
	}
}

static int decoder_prepare(struct media_slot *slot)
{
	enum fplinux_runtime_media_format format = slot->info.format;
	const unsigned char *bytes = slot->bytes;
	int error;

	if (format == FPLINUX_RUNTIME_MEDIA_AUTO && slot->byte_count >= 4 &&
	    !memcmp(bytes, "MThd", 4))
		format = FPLINUX_RUNTIME_MEDIA_MIDI;
	slot->decoded_format = format;
	if (format == FPLINUX_RUNTIME_MEDIA_MIDI)
		slot->midi = fplinux_midi_open_bytes(bytes, slot->byte_count);
	else if (format == FPLINUX_RUNTIME_MEDIA_TONE_SEQUENCE)
		slot->midi = fplinux_midi_open_tone_sequence(bytes,
							     slot->byte_count);
	else if (format == FPLINUX_RUNTIME_MEDIA_TONE)
		slot->midi = fplinux_midi_open_tone(slot->note, slot->tone_ms);
	else
		slot->decoder =
			fplinux_media_open_bytes(bytes, slot->byte_count);
	error = errno;
	free(slot->bytes);
	slot->bytes = NULL;
	if (!slot->midi && !slot->decoder) {
		errno = error;
		return -1;
	}
	if (slot->decoder && format != FPLINUX_RUNTIME_MEDIA_AUTO) {
		const struct fplinux_media_info *info =
			fplinux_media_info(slot->decoder);

		if ((format == FPLINUX_RUNTIME_MEDIA_WAV &&
		     info->codec != FPLINUX_MEDIA_WAV) ||
		    (format == FPLINUX_RUNTIME_MEDIA_MP3 &&
		     info->codec != FPLINUX_MEDIA_MP3)) {
			errno = ENOTSUP;
			return -1;
		}
	}
	return 0;
}

static int decoder_seek(struct media_slot *slot, uint64_t position_us)
{
	int result =
		slot->midi ?
			fplinux_midi_seek(slot->midi, position_us / 1000) :
			fplinux_media_seek(slot->decoder, position_us / 1000);

	slot->seek_pending = result > 0;
	return result < 0 ? -1 : 0;
}

static bool decoder_seeking(struct media_slot *slot)
{
	if (slot->midi) {
		struct fplinux_midi_info info;

		return fplinux_midi_get_info(slot->midi, &info) == 0 &&
		       info.seeking;
	}
	return fplinux_media_info(slot->decoder)->seeking;
}

static uint64_t decoder_position(struct media_slot *slot)
{
	if (slot->midi) {
		struct fplinux_midi_info info;

		if (fplinux_midi_get_info(slot->midi, &info) == 0)
			return info.position_us;
	} else if (slot->decoder) {
		const struct fplinux_media_info *info =
			fplinux_media_info(slot->decoder);

		return info->position_frames * 1000000 / info->rate;
	}
	return 0;
}

static ssize_t decoder_read(struct media_slot *slot, int16_t *samples)
{
	return slot->midi ? fplinux_midi_read(slot->midi, samples,
					      FPLINUX_MEDIA_BLOCK_FRAMES) :
			    fplinux_media_read(slot->decoder, samples,
					       FPLINUX_MEDIA_BLOCK_FRAMES);
}

static int execute(struct media_slot *slot,
		   enum fplinux_runtime_media_command command, uint64_t value)
{
	int result = 0;

	switch (command) {
	case FPLINUX_RUNTIME_MEDIA_PREPARE:
		return decoder_prepare(slot);
	case FPLINUX_RUNTIME_MEDIA_PREFETCH:
		if (slot->midi)
			result = fplinux_midi_prefetch(slot->midi);
		break;
	case FPLINUX_RUNTIME_MEDIA_START:
		/* Decoder EOF may still have unheard frames in a paused stream. */
		if (slot->eof && !slot->ended)
			break;
		if (slot->midi)
			result = fplinux_midi_start(slot->midi);
		if (!result && slot->ended) {
			result = decoder_seek(slot, 0);
			slot->resume_seek = false;
		} else if (!result && slot->resume_seek) {
			result = decoder_seek(slot, slot->resume_us);
			slot->resume_seek = false;
		}
		break;
	case FPLINUX_RUNTIME_MEDIA_PAUSE:
		if (slot->midi) {
			result = fplinux_midi_release_active(slot->midi);
			slot->resume_seek = true;
			slot->resume_us = value;
		}
		break;
	case FPLINUX_RUNTIME_MEDIA_RELEASE:
		if (slot->midi)
			result = fplinux_midi_unprefetch(slot->midi);
		slot->resume_seek = true;
		slot->resume_us = value;
		break;
	case FPLINUX_RUNTIME_MEDIA_SEEK:
		if (slot->info.format == FPLINUX_RUNTIME_MEDIA_MIDI &&
		    !slot->info.playing) {
			uint64_t frames = value / 1000 * slot->info.rate / 1000;

			slot->resume_seek = true;
			slot->resume_us = value;
			slot->seek_pending = false;
			slot->seek_position_us =
				frames * 1000000 / slot->info.rate;
		} else {
			slot->resume_seek = false;
			result = decoder_seek(slot, value);
		}
		break;
	case FPLINUX_RUNTIME_MEDIA_GAIN:
		result = slot->midi ?
				 fplinux_midi_set_gain(slot->midi,
						       (unsigned int)value) :
				 fplinux_media_set_gain(slot->decoder,
							(unsigned int)value);
		break;
	}
	return result;
}

static void *worker_main(void *context)
{
	struct fplinux_runtime_media *media = context;
	unsigned int cursor = 0;
	int16_t samples[FPLINUX_MEDIA_BLOCK_FRAMES * 2];

	pthread_mutex_lock(&media->lock);
	while (!media->stopping) {
		struct media_slot *slot = NULL;
		unsigned int i;

		for (i = 0; i < FPLINUX_RUNTIME_MEDIA_HANDLES; i++) {
			struct media_slot *candidate = &media->slots[cursor];

			cursor = (cursor + 1) % FPLINUX_RUNTIME_MEDIA_HANDLES;
			if (candidate->handle &&
			    (candidate->destroyed ||
			     candidate->rollback_start ||
			     candidate->requested || candidate->seek_pending ||
			     (candidate->rendering && candidate->allowed &&
			      !candidate->pending && !candidate->frames &&
			      !candidate->eof && !candidate->error))) {
				slot = candidate;
				break;
			}
		}
		if (!slot) {
			pthread_cond_wait(&media->changed, &media->lock);
			continue;
		}
		if (slot->destroyed) {
			size_t byte_count = slot->byte_count;

			pthread_mutex_unlock(&media->lock);
			decoder_close(slot);
			pthread_mutex_lock(&media->lock);
			media->source_bytes -= byte_count;
			memset(slot, 0, sizeof(*slot));
			continue;
		}
		if (slot->rollback_start) {
			slot->rollback_start = false;
			pthread_mutex_unlock(&media->lock);
			fplinux_midi_release_active(slot->midi);
			pthread_mutex_lock(&media->lock);
			slot->rollback_done = true;
			slot->result_ready = true;
			notify(media);
			continue;
		}
		if (slot->requested) {
			enum fplinux_runtime_media_command command =
				slot->command;
			uint64_t value = slot->value;
			struct fplinux_runtime_media_info info = slot->info;
			int error;

			slot->requested = false;
			pthread_mutex_unlock(&media->lock);
			error = execute(slot, command, value) < 0 ? errno : 0;
			decoder_info(slot, &info);
			if (!error && command == FPLINUX_RUNTIME_MEDIA_SEEK &&
			    !slot->seek_pending)
				info.position_us =
					slot->resume_seek ?
						slot->seek_position_us :
						decoder_position(slot);
			pthread_mutex_lock(&media->lock);
			slot->info.rate = info.rate;
			slot->info.channels = info.channels;
			slot->info.format = info.format;
			slot->info.duration_us = info.duration_us;
			slot->info.duration_known = info.duration_known;
			if (command == FPLINUX_RUNTIME_MEDIA_PREPARE)
				slot->prepared = !error;
			if (!error && command == FPLINUX_RUNTIME_MEDIA_SEEK &&
			    !slot->seek_pending) {
				slot->seek_position_us = info.position_us;
			}
			slot->error = error;
			slot->result_ready = error || !slot->seek_pending;
			notify(media);
			continue;
		}
		bool seeking = slot->seek_pending;
		ssize_t frames;
		int error;

		pthread_mutex_unlock(&media->lock);
		frames = decoder_read(slot, samples);
		error = frames < 0 ? errno : 0;
		bool still_seeking = seeking && decoder_seeking(slot);
		uint64_t sought_position =
			seeking && !still_seeking ? decoder_position(slot) : 0;
		struct fplinux_runtime_media_info decoded_info = { 0 };

		decoder_info(slot, &decoded_info);
		pthread_mutex_lock(&media->lock);
		if (slot->destroyed)
			continue;
		slot->info.duration_known = decoded_info.duration_known;
		slot->info.duration_us = decoded_info.duration_us;
		if (slot->requested &&
		    (slot->command == FPLINUX_RUNTIME_MEDIA_SEEK ||
		     slot->command == FPLINUX_RUNTIME_MEDIA_RELEASE ||
		     (slot->command == FPLINUX_RUNTIME_MEDIA_PAUSE &&
		      slot->info.format == FPLINUX_RUNTIME_MEDIA_MIDI)))
			continue;
		if (seeking) {
			slot->seek_pending = still_seeking;
			if (!still_seeking || (error && error != EAGAIN)) {
				if (!still_seeking &&
				    slot->command ==
					    FPLINUX_RUNTIME_MEDIA_SEEK) {
					slot->seek_position_us =
						sought_position;
				}
				slot->error = error == EAGAIN ? 0 : error;
				slot->result_ready = true;
				notify(media);
			}
		} else if (frames > 0) {
			memcpy(slot->samples, samples,
			       (size_t)frames * slot->info.channels *
				       sizeof(int16_t));
			slot->frames = (size_t)frames;
			slot->consumed = 0;
			notify(media);
		} else if (!frames) {
			slot->eof = true;
			notify(media);
		} else if (error != EAGAIN) {
			slot->error = error;
			slot->rendering = false;
			notify(media);
		}
	}
	pthread_mutex_unlock(&media->lock);
	for (unsigned int i = 0; i < FPLINUX_RUNTIME_MEDIA_HANDLES; i++)
		decoder_close(&media->slots[i]);
	return NULL;
}

static uint64_t heard_position(struct media_slot *slot)
{
	long queued = slot->stream ? fplinux_ui_audio_queued(slot->stream) : 0;
	uint64_t heard = slot->submitted;
	uint64_t position;

	if (queued < 0)
		return slot->info.position_us;
	if (queued > 0)
		heard = (uint64_t)queued < heard ? heard - (uint64_t)queued : 0;
	position = slot->base_us;
	if (slot->info.rate)
		position += heard * 1000000 / slot->info.rate;
	if (slot->info.duration_known && position > slot->info.duration_us)
		position = slot->info.duration_us;
	return position;
}

static void close_stream(struct media_slot *slot)
{
	if (slot->stream) {
		slot->info.position_us = heard_position(slot);
		slot->base_us = slot->info.position_us;
		slot->submitted = 0;
	}
	fplinux_ui_audio_close(slot->stream);
	slot->stream = NULL;
}

static void fd_ready(struct fplinux_ui *ui, int fd, short events, void *context)
{
	struct fplinux_runtime_media *media = context;
	struct fplinux_runtime_media_event
		delivered[FPLINUX_RUNTIME_MEDIA_HANDLES];
	size_t count = 0;
	bool active = false;
	uint64_t value;
	ssize_t result;
	(void)ui;
	(void)events;

	do {
		result = read(fd, &value, sizeof(value));
	} while (result < 0 && errno == EINTR);
	pthread_mutex_lock(&media->lock);
	for (unsigned int i = 0; i < FPLINUX_RUNTIME_MEDIA_HANDLES; i++) {
		struct media_slot *slot = &media->slots[i];
		struct fplinux_runtime_media_event event = { 0 };
		bool emit = false;

		if (!slot->handle || slot->destroyed)
			continue;
		if (slot->result_ready && !slot->error &&
		    slot->command == FPLINUX_RUNTIME_MEDIA_SEEK)
			slot->base_us = slot->seek_position_us;
		slot->info.position_us = heard_position(slot);
		if (slot->result_ready) {
			if (!slot->error &&
			    (slot->command == FPLINUX_RUNTIME_MEDIA_START ||
			     (slot->command == FPLINUX_RUNTIME_MEDIA_SEEK &&
			      slot->info.playing))) {
				if (!slot->stream) {
					slot->stream = fplinux_ui_audio_open(
						media->ui, slot->info.rate,
						slot->info.channels);
					if (!slot->stream)
						slot->error = errno;
				} else if (!fplinux_ui_audio_pause(slot->stream,
								   false)) {
					slot->error = errno;
				}
				if (!slot->error) {
					if (slot->ended) {
						slot->base_us = 0;
						slot->submitted = 0;
						slot->eof = false;
					}
					slot->ended = false;
					slot->rendering = true;
					slot->info.playing = true;
					slot->info.position_us =
						heard_position(slot);
				}
			}
			if (slot->error && slot->midi &&
			    slot->command == FPLINUX_RUNTIME_MEDIA_START &&
			    !slot->rollback_done) {
				slot->rollback_start = true;
				slot->result_ready = false;
				slot->rendering = false;
				slot->info.playing = false;
				close_stream(slot);
				continue;
			}
			slot->rollback_done = false;
			event.type = FPLINUX_RUNTIME_MEDIA_RESULT;
			event.command = slot->command;
			event.error = slot->error;
			slot->pending = false;
			slot->result_ready = false;
			if (slot->error) {
				slot->rendering = false;
				slot->info.playing = false;
				close_stream(slot);
			}
			emit = true;
		} else if (slot->error && slot->info.playing) {
			slot->info.playing = false;
			slot->rendering = false;
			close_stream(slot);
			event.type = FPLINUX_RUNTIME_MEDIA_ERROR;
			event.error = slot->error;
			emit = true;
		}
		if (slot->stream && slot->info.playing && !slot->pending) {
			struct fplinux_ui_audio_status status;

			if (!fplinux_ui_audio_status(slot->stream, &status) ||
			    status.state == FPLINUX_UI_AUDIO_ENDED) {
				slot->error = EPIPE;
				slot->allowed = false;
			} else {
				slot->allowed =
					media->visible &&
					status.state ==
						FPLINUX_UI_AUDIO_PLAYING;
			}
			if (slot->allowed && slot->frames) {
				long written = fplinux_ui_audio_write(
					slot->stream,
					slot->samples +
						slot->consumed *
							slot->info.channels,
					slot->frames - slot->consumed);

				if (written < 0) {
					slot->error = errno;
				} else {
					slot->consumed += (size_t)written;
					slot->submitted += (uint64_t)written;
					if (slot->consumed == slot->frames)
						slot->frames = slot->consumed =
							0;
				}
			}
			if (slot->eof && !slot->frames &&
			    fplinux_ui_audio_queued(slot->stream) == 0 &&
			    !emit) {
				slot->info.position_us = heard_position(slot);
				slot->info.playing = false;
				slot->rendering = false;
				slot->ended = true;
				close_stream(slot);
				event.type = FPLINUX_RUNTIME_MEDIA_END;
				emit = true;
			}
		}
		if (emit) {
			event.handle = slot->handle;
			event.info = slot->info;
			delivered[count++] = event;
		}
		active |= media->visible && slot->info.playing;
	}
	pthread_cond_signal(&media->changed);
	pthread_mutex_unlock(&media->lock);
	if (active != media->timer_active) {
		struct itimerspec interval = { 0 };

		if (active) {
			interval.it_interval.tv_nsec = 20000000;
			interval.it_value.tv_nsec = 20000000;
		}
		timerfd_settime(media->timer, 0, &interval, NULL);
		media->timer_active = active;
	}
	for (size_t i = 0; i < count; i++)
		media->callback(&delivered[i], media->context);
}

struct fplinux_runtime_media *
fplinux_runtime_media_open(struct fplinux_ui *ui,
			   fplinux_runtime_media_event_fn callback,
			   void *context)
{
	struct fplinux_runtime_media *media;
	int error;

	if (!ui || !callback || !fplinux_ui_audio_supported(ui)) {
		errno = ENOTSUP;
		return NULL;
	}
	media = calloc(1, sizeof(*media));
	if (!media)
		return NULL;
	media->notice = -1;
	media->timer = -1;
	media->ui = ui;
	media->callback = callback;
	media->context = context;
	media->visible = fplinux_ui_is_visible(ui);
	pthread_mutex_init(&media->lock, NULL);
	pthread_cond_init(&media->changed, NULL);
	media->notice = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	media->timer =
		timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
	if (media->notice < 0 || media->timer < 0 ||
	    !fplinux_ui_watch_fd(ui, media->notice, POLLIN, fd_ready, media) ||
	    !fplinux_ui_watch_fd(ui, media->timer, POLLIN, fd_ready, media))
		goto fail;
	error = pthread_create(&media->worker, NULL, worker_main, media);
	if (error) {
		errno = error;
		goto fail;
	}
	return media;
fail:
	error = errno;
	fplinux_ui_unwatch_fd(ui, media->notice);
	fplinux_ui_unwatch_fd(ui, media->timer);
	if (media->timer >= 0)
		close(media->timer);
	if (media->notice >= 0)
		close(media->notice);
	pthread_cond_destroy(&media->changed);
	pthread_mutex_destroy(&media->lock);
	free(media);
	errno = error;
	return NULL;
}

void fplinux_runtime_media_close(struct fplinux_runtime_media *media)
{
	if (!media)
		return;
	fplinux_ui_unwatch_fd(media->ui, media->notice);
	fplinux_ui_unwatch_fd(media->ui, media->timer);
	pthread_mutex_lock(&media->lock);
	media->stopping = true;
	pthread_cond_signal(&media->changed);
	pthread_mutex_unlock(&media->lock);
	pthread_join(media->worker, NULL);
	for (unsigned int i = 0; i < FPLINUX_RUNTIME_MEDIA_HANDLES; i++)
		close_stream(&media->slots[i]);
	close(media->timer);
	close(media->notice);
	pthread_cond_destroy(&media->changed);
	pthread_mutex_destroy(&media->lock);
	free(media);
}

uint32_t fplinux_runtime_media_create(struct fplinux_runtime_media *media,
				      enum fplinux_runtime_media_format format,
				      const void *bytes, size_t count,
				      unsigned int note, uint64_t duration_ms)
{
	struct media_slot *slot = NULL;
	void *copied = NULL;
	uint32_t handle = 0;

	if (!media || format > FPLINUX_RUNTIME_MEDIA_TONE ||
	    (format == FPLINUX_RUNTIME_MEDIA_TONE ?
		     (note > 127 || !duration_ms) :
		     (!bytes || !count))) {
		errno = EINVAL;
		return 0;
	}
	if (count > FPLINUX_RUNTIME_MEDIA_BYTES) {
		errno = EFBIG;
		return 0;
	}
	if (format != FPLINUX_RUNTIME_MEDIA_TONE) {
		copied = malloc(count);
		if (!copied)
			return 0;
		memcpy(copied, bytes, count);
	} else {
		count = 0;
	}
	pthread_mutex_lock(&media->lock);
	for (unsigned int i = 0; i < FPLINUX_RUNTIME_MEDIA_HANDLES; i++)
		if (!media->slots[i].handle) {
			slot = &media->slots[i];
			break;
		}
	if (!slot) {
		errno = EMFILE;
	} else if (count > FPLINUX_RUNTIME_MEDIA_BYTES - media->source_bytes) {
		errno = EFBIG;
	} else {
		media->serial = (media->serial % 0x7ffffffeU) + 1;
		handle = media->serial;
		slot->handle = handle;
		slot->bytes = copied;
		slot->byte_count = count;
		slot->note = note;
		slot->tone_ms = duration_ms;
		slot->info.format = format;
		slot->command = FPLINUX_RUNTIME_MEDIA_PREPARE;
		slot->pending = slot->requested = true;
		media->source_bytes += count;
		pthread_cond_signal(&media->changed);
	}
	pthread_mutex_unlock(&media->lock);
	if (!handle)
		free(copied);
	return handle;
}

void fplinux_runtime_media_destroy(struct fplinux_runtime_media *media,
				   uint32_t handle)
{
	if (!media || !handle)
		return;
	pthread_mutex_lock(&media->lock);
	struct media_slot *slot = find_slot(media, handle);

	if (slot) {
		slot->destroyed = true;
		close_stream(slot);
		pthread_cond_signal(&media->changed);
	}
	pthread_mutex_unlock(&media->lock);
}

int fplinux_runtime_media_command(struct fplinux_runtime_media *media,
				  uint32_t handle,
				  enum fplinux_runtime_media_command command,
				  uint64_t value)
{
	int result = -1;

	if (!media || command <= FPLINUX_RUNTIME_MEDIA_PREPARE ||
	    command > FPLINUX_RUNTIME_MEDIA_GAIN ||
	    (command == FPLINUX_RUNTIME_MEDIA_GAIN && value > 100)) {
		errno = EINVAL;
		return -1;
	}
	pthread_mutex_lock(&media->lock);
	struct media_slot *slot = find_slot(media, handle);

	if (!slot) {
		errno = EBADF;
	} else if (slot->pending) {
		errno = EBUSY;
	} else if (!slot->prepared) {
		errno = slot->error ? slot->error : EINVAL;
	} else if (command == FPLINUX_RUNTIME_MEDIA_START && !media->visible) {
		errno = EAGAIN;
	} else {
		slot->error = 0;
		slot->info.position_us = heard_position(slot);
		if (command == FPLINUX_RUNTIME_MEDIA_PAUSE) {
			slot->rendering = false;
			slot->info.playing = false;
			if (slot->info.format == FPLINUX_RUNTIME_MEDIA_MIDI) {
				value = slot->info.position_us;
				slot->base_us = value;
				slot->submitted = 0;
				close_stream(slot);
				slot->frames = slot->consumed = 0;
				slot->eof = false;
			} else if (slot->stream) {
				fplinux_ui_audio_pause(slot->stream, true);
			}
		} else if (command == FPLINUX_RUNTIME_MEDIA_RELEASE ||
			   command == FPLINUX_RUNTIME_MEDIA_SEEK) {
			if (command == FPLINUX_RUNTIME_MEDIA_RELEASE)
				value = slot->info.position_us;
			if (slot->info.duration_known &&
			    value > slot->info.duration_us)
				value = slot->info.duration_us;
			slot->base_us = value / 1000 * 1000;
			slot->submitted = 0;
			close_stream(slot);
			slot->frames = slot->consumed = 0;
			slot->eof = false;
			if (command == FPLINUX_RUNTIME_MEDIA_SEEK)
				slot->ended = false;
			slot->info.position_us = slot->base_us;
			if (command == FPLINUX_RUNTIME_MEDIA_RELEASE) {
				slot->rendering = false;
				slot->info.playing = false;
			}
		}
		slot->command = command;
		slot->value = value;
		slot->pending = slot->requested = true;
		slot->result_ready = false;
		pthread_cond_signal(&media->changed);
		result = 0;
	}
	pthread_mutex_unlock(&media->lock);
	return result;
}

bool fplinux_runtime_media_info(struct fplinux_runtime_media *media,
				uint32_t handle,
				struct fplinux_runtime_media_info *info)
{
	bool found = false;

	if (!media || !info)
		return false;
	pthread_mutex_lock(&media->lock);
	struct media_slot *slot = find_slot(media, handle);

	if (slot) {
		*info = slot->info;
		info->position_us = heard_position(slot);
		found = true;
	}
	pthread_mutex_unlock(&media->lock);
	return found;
}

void fplinux_runtime_media_set_visible(struct fplinux_runtime_media *media,
				       bool visible)
{
	if (!media)
		return;
	pthread_mutex_lock(&media->lock);
	if (media->visible == visible) {
		pthread_mutex_unlock(&media->lock);
		return;
	}
	media->visible = visible;
	for (unsigned int i = 0; i < FPLINUX_RUNTIME_MEDIA_HANDLES; i++) {
		struct media_slot *slot = &media->slots[i];

		slot->allowed = visible;
		if (slot->stream)
			fplinux_ui_audio_pause(slot->stream,
					       !visible || !slot->info.playing);
	}
	pthread_cond_signal(&media->changed);
	pthread_mutex_unlock(&media->lock);
	notify(media);
}
