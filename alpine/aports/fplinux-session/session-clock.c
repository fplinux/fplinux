/* SPDX-License-Identifier: GPL-2.0-only */
#include "session-clock.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "fplinux-ui-prefs.h"
#include "fplinux-ui-session.h"

#define CLOCK_RETRY_INITIAL_MS 1000U
#define CLOCK_RETRY_MAX_MS 30000U

static struct clock_result result(const struct fplinux_session_clock *clock,
				  enum clock_status status, bool applied)
{
	return (struct clock_result){
		.status = status,
		.applied = applied,
		.storage = clock ? clock->storage :
				   (struct fplinux_storage_result){
					   .state = FPLINUX_STORAGE_UNPUBLISHED,
					   .error = EINVAL,
				   },
	};
}

static void saved(struct fplinux_session_clock *clock,
		  struct fplinux_storage_result storage, uint64_t mono_ms)
{
	if (clock->storage.state != storage.state ||
	    clock->storage.error != storage.error) {
		if (storage.state == FPLINUX_STORAGE_DURABLE)
			fputs("fplinux-session: clock checkpoint saved\n",
			      stderr);
		else
			fprintf(stderr,
				"fplinux-session: clock checkpoint %s: %s\n",
				storage.state ==
						FPLINUX_STORAGE_PUBLISHED_UNSYNCED ?
					"saving not confirmed" :
					"not saved",
				strerror(storage.error));
	}
	clock->storage = storage;
	if (storage.state == FPLINUX_STORAGE_DURABLE) {
		clock->retry_at_ms = 0;
		clock->retry_delay_ms = CLOCK_RETRY_INITIAL_MS;
		return;
	}
	clock->retry_at_ms = mono_ms > UINT64_MAX - clock->retry_delay_ms ?
				     UINT64_MAX :
				     mono_ms + clock->retry_delay_ms;
	clock->retry_delay_ms = clock->retry_delay_ms > CLOCK_RETRY_MAX_MS / 2 ?
					CLOCK_RETRY_MAX_MS :
					2 * clock->retry_delay_ms;
}

static struct fplinux_storage_result
save_configuration(struct fplinux_session_clock *clock,
		   const char *configuration)
{
	if (!fplinux_ui_prefs_set(clock->preferences, "configuration",
				  configuration))
		return (struct fplinux_storage_result){
			.state = FPLINUX_STORAGE_UNPUBLISHED,
			.error = errno,
		};
	return fplinux_ui_prefs_save(clock->preferences);
}

static enum clock_status configurations(const struct clock_model *before,
					const struct clock_model *after,
					char *old, char *current)
{
	if (clock_wire_preferences_encode(before, old, CLOCK_WIRE_CAPACITY) !=
		    CLOCK_OK ||
	    clock_wire_preferences_encode(after, current,
					  CLOCK_WIRE_CAPACITY) != CLOCK_OK)
		return CLOCK_INVALID;
	return CLOCK_OK;
}

struct clock_result
fplinux_session_clock_init(struct fplinux_session_clock *clock,
			   int64_t wall_utc, uint64_t mono_ms,
			   const char *preferences_directory)
{
	const char *configuration;
	char normalized[CLOCK_WIRE_CAPACITY];
	enum clock_status status;

	if (!clock || !preferences_directory || !preferences_directory[0])
		return result(NULL, CLOCK_INVALID, false);
	memset(clock, 0, sizeof(*clock));
	clock->storage.state = FPLINUX_STORAGE_DURABLE;
	clock->retry_delay_ms = CLOCK_RETRY_INITIAL_MS;
	status = clock_model_init(&clock->model, wall_utc, mono_ms);
	if (status != CLOCK_OK)
		return result(clock, status, false);
	clock->preferences =
		fplinux_ui_prefs_open_directory(preferences_directory);
	if (!clock->preferences) {
		clock->storage = (struct fplinux_storage_result){
			.state = FPLINUX_STORAGE_UNPUBLISHED,
			.error = errno,
		};
		return result(clock, CLOCK_STORAGE_ERROR, false);
	}
	configuration =
		fplinux_ui_prefs_get(clock->preferences, "configuration", NULL);
	if (!configuration)
		return result(clock, CLOCK_OK, true);
	status = clock_wire_preferences_decode(
		configuration, strlen(configuration), &clock->model);
	if (status == CLOCK_OK)
		status = clock_wire_preferences_encode(
			&clock->model, normalized, sizeof(normalized));
	if (status != CLOCK_OK) {
		fplinux_session_clock_cleanup(clock);
		return result(clock, status, false);
	}
	if (strcmp(configuration, normalized))
		saved(clock, save_configuration(clock, normalized), mono_ms);
	return result(clock, CLOCK_OK, true);
}

