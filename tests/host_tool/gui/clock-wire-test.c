/* SPDX-License-Identifier: GPL-2.0-only */
#include "clock-wire.h"
#include "session-clock.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <json-c/json.h>

static void apply(struct fplinux_session_clock *clock, const char *json,
		  int64_t wall_utc, uint64_t mono_ms)
{
	struct clock_request request;

	assert(clock_wire_request_decode(json, strlen(json), &request) ==
	       CLOCK_OK);
	assert(fplinux_session_clock_apply(clock, &request, wall_utc, mono_ms)
		       .status == CLOCK_OK);
}

static void literal_requests_drive_the_session_owner(const char *directory)
{
	struct fplinux_session_clock clock;
	struct clock_request request;
	char json[256];
	static const char large_id[] =
		"{\"op\":\"dismiss\",\"event_id\":9223372036854775817}";

	assert(fplinux_session_clock_init(&clock, 1704067200, 1000, directory)
		       .status == CLOCK_OK);
	assert(fplinux_session_clock_deadline(&clock, 1000) == 0);
	apply(&clock, "{\"op\":\"stopwatch-start\"}", 1704067200, 1000);
	assert(fplinux_session_clock_deadline(&clock, 1000) == 0);
	apply(&clock, "{\"op\":\"timer-set\",\"seconds\":2}", 1704067200, 1000);
	apply(&clock, "{\"op\":\"timer-start\"}", 1704067200, 1000);
	assert(fplinux_session_clock_deadline(&clock, 1000) == 3000);
	apply(&clock, "{\"op\":\"timer-pause\"}", 1704153600, 1500);
	assert(clock.model.timer.remaining_ms == 1500);
	assert(fplinux_session_clock_deadline(&clock, 1500) == 0);
	apply(&clock, "{\"op\":\"timer-resume\"}", 1704067200, 2000);
	assert(fplinux_session_clock_deadline(&clock, 2000) == 3500);
	assert(fplinux_session_clock_tick(&clock, 1704067200, 3500).status ==
	       CLOCK_OK);
	assert(fplinux_session_clock_ringing(&clock));
	assert(fplinux_session_clock_deadline(&clock, 3500) == 63500);
	assert(fplinux_session_clock_deadline(&clock, 63500) == 63501);
	apply(&clock, "{\"op\":\"dismiss\",\"event_id\":1}", 1704067200, 3500);
	assert(!fplinux_session_clock_ringing(&clock));
	apply(&clock, "{\"op\":\"set-view\",\"view\":1}", 1704067200, 3500);
	apply(&clock, "{\"op\":\"set-snooze\",\"minutes\":7}", 1704067200,
	      3500);
	apply(&clock, "{\"op\":\"save\"}", 1704067200, 3500);
	apply(&clock, "{\"op\":\"world-add\",\"offset_minutes\":345}",
	      1704067200, 3500);
	apply(&clock,
	      "{\"op\":\"alarm-add\",\"alarm\":{\"label\":\"Чай \\\"утром\\\"\",\"schedule\":0,\"once_utc\":1704067260,\"hour\":0,\"minute\":0,\"repeat_days\":0,\"enabled\":true}}",
	      1704067200, 3500);
	assert(clock.model.view == CLOCK_VIEW_ANALOG);
	assert(clock.model.snooze_minutes == 7);
	assert(clock.model.world[0].id == 1 &&
	       clock.model.world[0].offset_minutes == 345);
	assert(!strcmp(clock.model.alarms[0].spec.label, "Чай \"утром\""));
	assert(clock.model.alarms[0].next_due_utc == 1704067260);
	assert(fplinux_session_clock_deadline(&clock, 3500) == 4500);
	assert(clock_wire_request_decode(large_id, sizeof(large_id) - 1,
					 &request) == CLOCK_OK);
	assert(request.event_id == UINT64_C(9223372036854775817));
	assert(clock_wire_request_encode(&request, json, sizeof(json)) ==
	       CLOCK_OK);
	assert(!strcmp(json, large_id));
	fplinux_session_clock_cleanup(&clock);
}

