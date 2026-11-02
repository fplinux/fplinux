/* SPDX-License-Identifier: GPL-2.0-only */
#include "session-clock.h"
#include "fplinux-ui-prefs.h"

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <json-c/json.h>

static bool fail_file_sync;
static bool fail_directory_sync;
static unsigned int file_sync_count;

int __real_fsync(int fd);
int __wrap_fsync(int fd);

int __wrap_fsync(int fd)
{
	struct stat status;

	assert(fstat(fd, &status) == 0);
	if (S_ISREG(status.st_mode)) {
		++file_sync_count;
		if (fail_file_sync) {
			fail_file_sync = false;
			errno = EIO;
			return -1;
		}
	}
	if (S_ISDIR(status.st_mode) && fail_directory_sync) {
		errno = EIO;
		return -1;
	}
	return __real_fsync(fd);
}

static void
configuration_survives_restart_without_transient_activity(const char *directory)
{
	struct fplinux_session_clock owner, restored;
	struct clock_request request = {
		.op = CLOCK_REQUEST_ALARM_ADD,
		.alarm = {
			.schedule = CLOCK_ALARM_ONCE,
			.once_utc = 1704067260,
			.enabled = true,
			.label = "Wake",
		},
	};
	struct clock_model before;
	struct stat status;
	char path[4096];
	uint64_t event_id;
	unsigned int saved_count;

	assert(fplinux_session_clock_init(&owner, 1704067200, 0, directory)
		       .status == CLOCK_OK);
	assert(owner.model.last_alarm_hour == 7 &&
	       owner.model.last_alarm_minute == 0);
	assert(fplinux_session_clock_apply(&owner, &request, 1704067200, 0)
		       .status == CLOCK_OK);
	assert(owner.model.alarms[0].id == 1 && owner.model.next_alarm_id == 2);
	assert(file_sync_count > 0);
	assert(snprintf(path, sizeof(path), "%s/preferences", directory) > 0);
	assert(stat(directory, &status) == 0 &&
	       (status.st_mode & 0777) == 0700);
	assert(stat(path, &status) == 0 && (status.st_mode & 0777) == 0600);

	request.alarm.once_utc = 1704153660;
	before = owner.model;
	fail_file_sync = true;
	assert(fplinux_session_clock_apply(&owner, &request, 1704067200, 0)
		       .status == CLOCK_STORAGE_ERROR);
	assert(!memcmp(&before, &owner.model, sizeof(before)));
	assert(fplinux_session_clock_init(&restored, 1704067200, 0, directory)
		       .status == CLOCK_OK);
	assert(restored.model.alarm_count == 1 &&
	       restored.model.next_alarm_id == 2);
	fplinux_session_clock_cleanup(&restored);
	assert(fplinux_session_clock_apply(&owner, &request, 1704067200, 0)
		       .status == CLOCK_OK);
	assert(owner.model.alarm_count == 2 && owner.model.alarms[1].id == 2);

	request = (struct clock_request){ .op = CLOCK_REQUEST_WORLD_ADD,
					  .offset_minutes = 345 };
	assert(fplinux_session_clock_apply(&owner, &request, 1704067200, 0)
		       .status == CLOCK_OK);
	request = (struct clock_request){ .op = CLOCK_REQUEST_SET_VIEW,
					  .view = CLOCK_VIEW_ANALOG };
	assert(fplinux_session_clock_apply(&owner, &request, 1704067200, 0)
		       .status == CLOCK_OK);
	request = (struct clock_request){ .op = CLOCK_REQUEST_SET_SNOOZE,
					  .minutes = 12 };
	assert(fplinux_session_clock_apply(&owner, &request, 1704067200, 0)
		       .status == CLOCK_OK);
	request = (struct clock_request){ .op = CLOCK_REQUEST_TIMER_SET,
					  .seconds = 90 };
	assert(fplinux_session_clock_apply(&owner, &request, 1704067200, 0)
		       .status == CLOCK_OK);
	saved_count = file_sync_count;
	request.op = CLOCK_REQUEST_TIMER_START;
	assert(fplinux_session_clock_apply(&owner, &request, 1704067200, 0)
		       .status == CLOCK_OK);
	request.op = CLOCK_REQUEST_STOPWATCH_START;
	assert(fplinux_session_clock_apply(&owner, &request, 1704067200, 0)
		       .status == CLOCK_OK);
	assert(file_sync_count == saved_count);
	assert(fplinux_session_clock_tick(&owner, 1704067259, 1000).status ==
	       CLOCK_OK);
	fail_file_sync = true;
	assert(fplinux_session_clock_tick(&owner, 1704067260, 2000).status ==
	       CLOCK_OK);
	assert(owner.model.wall_utc == 1704067260 &&
	       owner.model.monotonic_ms == 2000);
	event_id = owner.model.active.id;
	assert(event_id == 1 && !owner.model.alarms[0].spec.enabled);
	assert(owner.model.alarms[0].last_due_utc == 1704067260);
	assert(owner.storage.state == FPLINUX_STORAGE_UNPUBLISHED &&
	       owner.storage.error == EIO);
	assert(fplinux_session_clock_deadline(&owner, 2000) == 3000);
	saved_count = file_sync_count;
	assert(fplinux_session_clock_tick(&owner, 1704067260, 2000).status ==
	       CLOCK_OK);
	assert(fplinux_session_clock_tick(&owner, 1704067260, 2001).status ==
	       CLOCK_OK);
	assert(owner.model.active.id == event_id && !owner.model.queue_count);
	assert(file_sync_count == saved_count);
	assert(fplinux_session_clock_tick(&owner, 1704067261, 3000).status ==
	       CLOCK_OK);
	assert(owner.storage.state == FPLINUX_STORAGE_DURABLE &&
	       owner.storage.error == 0);
	assert(owner.model.active.id == event_id && !owner.model.queue_count);
	assert(fplinux_session_clock_init(&restored, 1704067261, 0, directory)
		       .status == CLOCK_OK);
	assert(restored.model.alarm_count == 2 &&
	       restored.model.alarms[1].id == 2);
	assert(!restored.model.alarms[0].spec.enabled);
	assert(restored.model.alarms[0].spec.once_utc == 1704067260);
	assert(restored.model.alarms[0].last_due_utc == 1704067260);
	assert(restored.model.alarms[1].next_due_utc == 1704153660);
	assert(restored.model.world_count == 1 &&
	       restored.model.world[0].offset_minutes == 345);
	assert(restored.model.view == CLOCK_VIEW_ANALOG &&
	       restored.model.snooze_minutes == 12);
	assert(restored.model.timer.duration_seconds == 90);
	assert(restored.model.timer.state == CLOCK_TIMER_STOPPED);
	assert(clock_model_timer_remaining(&restored.model) == 90000);
	assert(!restored.model.stopwatch.running &&
	       !restored.model.stopwatch.lap_count);
	assert(!restored.model.active.id && !restored.model.queue_count);
	assert(!restored.model.alarms[0].snoozed);
	request = (struct clock_request){ .op = CLOCK_REQUEST_TIMER_START };
	assert(fplinux_session_clock_apply(&restored, &request, 1704067261, 0)
		       .status == CLOCK_OK);
	assert(restored.model.timer.state == CLOCK_TIMER_RUNNING);
	assert(restored.model.timer.deadline_ms == 90000);
	assert(fplinux_session_clock_tick(&restored, 1704067262, 1000).status ==
	       CLOCK_OK);
	assert(clock_model_timer_remaining(&restored.model) == 89000);
	assert(!restored.model.active.id);
	assert(fplinux_session_clock_tick(&restored, 1704067200, 1000).status ==
	       CLOCK_OK);
	assert(fplinux_session_clock_tick(&restored, 1704067260, 2000).status ==
	       CLOCK_OK);
	assert(!restored.model.active.id);
	fplinux_session_clock_cleanup(&restored);
	fplinux_session_clock_cleanup(&owner);
}

