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

enum java_media_event {
	JAVA_MEDIA_END = 1,
	JAVA_MEDIA_ERROR,
	JAVA_MEDIA_STARTED,
	JAVA_MEDIA_STOPPED,
	JAVA_MEDIA_CLOSED,
	JAVA_MEDIA_VOLUME,
	JAVA_MEDIA_DURATION,
};

struct media_event {
	uint32_t handle;
	int type;
	uint64_t time;
	int error;
};

struct media_result {
	uint32_t handle;
	bool ready;
	bool waiting;
	bool destroyed;
	int error;
	uint64_t position_us;
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

static bool queue_event(uint32_t handle, int type, uint64_t time, int error)
{
	if (audio.count == MEDIA_EVENTS) {
		errno = EAGAIN;
		return false;
	}
	size_t next = (audio.first + audio.count) % MEDIA_EVENTS;

	audio.events[next] = (struct media_event){
		.handle = handle,
		.type = type,
		.time = time,
		.error = error,
	};
	audio.count++;
	wake_app();
	return true;
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
				!queue_event(event->handle, JAVA_MEDIA_DURATION,
					     event->info.duration_us, 0);
		result->duration_known = true;
		result->duration_us = event->info.duration_us;
	}
	if (event->type == FPLINUX_RUNTIME_MEDIA_RESULT) {
		result->ready = true;
		result->error = event->error;
		result->position_us = event->info.position_us;
		if (!event->error &&
		    (event->command == FPLINUX_RUNTIME_MEDIA_START ||
		     event->command == FPLINUX_RUNTIME_MEDIA_PAUSE)) {
			int type = event->command ==
						   FPLINUX_RUNTIME_MEDIA_START ?
					   JAVA_MEDIA_STARTED :
					   JAVA_MEDIA_STOPPED;

			if (!queue_event(event->handle, type,
					 event->info.position_us, 0)) {
				result->event_pending = true;
				result->event = (struct media_event){
					.handle = event->handle,
					.type = type,
					.time = event->info.position_us,
				};
			}
		}
	} else {
		int type = event->type == FPLINUX_RUNTIME_MEDIA_END ?
				   JAVA_MEDIA_END :
				   JAVA_MEDIA_ERROR;

		if (!queue_event(event->handle, type, event->info.position_us,
				 event->error)) {
			result->event_pending = true;
			result->event = (struct media_event){
				.handle = event->handle,
				.type = type,
				.time = event->info.position_us,
				.error = event->error,
			};
		}
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
			queue_event(result->handle, JAVA_MEDIA_DURATION,
				    result->duration_us, 0);
			result->duration_pending = false;
		}
		if (result->event_pending && !result->duration_pending &&
		    audio.count < MEDIA_EVENTS) {
			queue_event(result->event.handle, result->event.type,
				    result->event.time, result->event.error);
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
static bool wait_result(uint32_t handle, int *error, uint64_t *position)
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
			queue_event(handle, JAVA_MEDIA_ERROR, 0, ENOMEM);
			memset(result, 0, sizeof(*result));
			*error = ENOMEM;
			return true;
		}
		return false;
	}
	*error = result->error;
	if (position)
		*position = result->position_us;
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
				audio.broker,
				(enum fplinux_runtime_media_format)format,
				bytes, (size_t)count, (unsigned int)note,
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

KNIEXPORT KNI_RETURNTYPE_LONG
Java_org_fplinux_phoneme_media_PlatformPlayer_command0(void)
{
	MidpReentryData *reentry = (MidpReentryData *)SNI_GetReentryData(NULL);
	uint32_t handle = (uint32_t)KNI_GetParameterAsInt(1);
	int command = KNI_GetParameterAsInt(2);
	uint64_t value = (uint64_t)KNI_GetParameterAsLong(3);
	int error = 0;
	uint64_t position = 0;

	if (audio.stopped)
		error = ECANCELED;
	if (!reentry && audio.count >= MEDIA_EVENTS - MEDIA_EVENT_RESERVE)
		error = EAGAIN;
	if (!error && !reentry &&
	    fplinux_runtime_media_command(
		    audio.broker, handle,
		    (enum fplinux_runtime_media_command)command, value) < 0)
		error = errno;
	if (!error && !wait_result(handle, &error, &position))
		KNI_ReturnLong(0);
	if (error)
		throw_media(error);
	KNI_ReturnLong((jlong)position);
}

KNIEXPORT KNI_RETURNTYPE_LONG
Java_org_fplinux_phoneme_media_PlatformPlayer_query0(void)
{
	struct fplinux_runtime_media_info info;
	uint32_t handle = (uint32_t)KNI_GetParameterAsInt(1);
	int field = KNI_GetParameterAsInt(2);
	jlong value = -1;

	if (fplinux_runtime_media_info(audio.broker, handle, &info)) {
		if (field == 0)
			value = (jlong)info.position_us;
		else if (field == 1 && info.duration_known)
			value = (jlong)info.duration_us;
		else if (field == 2)
			value = (jlong)info.format;
	}
	KNI_ReturnLong(value);
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

		if (event.handle != handle)
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

KNIEXPORT KNI_RETURNTYPE_VOID
Java_org_fplinux_phoneme_media_PlatformPlayer_post0(void)
{
	uint32_t handle = (uint32_t)KNI_GetParameterAsInt(1);
	int type = KNI_GetParameterAsInt(2);
	uint64_t time = (uint64_t)KNI_GetParameterAsLong(3);

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
			audio.count < MEDIA_EVENTS - MEDIA_EVENT_RESERVE))
		queued = queue_event(handle, type, time, 0);
	if (!queued)
		KNI_ThrowNew("java/lang/IllegalStateException",
			     "Media event queue full");
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
		if (!KNI_IsNullHandle(output) &&
		    KNI_GetArrayLength(output) >= 4) {
			KNI_SetLongArrayElement(output, 0, (jlong)event.handle);
			KNI_SetLongArrayElement(output, 1, (jlong)event.type);
			KNI_SetLongArrayElement(output, 2, (jlong)event.time);
			KNI_SetLongArrayElement(output, 3, (jlong)event.error);
			available = true;
		}
		KNI_EndHandles();
		audio.first = (audio.first + 1) % MEDIA_EVENTS;
		audio.count--;
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
