/* SPDX-License-Identifier: 0BSD */
#include "audio-broker.h"
#include <fplinux/fplinux-runtime-media.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

struct audio_handle {
	uint32_t id, runtime_id, kind, format;
	uint32_t total, uploaded, start_ms, reply_sequence;
	unsigned char *upload;
	unsigned int remaining, gain;
	uint32_t operation;
	bool prepared, starting, seek_done, repeat_start, pending;
	bool started;
};

struct mre_audio_broker {
	struct fplinux_runtime_media *media;
	struct audio_handle handles[MRE_AUDIO_HANDLES];
	mre_audio_reply_fn reply;
	mre_audio_event_fn event;
	void *context;
	size_t encoded_bytes;
	uint32_t last_id;
	unsigned int gain;
	bool failed, visible;
};

static void drive(struct mre_audio_broker *broker, struct audio_handle *handle);

static struct audio_handle *find_handle(struct mre_audio_broker *broker,
					uint32_t id, bool runtime)
{
	for (size_t i = 0; i < MRE_AUDIO_HANDLES; ++i) {
		struct audio_handle *handle = &broker->handles[i];
		if (handle->id &&
		    (runtime ? handle->runtime_id : handle->id) == id)
			return handle;
	}
	return NULL;
}

static void retire(struct mre_audio_broker *broker, struct audio_handle *handle)
{
	uint32_t runtime_id = handle->runtime_id;

	broker->encoded_bytes -= handle->total;
	free(handle->upload);
	memset(handle, 0, sizeof(*handle));
	if (runtime_id)
		fplinux_runtime_media_destroy(broker->media, runtime_id);
}

static void fail_transport(struct mre_audio_broker *broker)
{
	broker->failed = true;
	for (size_t i = 0; i < MRE_AUDIO_HANDLES; ++i)
		if (broker->handles[i].id)
			retire(broker, &broker->handles[i]);
}

static void reply(struct mre_audio_broker *broker, uint32_t sequence,
		  int32_t result)
{
	if (!broker->reply(sequence, result, broker->context))
		fail_transport(broker);
}

static void notify(struct mre_audio_broker *broker, uint32_t id, int32_t event)
{
	if (!broker->event(id, event, broker->context))
		fail_transport(broker);
}

static void fail_handle(struct mre_audio_broker *broker,
			struct audio_handle *handle)
{
	uint32_t sequence = handle->reply_sequence;
	uint32_t id = handle->id;
	int32_t event = handle->kind == MRE_AUDIO_MIDI ? 2 : -1;
	bool started = handle->started;

	retire(broker, handle);
	if (sequence)
		reply(broker, sequence, -1);
	if (started && !broker->failed)
		notify(broker, id, event);
}

static bool command(struct mre_audio_broker *broker,
		    struct audio_handle *handle,
		    enum fplinux_runtime_media_command operation,
		    uint64_t value)
{
	if (fplinux_runtime_media_command(broker->media, handle->runtime_id,
					  operation, value) < 0) {
		fail_handle(broker, handle);
		return false;
	}
	handle->pending = true;
	return true;
}