static void signed_offsets_and_deleted_ids_survive_a_real_preferences_restart(
	const char *directory, const char *fixtures)
{
	struct fplinux_session_clock owner, restored;
	struct fplinux_ui_prefs *prefs;
	struct clock_request request = {
		.op = CLOCK_REQUEST_WORLD_ADD,
		.offset_minutes = -240,
	};
	struct clock_model before;
	struct json_object *expected, *actual;
	const char *configuration;
	char world_directory[4096], fixture_path[4096];
	int written;

	written = snprintf(world_directory, sizeof(world_directory), "%s/world",
			   directory);
	assert(written > 0 && (size_t)written < sizeof(world_directory));
	written = snprintf(fixture_path, sizeof(fixture_path),
			   "%s/preferences.json", fixtures);
	assert(written > 0 && (size_t)written < sizeof(fixture_path));
	assert(fplinux_session_clock_init(&owner, 1704067200, 1000,
					  world_directory)
		       .status == CLOCK_OK);
	assert(fplinux_session_clock_apply(&owner, &request, 1704067200, 1000)
		       .status == CLOCK_OK);
	request.offset_minutes = 330;
	assert(fplinux_session_clock_apply(&owner, &request, 1704067200, 1000)
		       .status == CLOCK_OK);
	request.offset_minutes = 345;
	assert(fplinux_session_clock_apply(&owner, &request, 1704067200, 1000)
		       .status == CLOCK_OK);
	request.offset_minutes = 0;
	assert(fplinux_session_clock_apply(&owner, &request, 1704067200, 1000)
		       .status == CLOCK_OK);
	request.op = CLOCK_REQUEST_WORLD_DELETE;
	request.id = 2;
	assert(fplinux_session_clock_apply(&owner, &request, 1704067200, 1000)
		       .status == CLOCK_OK);
	request.op = CLOCK_REQUEST_WORLD_EDIT;
	request.id = 1;
	request.offset_minutes = -210;
	assert(fplinux_session_clock_apply(&owner, &request, 1704067200, 1000)
		       .status == CLOCK_OK);
	before = owner.model;
	request.offset_minutes = 345;
	assert(fplinux_session_clock_apply(&owner, &request, 1704067200, 1000)
		       .status == CLOCK_INVALID);
	assert(!memcmp(&owner.model, &before, sizeof(before)));

	expected = json_object_from_file(fixture_path);
	assert(expected);
	prefs = fplinux_ui_prefs_open_directory(world_directory);
	assert(prefs);
	configuration = fplinux_ui_prefs_get(prefs, "configuration", NULL);
	assert(configuration);
	actual = json_tokener_parse(configuration);
	assert(actual && json_object_equal(actual, expected));
	json_object_put(actual);
	json_object_put(expected);
	fplinux_ui_prefs_close(prefs);

	assert(fplinux_session_clock_init(&restored, 1704153600, 0,
					  world_directory)
		       .status == CLOCK_OK);
	assert(restored.model.world_count == 3);
	assert(restored.model.world[0].id == 1 &&
	       restored.model.world[0].offset_minutes == -210);
	assert(restored.model.world[1].id == 3 &&
	       restored.model.world[1].offset_minutes == 345);
	assert(restored.model.world[2].id == 4 &&
	       restored.model.world[2].offset_minutes == 0);
	assert(restored.model.next_world_id == 5);
	request.op = CLOCK_REQUEST_WORLD_ADD;
	request.offset_minutes = 330;
	assert(fplinux_session_clock_apply(&restored, &request, 1704153600, 0)
		       .status == CLOCK_OK);
	assert(restored.model.world[3].id == 5 &&
	       restored.model.world[3].offset_minutes == 330);
	fplinux_session_clock_cleanup(&restored);
	fplinux_session_clock_cleanup(&owner);
}

