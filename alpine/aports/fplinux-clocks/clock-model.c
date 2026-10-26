/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "clock-model.h"
#include "fplinux-utf8.h"

#include <limits.h>
#include <string.h>
#include <time.h>

#define SECONDS_PER_DAY INT64_C(86400)

static bool valid_wall(int64_t wall_utc)
{
	return wall_utc >= CLOCK_WALL_MIN && wall_utc <= CLOCK_WALL_MAX;
}

static uint64_t saturated_add(uint64_t value, uint64_t increment)
{
	return increment > UINT64_MAX - value ? UINT64_MAX : value + increment;
}

_Static_assert(sizeof(time_t) >= sizeof(int64_t),
	       "Clock requires the platform's 64-bit time_t");

enum clock_status clock_model_utc_time(int64_t wall_utc,
				       struct clock_local_time *calendar)
{
	struct tm value;
	time_t instant;

	if (!calendar || !valid_wall(wall_utc))
		return CLOCK_INVALID;
	instant = (time_t)wall_utc;
	if (!gmtime_r(&instant, &value))
		return CLOCK_INVALID;
	calendar->year = value.tm_year + 1900;
	calendar->month = (unsigned int)value.tm_mon + 1;
	calendar->day = (unsigned int)value.tm_mday;
	calendar->weekday = (unsigned int)(value.tm_wday + 6) % 7;
	calendar->hour = (unsigned int)value.tm_hour;
	calendar->minute = (unsigned int)value.tm_min;
	calendar->second = (unsigned int)value.tm_sec;
	return CLOCK_OK;
}

enum clock_status
clock_model_utc_from_calendar(const struct clock_local_time *calendar,
			      int64_t *wall_utc)
{
	struct clock_local_time checked;
	struct tm value = { 0 };
	time_t instant;

	if (!calendar || !wall_utc || calendar->year < 1980 ||
	    calendar->year > 2099 || calendar->month < 1 ||
	    calendar->month > 12 || calendar->day < 1 || calendar->day > 31 ||
	    calendar->hour > 23 || calendar->minute > 59 ||
	    calendar->second > 59)
		return CLOCK_INVALID;
	value.tm_year = calendar->year - 1900;
	value.tm_mon = (int)calendar->month - 1;
	value.tm_mday = (int)calendar->day;
	value.tm_hour = (int)calendar->hour;
	value.tm_min = (int)calendar->minute;
	value.tm_sec = (int)calendar->second;
	instant = timegm(&value);
	if (clock_model_utc_time((int64_t)instant, &checked) != CLOCK_OK ||
	    checked.year != calendar->year ||
	    checked.month != calendar->month || checked.day != calendar->day ||
	    checked.hour != calendar->hour ||
	    checked.minute != calendar->minute ||
	    checked.second != calendar->second)
		return CLOCK_INVALID;
	*wall_utc = (int64_t)instant;
	return CLOCK_OK;
}

enum clock_status clock_model_validate_text(const char *text, size_t length,
					    bool allow_empty)
{
	const unsigned char *cursor;
	const unsigned char *end;
	size_t characters = 0;

	if (!text || length > CLOCK_TEXT_BYTES || (!allow_empty && !length))
		return CLOCK_INVALID;
	cursor = (const unsigned char *)text;
	end = cursor + length;
	while (cursor < end) {
		uint32_t codepoint;

		if (!fplinux_utf8_next(&cursor, end, &codepoint) ||
		    codepoint < 0x20 ||
		    (codepoint >= 0x7f && codepoint <= 0x9f))
			return CLOCK_INVALID;
		if (++characters > CLOCK_TEXT_CHARACTERS)
			return CLOCK_INVALID;
	}
	return CLOCK_OK;
}

static enum clock_status validate_string(const char *text, bool allow_empty)
{
	size_t length = 0;

	if (!text)
		return CLOCK_INVALID;
	while (length <= CLOCK_TEXT_BYTES && text[length])
		length++;
	return clock_model_validate_text(text, length, allow_empty);
}

static bool valid_spec(const struct clock_alarm_spec *spec)
{
	if (!spec || validate_string(spec->label, true) != CLOCK_OK)
		return false;
	if (spec->schedule == CLOCK_ALARM_ONCE)
		return valid_wall(spec->once_utc) && spec->once_utc % 60 == 0 &&
		       !spec->hour && !spec->minute && !spec->repeat_days;
	return spec->schedule == CLOCK_ALARM_WEEKLY && !spec->once_utc &&
	       spec->hour < 24 && spec->minute < 60 && spec->repeat_days &&
	       spec->repeat_days <= 0x7f;
}