static void malformed_requests_leave_the_output_unchanged(void)
{
	static const char *const invalid[] = {
		"{\"op\":\"snapshot\",\"op\":\"snapshot\"}",
		"{\"op\":\"snapshot\",\"\\u006fp\":\"snapshot\"}",
		"{\"op\":\"snapshot\",\"extra\":0}",
		"{\"op\":\"save\",\"extra\":0}",
		"{\"op\":\"snapshot\"} {}",
		"{\"op\":\"snapshot\",}",
		"{\"op\":\"snapshot\\u0000ignored\"}",
		"{\"op\":\"timer-set\",\"seconds\":\"2\"}",
		"{\"op\":\"timer-set\",\"seconds\":true}",
		"{\"op\":\"timer-set\",\"seconds\":2.0}",
		"{\"op\":\"timer-set\",\"seconds\":-1}",
		"{\"op\":\"timer-set\",\"seconds\":360000}",
		"{\"op\":\"dismiss\",\"event_id\":0}",
		"{\"op\":\"dismiss\",\"event_id\":18446744073709551616}",
		"{\"op\":\"alarm-delete\",\"id\":4294967296,\"cancel_active\":false}",
		"{\"op\":\"alarm-enable\",\"id\":1,\"enabled\":1,\"cancel_active\":false}",
		"{\"op\":\"alarm-add\",\"alarm\":{\"label\":\"\",\"hour\":1,\"minute\":0,\"repeat_days\":0,\"enabled\":true}}",
		"{\"op\":\"alarm-add\",\"alarm\":{\"label\":\"\",\"schedule\":1,\"once_utc\":0,\"hour\":24,\"minute\":0,\"repeat_days\":1,\"enabled\":true}}",
		"{\"op\":\"alarm-add\",\"alarm\":{\"label\":\"\",\"schedule\":1,\"once_utc\":0,\"hour\":1,\"hour\":1,\"minute\":0,\"repeat_days\":1,\"enabled\":true}}",
		"{\"op\":\"alarm-add\",\"alarm\":{\"label\":\"a\\u0000b\",\"schedule\":1,\"once_utc\":0,\"hour\":1,\"minute\":0,\"repeat_days\":1,\"enabled\":true}}",
		"{\"op\":\"alarm-add\",\"alarm\":{\"label\":\"\\ud800\",\"schedule\":1,\"once_utc\":0,\"hour\":1,\"minute\":0,\"repeat_days\":1,\"enabled\":true}}",
		"{\"op\":\"alarm-add\",\"alarm\":{\"label\":\"\",\"schedule\":true,\"once_utc\":0,\"hour\":1,\"minute\":0,\"repeat_days\":1,\"enabled\":true}}",
		"{\"op\":\"alarm-add\",\"alarm\":{\"label\":\"\",\"schedule\":1,\"once_utc\":0,\"hour\":1,\"minute\":0,\"repeat_days\":0,\"enabled\":true}}",
		"{\"op\":\"alarm-add\",\"alarm\":{\"label\":\"\",\"schedule\":1,\"once_utc\":1704067260,\"hour\":1,\"minute\":0,\"repeat_days\":1,\"enabled\":true}}",
		"{\"op\":\"alarm-add\",\"alarm\":{\"label\":\"\",\"schedule\":0,\"once_utc\":1704067261,\"hour\":0,\"minute\":0,\"repeat_days\":0,\"enabled\":true}}",
		"{\"op\":\"alarm-add\",\"alarm\":{\"label\":\"\",\"schedule\":0,\"once_utc\":315532740,\"hour\":0,\"minute\":0,\"repeat_days\":0,\"enabled\":true}}",
		"{\"op\":\"alarm-add\",\"alarm\":{\"label\":\"\",\"schedule\":0,\"once_utc\":4102444800,\"hour\":0,\"minute\":0,\"repeat_days\":0,\"enabled\":true}}",
		"{\"op\":\"alarm-add\",\"alarm\":{\"label\":\"\",\"schedule\":0,\"once_utc\":1704067260,\"hour\":1,\"minute\":0,\"repeat_days\":0,\"enabled\":true}}",
		"{\"op\":\"world-add\"}",
		"{\"op\":\"world-add\",\"offset_minutes\":-735}",
		"{\"op\":\"world-add\",\"offset_minutes\":855}",
		"{\"op\":\"world-add\",\"offset_minutes\":-721}",
		"{\"op\":\"world-add\",\"offset_minutes\":841}",
		"{\"op\":\"world-add\",\"offset_minutes\":1}",
		"{\"op\":\"world-add\",\"offset_minutes\":-1}",
		"{\"op\":\"world-add\",\"offset_minutes\":346}",
		"{\"op\":\"world-add\",\"offset_minutes\":345.0}",
		"{\"op\":\"world-add\",\"offset_minutes\":\"345\"}",
		"{\"op\":\"world-add\",\"offset_minutes\":true}",
		"{\"op\":\"world-add\",\"offset_minutes\":null}",
		"{\"op\":\"world-add\",\"offset_minutes\":[]}",
		"{\"op\":\"world-add\",\"offset_minutes\":{}}",
		"{\"op\":\"world-add\",\"offset_minutes\":4294967296}",
		"{\"op\":\"world-add\",\"offset_minutes\":18446744073709551615}",
		"{\"op\":\"world-add\",\"offset_minutes\":-9223372036854775809}",
		"{\"op\":\"world-add\",\"offset_minutes\":0,\"offset_minutes\":0}",
		"{\"op\":\"world-add\",\"extra\":\"label\",\"offset_minutes\":0}",
		"{\"op\":\"world-edit\",\"offset_minutes\":345}",
		"{\"op\":\"world-edit\",\"id\":0,\"offset_minutes\":345}",
		"{\"op\":\"world-edit\",\"id\":1}",
		"{\"op\":\"world-edit\",\"id\":1,\"offset_minutes\":-211}",
		"{\"op\":\"world-edit\",\"id\":1,\"offset_minutes\":\"345\"}",
		"{\"op\":\"world-edit\",\"id\":1,\"offset_minutes\":345,\"extra\":true}",
		"{\"op\":\"set-home\",\"offset_minutes\":0,\"hour24\":true}",
		"{\"op\":\"set-view\",\"view\":2}",
		"{\"op\":\"set-view\",\"view\":\"analog\"}",
		"{\"op\":\"set-snooze\",\"minutes\":0}",
		"{\"op\":\"snooze\",\"event_id\":1,\"minutes\":31}",
		"{\"op\":\"demo-advance\",\"seconds\":60}",
		"{\"op\":\"demo-reset\"}",
		"{\"op\":\"set-tone-available\",\"available\":true}",
	};
	struct clock_request request, before;

	memset(&before, 0xa5, sizeof(before));
	for (size_t index = 0; index < sizeof(invalid) / sizeof(invalid[0]);
	     index++) {
		request = before;
		assert(clock_wire_request_decode(invalid[index],
						 strlen(invalid[index]),
						 &request) == CLOCK_INVALID);
		assert(!memcmp(&request, &before, sizeof(request)));
	}
}

