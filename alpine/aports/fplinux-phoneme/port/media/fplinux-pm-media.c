/* SPDX-License-Identifier: GPL-2.0-only */
/* fplinux-check: package-embedded */
#include "fplinux-pm-media.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <kni.h>
#include <midpServices.h>
#include <midp_thread.h>
#include <sni.h>

#include <fplinux/fplinux-runtime-media.h>
#include "../app/fplinux-pm-app.h"

#define MEDIA_EVENT_DESCRIPTOR (INT_MIN + 0x4d45)
#define MEDIA_EVENTS 64U
#define MEDIA_EVENT_RESERVE (2U * FPLINUX_RUNTIME_MEDIA_HANDLES)
#define MEDIA_SOURCE_BYTES (2U * 1024U * 1024U)

_Static_assert(MEDIA_EVENT_RESERVE < MEDIA_EVENTS, "media completion reserve");

/* Format and event IDs used by PlatformPlayer. */
enum java_media_format {
	JAVA_MEDIA_AUTO = 0,
	JAVA_MEDIA_WAV = 1,
	JAVA_MEDIA_MP3 = 2,
	JAVA_MEDIA_MIDI = 3,
	JAVA_MEDIA_SEQUENCE = 4,
	JAVA_MEDIA_TONE = 5,
	JAVA_MEDIA_AMR_NB = 6,
	JAVA_MEDIA_FORMAT_COUNT,
};

static const enum fplinux_runtime_media_format
	runtime_formats[JAVA_MEDIA_FORMAT_COUNT] = {
		[JAVA_MEDIA_AUTO] = FPLINUX_RUNTIME_MEDIA_AUTO,
		[JAVA_MEDIA_WAV] = FPLINUX_RUNTIME_MEDIA_WAV,
		[JAVA_MEDIA_MP3] = FPLINUX_RUNTIME_MEDIA_MP3,
		[JAVA_MEDIA_MIDI] = FPLINUX_RUNTIME_MEDIA_MIDI,
		[JAVA_MEDIA_SEQUENCE] = FPLINUX_RUNTIME_MEDIA_TONE_SEQUENCE,
		[JAVA_MEDIA_TONE] = FPLINUX_RUNTIME_MEDIA_TONE,
		[JAVA_MEDIA_AMR_NB] = FPLINUX_RUNTIME_MEDIA_AMR_NB,
	};

enum java_media_event {
	JAVA_MEDIA_END = 1,
	JAVA_MEDIA_ERROR = 2,
	JAVA_MEDIA_STARTED = 3,
	JAVA_MEDIA_STOPPED = 4,
	JAVA_MEDIA_CLOSED = 5,
	JAVA_MEDIA_VOLUME = 6,
	JAVA_MEDIA_DURATION = 7,
};

struct media_event {
	int32_t handle; /* Broker handle or an unrealized player's client ID. */
	enum java_media_event type;
	int64_t time_us;
	int error;
	uint32_t playback;
};

static struct {
	jfieldID handle;
	jfieldID type;
	jfieldID time_us;
	jfieldID error;
	jfieldID playback;
} event_fields;

struct media_result {
	uint32_t handle;
	bool ready;
	bool waiting;
	bool destroyed;
	int error;
	uint64_t value;
	uint32_t playback;
	bool event_pending;
	bool duration_known;
	bool duration_pending;
	uint64_t duration_us;
	struct media_event event;
};

static struct {
	struct fplinux_runtime_media *broker;
	void (*wakeup)(void *);
	void *context;
	bool visible;
	bool stopped;
	struct media_result results[FPLINUX_RUNTIME_MEDIA_HANDLES];
	struct media_event events[MEDIA_EVENTS];
	size_t first;
	size_t count;
} audio = { .visible = true };

static void wake_app(void)
{
	if (audio.wakeup)
		audio.wakeup(audio.context);
}