enum clock_status
clock_model_alarm_next_due(const struct clock_alarm_spec *spec, int64_t after,
			   int64_t *due_utc)
{
	int64_t day;
	unsigned int offset;

	if (!due_utc || !valid_spec(spec) || !valid_wall(after))
		return CLOCK_INVALID;
	if (spec->schedule == CLOCK_ALARM_ONCE) {
		if (spec->once_utc <= after)
			return CLOCK_INVALID;
		*due_utc = spec->once_utc;
		return CLOCK_OK;
	}
	day = after / SECONDS_PER_DAY;
	for (offset = 0; offset <= 7; offset++) {
		int64_t local_day = day + offset;
		unsigned int weekday = (unsigned int)((local_day + 3) % 7);
		int64_t candidate = local_day * SECONDS_PER_DAY +
				    (int64_t)spec->hour * 3600 +
				    (int64_t)spec->minute * 60;

		if (candidate > after && valid_wall(candidate) &&
		    (spec->repeat_days & (1U << weekday))) {
			*due_utc = candidate;
			return CLOCK_OK;
		}
	}
	return CLOCK_INVALID;
}

static void remember_alarm_time(struct clock_model *model,
				const struct clock_alarm_spec *spec)
{
	if (spec->schedule == CLOCK_ALARM_ONCE) {
		int64_t seconds = spec->once_utc % SECONDS_PER_DAY;

		model->last_alarm_hour = (unsigned int)(seconds / 3600);
		model->last_alarm_minute = (unsigned int)(seconds / 60 % 60);
	} else {
		model->last_alarm_hour = spec->hour;
		model->last_alarm_minute = spec->minute;
	}
}

static struct clock_alarm *find_alarm(struct clock_model *model, uint32_t id)
{
	size_t index;

	for (index = 0; index < model->alarm_count; index++)
		if (model->alarms[index].id == id)
			return &model->alarms[index];
	return NULL;
}

const struct clock_alarm *
clock_model_alarm_find(const struct clock_model *model, uint32_t id)
{
	size_t index;

	if (!model)
		return NULL;
	for (index = 0; index < model->alarm_count; index++)
		if (model->alarms[index].id == id)
			return &model->alarms[index];
	return NULL;
}

static void promote_event(struct clock_model *model)
{
	if (model->active.kind != CLOCK_EVENT_NONE || !model->queue_count)
		return;
	model->active = model->queue[0];
	model->active.ring_started_ms = model->monotonic_ms;
	model->queue_count--;
	memmove(model->queue, model->queue + 1,
		model->queue_count * sizeof(model->queue[0]));
	memset(&model->queue[model->queue_count], 0, sizeof(model->queue[0]));
}

static void record_missed(struct clock_model *model, struct clock_event event,
			  enum clock_missed_reason reason)
{
	event.missed_reason = reason;
	if (model->missed_count == CLOCK_MISSED_LIMIT) {
		memmove(model->missed, model->missed + 1,
			(CLOCK_MISSED_LIMIT - 1) * sizeof(model->missed[0]));
		model->missed_count--;
	}
	model->missed[model->missed_count++] = event;
	model->missed_total = saturated_add(model->missed_total, 1);
}

static void submit_event(struct clock_model *model, struct clock_event event,
			 bool late)
{
	if (late) {
		record_missed(model, event, CLOCK_MISSED_LATE);
	} else if (model->active.kind == CLOCK_EVENT_NONE) {
		model->active = event;
		model->active.ring_started_ms = model->monotonic_ms;
	} else if (model->queue_count < CLOCK_QUEUE_LIMIT) {
		model->queue[model->queue_count++] = event;
	} else {
		record_missed(model, event, CLOCK_MISSED_QUEUE_FULL);
	}
}

static bool active_alarm(const struct clock_model *model, uint32_t id)
{
	return model->active.kind == CLOCK_EVENT_ALARM &&
	       model->active.source_id == id;
}

static bool queued_alarm(const struct clock_model *model, uint32_t id)
{
	size_t index;

	for (index = 0; index < model->queue_count; index++)
		if (model->queue[index].kind == CLOCK_EVENT_ALARM &&
		    model->queue[index].source_id == id)
			return true;
	return false;
}