static void
pending_checkpoint_keeps_time_and_ring_controls_available(const char *directory)
{
	struct fplinux_session_clock owner, restored;
	struct clock_request request = {
		.op = CLOCK_REQUEST_ALARM_ADD,
		.alarm = {
			.schedule = CLOCK_ALARM_WEEKLY,
			.minute = 1,
			.repeat_days = 127,
			.enabled = true,
		},
	};
	struct clock_result result;
	char path[4096];
	unsigned int saved_count;
	uint64_t event_id;

	assert(snprintf(path, sizeof(path), "%s/controls", directory) > 0);
	assert(fplinux_session_clock_init(&owner, 1704067200, 0, path).applied);
	assert(fplinux_session_clock_apply(&owner, &request, 1704067200, 0)
		       .status == CLOCK_OK);
	request = (struct clock_request){ .op = CLOCK_REQUEST_TIMER_SET,
					  .seconds = 120 };
	assert(fplinux_session_clock_apply(&owner, &request, 1704067200, 0)
		       .status == CLOCK_OK);
	request.op = CLOCK_REQUEST_TIMER_START;
	assert(fplinux_session_clock_apply(&owner, &request, 1704067200, 0)
		       .status == CLOCK_OK);
	request.op = CLOCK_REQUEST_STOPWATCH_START;
	assert(fplinux_session_clock_apply(&owner, &request, 1704067200, 0)
		       .status == CLOCK_OK);
	fail_file_sync = true;
	result = fplinux_session_clock_tick(&owner, 1704067260, 2000);
	assert(result.status == CLOCK_OK && result.applied);
	assert(result.storage.state == FPLINUX_STORAGE_UNPUBLISHED &&
	       result.storage.error == EIO);
	event_id = owner.model.active.id;
	assert(event_id == 1 && owner.model.active.kind == CLOCK_EVENT_ALARM);
	saved_count = file_sync_count;
	request = (struct clock_request){ .op = CLOCK_REQUEST_SNAPSHOT };
	result =
		fplinux_session_clock_apply(&owner, &request, 1704067260, 2001);
	assert(result.status == CLOCK_OK && result.applied);
	assert(result.storage.state == FPLINUX_STORAGE_UNPUBLISHED);
	request.op = CLOCK_REQUEST_TIMER_STOP;
	assert(fplinux_session_clock_apply(&owner, &request, 1704067260, 2002)
		       .status == CLOCK_OK);
	assert(owner.model.timer.state == CLOCK_TIMER_STOPPED);
	request = (struct clock_request){ .op = CLOCK_REQUEST_SNOOZE,
					  .event_id = event_id,
					  .minutes = 1 };
	assert(fplinux_session_clock_apply(&owner, &request, 1704067260, 2003)
		       .status == CLOCK_OK);
	assert(owner.model.active.kind == CLOCK_EVENT_NONE);
	assert(owner.model.alarms[0].snoozed &&
	       owner.model.alarms[0].snooze_deadline_ms == 62003);
	assert(clock_model_stopwatch_elapsed(&owner.model) == 2003);
	assert(file_sync_count == saved_count);
	fail_file_sync = true;
	result = fplinux_session_clock_tick(&owner, 1704067320, 62003);
	assert(result.status == CLOCK_OK && result.applied);
	assert(result.storage.state == FPLINUX_STORAGE_UNPUBLISHED);
	assert(owner.model.active.kind == CLOCK_EVENT_ALARM &&
	       owner.model.active.id == 2 && !owner.model.queue_count);
	request = (struct clock_request){ .op = CLOCK_REQUEST_DISMISS,
					  .event_id = 2 };
	result = fplinux_session_clock_apply(&owner, &request, 1704067320,
					     62004);
	assert(result.status == CLOCK_OK && result.applied);
	assert(owner.model.active.kind == CLOCK_EVENT_NONE);
	assert(clock_model_stopwatch_elapsed(&owner.model) == 62004);
	assert(owner.retry_at_ms == 64003);
	assert(fplinux_session_clock_tick(&owner, 1704067322, 64003).status ==
	       CLOCK_OK);
	assert(owner.storage.state == FPLINUX_STORAGE_DURABLE);
	assert(owner.model.active.kind == CLOCK_EVENT_NONE &&
	       !owner.model.queue_count);
	assert(fplinux_session_clock_init(&restored, 1704067322, 0, path)
		       .applied);
	assert(restored.model.alarms[0].last_due_utc == 1704067260);
	assert(!restored.model.alarms[0].snoozed && !restored.model.active.id);
	fplinux_session_clock_cleanup(&restored);
	fplinux_session_clock_cleanup(&owner);
}