static struct media_result *find_result(uint32_t handle)
{
	for (unsigned int i = 0; i < FPLINUX_RUNTIME_MEDIA_HANDLES; i++)
		if (audio.results[i].handle == handle)
			return &audio.results[i];
	return NULL;
}

static bool queue_event(struct media_event event)
{
	if (audio.count == MEDIA_EVENTS) {
		errno = EAGAIN;
		return false;
	}
	size_t next = (audio.first + audio.count) % MEDIA_EVENTS;

	audio.events[next] = event;
	audio.count++;
	wake_app();
	return true;
}

static void
queue_playback_event(struct media_result *result, enum java_media_event type,
		     const struct fplinux_runtime_media_event *event)
{
	struct media_event queued = {
		.handle = event->handle,
		.type = type,
		.time_us = event->info.position_us,
		.error = event->error,
		.playback = result->playback,
	};

	if (!queue_event(queued)) {
		result->event_pending = true;
		result->event = queued;
	}
}

static void broker_event(const struct fplinux_runtime_media_event *event,
			 void *context)
{
	struct media_result *result = find_result(event->handle);
	(void)context;

	if (!result)
		return;
	if (event->info.duration_known &&
	    (!result->duration_known ||
	     result->duration_us != event->info.duration_us)) {
		if (event->type != FPLINUX_RUNTIME_MEDIA_RESULT ||
		    event->command != FPLINUX_RUNTIME_MEDIA_PREPARE)
			result->duration_pending =
				!queue_event((struct media_event){
					.handle = event->handle,
					.type = JAVA_MEDIA_DURATION,
					.time_us = event->info.duration_us,
				});
		result->duration_known = true;
		result->duration_us = event->info.duration_us;
	}
	if (event->type == FPLINUX_RUNTIME_MEDIA_RESULT) {
		result->ready = true;
		result->error = event->error;
		result->value = event->info.position_us;
		if (!event->error &&
		    event->command == FPLINUX_RUNTIME_MEDIA_START) {
			/* Give Java the same identity carried by completion. */
			result->value = ++result->playback;
			queue_playback_event(result, JAVA_MEDIA_STARTED, event);
		} else if (!event->error &&
			   event->command == FPLINUX_RUNTIME_MEDIA_PAUSE) {
			queue_playback_event(result, JAVA_MEDIA_STOPPED, event);
		}
	} else {
		enum java_media_event type =
			event->type == FPLINUX_RUNTIME_MEDIA_END ?
				JAVA_MEDIA_END :
				JAVA_MEDIA_ERROR;

		queue_playback_event(result, type, event);
	}
	wake_app();
}

static bool ensure_broker(void)
{
	if (audio.stopped) {
		errno = ECANCELED;
		return false;
	}
	if (!audio.broker) {
		audio.broker = fplinux_runtime_media_open(fplinux_pm_app_ui(),
							  broker_event, NULL);
		if (audio.broker)
			fplinux_runtime_media_set_visible(audio.broker,
							  audio.visible);
	}
	return audio.broker != NULL;
}

void fplinux_pm_media_set_wakeup(void (*wakeup)(void *), void *context)
{
	audio.wakeup = wakeup;
	audio.context = context;
}

void fplinux_pm_media_set_visible(bool visible)
{
	audio.visible = visible;
	if (audio.broker)
		fplinux_runtime_media_set_visible(audio.broker, visible);
}

static void flush_pending_events(void)
{
	for (unsigned int i = 0; i < FPLINUX_RUNTIME_MEDIA_HANDLES; i++) {
		struct media_result *result = &audio.results[i];

		if (result->duration_pending && audio.count < MEDIA_EVENTS) {
			queue_event((struct media_event){
				.handle = result->handle,
				.type = JAVA_MEDIA_DURATION,
				.time_us = result->duration_us,
			});
			result->duration_pending = false;
		}
		if (result->event_pending && !result->duration_pending &&
		    audio.count < MEDIA_EVENTS) {
			queue_event(result->event);
			result->event_pending = false;
		}
	}
}