static void cancel_events(struct clock_model *model, enum clock_event_kind kind,
			  uint32_t source_id)
{
	size_t read_index, write_index = 0;

	for (read_index = 0; read_index < model->queue_count; read_index++) {
		const struct clock_event *event = &model->queue[read_index];

		if (event->kind == kind && event->source_id == source_id)
			continue;
		model->queue[write_index++] = *event;
	}
	memset(model->queue + write_index, 0,
	       (model->queue_count - write_index) * sizeof(model->queue[0]));
	model->queue_count = write_index;
	if (model->active.kind == kind &&
	    model->active.source_id == source_id) {
		memset(&model->active, 0, sizeof(model->active));
		promote_event(model);
	}
}

static enum clock_status create_event(struct clock_model *model,
				      struct clock_event *event,
				      const struct clock_alarm *alarm,
				      int64_t due_wall, uint64_t due_monotonic,
				      bool snoozed)
{
	if (!model->next_event_id)
		return CLOCK_EXHAUSTED;
	memset(event, 0, sizeof(*event));
	event->id = model->next_event_id++;
	event->kind = alarm ? CLOCK_EVENT_ALARM : CLOCK_EVENT_TIMER;
	event->source_id = alarm ? alarm->id : 0;
	event->generation = alarm ? alarm->generation : model->timer.generation;
	if (alarm)
		memcpy(event->label, alarm->spec.label, sizeof(event->label));
	event->due_wall_utc = due_wall;
	event->due_monotonic_ms = due_monotonic;
	event->snoozed = snoozed;
	return CLOCK_OK;
}

enum clock_status clock_model_init(struct clock_model *model, int64_t wall_utc,
				   uint64_t monotonic_ms)
{
	if (!model || !valid_wall(wall_utc))
		return CLOCK_INVALID;
	memset(model, 0, sizeof(*model));
	model->wall_utc = wall_utc;
	model->monotonic_ms = monotonic_ms;
	model->snooze_minutes = 5;
	model->last_alarm_hour = 7;
	model->next_alarm_id = 1;
	model->next_world_id = 1;
	model->next_event_id = 1;
	model->timer.generation = 1;
	return CLOCK_OK;
}

static enum clock_status tick_alarms(struct clock_model *model, bool snoozed)
{
	size_t processed;

	/* Each alarm contributes at most one scheduled and one snoozed event per
	 * tick. Selecting the earliest due item makes ordering independent of rows.
	 */
	for (processed = 0; processed < model->alarm_count; processed++) {
		struct clock_alarm *due = NULL;
		struct clock_event event;
		size_t index;
		enum clock_status status;
		bool late;

		for (index = 0; index < model->alarm_count; index++) {
			struct clock_alarm *alarm = &model->alarms[index];
			bool ready =
				snoozed ? alarm->snoozed &&
						  alarm->snooze_deadline_ms <=
							  model->monotonic_ms :
					  alarm->spec.enabled &&
						  alarm->next_due_utc <=
							  model->wall_utc;
			bool earlier;

			if (!ready)
				continue;
			earlier = !due ||
				  (snoozed ? alarm->snooze_deadline_ms <
						     due->snooze_deadline_ms :
					     alarm->next_due_utc <
						     due->next_due_utc);
			if (earlier ||
			    (due &&
			     (snoozed ? alarm->snooze_deadline_ms ==
						due->snooze_deadline_ms :
					alarm->next_due_utc ==
						due->next_due_utc) &&
			     alarm->id < due->id))
				due = alarm;
		}
		if (!due)
			break;
		status = create_event(
			model, &event, due,
			snoozed ? model->wall_utc : due->next_due_utc,
			snoozed ? due->snooze_deadline_ms : model->monotonic_ms,
			snoozed);
		if (status != CLOCK_OK)
			return status;
		late = snoozed ? model->monotonic_ms - due->snooze_deadline_ms >
					 CLOCK_RING_TIMEOUT_MS :
				 model->wall_utc - due->next_due_utc >
					 CLOCK_ALARM_GRACE_SECONDS;
		if (snoozed) {
			due->snoozed = false;
			due->snooze_deadline_ms = 0;
		} else {
			due->last_due_utc = due->next_due_utc;
			if (due->spec.schedule == CLOCK_ALARM_ONCE) {
				due->spec.enabled = false;
				due->next_due_utc = 0;
			} else {
				if (clock_model_alarm_next_due(
					    &due->spec, model->wall_utc,
					    &due->next_due_utc) != CLOCK_OK) {
					due->next_due_utc = 0;
					due->spec.enabled = false;
				}
			}
		}
		submit_event(model, event, late);
	}
	return CLOCK_OK;
}