static void explicit_schedules_and_selections_encode_literal_fields(void)
{
	static const char *const messages[] = {
		"{\"op\":\"alarm-add\",\"alarm\":{\"label\":\"День\",\"schedule\":0,\"once_utc\":1706745600,\"hour\":0,\"minute\":0,\"repeat_days\":0,\"enabled\":true}}",
		"{\"op\":\"alarm-edit\",\"id\":37,\"cancel_active\":false,\"alarm\":{\"label\":\"Утро\",\"schedule\":1,\"once_utc\":0,\"hour\":7,\"minute\":30,\"repeat_days\":42,\"enabled\":true}}",
		"{\"op\":\"world-add\",\"offset_minutes\":-720}",
		"{\"op\":\"world-add\",\"offset_minutes\":-210}",
		"{\"op\":\"world-add\",\"offset_minutes\":0}",
		"{\"op\":\"world-add\",\"offset_minutes\":330}",
		"{\"op\":\"world-add\",\"offset_minutes\":345}",
		"{\"op\":\"world-add\",\"offset_minutes\":840}",
		"{\"op\":\"world-edit\",\"id\":37,\"offset_minutes\":-210}",
		"{\"op\":\"world-delete\",\"id\":37}",
		"{\"op\":\"set-view\",\"view\":0}",
		"{\"op\":\"timer-set\",\"seconds\":359999}",
		"{\"op\":\"alarm-cancel-snooze\",\"id\":37}",
	};
	struct clock_request request;
	char json[512];

	for (size_t index = 0; index < sizeof(messages) / sizeof(messages[0]);
	     ++index) {
		assert(clock_wire_request_decode(messages[index],
						 strlen(messages[index]),
						 &request) == CLOCK_OK);
		assert(clock_wire_request_encode(&request, json,
						 sizeof(json)) == CLOCK_OK);
		assert(!strcmp(json, messages[index]));
	}
}

static void disabled_once_saves_retain_the_date_without_enabling(void)
{
	static const char disabled_save[] =
		"{\"op\":\"alarm-add\",\"alarm\":{\"label\":\"Later\",\"schedule\":0,"
		"\"once_utc\":1704067260,\"hour\":0,\"minute\":0,\"repeat_days\":0,\"enabled\":false}}";
	struct clock_model model, before;
	struct clock_request request;

	assert(clock_model_init(&model, 1704067200, 0) == CLOCK_OK);
	assert(clock_wire_request_decode(disabled_save,
					 sizeof(disabled_save) - 1,
					 &request) == CLOCK_OK);
	/* The date was future while editing, but passed before owner acceptance. */
	assert(clock_model_tick(&model, 1704067320, 1000) == CLOCK_OK);
	assert(clock_wire_request_apply(&model, &request) == CLOCK_OK);
	assert(model.alarm_count == 1 && model.alarms[0].id == 1);
	assert(!model.alarms[0].spec.enabled && !model.alarms[0].next_due_utc);
	assert(model.alarms[0].spec.once_utc == 1704067260);
	request.op = CLOCK_REQUEST_ALARM_EDIT;
	request.id = 1;
	request.alarm.once_utc = 1704067200;
	assert(clock_wire_request_apply(&model, &request) == CLOCK_OK);
	assert(model.alarms[0].spec.once_utc == 1704067200);
	request.alarm.enabled = true;
	before = model;
	assert(clock_wire_request_apply(&model, &request) == CLOCK_INVALID);
	assert(!memcmp(&model, &before, sizeof(model)));
	request.alarm.enabled = false;
	request.alarm.once_utc = 1704240060; /* 2024-01-03 00:01 UTC */
	assert(clock_wire_request_apply(&model, &request) == CLOCK_OK);
	assert(!model.alarms[0].spec.enabled);
	assert(model.alarms[0].spec.once_utc == 1704240060);
	request.op = CLOCK_REQUEST_ALARM_ADD;
	request.alarm.once_utc = 1704067380; /* 2024-01-01 00:03 UTC */
	request.alarm.enabled = true;
	assert(clock_wire_request_apply(&model, &request) == CLOCK_OK);
	assert(clock_model_tick(&model, 1704067380, 2000) == CLOCK_OK);
	assert(model.active.source_id == 2 && !model.alarms[1].spec.enabled);
	request = (struct clock_request){
		.op = CLOCK_REQUEST_ALARM_ENABLE,
		.id = 2,
		.enabled = false,
		.cancel_active = true,
	};
	assert(clock_wire_request_apply(&model, &request) == CLOCK_OK);
	assert(!model.active.id && !model.alarms[1].spec.enabled);
	assert(model.alarms[1].spec.once_utc == 1704067380);
	assert(model.alarms[1].last_due_utc == 1704067380);
}