void fplinux_pm_media_check_events(JVMSPI_BlockedThreadInfo *threads, int count)
{
	flush_pending_events();
	for (unsigned int i = 0; i < FPLINUX_RUNTIME_MEDIA_HANDLES; i++) {
		struct media_result *result = &audio.results[i];

		if (result->handle && result->ready)
			midp_thread_signal_list(threads, count, AMS_SIGNAL,
						-(int)result->handle, 0);
	}
	if (audio.count || audio.stopped)
		midp_thread_signal_list(threads, count, AMS_SIGNAL,
					MEDIA_EVENT_DESCRIPTOR, 0);
}

void fplinux_pm_media_cleanup(void)
{
	fplinux_runtime_media_close(audio.broker);
	audio.broker = NULL;
	audio.stopped = true;
	audio.first = audio.count = 0;
	memset(audio.results, 0, sizeof(audio.results));
}

void fplinux_pm_media_cancel(void)
{
	if (audio.broker)
		for (unsigned int i = 0; i < FPLINUX_RUNTIME_MEDIA_HANDLES;
		     i++) {
			struct media_result *result = &audio.results[i];

			if (!result->handle)
				continue;
			fplinux_runtime_media_destroy(audio.broker,
						      result->handle);
			result->ready = true;
			result->error = ECANCELED;
		}
	audio.stopped = true;
	wake_app();
}

static void throw_media(int error)
{
	KNI_ThrowNew("javax/microedition/media/MediaException",
		     strerror(error));
}

/* Blocks only the calling Java thread. Its native is re-entered after a
 * normal VM slice observes completion; the UI loop keeps pumping meanwhile. */
static bool wait_result(uint32_t handle, int *error, uint64_t *value)
{
	struct media_result *result = find_result(handle);

	if (!result) {
		*error = EBADF;
		return true;
	}
	if (!result->ready) {
		result->waiting = true;
		if (midp_thread_wait(AMS_SIGNAL, -(int)handle,
				     (void *)(uintptr_t)handle) < 0) {
			fplinux_runtime_media_destroy(audio.broker, handle);
			queue_event((struct media_event){
				.handle = handle,
				.type = JAVA_MEDIA_ERROR,
				.error = ENOMEM,
				.playback = result->playback,
			});
			memset(result, 0, sizeof(*result));
			*error = ENOMEM;
			return true;
		}
		return false;
	}
	*error = result->error;
	if (value)
		*value = result->value;
	result->waiting = false;
	result->ready = false;
	if (result->destroyed)
		memset(result, 0, sizeof(*result));
	return true;
}

KNIEXPORT KNI_RETURNTYPE_INT
Java_org_fplinux_phoneme_media_PlatformPlayer_create0(void)
{
	uint32_t handle = 0;
	int error = 0;

	{
		int format = KNI_GetParameterAsInt(1);
		int note = KNI_GetParameterAsInt(3);
		int duration = KNI_GetParameterAsInt(4);
		void *bytes = NULL;
		jsize count = 0;
		struct media_result *result = NULL;

		if (format < 0 || format >= JAVA_MEDIA_FORMAT_COUNT) {
			throw_media(EINVAL);
			KNI_ReturnInt(0);
		}
		KNI_StartHandles(1);
		KNI_DeclareHandle(input);
		KNI_GetParameterAsObject(2, input);
		if (!KNI_IsNullHandle(input)) {
			count = KNI_GetArrayLength(input);
			if (count > 0 &&
			    (unsigned int)count <= MEDIA_SOURCE_BYTES) {
				bytes = malloc((size_t)count);
				if (bytes)
					KNI_GetRawArrayRegion(input, 0, count,
							      bytes);
				else
					error = ENOMEM;
			} else {
				error = count > 0 ? EFBIG : EINVAL;
			}
		}
		KNI_EndHandles();
		for (unsigned int i = 0; i < FPLINUX_RUNTIME_MEDIA_HANDLES; i++)
			if (!audio.results[i].handle) {
				result = &audio.results[i];
				break;
			}
		if (!error && !result)
			error = EMFILE;
		if (!error && audio.count >= MEDIA_EVENTS - MEDIA_EVENT_RESERVE)
			error = EAGAIN;
		if (!error && !ensure_broker())
			error = errno;
		if (!error) {
			handle = fplinux_runtime_media_create(
				audio.broker, runtime_formats[format], bytes,
				(size_t)count, (unsigned int)note,
				(uint64_t)duration);
			if (!handle)
				error = errno;
			else
				result->handle = handle;
		}
		free(bytes);
	}
	if (error) {
		struct media_result *result = find_result(handle);

		if (result) {
			fplinux_runtime_media_destroy(audio.broker, handle);
			memset(result, 0, sizeof(*result));
		}
		throw_media(error);
	}
	KNI_ReturnInt((jint)handle);
}