static void published_add_retains_identity_and_requires_directory_sync(
	const char *directory)
{
	struct fplinux_session_clock owner, restored;
	struct fplinux_ui_prefs *visible;
	struct clock_model decoded;
	struct clock_request request = {
		.op = CLOCK_REQUEST_ALARM_ADD,
		.alarm = {
			.schedule = CLOCK_ALARM_ONCE,
			.once_utc = 1704067260,
			.enabled = true,
		},
	};
	struct clock_result result;
	const char *configuration;
	char path[4096];
	unsigned int saved_count;

	assert(snprintf(path, sizeof(path), "%s/published", directory) > 0);
	assert(fplinux_session_clock_init(&owner, 1704067200, 0, path).applied);
	fail_directory_sync = true;
	result = fplinux_session_clock_apply(&owner, &request, 1704067200, 0);
	assert(result.status == CLOCK_STORAGE_ERROR && result.applied);
	assert(result.storage.state == FPLINUX_STORAGE_PUBLISHED_UNSYNCED &&
	       result.storage.error == EIO);
	assert(owner.model.alarm_count == 1 && owner.model.alarms[0].id == 1 &&
	       owner.model.next_alarm_id == 2);
	visible = fplinux_ui_prefs_open_directory(path);
	assert(visible);
	configuration = fplinux_ui_prefs_get(visible, "configuration", NULL);
	assert(configuration);
	assert(clock_model_init(&decoded, 1704067200, 0) == CLOCK_OK);
	assert(clock_wire_preferences_decode(configuration,
					     strlen(configuration),
					     &decoded) == CLOCK_OK);
	assert(decoded.alarm_count == 1 && decoded.alarms[0].id == 1 &&
	       decoded.next_alarm_id == 2);
	fplinux_ui_prefs_close(visible);
	saved_count = file_sync_count;
	result = fplinux_session_clock_retry(&owner, 1);
	assert(result.status == CLOCK_STORAGE_ERROR && result.applied);
	assert(result.storage.state == FPLINUX_STORAGE_PUBLISHED_UNSYNCED &&
	       result.storage.error == EIO);
	assert(file_sync_count == saved_count);
	request = (struct clock_request){ .op = CLOCK_REQUEST_SET_VIEW,
					  .view = CLOCK_VIEW_DIGITAL };
	result = fplinux_session_clock_apply(&owner, &request, 1704067200, 2);
	assert(result.status == CLOCK_STORAGE_ERROR && result.applied);
	assert(result.storage.state == FPLINUX_STORAGE_PUBLISHED_UNSYNCED);
	assert(file_sync_count == saved_count);
	fail_directory_sync = false;
	result = fplinux_session_clock_retry(&owner, 3);
	assert(result.status == CLOCK_OK && result.applied);
	assert(result.storage.state == FPLINUX_STORAGE_DURABLE &&
	       result.storage.error == 0);
	assert(file_sync_count == saved_count && !owner.retry_at_ms);
	assert(owner.model.alarm_count == 1 && owner.model.next_alarm_id == 2);
	assert(fplinux_session_clock_init(&restored, 1704067200, 0, path)
		       .applied);
	assert(restored.model.alarm_count == 1 &&
	       restored.model.alarms[0].id == 1 &&
	       restored.model.next_alarm_id == 2);
	fplinux_session_clock_cleanup(&restored);
	fplinux_session_clock_cleanup(&owner);
}