static void preferences_restore_configuration_without_replaying_runtime(void)
{
	static const char configuration[] =
		"{\"view\":1,\"snooze_minutes\":7,\"last_alarm_hour\":7,"
		"\"last_alarm_minute\":30,\"timer_duration_seconds\":90,"
		"\"next_alarm_id\":16,\"next_world_id\":38,\"alarms\":["
		"{\"id\":9,\"generation\":7,\"spec\":{\"label\":\"Прошедший\","
		"\"schedule\":0,\"once_utc\":1704067260,\"hour\":0,\"minute\":0,"
		"\"repeat_days\":0,\"enabled\":true},\"last_due_utc\":0},"
		"{\"id\":11,\"generation\":2,\"spec\":{\"label\":\"Февраль\","
		"\"schedule\":0,\"once_utc\":1706745600,\"hour\":0,\"minute\":0,"
		"\"repeat_days\":0,\"enabled\":true},\"last_due_utc\":0},"
		"{\"id\":15,\"generation\":4,\"spec\":{\"label\":\"Вторник\","
		"\"schedule\":1,\"once_utc\":0,\"hour\":7,\"minute\":30,"
		"\"repeat_days\":2,\"enabled\":true},\"last_due_utc\":1704180600}],"
		"\"world\":[{\"id\":37,\"offset_minutes\":345}]}";
	struct clock_model model, before, end_range;
	struct json_object *object, *value, *alarms, *alarm, *spec;
	const char *malformed;
	char json[CLOCK_WIRE_CAPACITY];
	uint32_t id;

	assert(clock_model_init(&model, 1704153600, 7000) == CLOCK_OK);
	assert(clock_model_timer_set(&model, 10) == CLOCK_OK);
	assert(clock_model_timer_start(&model) == CLOCK_OK);
	assert(clock_model_stopwatch_start(&model) == CLOCK_OK);
	assert(clock_model_tick(&model, 1704153600, 17000) == CLOCK_OK);
	assert(model.active.kind == CLOCK_EVENT_TIMER);
	assert(clock_wire_preferences_decode(configuration,
					     sizeof(configuration) - 1,
					     &model) == CLOCK_OK);
	assert(model.wall_utc == 1704153600 && model.monotonic_ms == 17000);
	assert(model.view == CLOCK_VIEW_ANALOG && model.snooze_minutes == 7);
	assert(model.last_alarm_hour == 7 && model.last_alarm_minute == 30);
	assert(model.alarm_count == 3 && model.world_count == 1);
	assert(model.alarms[0].id == 9 && model.alarms[0].generation == 7);
	assert(!model.alarms[0].spec.enabled);
	assert(model.alarms[0].spec.once_utc == 1704067260);
	assert(model.alarms[0].next_due_utc == 0);
	assert(model.alarms[1].id == 11 && model.alarms[1].spec.enabled);
	assert(model.alarms[1].next_due_utc == 1706745600);
	/* The consumed Tuesday cannot replay after a wall-clock adjustment. */
	assert(model.alarms[2].id == 15 && model.alarms[2].spec.enabled);
	assert(model.alarms[2].last_due_utc == 1704180600);
	assert(model.alarms[2].next_due_utc == 1704785400);
	assert(model.world[0].id == 37 && model.world[0].offset_minutes == 345);
	assert(model.active.kind == CLOCK_EVENT_NONE && model.queue_count == 0);
	assert(model.missed_count == 0 && model.missed_total == 0);
	assert(model.next_event_id == 1);
	assert(model.timer.state == CLOCK_TIMER_STOPPED);
	assert(model.timer.duration_seconds == 90 &&
	       model.timer.remaining_ms == 90000);
	assert(model.timer.deadline_ms == 0 && !model.stopwatch.running);
	assert(model.stopwatch.elapsed_ms == 0 &&
	       model.stopwatch.lap_count == 0);
	assert(clock_wire_preferences_encode(&model, json, sizeof(json)) ==
	       CLOCK_OK);
	object = json_tokener_parse(json);
	assert(object && json_object_object_length(object) == 9);
	assert(!json_object_object_get_ex(object, "active", &value));
	assert(!json_object_object_get_ex(object, "wall_utc", &value));
	assert(!json_object_object_get_ex(object, "monotonic_ms", &value));
	assert(!json_object_object_get_ex(object, "stopwatch", &value));
	assert(json_object_object_get_ex(object, "alarms", &alarms));
	alarm = json_object_array_get_idx(alarms, 0);
	assert(json_object_object_length(alarm) == 4);
	assert(!json_object_object_get_ex(alarm, "snooze_deadline_ms", &value));
	assert(json_object_object_get_ex(alarm, "spec", &spec));
	assert(json_object_object_get_ex(spec, "enabled", &value));
	assert(!json_object_get_boolean(value));
	before = model;
	json_object_object_add(object, "view", json_object_new_int(2));
	malformed =
		json_object_to_json_string_ext(object, JSON_C_TO_STRING_PLAIN);
	assert(clock_wire_preferences_decode(malformed, strlen(malformed),
					     &model) == CLOCK_INVALID);
	assert(!memcmp(&model, &before, sizeof(model)));
	json_object_object_add(object, "view", json_object_new_int(1));
	alarm = json_object_array_get_idx(alarms, 1);
	json_object_object_add(alarm, "id", json_object_new_int(9));
	malformed =
		json_object_to_json_string_ext(object, JSON_C_TO_STRING_PLAIN);
	assert(clock_wire_preferences_decode(malformed, strlen(malformed),
					     &model) == CLOCK_INVALID);
	assert(!memcmp(&model, &before, sizeof(model)));
	json_object_object_add(alarm, "id", json_object_new_int(11));
	json_object_object_add(object, "active", json_object_new_object());
	malformed =
		json_object_to_json_string_ext(object, JSON_C_TO_STRING_PLAIN);
	assert(clock_wire_preferences_decode(malformed, strlen(malformed),
					     &model) == CLOCK_INVALID);
	assert(!memcmp(&model, &before, sizeof(model)));
	json_object_put(object);
	assert(clock_model_alarm_add(&model, &model.alarms[1].spec, &id) ==
	       CLOCK_OK);
	assert(id == 16);
	assert(clock_model_world_add(&model, -210, &id) == CLOCK_OK &&
	       id == 38);
	assert(clock_model_init(&end_range, 4102444799, 1) == CLOCK_OK);
	assert(clock_wire_preferences_decode(configuration,
					     sizeof(configuration) - 1,
					     &end_range) == CLOCK_OK);
	assert(!end_range.alarms[2].spec.enabled);
	assert(end_range.alarms[2].next_due_utc == 0);
	assert(clock_model_tick(&end_range, 4102444799, 2) == CLOCK_OK);
	assert(end_range.active.kind == CLOCK_EVENT_NONE &&
	       end_range.missed_total == 0);
}