KNIEXPORT KNI_RETURNTYPE_VOID
Java_org_fplinux_phoneme_media_PlatformPlayer_await0(void)
{
	uint32_t handle = (uint32_t)KNI_GetParameterAsInt(1);
	int error = 0;

	if (!wait_result(handle, &error, NULL))
		KNI_ReturnVoid();
	if (error)
		throw_media(error);
	KNI_ReturnVoid();
}

static uint64_t run_command(uint32_t handle,
			    enum fplinux_runtime_media_command command,
			    uint64_t value)
{
	MidpReentryData *reentry = (MidpReentryData *)SNI_GetReentryData(NULL);
	int error = 0;
	uint64_t result = 0;

	if (audio.stopped)
		error = ECANCELED;
	if (!reentry && audio.count >= MEDIA_EVENTS - MEDIA_EVENT_RESERVE)
		error = EAGAIN;
	if (!error && !reentry &&
	    fplinux_runtime_media_command(audio.broker, handle, command,
					  value) < 0)
		error = errno;
	if (!error && !wait_result(handle, &error, &result))
		return 0;
	if (error)
		throw_media(error);
	return result;
}

KNIEXPORT KNI_RETURNTYPE_VOID
Java_org_fplinux_phoneme_media_PlatformPlayer_prefetch0(void)
{
	run_command((uint32_t)KNI_GetParameterAsInt(1),
		    FPLINUX_RUNTIME_MEDIA_PREFETCH, 0);
	KNI_ReturnVoid();
}

KNIEXPORT KNI_RETURNTYPE_LONG
Java_org_fplinux_phoneme_media_PlatformPlayer_start0(void)
{
	KNI_ReturnLong(run_command((uint32_t)KNI_GetParameterAsInt(1),
				   FPLINUX_RUNTIME_MEDIA_START, 0));
}

KNIEXPORT KNI_RETURNTYPE_VOID
Java_org_fplinux_phoneme_media_PlatformPlayer_pause0(void)
{
	run_command((uint32_t)KNI_GetParameterAsInt(1),
		    FPLINUX_RUNTIME_MEDIA_PAUSE, 0);
	KNI_ReturnVoid();
}

KNIEXPORT KNI_RETURNTYPE_VOID
Java_org_fplinux_phoneme_media_PlatformPlayer_release0(void)
{
	run_command((uint32_t)KNI_GetParameterAsInt(1),
		    FPLINUX_RUNTIME_MEDIA_RELEASE, 0);
	KNI_ReturnVoid();
}

KNIEXPORT KNI_RETURNTYPE_LONG
Java_org_fplinux_phoneme_media_PlatformPlayer_seek0(void)
{
	uint32_t handle = (uint32_t)KNI_GetParameterAsInt(1);
	uint64_t time_us = (uint64_t)KNI_GetParameterAsLong(2);

	KNI_ReturnLong(
		run_command(handle, FPLINUX_RUNTIME_MEDIA_SEEK, time_us));
}

