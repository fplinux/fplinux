/* SPDX-License-Identifier: GPL-2.0-only */
#include "clock-model.h"

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

static void countdown_and_stopwatch_use_monotonic_time(void)
{
	struct clock_model model, before;

	assert(clock_model_init(&model, 1704067200, 1000) == CLOCK_OK);
	assert(model.alarm_count == 0 && model.world_count == 0);
	assert(clock_model_timer_start(&model) == CLOCK_INVALID);
	assert(clock_model_timer_set(&model, 10) == CLOCK_OK);
	assert(clock_model_timer_start(&model) == CLOCK_OK);
	assert(clock_model_stopwatch_start(&model) == CLOCK_OK);
	assert(clock_model_tick(&model, 1704153600, 3500) == CLOCK_OK);
	assert(clock_model_timer_remaining(&model) == 7500);
	assert(clock_model_stopwatch_elapsed(&model) == 2500);
	assert(clock_model_stopwatch_lap(&model) == CLOCK_OK);
	assert(model.stopwatch.laps[0].elapsed_ms == 2500);
	assert(model.stopwatch.laps[0].difference_ms == 2500);
	assert(clock_model_timer_pause(&model) == CLOCK_OK);
	assert(clock_model_stopwatch_pause(&model) == CLOCK_OK);
	assert(clock_model_tick(&model, 1704067200, 9000) == CLOCK_OK);
	assert(clock_model_timer_remaining(&model) == 7500);
	assert(clock_model_stopwatch_elapsed(&model) == 2500);
	assert(clock_model_timer_resume(&model) == CLOCK_OK);
	assert(clock_model_stopwatch_start(&model) == CLOCK_OK);
	assert(clock_model_tick(&model, 1704067100, 10000) == CLOCK_OK);
	assert(clock_model_timer_remaining(&model) == 6500);
	assert(clock_model_stopwatch_lap(&model) == CLOCK_OK);
	assert(model.stopwatch.laps[1].elapsed_ms == 3500);
	assert(model.stopwatch.laps[1].difference_ms == 1000);
	before = model;
	assert(clock_model_tick(&model, 1704067200, 9999) ==
	       CLOCK_TIME_BACKWARD);
	assert(!memcmp(&model, &before, sizeof(model)));
	assert(clock_model_tick(&model, 1704067100, 16500) == CLOCK_OK);
	assert(model.timer.state == CLOCK_TIMER_FINISHED);
	assert(model.active.kind == CLOCK_EVENT_TIMER);
	assert(!model.active.label[0]);
	assert(model.active.due_monotonic_ms == 16500);
	assert(clock_model_snooze(&model, model.active.id, 1) ==
	       CLOCK_WRONG_STATE);
	assert(clock_model_timer_restart(&model) == CLOCK_OK);
	assert(model.active.kind == CLOCK_EVENT_NONE);
	assert(clock_model_timer_remaining(&model) == 10000);
	assert(clock_model_timer_stop(&model) == CLOCK_OK);
	assert(model.timer.state == CLOCK_TIMER_STOPPED);
	assert(clock_model_stopwatch_reset(&model) == CLOCK_OK);
	assert(!model.stopwatch.running && model.stopwatch.lap_count == 0);
	assert(clock_model_stopwatch_elapsed(&model) == 0);
}