static void snapshot_retains_user_values_and_checks_encoder_independently(
	const char *directory)
{
	struct fplinux_session_clock clock;
	struct clock_model decoded, before;
	struct clock_ring ring;
	struct clock_result result = {
		.status = CLOCK_FULL,
		.storage.state = FPLINUX_STORAGE_DURABLE,
	};
	const struct clock_result failed = {
		.status = CLOCK_STORAGE_ERROR,
		.applied = true,
		.storage = {
			.state = FPLINUX_STORAGE_PUBLISHED_UNSYNCED,
			.error = EIO,
		},
	};
	uint64_t serial = 4;
	char json[CLOCK_WIRE_CAPACITY];
	struct json_object *object, *model, *value, *alarms, *alarm, *spec,
		*stopwatch;

	assert(fplinux_session_clock_init(&clock, 1704067200, 1000, directory)
		       .status == CLOCK_OK);
	apply(&clock, "{\"op\":\"set-view\",\"view\":1}", 1704067200, 1000);
	apply(&clock, "{\"op\":\"world-add\",\"offset_minutes\":345}",
	      1704067200, 1000);
	apply(&clock,
	      "{\"op\":\"alarm-add\",\"alarm\":{\"label\":\"Чай\",\"schedule\":0,\"once_utc\":1704067260,\"hour\":0,\"minute\":0,\"repeat_days\":0,\"enabled\":true}}",
	      1704067200, 1000);
	apply(&clock, "{\"op\":\"timer-set\",\"seconds\":12}", 1704067200,
	      1000);
	apply(&clock, "{\"op\":\"timer-start\"}", 1704067200, 1000);
	apply(&clock, "{\"op\":\"stopwatch-start\"}", 1704067200, 1000);
	assert(fplinux_session_clock_tick(&clock, 1704067260, 2500).status ==
	       CLOCK_OK);
	apply(&clock, "{\"op\":\"stopwatch-lap\"}", 1704067260, 2500);
	assert(clock_wire_snapshot_encode(&clock.model, &failed,
					  UINT64_C(18446744073709551615), json,
					  sizeof(json)) == CLOCK_OK);
	object = json_tokener_parse(json);
	assert(object);
	assert(json_object_object_get_ex(object, "status", &value));
	assert(!strcmp(json_object_get_string(value), "storage-error"));
	assert(json_object_object_get_ex(object, "applied", &value));
	assert(json_object_get_boolean(value));
	assert(json_object_object_get_ex(object, "storage", &value));
	assert(!strcmp(json_object_get_string(value), "published-unsynced"));
	assert(json_object_object_get_ex(object, "error", &value));
	assert(json_object_get_int(value) == EIO);
	assert(json_object_object_get_ex(object, "serial", &value));
	assert(json_object_get_uint64(value) == UINT64_C(18446744073709551615));
	assert(json_object_object_get_ex(object, "model", &model));
	assert(json_object_object_get_ex(model, "view", &value));
	assert(json_object_get_int(value) == 1);
	assert(json_object_object_get_ex(model, "last_alarm_hour", &value));
	assert(json_object_get_int(value) == 0);
	assert(json_object_object_get_ex(model, "last_alarm_minute", &value));
	assert(json_object_get_int(value) == 1);
	assert(!json_object_object_get_ex(model, "home_offset_minutes",
					  &value));
	assert(!json_object_object_get_ex(model, "hour24", &value));
	assert(json_object_object_get_ex(model, "stopwatch", &stopwatch));
	assert(json_object_object_length(stopwatch) == 3);
	assert(json_object_object_get_ex(stopwatch, "running", &value));
	assert(json_object_get_boolean(value));
	assert(json_object_object_get_ex(stopwatch, "elapsed_ms", &value));
	assert(json_object_get_uint64(value) == 1500);
	assert(json_object_object_get_ex(stopwatch, "laps", &value));
	assert(json_object_array_length(value) == 1);
	assert(json_object_object_get_ex(model, "alarms", &alarms));
	alarm = json_object_array_get_idx(alarms, 0);
	assert(json_object_object_get_ex(alarm, "spec", &spec));
	assert(json_object_object_get_ex(spec, "label", &value));
	assert(!strcmp(json_object_get_string(value), "Чай"));
	assert(json_object_object_get_ex(spec, "schedule", &value));
	assert(json_object_get_int(value) == 0);
	assert(json_object_object_get_ex(spec, "once_utc", &value));
	assert(json_object_get_int64(value) == 1704067260);
	json_object_put(object);
	assert(clock_wire_snapshot_decode(json, strlen(json), &decoded, &result,
					  &serial) == CLOCK_OK);
	assert(result.status == CLOCK_STORAGE_ERROR && result.applied &&
	       result.storage.state == FPLINUX_STORAGE_PUBLISHED_UNSYNCED &&
	       result.storage.error == EIO &&
	       serial == UINT64_C(18446744073709551615));
	assert(decoded.view == CLOCK_VIEW_ANALOG);
	assert(decoded.world_count == 1 &&
	       decoded.world[0].offset_minutes == 345);
	assert(decoded.alarm_count == 1 && !decoded.alarms[0].spec.enabled);
	assert(decoded.alarms[0].spec.once_utc == 1704067260);
	assert(decoded.active.source_id == 1 &&
	       decoded.active.due_wall_utc == 1704067260);
	assert(decoded.timer.state == CLOCK_TIMER_RUNNING &&
	       decoded.timer.remaining_ms == 10500);
	assert(decoded.stopwatch.lap_count == 1 &&
	       decoded.stopwatch.laps[0].difference_ms == 1500);
	clock_wire_ring_from_model(&decoded, &ring);
	assert(ring.event.id == 1 && !strcmp(ring.event.label, "Чай"));
	before = decoded;
	assert(clock_wire_snapshot_decode("{}", 2, &decoded, &result,
					  &serial) == CLOCK_INVALID);
	assert(!memcmp(&before, &decoded, sizeof(decoded)));
	assert(result.status == CLOCK_STORAGE_ERROR && result.applied &&
	       result.storage.state == FPLINUX_STORAGE_PUBLISHED_UNSYNCED &&
	       result.storage.error == EIO &&
	       serial == UINT64_C(18446744073709551615));
	fplinux_session_clock_cleanup(&clock);
}