KNIEXPORT KNI_RETURNTYPE_VOID
Java_org_fplinux_phoneme_media_PlatformPlayer_setGain0(void)
{
	uint32_t handle = (uint32_t)KNI_GetParameterAsInt(1);
	int gain = KNI_GetParameterAsInt(2);

	run_command(handle, FPLINUX_RUNTIME_MEDIA_GAIN, (uint64_t)gain);
	KNI_ReturnVoid();
}

KNIEXPORT KNI_RETURNTYPE_LONG
Java_org_fplinux_phoneme_media_PlatformPlayer_mediaTime0(void)
{
	struct fplinux_runtime_media_info info;
	uint32_t handle = (uint32_t)KNI_GetParameterAsInt(1);

	KNI_ReturnLong(fplinux_runtime_media_info(audio.broker, handle, &info) ?
			       (jlong)info.position_us :
			       -1);
}

KNIEXPORT KNI_RETURNTYPE_LONG
Java_org_fplinux_phoneme_media_PlatformPlayer_duration0(void)
{
	struct fplinux_runtime_media_info info;
	uint32_t handle = (uint32_t)KNI_GetParameterAsInt(1);

	if (fplinux_runtime_media_info(audio.broker, handle, &info) &&
	    info.duration_known)
		KNI_ReturnLong((jlong)info.duration_us);
	KNI_ReturnLong(-1);
}

KNIEXPORT KNI_RETURNTYPE_INT
Java_org_fplinux_phoneme_media_PlatformPlayer_format0(void)
{
	struct fplinux_runtime_media_info info;
	uint32_t handle = (uint32_t)KNI_GetParameterAsInt(1);

	if (!fplinux_runtime_media_info(audio.broker, handle, &info))
		KNI_ReturnInt(-1);
	for (int i = 0; i < JAVA_MEDIA_FORMAT_COUNT; i++)
		if (runtime_formats[i] == info.format)
			KNI_ReturnInt(i);
	KNI_ReturnInt(-1);
}

KNIEXPORT KNI_RETURNTYPE_VOID
Java_org_fplinux_phoneme_media_PlatformPlayer_destroy0(void)
{
	uint32_t handle = (uint32_t)KNI_GetParameterAsInt(1);
	struct media_result *result = find_result(handle);

	fplinux_runtime_media_destroy(audio.broker, handle);
	/* CLOSED supersedes events which Java would discard for this handle. */
	size_t kept = 0;

	for (size_t i = 0; i < audio.count; i++) {
		struct media_event event =
			audio.events[(audio.first + i) % MEDIA_EVENTS];

		if (event.handle != (int32_t)handle)
			audio.events[(audio.first + kept++) % MEDIA_EVENTS] =
				event;
	}
	audio.count = kept;
	if (result) {
		if (result->waiting) {
			result->destroyed = true;
			result->error = ECANCELED;
			result->ready = true;
			wake_app();
		} else {
			memset(result, 0, sizeof(*result));
		}
	}
	KNI_ReturnVoid();
}

static void post_event(int32_t handle, enum java_media_event type,
		       int64_t time_us)
{
	bool queued = false;

	if (type == JAVA_MEDIA_VOLUME) {
		for (size_t i = 0; i < audio.count; i++) {
			struct media_event *event =
				&audio.events[(audio.first + i) % MEDIA_EVENTS];

			if (event->handle == handle && event->type == type) {
				queued = true;
				break;
			}
		}
	}
	if (!queued && (type == JAVA_MEDIA_CLOSED || type == JAVA_MEDIA_ERROR ||
			audio.count < MEDIA_EVENTS - MEDIA_EVENT_RESERVE)) {
		struct media_result *result = find_result((uint32_t)handle);

		queued = queue_event((struct media_event){
			.handle = handle,
			.type = type,
			.time_us = time_us,
			.playback = result ? result->playback : 0,
		});
	}
	if (!queued)
		KNI_ThrowNew("java/lang/IllegalStateException",
			     "Media event queue full");
}