void fplinux_session_clock_cleanup(struct fplinux_session_clock *clock)
{
	if (!clock)
		return;
	fplinux_ui_prefs_close(clock->preferences);
	clock->preferences = NULL;
}

struct clock_result
fplinux_session_clock_retry(struct fplinux_session_clock *clock,
			    uint64_t mono_ms)
{
	char configuration[CLOCK_WIRE_CAPACITY];

	if (!clock || !clock->preferences)
		return result(clock, CLOCK_INVALID, false);
	if (clock->storage.state != FPLINUX_STORAGE_DURABLE) {
		if (clock_wire_preferences_encode(&clock->model, configuration,
						  sizeof(configuration)) !=
		    CLOCK_OK)
			return result(clock, CLOCK_INVALID, false);
		saved(clock, save_configuration(clock, configuration), mono_ms);
	}
	return result(clock,
		      clock->storage.state == FPLINUX_STORAGE_DURABLE ?
			      CLOCK_OK :
			      CLOCK_STORAGE_ERROR,
		      true);
}

struct clock_result
fplinux_session_clock_tick(struct fplinux_session_clock *clock,
			   int64_t wall_utc, uint64_t mono_ms)
{
	struct clock_model candidate;
	char before[CLOCK_WIRE_CAPACITY], after[CLOCK_WIRE_CAPACITY];
	enum clock_status status;

	if (!clock || !clock->preferences)
		return result(clock, CLOCK_INVALID, false);
	candidate = clock->model;
	status = clock_model_tick(&candidate, wall_utc, mono_ms);
	if (status == CLOCK_OK)
		status = configurations(&clock->model, &candidate, before,
					after);
	if (status != CLOCK_OK)
		return result(clock, status, false);
	clock->model = candidate;
	if (strcmp(before, after))
		saved(clock, save_configuration(clock, after), mono_ms);
	else if (clock->retry_at_ms && mono_ms >= clock->retry_at_ms)
		fplinux_session_clock_retry(clock, mono_ms);
	return result(clock, CLOCK_OK, true);
}

static bool configuration_request(enum clock_request_op operation)
{
	switch (operation) {
	case CLOCK_REQUEST_SAVE:
	case CLOCK_REQUEST_ALARM_ADD:
	case CLOCK_REQUEST_ALARM_EDIT:
	case CLOCK_REQUEST_ALARM_ENABLE:
	case CLOCK_REQUEST_ALARM_DELETE:
	case CLOCK_REQUEST_TIMER_SET:
	case CLOCK_REQUEST_WORLD_ADD:
	case CLOCK_REQUEST_WORLD_EDIT:
	case CLOCK_REQUEST_WORLD_DELETE:
	case CLOCK_REQUEST_SET_VIEW:
	case CLOCK_REQUEST_SET_SNOOZE:
		return true;
	default:
		return false;
	}
}

struct clock_result
fplinux_session_clock_apply(struct fplinux_session_clock *clock,
			    const struct clock_request *request,
			    int64_t wall_utc, uint64_t mono_ms)
{
	struct clock_model candidate;
	struct clock_result outcome;
	struct fplinux_storage_result storage;
	char before[CLOCK_WIRE_CAPACITY], after[CLOCK_WIRE_CAPACITY];
	enum clock_status status;

	if (!clock || !request)
		return result(clock, CLOCK_INVALID, false);
	outcome = fplinux_session_clock_tick(clock, wall_utc, mono_ms);
	if (outcome.status != CLOCK_OK)
		return outcome;
	candidate = clock->model;
	status = clock_wire_request_apply(&candidate, request);
	if (status == CLOCK_OK)
		status = configurations(&clock->model, &candidate, before,
					after);
	if (status != CLOCK_OK)
		return result(clock, status, false);
	if (!strcmp(before, after)) {
		clock->model = candidate;
		if (configuration_request(request->op))
			return fplinux_session_clock_retry(clock, mono_ms);
		return result(clock, CLOCK_OK, true);
	}
	storage = save_configuration(clock, after);
	if (storage.state == FPLINUX_STORAGE_UNPUBLISHED) {
		/* Rejected edits must not become the next runtime checkpoint. */
		fplinux_ui_prefs_set(clock->preferences, "configuration",
				     before);
		outcome = result(clock, CLOCK_STORAGE_ERROR, false);
		outcome.storage = storage;
		return outcome;
	}
	clock->model = candidate;
	saved(clock, storage, mono_ms);
	return result(clock,
		      storage.state == FPLINUX_STORAGE_DURABLE ?
			      CLOCK_OK :
			      CLOCK_STORAGE_ERROR,
		      true);
}