enum clock_status clock_model_tick(struct clock_model *model, int64_t wall_utc,
				   uint64_t monotonic_ms)
{
	enum clock_status status;
	uint64_t elapsed;

	if (!model || !valid_wall(wall_utc))
		return CLOCK_INVALID;
	if (monotonic_ms < model->monotonic_ms)
		return CLOCK_TIME_BACKWARD;
	elapsed = monotonic_ms - model->monotonic_ms;
	model->wall_utc = wall_utc;
	model->monotonic_ms = monotonic_ms;
	if (model->stopwatch.running)
		model->stopwatch.elapsed_ms =
			saturated_add(model->stopwatch.elapsed_ms, elapsed);
	if (model->active.kind != CLOCK_EVENT_NONE &&
	    monotonic_ms - model->active.ring_started_ms >=
		    CLOCK_RING_TIMEOUT_MS) {
		record_missed(model, model->active, CLOCK_MISSED_EXPIRED);
		memset(&model->active, 0, sizeof(model->active));
		promote_event(model);
	}
	status = tick_alarms(model, false);
	if (status != CLOCK_OK)
		return status;
	status = tick_alarms(model, true);
	if (status != CLOCK_OK)
		return status;
	if (model->timer.state == CLOCK_TIMER_RUNNING) {
		model->timer.remaining_ms = clock_model_timer_remaining(model);
		if (!model->timer.remaining_ms) {
			struct clock_event event;

			status = create_event(model, &event, NULL, wall_utc,
					      model->timer.deadline_ms, false);
			if (status != CLOCK_OK)
				return status;
			model->timer.state = CLOCK_TIMER_FINISHED;
			submit_event(model, event, false);
		}
	}
	return CLOCK_OK;
}

enum clock_status clock_model_set_view(struct clock_model *model,
				       enum clock_view view)
{
	if (!model || (view != CLOCK_VIEW_DIGITAL && view != CLOCK_VIEW_ANALOG))
		return CLOCK_INVALID;
	model->view = view;
	return CLOCK_OK;
}

enum clock_status clock_model_set_snooze(struct clock_model *model,
					 unsigned int minutes)
{
	if (!model || minutes < 1 || minutes > 30)
		return CLOCK_INVALID;
	model->snooze_minutes = minutes;
	return CLOCK_OK;
}

enum clock_status clock_model_alarm_add(struct clock_model *model,
					const struct clock_alarm_spec *spec,
					uint32_t *id)
{
	struct clock_alarm *alarm;
	int64_t due = 0;

	if (!model || !valid_spec(spec) ||
	    (spec->enabled && clock_model_alarm_next_due(spec, model->wall_utc,
							 &due) != CLOCK_OK))
		return CLOCK_INVALID;
	if (model->alarm_count == CLOCK_ALARM_LIMIT)
		return CLOCK_FULL;
	if (!model->next_alarm_id)
		return CLOCK_EXHAUSTED;
	alarm = &model->alarms[model->alarm_count++];
	memset(alarm, 0, sizeof(*alarm));
	alarm->id = model->next_alarm_id++;
	alarm->generation = 1;
	alarm->spec = *spec;
	alarm->next_due_utc = due;
	remember_alarm_time(model, spec);
	if (id)
		*id = alarm->id;
	return CLOCK_OK;
}

enum clock_status clock_model_alarm_edit(struct clock_model *model, uint32_t id,
					 const struct clock_alarm_spec *spec,
					 bool cancel_active)
{
	struct clock_alarm *alarm;
	int64_t after, due = 0;

	if (!model || !valid_spec(spec))
		return CLOCK_INVALID;
	alarm = find_alarm(model, id);
	if (!alarm)
		return CLOCK_NOT_FOUND;
	if (active_alarm(model, id) && !cancel_active)
		return CLOCK_ACTIVE_EVENT;
	if (alarm->generation == UINT32_MAX)
		return CLOCK_EXHAUSTED;
	after = model->wall_utc > alarm->last_due_utc ? model->wall_utc :
							alarm->last_due_utc;
	if (spec->enabled &&
	    clock_model_alarm_next_due(spec, after, &due) != CLOCK_OK)
		return CLOCK_INVALID;
	/* The caller may pass the model's own snapshot as spec. Copy before the
	 * cancellation step promotes another event, and retain occurrence history.
	 */
	alarm->spec = *spec;
	alarm->generation++;
	alarm->snoozed = false;
	alarm->snooze_deadline_ms = 0;
	alarm->next_due_utc = due;
	remember_alarm_time(model, &alarm->spec);
	cancel_events(model, CLOCK_EVENT_ALARM, id);
	return CLOCK_OK;
}