static void scheduled_alarms_do_not_replay_after_wall_adjustment(void)
{
	struct clock_model model;
	struct clock_alarm_spec once = { .label = "Once",
					 .schedule = CLOCK_ALARM_ONCE,
					 .once_utc = 1704067260,
					 .enabled = true };
	struct clock_alarm_spec monday = { .label = "Monday",
					   .schedule = CLOCK_ALARM_WEEKLY,
					   .minute = 1,
					   .repeat_days = 1,
					   .enabled = true };
	uint32_t once_id, monday_id;
	uint64_t first_event;

	/* 2024-01-01 is Monday; both alarms select 00:01 UTC. */
	assert(clock_model_init(&model, 1704067200, 1000) == CLOCK_OK);
	assert(clock_model_alarm_add(&model, &once, &once_id) == CLOCK_OK);
	assert(clock_model_alarm_add(&model, &monday, &monday_id) == CLOCK_OK);
	assert(once_id == 1 && monday_id == 2);
	assert(model.alarms[0].next_due_utc == 1704067260);
	assert(clock_model_tick(&model, 1704067260, 2000) == CLOCK_OK);
	first_event = model.active.id;
	assert(model.active.source_id == 1 && model.queue_count == 1);
	assert(!model.alarms[0].spec.enabled);
	assert(model.alarms[1].next_due_utc == 1704672060);
	assert(clock_model_alarm_delete(&model, once_id, false) ==
	       CLOCK_ACTIVE_EVENT);
	assert(clock_model_dismiss(&model, first_event) == CLOCK_OK);
	assert(model.active.source_id == 2);
	assert(clock_model_dismiss(&model, first_event) == CLOCK_STALE_EVENT);
	assert(clock_model_dismiss(&model, model.active.id) == CLOCK_OK);
	assert(clock_model_alarm_delete(&model, once_id, false) == CLOCK_OK);
	assert(model.alarms[0].id == monday_id);
	assert(clock_model_tick(&model, 1704067200, 3000) == CLOCK_OK);
	assert(clock_model_alarm_edit(&model, monday_id, &monday, false) ==
	       CLOCK_OK);
	assert(model.alarms[0].next_due_utc == 1704672060);
	assert(clock_model_tick(&model, 1704067260, 4000) == CLOCK_OK);
	assert(model.active.kind == CLOCK_EVENT_NONE && model.queue_count == 0);
	assert(clock_model_tick(&model, 1704672060, 5000) == CLOCK_OK);
	assert(model.active.source_id == monday_id);
	assert(model.active.due_wall_utc == 1704672060);
}

static void snooze_uses_deadline_and_rejects_stale_actions(void)
{
	struct clock_model model;
	struct clock_alarm_spec spec = { .label = "Wake",
					 .schedule = CLOCK_ALARM_ONCE,
					 .once_utc = 1704067260,
					 .enabled = true };
	uint64_t first_event;

	assert(clock_model_init(&model, 1704067200, 0) == CLOCK_OK);
	assert(clock_model_alarm_add(&model, &spec, NULL) == CLOCK_OK);
	assert(clock_model_tick(&model, 1704067260, 1000) == CLOCK_OK);
	first_event = model.active.id;
	assert(clock_model_set_snooze(&model, 1) == CLOCK_OK);
	assert(clock_model_snooze(&model, first_event, 0) == CLOCK_OK);
	assert(model.alarms[0].snooze_deadline_ms == 61000);
	assert(clock_model_tick(&model, 1704153600, 60999) == CLOCK_OK);
	assert(model.active.kind == CLOCK_EVENT_NONE);
	assert(clock_model_tick(&model, 1704067200, 61000) == CLOCK_OK);
	assert(model.active.kind == CLOCK_EVENT_ALARM && model.active.snoozed);
	assert(model.active.source_id == 1 && model.active.id != first_event);
	assert(clock_model_snooze(&model, first_event, 1) == CLOCK_STALE_EVENT);
	assert(clock_model_dismiss(&model, first_event) == CLOCK_STALE_EVENT);
	assert(clock_model_alarm_enable(&model, 1, false, false) ==
	       CLOCK_ACTIVE_EVENT);
	assert(clock_model_alarm_enable(&model, 1, false, true) == CLOCK_OK);
	assert(model.active.kind == CLOCK_EVENT_NONE &&
	       !model.alarms[0].snoozed);
}