static void media_event(const struct fplinux_runtime_media_event *event,
			void *context)
{
	struct mre_audio_broker *broker = context;
	struct audio_handle *handle = find_handle(broker, event->handle, true);

	if (!handle || broker->failed)
		return;
	if (event->error || event->type == FPLINUX_RUNTIME_MEDIA_ERROR) {
		fail_handle(broker, handle);
		return;
	}
	if (event->type == FPLINUX_RUNTIME_MEDIA_END) {
		if (handle->kind == MRE_AUDIO_MIDI &&
		    (!handle->remaining || handle->remaining > 1U)) {
			if (handle->remaining > 1U)
				handle->remaining--;
			handle->repeat_start = true;
			drive(broker, handle);
			return;
		}
		uint32_t id = handle->id;
		int32_t result = handle->kind == MRE_AUDIO_MIDI ? 2 : 5;

		retire(broker, handle);
		notify(broker, id, result);
		return;
	}
	if (event->type != FPLINUX_RUNTIME_MEDIA_RESULT)
		return;
	handle->pending = false;
	switch (event->command) {
	case FPLINUX_RUNTIME_MEDIA_PREPARE:
		handle->prepared = true;
		break;
	case FPLINUX_RUNTIME_MEDIA_GAIN:
		/* Store the value submitted, even if vm_set_volume changed again. */
		break;
	case FPLINUX_RUNTIME_MEDIA_SEEK:
		handle->seek_done = true;
		break;
	case FPLINUX_RUNTIME_MEDIA_START: {
		uint32_t id = handle->id;
		bool initial = handle->starting;
		bool midi = handle->kind == MRE_AUDIO_MIDI;
		bool resuming = handle->operation == MRE_AUDIO_RESUME;
		uint32_t sequence =
			initial || resuming ? handle->reply_sequence : 0;

		handle->starting = handle->repeat_start = false;
		handle->started = true;
		/* A user pause arriving during an internal loop restart still needs
		 * its own PAUSE result; START cannot acknowledge that operation. */
		if (initial || resuming)
			handle->reply_sequence = handle->operation = 0;
		if (sequence)
			reply(broker, sequence,
			      initial && midi ? (int32_t)id : 0);
		if (initial && midi && !broker->failed)
			notify(broker, id, 1);
		break;
	}
	case FPLINUX_RUNTIME_MEDIA_PAUSE: {
		uint32_t sequence = handle->reply_sequence;

		handle->reply_sequence = handle->operation = 0;
		if (sequence)
			reply(broker, sequence, 0);
		break;
	}
	default:
		break;
	}
	if (handle->id && !broker->failed)
		drive(broker, handle);
}

static void drive(struct mre_audio_broker *broker, struct audio_handle *handle)
{
	if (broker->failed || handle->pending || !handle->prepared)
		return;
	if (handle->gain != broker->gain) {
		handle->gain = broker->gain;
		(void)command(broker, handle, FPLINUX_RUNTIME_MEDIA_GAIN,
			      handle->gain);
		return;
	}
	if (handle->starting && !handle->seek_done) {
		(void)command(broker, handle, FPLINUX_RUNTIME_MEDIA_SEEK,
			      (uint64_t)handle->start_ms * 1000U);
		return;
	}
	if (handle->starting || handle->repeat_start ||
	    handle->operation == MRE_AUDIO_RESUME) {
		if (!broker->visible && handle->repeat_start)
			return;
		(void)command(broker, handle, FPLINUX_RUNTIME_MEDIA_START, 0);
		return;
	}
	if (handle->operation == MRE_AUDIO_PAUSE)
		(void)command(broker, handle, FPLINUX_RUNTIME_MEDIA_PAUSE, 0);
}

struct mre_audio_broker *mre_audio_broker_open(struct fplinux_ui *ui,
					       mre_audio_reply_fn reply_fn,
					       mre_audio_event_fn event,
					       void *context)
{
	if (!ui || !reply_fn || !event) {
		errno = EINVAL;
		return NULL;
	}
	struct mre_audio_broker *broker = calloc(1, sizeof(*broker));

	if (!broker)
		return NULL;
	broker->gain = 100;
	broker->visible = true;
	broker->reply = reply_fn;
	broker->event = event;
	broker->context = context;
	broker->media = fplinux_runtime_media_open(ui, media_event, broker);
	if (!broker->media) {
		free(broker);
		return NULL;
	}
	return broker;
}

void mre_audio_broker_close(struct mre_audio_broker *broker)
{
	if (!broker)
		return;
	for (size_t i = 0; i < MRE_AUDIO_HANDLES; ++i)
		if (broker->handles[i].id)
			retire(broker, &broker->handles[i]);
	fplinux_runtime_media_close(broker->media);
	free(broker);
}

void mre_audio_broker_set_visible(struct mre_audio_broker *broker, bool visible)
{
	if (!broker)
		return;
	broker->visible = visible;
	fplinux_runtime_media_set_visible(broker->media, visible);
	if (visible)
		for (size_t i = 0; i < MRE_AUDIO_HANDLES; ++i)
			if (broker->handles[i].id)
				drive(broker, &broker->handles[i]);
}

static enum fplinux_runtime_media_format runtime_format(uint32_t format)
{
	switch (format) {
	case MRE_AUDIO_MP3:
		return FPLINUX_RUNTIME_MEDIA_MP3;
	default:
		return FPLINUX_RUNTIME_MEDIA_MIDI;
	}
}