static void
literal_ring_preserves_large_identity_and_rejects_partial_output(void)
{
	static const char message[] =
		"{\"op\":\"ring\",\"serial\":18446744073709551615,"
		"\"event\":{\"id\":9223372036854775817,\"kind\":1,\"source_id\":37,"
		"\"generation\":2,\"label\":\"Чай\",\"due_wall_utc\":1704067260,"
		"\"due_monotonic_ms\":2000,\"ring_started_ms\":2000,"
		"\"snoozed\":true,\"missed_reason\":0},"
		"\"snooze_minutes\":7,\"missed_total\":9}";
	struct clock_ring ring, before;
	uint64_t serial;
	char json[CLOCK_WIRE_CAPACITY];

	assert(clock_wire_ring_decode(message, sizeof(message) - 1, &ring,
				      &serial) == CLOCK_OK);
	assert(ring.event.id == UINT64_C(9223372036854775817));
	assert(ring.event.source_id == 37 && ring.event.generation == 2);
	assert(!strcmp(ring.event.label, "Чай") && ring.event.snoozed);
	assert(ring.snooze_minutes == 7 && ring.missed_total == 9);
	assert(serial == UINT64_C(18446744073709551615));
	assert(clock_wire_ring_encode(&ring, serial, json, sizeof(json)) ==
	       CLOCK_OK);
	assert(!strcmp(json, message));
	strcpy(json, "unchanged");
	assert(clock_wire_ring_encode(&ring, serial, json, 5) == CLOCK_FULL);
	assert(!strcmp(json, "unchanged"));
	before = ring;
	assert(clock_wire_ring_decode(message, sizeof(message) - 2, &ring,
				      &serial) == CLOCK_INVALID);
	assert(!memcmp(&before, &ring, sizeof(ring)));
	assert(serial == UINT64_C(18446744073709551615));
}