enum clock_status clock_model_alarm_enable(struct clock_model *model,
					   uint32_t id, bool enabled,
					   bool cancel_active)
{
	struct clock_alarm *alarm;
	struct clock_alarm_spec spec;

	if (!model)
		return CLOCK_INVALID;
	alarm = find_alarm(model, id);
	if (!alarm)
		return CLOCK_NOT_FOUND;
	if (!enabled && active_alarm(model, id) && !cancel_active)
		return CLOCK_ACTIVE_EVENT;
	if (alarm->spec.enabled == enabled &&
	    (enabled || (!alarm->snoozed && !active_alarm(model, id) &&
			 !queued_alarm(model, id))))
		return CLOCK_OK;
	spec = alarm->spec;
	spec.enabled = enabled;
	return clock_model_alarm_edit(model, id, &spec, cancel_active);
}

enum clock_status clock_model_alarm_delete(struct clock_model *model,
					   uint32_t id, bool cancel_active)
{
	struct clock_alarm *alarm;
	size_t index;

	if (!model)
		return CLOCK_INVALID;
	alarm = find_alarm(model, id);
	if (!alarm)
		return CLOCK_NOT_FOUND;
	if (active_alarm(model, id) && !cancel_active)
		return CLOCK_ACTIVE_EVENT;
	index = (size_t)(alarm - model->alarms);
	cancel_events(model, CLOCK_EVENT_ALARM, id);
	model->alarm_count--;
	memmove(model->alarms + index, model->alarms + index + 1,
		(model->alarm_count - index) * sizeof(model->alarms[0]));
	memset(&model->alarms[model->alarm_count], 0, sizeof(model->alarms[0]));
	return CLOCK_OK;
}

enum clock_status clock_model_alarm_cancel_snooze(struct clock_model *model,
						  uint32_t id)
{
	struct clock_alarm *alarm;

	if (!model)
		return CLOCK_INVALID;
	alarm = find_alarm(model, id);
	if (!alarm)
		return CLOCK_NOT_FOUND;
	alarm->snoozed = false;
	alarm->snooze_deadline_ms = 0;
	return CLOCK_OK;
}

enum clock_status clock_model_dismiss(struct clock_model *model,
				      uint64_t event_id)
{
	if (!model)
		return CLOCK_INVALID;
	if (model->active.kind == CLOCK_EVENT_NONE)
		return CLOCK_NO_EVENT;
	if (model->active.id != event_id)
		return CLOCK_STALE_EVENT;
	memset(&model->active, 0, sizeof(model->active));
	promote_event(model);
	return CLOCK_OK;
}

enum clock_status clock_model_snooze(struct clock_model *model,
				     uint64_t event_id, unsigned int minutes)
{
	struct clock_alarm *alarm;
	uint64_t duration;

	if (!model)
		return CLOCK_INVALID;
	if (!minutes)
		minutes = model->snooze_minutes;
	if (minutes < 1 || minutes > 30)
		return CLOCK_INVALID;
	if (model->active.kind == CLOCK_EVENT_NONE)
		return CLOCK_NO_EVENT;
	if (model->active.id != event_id)
		return CLOCK_STALE_EVENT;
	if (model->active.kind != CLOCK_EVENT_ALARM)
		return CLOCK_WRONG_STATE;
	alarm = find_alarm(model, model->active.source_id);
	if (!alarm || alarm->generation != model->active.generation)
		return CLOCK_STALE_EVENT;
	duration = (uint64_t)minutes * 60000;
	if (duration > UINT64_MAX - model->monotonic_ms)
		return CLOCK_EXHAUSTED;
	alarm->snoozed = true;
	alarm->snooze_deadline_ms = model->monotonic_ms + duration;
	return clock_model_dismiss(model, event_id);
}