static void late_alarms_and_bounded_queue_record_misses(void)
{
	struct clock_model model;
	struct clock_alarm_spec spec = { .schedule = CLOCK_ALARM_WEEKLY,
					 .minute = 1,
					 .repeat_days = 127,
					 .enabled = true };

	assert(clock_model_init(&model, 1704067200, 0) == CLOCK_OK);
	assert(clock_model_alarm_add(&model, &spec, NULL) == CLOCK_OK);
	assert(clock_model_tick(&model, 1704067320, 1000) == CLOCK_OK);
	assert(model.active.kind == CLOCK_EVENT_ALARM);
	assert(clock_model_dismiss(&model, model.active.id) == CLOCK_OK);
	assert(clock_model_tick(&model, 1704240121, 2000) == CLOCK_OK);
	assert(model.active.kind == CLOCK_EVENT_NONE);
	assert(model.missed_count == 1 && model.missed_total == 1);
	assert(model.missed[0].due_wall_utc == 1704153660);
	assert(model.missed[0].missed_reason == CLOCK_MISSED_LATE);
	assert(model.alarms[0].next_due_utc == 1704326460);

	assert(clock_model_init(&model, 1704067200, 0) == CLOCK_OK);
	for (unsigned int index = 0; index < 8; index++)
		assert(clock_model_alarm_add(&model, &spec, NULL) == CLOCK_OK);
	assert(clock_model_tick(&model, 1704067260, 1000) == CLOCK_OK);
	assert(model.active.source_id == 1 && model.queue_count == 7);
	assert(model.queue[0].source_id == 2 && model.queue[6].source_id == 8);
	assert(clock_model_tick(&model, 1704153660, 2000) == CLOCK_OK);
	assert(model.queue_count == 8 && model.missed_total == 7);
	assert(model.queue[7].source_id == 1 && model.queue[7].id == 9);
	assert(model.missed[0].source_id == 2 &&
	       model.missed[6].source_id == 8);
	assert(model.missed[0].missed_reason == CLOCK_MISSED_QUEUE_FULL);
	assert(clock_model_tick(&model, 1704153660, 61000) == CLOCK_OK);
	assert(model.active.source_id == 2 &&
	       model.active.ring_started_ms == 61000);
	assert(model.queue_count == 7 && model.missed_total == 8);
	assert(model.missed[7].id == 1);
	assert(model.missed[7].missed_reason == CLOCK_MISSED_EXPIRED);
	assert(clock_model_tick(&model, 1704153660, 121000) == CLOCK_OK);
	assert(model.missed_total == 9 && model.missed_count == 8);
	assert(model.missed[0].source_id == 3);
	clock_model_clear_missed(&model);
	assert(model.missed_total == 0 && model.missed_count == 0);
}

static void
world_offsets_keep_stable_ids_and_reject_invalid_or_duplicate_values(void)
{
	static const int invalid[] = {
		INT_MIN, -735, -721, -719, -1,	1,	 14,
		16,	 329,  346,  841,  855, INT_MAX,
	};
	static const int accepted[] = { -720, -210, 0, 60, 330, 345, 765, 840 };
	struct clock_model model, before;
	uint32_t first_id, second_id;

	assert(clock_model_init(&model, 1704067200, 0) == CLOCK_OK);
	for (size_t index = 0; index < sizeof(invalid) / sizeof(invalid[0]);
	     ++index) {
		before = model;
		assert(!clock_model_world_offset_valid(invalid[index]));
		assert(clock_model_world_add(&model, invalid[index], NULL) ==
		       CLOCK_INVALID);
		assert(!memcmp(&model, &before, sizeof(model)));
	}
	assert(clock_model_world_add(&model, 345, &first_id) == CLOCK_OK);
	assert(clock_model_world_add(&model, -210, &second_id) == CLOCK_OK);
	assert(first_id == 1 && second_id == 2);
	before = model;
	assert(clock_model_world_add(&model, 345, NULL) == CLOCK_INVALID);
	assert(!memcmp(&model, &before, sizeof(model)));
	assert(clock_model_world_edit(&model, first_id, 330) == CLOCK_OK);
	assert(model.world_count == 2 && model.world[0].id == first_id);
	assert(model.world[0].offset_minutes == 330 &&
	       model.next_world_id == 3);
	assert(model.world[1].id == second_id &&
	       model.world[1].offset_minutes == -210);
	before = model;
	assert(clock_model_world_edit(&model, first_id, 330) == CLOCK_OK);
	assert(!memcmp(&model, &before, sizeof(model)));
	assert(clock_model_world_edit(&model, first_id, -210) == CLOCK_INVALID);
	assert(!memcmp(&model, &before, sizeof(model)));
	assert(clock_model_world_edit(&model, first_id, 346) == CLOCK_INVALID);
	assert(!memcmp(&model, &before, sizeof(model)));
	assert(clock_model_world_edit(&model, 99, 345) == CLOCK_NOT_FOUND);
	assert(!memcmp(&model, &before, sizeof(model)));
	assert(clock_model_world_delete(&model, first_id) == CLOCK_OK);
	assert(model.world_count == 1 && model.world[0].id == second_id);
	assert(model.world[0].offset_minutes == -210);
	assert(clock_model_world_add(&model, 345, &first_id) == CLOCK_OK);
	assert(first_id == 3 && model.world[1].id == 3);
	assert(clock_model_world_delete(&model, 1) == CLOCK_NOT_FOUND);
	assert(clock_model_world_delete(&model, 0) == CLOCK_NOT_FOUND);

	assert(clock_model_init(&model, 1704067200, 0) == CLOCK_OK);
	for (size_t index = 0; index < sizeof(accepted) / sizeof(accepted[0]);
	     ++index) {
		assert(clock_model_world_offset_valid(accepted[index]));
		assert(clock_model_world_add(&model, accepted[index], NULL) ==
		       CLOCK_OK);
		assert(model.world[index].offset_minutes == accepted[index]);
	}
	before = model;
	assert(model.world_count == 8);
	assert(clock_model_world_add(&model, 15, NULL) == CLOCK_FULL);
	assert(!memcmp(&model, &before, sizeof(model)));
}