static struct json_object *world_fixture(const char *directory,
					 const char *name)
{
	char path[4096];
	int written = snprintf(path, sizeof(path), "%s/%s", directory, name);

	assert(written > 0 && (size_t)written < sizeof(path));
	struct json_object *object = json_object_from_file(path);
	assert(object);
	return object;
}

static void assert_world_entries(const struct clock_model *model)
{
	assert(model->world_count == 3);
	assert(model->world[0].id == 1 &&
	       model->world[0].offset_minutes == -210);
	assert(model->world[1].id == 3 &&
	       model->world[1].offset_minutes == 345);
	assert(model->world[2].id == 4 && model->world[2].offset_minutes == 0);
	assert(model->next_world_id == 5);
}

static void
signed_world_values_match_literal_snapshot_and_preferences(const char *fixtures)
{
	struct clock_model model, decoded, before;
	struct clock_request request = {
		.op = CLOCK_REQUEST_WORLD_ADD,
		.offset_minutes = -240,
	};
	struct json_object *expected, *actual;
	const char *json;
	char encoded[CLOCK_WIRE_CAPACITY];
	struct clock_result result = {
		.status = CLOCK_OK,
		.applied = true,
		.storage.state = FPLINUX_STORAGE_DURABLE,
	};
	uint64_t serial = 0;

	assert(clock_model_init(&model, 1704067200, 1000) == CLOCK_OK);
	assert(clock_wire_request_apply(&model, &request) == CLOCK_OK);
	request.offset_minutes = 330;
	assert(clock_wire_request_apply(&model, &request) == CLOCK_OK);
	request.offset_minutes = 345;
	assert(clock_wire_request_apply(&model, &request) == CLOCK_OK);
	request.offset_minutes = 0;
	assert(clock_wire_request_apply(&model, &request) == CLOCK_OK);
	request.op = CLOCK_REQUEST_WORLD_DELETE;
	request.id = 2;
	assert(clock_wire_request_apply(&model, &request) == CLOCK_OK);
	request.op = CLOCK_REQUEST_WORLD_EDIT;
	request.id = 1;
	request.offset_minutes = -210;
	assert(clock_wire_request_apply(&model, &request) == CLOCK_OK);
	assert_world_entries(&model);
	before = model;
	assert(clock_wire_request_apply(&model, &request) == CLOCK_OK);
	assert(!memcmp(&model, &before, sizeof(model)));
	request.offset_minutes = 345;
	assert(clock_wire_request_apply(&model, &request) == CLOCK_INVALID);
	assert(!memcmp(&model, &before, sizeof(model)));
	request.id = 99;
	request.offset_minutes = 330;
	assert(clock_wire_request_apply(&model, &request) == CLOCK_NOT_FOUND);
	assert(!memcmp(&model, &before, sizeof(model)));
	request.op = CLOCK_REQUEST_WORLD_ADD;
	request.offset_minutes = -210;
	assert(clock_wire_request_apply(&model, &request) == CLOCK_INVALID);
	assert(!memcmp(&model, &before, sizeof(model)));

	expected = world_fixture(fixtures, "snapshot.json");
	assert(clock_wire_snapshot_encode(&model, &result, 9, encoded,
					  sizeof(encoded)) == CLOCK_OK);
	actual = json_tokener_parse(encoded);
	assert(actual && json_object_equal(actual, expected));
	json_object_put(actual);
	json = json_object_to_json_string_ext(expected, JSON_C_TO_STRING_PLAIN);
	assert(clock_wire_snapshot_decode(json, strlen(json), &decoded, &result,
					  &serial) == CLOCK_OK);
	assert(result.status == CLOCK_OK && result.applied &&
	       result.storage.state == FPLINUX_STORAGE_DURABLE &&
	       result.storage.error == 0 && serial == 9);
	assert_world_entries(&decoded);
	json_object_put(expected);

	expected = world_fixture(fixtures, "preferences.json");
	assert(clock_wire_preferences_encode(&model, encoded,
					     sizeof(encoded)) == CLOCK_OK);
	actual = json_tokener_parse(encoded);
	assert(actual && json_object_equal(actual, expected));
	json_object_put(actual);
	assert(clock_model_init(&decoded, 1704153600, 2000) == CLOCK_OK);
	json = json_object_to_json_string_ext(expected, JSON_C_TO_STRING_PLAIN);
	assert(clock_wire_preferences_decode(json, strlen(json), &decoded) ==
	       CLOCK_OK);
	assert(decoded.wall_utc == 1704153600 && decoded.monotonic_ms == 2000);
	assert_world_entries(&decoded);
	json_object_put(expected);
}