KNIEXPORT KNI_RETURNTYPE_VOID
Java_org_fplinux_phoneme_media_PlatformPlayer_postClosed0(void)
{
	post_event(KNI_GetParameterAsInt(1), JAVA_MEDIA_CLOSED, 0);
	KNI_ReturnVoid();
}

KNIEXPORT KNI_RETURNTYPE_VOID
Java_org_fplinux_phoneme_media_PlatformPlayer_postError0(void)
{
	post_event(KNI_GetParameterAsInt(1), JAVA_MEDIA_ERROR, 0);
	KNI_ReturnVoid();
}

KNIEXPORT KNI_RETURNTYPE_VOID
Java_org_fplinux_phoneme_media_PlatformPlayer_postVolume0(void)
{
	post_event(KNI_GetParameterAsInt(1), JAVA_MEDIA_VOLUME, 0);
	KNI_ReturnVoid();
}

KNIEXPORT KNI_RETURNTYPE_VOID
Java_org_fplinux_phoneme_media_PlatformPlayer_postDuration0(void)
{
	int32_t handle = KNI_GetParameterAsInt(1);
	int64_t duration_us = KNI_GetParameterAsLong(2);

	post_event(handle, JAVA_MEDIA_DURATION, duration_us);
	KNI_ReturnVoid();
}

KNIEXPORT KNI_RETURNTYPE_VOID
Java_org_fplinux_phoneme_media_PlatformPlayer_initEvent0(void)
{
	/* Class-mirror and field lookup stay outside the polling loop. */
	KNI_StartHandles(2);
	KNI_DeclareHandle(output);
	KNI_DeclareHandle(cls);
	KNI_GetParameterAsObject(1, output);
	KNI_GetObjectClass(output, cls);
	if (!KNI_IsNullHandle(cls)) {
		event_fields.handle = KNI_GetFieldID(cls, "handle", "I");
		event_fields.type = KNI_GetFieldID(cls, "type", "I");
		event_fields.time_us = KNI_GetFieldID(cls, "timeUs", "J");
		event_fields.error = KNI_GetFieldID(cls, "error", "I");
		event_fields.playback = KNI_GetFieldID(cls, "playbackId", "J");
	}
	if (KNI_IsNullHandle(cls) || !event_fields.handle ||
	    !event_fields.type || !event_fields.time_us ||
	    !event_fields.error || !event_fields.playback)
		KNI_ThrowNew("java/lang/IllegalStateException",
			     "MediaEvent fields unavailable");
	KNI_EndHandles();
	KNI_ReturnVoid();
}

KNIEXPORT KNI_RETURNTYPE_BOOLEAN
Java_org_fplinux_phoneme_media_PlatformPlayer_nextEvent0(void)
{
	bool available = false;

	flush_pending_events();
	if (audio.count) {
		struct media_event event = audio.events[audio.first];

		KNI_StartHandles(1);
		KNI_DeclareHandle(output);
		KNI_GetParameterAsObject(1, output);
		KNI_SetIntField(output, event_fields.handle, event.handle);
		KNI_SetIntField(output, event_fields.type, event.type);
		KNI_SetLongField(output, event_fields.time_us, event.time_us);
		KNI_SetIntField(output, event_fields.error, event.error);
		KNI_SetLongField(output, event_fields.playback,
				 (jlong)event.playback);
		KNI_EndHandles();
		audio.first = (audio.first + 1) % MEDIA_EVENTS;
		audio.count--;
		available = true;
	} else if (!audio.stopped) {
		if (midp_thread_wait(AMS_SIGNAL, MEDIA_EVENT_DESCRIPTOR, NULL) <
		    0) {
			fplinux_pm_media_cancel();
			KNI_ThrowNew("java/lang/OutOfMemoryError",
				     "Cannot wait for media events");
		}
	}
	KNI_ReturnBoolean(available ? KNI_TRUE : KNI_FALSE);
}