static void bounded_utf8_preserves_user_text(void)
{
	static const char thirty_two[] = "😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀"
					 "😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀";

	assert(clock_model_validate_text(thirty_two, 128, false) == CLOCK_OK);
	assert(clock_model_validate_text("123456789012345678901234567890123",
					 33, false) == CLOCK_INVALID);
	assert(clock_model_validate_text("a\0b", 3, false) == CLOCK_INVALID);
	assert(clock_model_validate_text("\xc0\xaf", 2, false) ==
	       CLOCK_INVALID);
	assert(clock_model_validate_text("\xed\xa0\x80", 3, false) ==
	       CLOCK_INVALID);
	assert(clock_model_validate_text("\xf0\x9f", 2, false) ==
	       CLOCK_INVALID);
	assert(clock_model_validate_text("\xc2\x85", 2, false) ==
	       CLOCK_INVALID);
}

static void explicit_dates_and_weekdays_choose_absolute_utc_minutes(void)
{
	struct clock_model model, before;
	struct clock_local_time calendar = {
		.year = 2024,
		.month = 2,
		.day = 29,
		.hour = 7,
		.minute = 30,
	};
	struct clock_alarm_spec once = {
		.schedule = CLOCK_ALARM_ONCE,
		.once_utc = 1709191800, /* 2024-02-29 07:30 UTC */
		.enabled = true,
	};
	struct clock_alarm_spec weekly = {
		.schedule = CLOCK_ALARM_WEEKLY,
		.hour = 7,
		.minute = 30,
		.repeat_days = (1U << 1) | (1U << 3) | (1U << 5),
		.enabled = true,
	};
	int64_t utc = 123;

	assert(clock_model_utc_from_calendar(&calendar, &utc) == CLOCK_OK);
	assert(utc == 1709191800);
	assert(clock_model_utc_time(1709191800, &calendar) == CLOCK_OK);
	assert(calendar.year == 2024 && calendar.month == 2 &&
	       calendar.day == 29);
	assert(calendar.weekday == 3 && calendar.hour == 7 &&
	       calendar.minute == 30);
	calendar.year = 2023;
	assert(clock_model_utc_from_calendar(&calendar, &utc) == CLOCK_INVALID);
	assert(utc == 1709191800);
	calendar.year = 2099;
	calendar.month = 12;
	calendar.day = 31;
	calendar.hour = calendar.minute = 0;
	assert(clock_model_utc_from_calendar(&calendar, &utc) == CLOCK_OK);
	assert(utc == 4102358400);
	calendar.month = 4;
	calendar.day = 31;
	assert(clock_model_utc_from_calendar(&calendar, &utc) == CLOCK_INVALID);
	assert(utc == 4102358400);

	assert(clock_model_init(&model, 1704067200, 0) == CLOCK_OK);
	assert(clock_model_alarm_add(&model, &once, NULL) == CLOCK_OK);
	assert(model.alarms[0].next_due_utc == 1709191800);
	assert(model.last_alarm_hour == 7 && model.last_alarm_minute == 30);
	assert(clock_model_alarm_next_due(&weekly, 1704067200, &utc) ==
	       CLOCK_OK);
	assert(utc == 1704180600); /* Tuesday 2024-01-02 07:30 UTC */
	assert(clock_model_alarm_next_due(&weekly, 1704180600, &utc) ==
	       CLOCK_OK);
	assert(utc == 1704353400); /* Thursday 2024-01-04 07:30 UTC */
	weekly.repeat_days = 0;
	before = model;
	assert(clock_model_alarm_add(&model, &weekly, NULL) == CLOCK_INVALID);
	assert(!memcmp(&before, &model, sizeof(model)));
	once.once_utc++;
	assert(clock_model_alarm_edit(&model, 1, &once, false) ==
	       CLOCK_INVALID);
	assert(!memcmp(&before, &model, sizeof(model)));
	assert(clock_model_tick(&model, 1709191800, 1000) == CLOCK_OK);
	assert(!model.alarms[0].spec.enabled);
	assert(model.alarms[0].spec.once_utc == 1709191800);
	assert(clock_model_dismiss(&model, model.active.id) == CLOCK_OK);
	before = model;
	assert(clock_model_alarm_enable(&model, 1, true, false) ==
	       CLOCK_INVALID);
	assert(!memcmp(&before, &model, sizeof(model)));
}