static void assert_malformed_world_documents(struct json_object *snapshot,
					     struct json_object *preferences)
{
	struct clock_model model, before;
	struct clock_result result = {
		.status = CLOCK_FULL,
		.storage.state = FPLINUX_STORAGE_DURABLE,
	};
	uint64_t serial = 19;
	const char *json;

	assert(clock_model_init(&model, 1704153600, 2000) == CLOCK_OK);
	assert(clock_model_world_add(&model, 330, NULL) == CLOCK_OK);
	before = model;
	json = json_object_to_json_string_ext(snapshot, JSON_C_TO_STRING_PLAIN);
	assert(clock_wire_snapshot_decode(json, strlen(json), &model, &result,
					  &serial) == CLOCK_INVALID);
	assert(!memcmp(&model, &before, sizeof(model)));
	assert(result.status == CLOCK_FULL && !result.applied &&
	       result.storage.state == FPLINUX_STORAGE_DURABLE &&
	       result.storage.error == 0 && serial == 19);
	json = json_object_to_json_string_ext(preferences,
					      JSON_C_TO_STRING_PLAIN);
	assert(clock_wire_preferences_decode(json, strlen(json), &model) ==
	       CLOCK_INVALID);
	assert(!memcmp(&model, &before, sizeof(model)));
}

static void
world_documents_reject_wrong_offsets_and_duplicate_entries(const char *fixtures)
{
	static const char *const invalid[] = {
		"-735", "855", "-1", "346", "345.0", "\"345\"", "true", "[]",
	};
	struct json_object *snapshot = world_fixture(fixtures, "snapshot.json");
	struct json_object *preferences =
		world_fixture(fixtures, "preferences.json");
	struct json_object *model, *snapshot_world, *preferences_world;
	struct json_object *snapshot_entry, *preferences_entry;

	assert(json_object_object_get_ex(snapshot, "model", &model));
	assert(json_object_object_get_ex(model, "world", &snapshot_world));
	assert(json_object_object_get_ex(preferences, "world",
					 &preferences_world));
	snapshot_entry = json_object_array_get_idx(snapshot_world, 0);
	preferences_entry = json_object_array_get_idx(preferences_world, 0);
	for (size_t index = 0; index < sizeof(invalid) / sizeof(invalid[0]);
	     ++index) {
		json_object_object_add(snapshot_entry, "offset_minutes",
				       json_tokener_parse(invalid[index]));
		json_object_object_add(preferences_entry, "offset_minutes",
				       json_tokener_parse(invalid[index]));
		assert_malformed_world_documents(snapshot, preferences);
	}
	json_object_object_add(snapshot_entry, "offset_minutes",
			       json_object_new_int(345));
	json_object_object_add(preferences_entry, "offset_minutes",
			       json_object_new_int(345));
	assert_malformed_world_documents(snapshot, preferences);
	json_object_object_add(snapshot_entry, "offset_minutes",
			       json_object_new_int(-210));
	json_object_object_add(preferences_entry, "offset_minutes",
			       json_object_new_int(-210));
	json_object_object_add(snapshot_entry, "id", json_object_new_int(3));
	json_object_object_add(preferences_entry, "id", json_object_new_int(3));
	assert_malformed_world_documents(snapshot, preferences);
	json_object_put(snapshot);
	json_object_put(preferences);
}

int main(int argc, char **argv)
{
	assert(argc == 4);
	literal_requests_drive_the_session_owner(argv[1]);
	malformed_requests_leave_the_output_unchanged();
	explicit_schedules_and_selections_encode_literal_fields();
	disabled_once_saves_retain_the_date_without_enabling();
	preferences_restore_configuration_without_replaying_runtime();
	snapshot_retains_user_values_and_checks_encoder_independently(argv[2]);
	literal_ring_preserves_large_identity_and_rejects_partial_output();
	signed_world_values_match_literal_snapshot_and_preferences(argv[3]);
	world_documents_reject_wrong_offsets_and_duplicate_entries(argv[3]);
	puts("clock-wire: strict messages and owner state passed");
	return 0;
}