static void consider(uint64_t *earliest, uint64_t deadline, uint64_t mono_ms)
{
	if (deadline <= mono_ms)
		deadline = mono_ms == UINT64_MAX ? UINT64_MAX : mono_ms + 1;
	if (!*earliest || deadline < *earliest)
		*earliest = deadline;
}

uint64_t
fplinux_session_clock_deadline(const struct fplinux_session_clock *clock,
			       uint64_t mono_ms)
{
	const struct clock_model *model = &clock->model;
	uint64_t earliest = 0;

	if (clock->retry_at_ms)
		consider(&earliest, clock->retry_at_ms, mono_ms);

	if (model->active.kind != CLOCK_EVENT_NONE) {
		uint64_t started = model->active.ring_started_ms;
		uint64_t expiry = started > UINT64_MAX - CLOCK_RING_TIMEOUT_MS ?
					  UINT64_MAX :
					  started + CLOCK_RING_TIMEOUT_MS;

		consider(&earliest, expiry, mono_ms);
	}
	if (model->timer.state == CLOCK_TIMER_RUNNING)
		consider(&earliest, model->timer.deadline_ms, mono_ms);
	for (size_t i = 0; i < model->alarm_count; ++i) {
		const struct clock_alarm *alarm = &model->alarms[i];

		if (alarm->snoozed)
			consider(&earliest, alarm->snooze_deadline_ms, mono_ms);
	}
	return earliest;
}

int64_t
fplinux_session_clock_alarm_deadline(const struct fplinux_session_clock *clock)
{
	int64_t earliest = 0;

	for (size_t i = 0; i < clock->model.alarm_count; ++i) {
		const struct clock_alarm *alarm = &clock->model.alarms[i];

		if (alarm->spec.enabled && alarm->next_due_utc &&
		    (!earliest || alarm->next_due_utc < earliest))
			earliest = alarm->next_due_utc;
	}
	return earliest;
}

void fplinux_session_clock_presentation(
	const struct fplinux_session_clock *clock,
	struct fplinux_ui_session_clock_presentation *presentation)
{
	const struct clock_model *model = &clock->model;
	uint64_t nearest_ms = UINT64_MAX;

	memset(presentation, 0, sizeof(*presentation));
	presentation->analog = model->view == CLOCK_VIEW_ANALOG;
	presentation->missed_total = model->missed_total;
	if (model->timer.state == CLOCK_TIMER_RUNNING)
		presentation->timer_deadline_ms = model->timer.deadline_ms;
	else if (model->timer.state == CLOCK_TIMER_PAUSED)
		presentation->timer_remaining_ms = model->timer.remaining_ms;
	for (size_t i = 0; i < model->alarm_count; ++i) {
		const struct clock_alarm *alarm = &model->alarms[i];
		uint64_t remaining_ms;

		if (alarm->spec.enabled && alarm->next_due_utc) {
			remaining_ms = alarm->next_due_utc > model->wall_utc ?
					       (uint64_t)(alarm->next_due_utc -
							  model->wall_utc) *
						       1000 :
					       0;
			if (remaining_ms < nearest_ms) {
				nearest_ms = remaining_ms;
				presentation->alarm_utc = alarm->next_due_utc;
				presentation->snooze_deadline_ms = 0;
				memcpy(presentation->alarm_label,
				       alarm->spec.label,
				       sizeof(presentation->alarm_label));
			}
		}
		if (!alarm->snoozed)
			continue;
		remaining_ms = alarm->snooze_deadline_ms > model->monotonic_ms ?
				       alarm->snooze_deadline_ms -
					       model->monotonic_ms :
				       0;
		if (remaining_ms < nearest_ms) {
			nearest_ms = remaining_ms;
			presentation->alarm_utc = 0;
			presentation->snooze_deadline_ms =
				alarm->snooze_deadline_ms;
			memcpy(presentation->alarm_label, alarm->spec.label,
			       sizeof(presentation->alarm_label));
		}
	}
}

bool fplinux_session_clock_ringing(const struct fplinux_session_clock *clock)
{
	return clock->model.active.kind != CLOCK_EVENT_NONE;
}