void clock_model_clear_missed(struct clock_model *model)
{
	if (!model)
		return;
	memset(model->missed, 0, sizeof(model->missed));
	model->missed_count = 0;
	model->missed_total = 0;
}

uint64_t clock_model_timer_remaining(const struct clock_model *model)
{
	if (!model)
		return 0;
	if (model->timer.state == CLOCK_TIMER_RUNNING)
		return model->timer.deadline_ms > model->monotonic_ms ?
			       model->timer.deadline_ms - model->monotonic_ms :
			       0;
	return model->timer.remaining_ms;
}

enum clock_status clock_model_timer_set(struct clock_model *model,
					unsigned int seconds)
{
	if (!model || seconds > CLOCK_TIMER_MAX_SECONDS)
		return CLOCK_INVALID;
	if (model->timer.generation == UINT32_MAX)
		return CLOCK_EXHAUSTED;
	model->timer.generation++;
	model->timer.state = CLOCK_TIMER_STOPPED;
	model->timer.duration_seconds = seconds;
	model->timer.remaining_ms = (uint64_t)seconds * 1000;
	model->timer.deadline_ms = 0;
	cancel_events(model, CLOCK_EVENT_TIMER, 0);
	return CLOCK_OK;
}

enum clock_status clock_model_timer_start(struct clock_model *model)
{
	if (!model || !model->timer.duration_seconds)
		return CLOCK_INVALID;
	if (model->timer.state != CLOCK_TIMER_STOPPED)
		return CLOCK_WRONG_STATE;
	if (model->timer.remaining_ms > UINT64_MAX - model->monotonic_ms)
		return CLOCK_EXHAUSTED;
	model->timer.deadline_ms =
		model->monotonic_ms + model->timer.remaining_ms;
	model->timer.state = CLOCK_TIMER_RUNNING;
	return CLOCK_OK;
}

enum clock_status clock_model_timer_pause(struct clock_model *model)
{
	if (!model)
		return CLOCK_INVALID;
	if (model->timer.state != CLOCK_TIMER_RUNNING)
		return CLOCK_WRONG_STATE;
	model->timer.remaining_ms = clock_model_timer_remaining(model);
	model->timer.deadline_ms = 0;
	model->timer.state = CLOCK_TIMER_PAUSED;
	return CLOCK_OK;
}

enum clock_status clock_model_timer_resume(struct clock_model *model)
{
	if (!model)
		return CLOCK_INVALID;
	if (model->timer.state != CLOCK_TIMER_PAUSED)
		return CLOCK_WRONG_STATE;
	if (model->timer.remaining_ms > UINT64_MAX - model->monotonic_ms)
		return CLOCK_EXHAUSTED;
	model->timer.deadline_ms =
		model->monotonic_ms + model->timer.remaining_ms;
	model->timer.state = CLOCK_TIMER_RUNNING;
	return CLOCK_OK;
}

enum clock_status clock_model_timer_stop(struct clock_model *model)
{
	if (!model)
		return CLOCK_INVALID;
	return clock_model_timer_set(model, model->timer.duration_seconds);
}

enum clock_status clock_model_timer_restart(struct clock_model *model)
{
	enum clock_status status;
	uint64_t duration;

	if (!model || !model->timer.duration_seconds)
		return CLOCK_INVALID;
	duration = (uint64_t)model->timer.duration_seconds * 1000;
	if (duration > UINT64_MAX - model->monotonic_ms)
		return CLOCK_EXHAUSTED;
	status = clock_model_timer_stop(model);
	return status == CLOCK_OK ? clock_model_timer_start(model) : status;
}

uint64_t clock_model_stopwatch_elapsed(const struct clock_model *model)
{
	return model ? model->stopwatch.elapsed_ms : 0;
}

enum clock_status clock_model_stopwatch_start(struct clock_model *model)
{
	if (!model)
		return CLOCK_INVALID;
	if (model->stopwatch.running)
		return CLOCK_WRONG_STATE;
	model->stopwatch.running = true;
	return CLOCK_OK;
}

enum clock_status clock_model_stopwatch_pause(struct clock_model *model)
{
	if (!model)
		return CLOCK_INVALID;
	if (!model->stopwatch.running)
		return CLOCK_WRONG_STATE;
	model->stopwatch.running = false;
	return CLOCK_OK;
}