static void
normalization_failure_keeps_a_valid_owner_ready(const char *directory)
{
	struct fplinux_session_clock owner, restored;
	struct clock_request request = {
		.op = CLOCK_REQUEST_ALARM_ADD,
		.alarm = {
			.schedule = CLOCK_ALARM_ONCE,
			.once_utc = 1704067260,
			.enabled = true,
		},
	};
	struct clock_result result;
	char path[4096];

	assert(snprintf(path, sizeof(path), "%s/normalization", directory) > 0);
	assert(fplinux_session_clock_init(&owner, 1704067200, 0, path).applied);
	assert(fplinux_session_clock_apply(&owner, &request, 1704067200, 0)
		       .status == CLOCK_OK);
	fplinux_session_clock_cleanup(&owner);
	fail_file_sync = true;
	result = fplinux_session_clock_init(&owner, 1704067261, 0, path);
	assert(result.status == CLOCK_OK && result.applied);
	assert(result.storage.state == FPLINUX_STORAGE_UNPUBLISHED &&
	       result.storage.error == EIO);
	assert(owner.model.alarm_count == 1 &&
	       !owner.model.alarms[0].spec.enabled && !owner.model.active.id);
	request = (struct clock_request){ .op = CLOCK_REQUEST_SNAPSHOT };
	assert(fplinux_session_clock_apply(&owner, &request, 1704067261, 1)
		       .status == CLOCK_OK);
	assert(owner.storage.state == FPLINUX_STORAGE_UNPUBLISHED);
	assert(fplinux_session_clock_retry(&owner, 2).status == CLOCK_OK);
	assert(fplinux_session_clock_init(&restored, 1704067262, 0, path)
		       .applied);
	assert(!restored.model.alarms[0].spec.enabled &&
	       !restored.model.active.id);
	fplinux_session_clock_cleanup(&restored);
	fplinux_session_clock_cleanup(&owner);
}

int main(int argc, char **argv)
{
	assert(argc == 3);
	configuration_survives_restart_without_transient_activity(argv[1]);
	signed_offsets_and_deleted_ids_survive_a_real_preferences_restart(
		argv[1], argv[2]);
	pending_checkpoint_keeps_time_and_ring_controls_available(argv[1]);
	published_add_retains_identity_and_requires_directory_sync(argv[1]);
	normalization_failure_keeps_a_valid_owner_ready(argv[1]);
	puts("clock-storage: durable configuration and transactional failures passed");
	return 0;
}