static bool begin(struct mre_audio_broker *broker,
		  const struct mre_audio_request *request)
{
	if (!broker->visible || !request->handle ||
	    request->handle > INT32_MAX || request->handle <= broker->last_id ||
	    !request->total_bytes ||
	    request->total_bytes > MRE_AUDIO_BYTES - broker->encoded_bytes ||
	    (request->kind != MRE_AUDIO_MIDI &&
	     request->kind != MRE_AUDIO_GENERIC) ||
	    (request->kind == MRE_AUDIO_MIDI &&
	     request->format != MRE_AUDIO_SMF) ||
	    (request->kind == MRE_AUDIO_GENERIC &&
	     request->format != MRE_AUDIO_MP3) ||
	    request->repeat < 0 || request->repeat > 255)
		return false;
	struct audio_handle *handle = NULL;

	for (size_t i = 0; i < MRE_AUDIO_HANDLES; ++i)
		if (!broker->handles[i].id) {
			handle = &broker->handles[i];
			break;
		}
	if (!handle)
		return false;
	unsigned char *upload = malloc(request->total_bytes);

	if (!upload)
		return false;
	*handle = (struct audio_handle){
		.id = request->handle,
		.kind = request->kind,
		.format = request->format,
		.total = request->total_bytes,
		.start_ms = request->start_ms,
		.remaining = (unsigned int)request->repeat,
		.upload = upload,
		.gain = 100,
	};
	broker->last_id = request->handle;
	broker->encoded_bytes += request->total_bytes;
	return true;
}

bool mre_audio_broker_dispatch(struct mre_audio_broker *broker,
			       const struct mre_audio_request *request)
{
	if (!broker || broker->failed || !request || !request->sequence)
		return false;
	struct audio_handle *handle =
		find_handle(broker, request->handle, false);
	int32_t result = -1;

	switch (request->operation) {
	case MRE_AUDIO_BEGIN:
		result = begin(broker, request) ? 0 : -1;
		break;
	case MRE_AUDIO_WRITE:
		if (handle && handle->upload && request->bytes &&
		    request->bytes <= MRE_AUDIO_CHUNK_BYTES &&
		    request->offset == handle->uploaded &&
		    request->bytes <= handle->total - handle->uploaded) {
			memcpy(handle->upload + handle->uploaded, request->data,
			       request->bytes);
			handle->uploaded += request->bytes;
			result = 0;
		}
		break;
	case MRE_AUDIO_PLAY:
		if (!handle || !handle->upload ||
		    handle->uploaded != handle->total)
			break;
		handle->runtime_id = fplinux_runtime_media_create(
			broker->media, runtime_format(handle->format),
			handle->upload, handle->total, 0, 0);
		if (!handle->runtime_id) {
			retire(broker, handle);
			break;
		}
		free(handle->upload);
		handle->upload = NULL;
		handle->starting = handle->pending = true;
		handle->reply_sequence = request->sequence;
		return true;
	case MRE_AUDIO_PAUSE:
	case MRE_AUDIO_RESUME:
		if (!handle || !handle->started || handle->reply_sequence)
			break;
		handle->operation = request->operation;
		handle->reply_sequence = request->sequence;
		drive(broker, handle);
		return !broker->failed;
	case MRE_AUDIO_STOP:
		if (handle) {
			retire(broker, handle);
			result = 0;
		}
		break;
	case MRE_AUDIO_STOP_MIDI:
		for (size_t i = 0; i < MRE_AUDIO_HANDLES; ++i)
			if (broker->handles[i].id &&
			    broker->handles[i].kind == MRE_AUDIO_MIDI)
				retire(broker, &broker->handles[i]);
		result = 0;
		break;
	case MRE_AUDIO_VOLUME:
		if (request->volume < 0 || request->volume > 6)
			break;
		broker->gain = ((unsigned int)request->volume * 100U + 3U) / 6U;
		for (size_t i = 0; i < MRE_AUDIO_HANDLES; ++i)
			if (broker->handles[i].id)
				drive(broker, &broker->handles[i]);
		result = 0;
		break;
	default:
		break;
	}
	reply(broker, request->sequence, result);
	return !broker->failed;
}