enum clock_status clock_model_stopwatch_reset(struct clock_model *model)
{
	if (!model)
		return CLOCK_INVALID;
	memset(&model->stopwatch, 0, sizeof(model->stopwatch));
	return CLOCK_OK;
}

enum clock_status clock_model_stopwatch_lap(struct clock_model *model)
{
	struct clock_lap *lap;
	uint64_t previous = 0;

	if (!model)
		return CLOCK_INVALID;
	if (!model->stopwatch.running)
		return CLOCK_WRONG_STATE;
	if (model->stopwatch.lap_count == CLOCK_LAP_LIMIT)
		return CLOCK_FULL;
	if (model->stopwatch.lap_count)
		previous = model->stopwatch.laps[model->stopwatch.lap_count - 1]
				   .elapsed_ms;
	lap = &model->stopwatch.laps[model->stopwatch.lap_count++];
	lap->elapsed_ms = model->stopwatch.elapsed_ms;
	lap->difference_ms = lap->elapsed_ms - previous;
	return CLOCK_OK;
}

bool clock_model_world_offset_valid(int offset_minutes)
{
	return offset_minutes >= CLOCK_WORLD_MIN_OFFSET_MINUTES &&
	       offset_minutes <= CLOCK_WORLD_MAX_OFFSET_MINUTES &&
	       offset_minutes % CLOCK_WORLD_STEP_MINUTES == 0;
}

enum clock_status clock_model_world_add(struct clock_model *model,
					int offset_minutes, uint32_t *id)
{
	struct clock_world_entry *entry;

	if (!model || !clock_model_world_offset_valid(offset_minutes))
		return CLOCK_INVALID;
	for (size_t index = 0; index < model->world_count; ++index)
		if (model->world[index].offset_minutes == offset_minutes)
			return CLOCK_INVALID;
	if (model->world_count == CLOCK_WORLD_LIMIT)
		return CLOCK_FULL;
	if (!model->next_world_id)
		return CLOCK_EXHAUSTED;
	entry = &model->world[model->world_count++];
	memset(entry, 0, sizeof(*entry));
	entry->id = model->next_world_id++;
	entry->offset_minutes = offset_minutes;
	if (id)
		*id = entry->id;
	return CLOCK_OK;
}

enum clock_status clock_model_world_edit(struct clock_model *model, uint32_t id,
					 int offset_minutes)
{
	struct clock_world_entry *entry = NULL;

	if (!model || !clock_model_world_offset_valid(offset_minutes))
		return CLOCK_INVALID;
	for (size_t index = 0; index < model->world_count; ++index)
		if (model->world[index].id == id) {
			entry = &model->world[index];
			break;
		}
	if (!entry)
		return CLOCK_NOT_FOUND;
	for (size_t index = 0; index < model->world_count; ++index)
		if (model->world[index].id != id &&
		    model->world[index].offset_minutes == offset_minutes)
			return CLOCK_INVALID;
	entry->offset_minutes = offset_minutes;
	return CLOCK_OK;
}

enum clock_status clock_model_world_delete(struct clock_model *model,
					   uint32_t id)
{
	size_t index;

	if (!model)
		return CLOCK_INVALID;
	for (index = 0; index < model->world_count; index++) {
		if (model->world[index].id != id)
			continue;
		model->world_count--;
		memmove(model->world + index, model->world + index + 1,
			(model->world_count - index) * sizeof(model->world[0]));
		memset(&model->world[model->world_count], 0,
		       sizeof(model->world[0]));
		return CLOCK_OK;
	}
	return CLOCK_NOT_FOUND;
}

const char *clock_model_status_name(enum clock_status status)
{
	switch (status) {
	case CLOCK_OK:
		return "ok";
	case CLOCK_INVALID:
		return "invalid";
	case CLOCK_NOT_FOUND:
		return "not-found";
	case CLOCK_FULL:
		return "full";
	case CLOCK_ACTIVE_EVENT:
		return "active-event";
	case CLOCK_STALE_EVENT:
		return "stale-event";
	case CLOCK_NO_EVENT:
		return "no-event";
	case CLOCK_WRONG_STATE:
		return "wrong-state";
	case CLOCK_TIME_BACKWARD:
		return "time-backward";
	case CLOCK_EXHAUSTED:
		return "exhausted";
	case CLOCK_STORAGE_ERROR:
		return "storage-error";
	}
	return "invalid";
}