static void cancelling_snooze_preserves_regular_alarm_schedule(void)
{
	for (unsigned int weekly = 0; weekly < 2; ++weekly) {
		struct clock_model model;
		struct clock_alarm_spec spec = {
			.schedule = weekly ? CLOCK_ALARM_WEEKLY :
					     CLOCK_ALARM_ONCE,
			.once_utc = weekly ? 0 : 1704067260,
			.minute = weekly ? 1 : 0,
			.repeat_days = weekly ? 127 : 0,
			.enabled = true,
		};
		int64_t next;
		uint32_t generation;

		assert(clock_model_init(&model, 1704067200, 1000) == CLOCK_OK);
		assert(clock_model_alarm_add(&model, &spec, NULL) == CLOCK_OK);
		assert(clock_model_tick(&model, 1704067260, 61000) == CLOCK_OK);
		assert(clock_model_snooze(&model, model.active.id, 5) ==
		       CLOCK_OK);
		assert(model.alarms[0].snoozed);
		assert(model.alarms[0].spec.enabled == (bool)weekly);
		next = model.alarms[0].next_due_utc;
		generation = model.alarms[0].generation;
		assert(clock_model_alarm_cancel_snooze(&model, 1) == CLOCK_OK);
		assert(!model.alarms[0].snoozed &&
		       !model.alarms[0].snooze_deadline_ms);
		assert(model.alarms[0].next_due_utc == next);
		assert(model.alarms[0].generation == generation);
		assert(model.alarms[0].spec.enabled == (bool)weekly);
		assert(clock_model_tick(&model, 1704067560, 361000) ==
		       CLOCK_OK);
		assert(!model.active.id);
		if (weekly) {
			assert(next == 1704153660);
			assert(clock_model_tick(&model, next, 86461000) ==
			       CLOCK_OK);
			assert(model.active.kind == CLOCK_EVENT_ALARM);
		}
	}
}

int main(void)
{
	countdown_and_stopwatch_use_monotonic_time();
	scheduled_alarms_do_not_replay_after_wall_adjustment();
	snooze_uses_deadline_and_rejects_stale_actions();
	cancelling_snooze_preserves_regular_alarm_schedule();
	late_alarms_and_bounded_queue_record_misses();
	world_offsets_keep_stable_ids_and_reject_invalid_or_duplicate_values();
	bounded_utf8_preserves_user_text();
	explicit_dates_and_weekdays_choose_absolute_utc_minutes();
	puts("clock-model: time, events and user values passed");
	return 0;
}
