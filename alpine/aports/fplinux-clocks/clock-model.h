/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_CLOCK_MODEL_H
#define FPLINUX_CLOCK_MODEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CLOCK_ALARM_LIMIT 8
#define CLOCK_WORLD_LIMIT 8
#define CLOCK_WORLD_MIN_OFFSET_MINUTES (-720)
#define CLOCK_WORLD_MAX_OFFSET_MINUTES 840
#define CLOCK_WORLD_STEP_MINUTES 15
#define CLOCK_QUEUE_LIMIT 8
#define CLOCK_MISSED_LIMIT 8
#define CLOCK_LAP_LIMIT 20
#define CLOCK_TEXT_BYTES 128
#define CLOCK_TEXT_CHARACTERS 32
#define CLOCK_TIMER_MAX_SECONDS 359999
#define CLOCK_RING_TIMEOUT_MS UINT64_C(60000)
#define CLOCK_ALARM_GRACE_SECONDS INT64_C(60)
#define CLOCK_WALL_MIN INT64_C(315532800) /* 1980-01-01 00:00:00 UTC */
#define CLOCK_WALL_MAX INT64_C(4102444799) /* 2099-12-31 23:59:59 UTC */

enum clock_status {
	CLOCK_OK,
	CLOCK_INVALID,
	CLOCK_NOT_FOUND,
	CLOCK_FULL,
	CLOCK_ACTIVE_EVENT,
	CLOCK_STALE_EVENT,
	CLOCK_NO_EVENT,
	CLOCK_WRONG_STATE,
	CLOCK_TIME_BACKWARD,
	CLOCK_EXHAUSTED,
	CLOCK_STORAGE_ERROR,
};

enum clock_alarm_schedule {
	CLOCK_ALARM_ONCE,
	CLOCK_ALARM_WEEKLY,
};

enum clock_view {
	CLOCK_VIEW_DIGITAL,
	CLOCK_VIEW_ANALOG,
};

enum clock_event_kind {
	CLOCK_EVENT_NONE,
	CLOCK_EVENT_ALARM,
	CLOCK_EVENT_TIMER,
};

enum clock_missed_reason {
	CLOCK_MISSED_NONE,
	CLOCK_MISSED_LATE,
	CLOCK_MISSED_QUEUE_FULL,
	CLOCK_MISSED_EXPIRED,
};

enum clock_timer_state {
	CLOCK_TIMER_STOPPED,
	CLOCK_TIMER_RUNNING,
	CLOCK_TIMER_PAUSED,
	CLOCK_TIMER_FINISHED,
};

struct clock_alarm_spec {
	char label[CLOCK_TEXT_BYTES + 1];
	enum clock_alarm_schedule schedule;
	/* ONCE uses an absolute UTC minute; its weekly fields are zero. */
	int64_t once_utc;
	unsigned int hour;
	unsigned int minute;
	/* WEEKLY uses Monday bit 0 through Sunday bit 6, and once_utc is zero. */
	unsigned int repeat_days;
	bool enabled;
};

struct clock_alarm {
	uint32_t id;
	uint32_t generation;
	struct clock_alarm_spec spec;
	/* Zero while disabled or with no future occurrence in the supported range. */
	int64_t next_due_utc;
	/* Zero until consumed, otherwise the last scheduled occurrence (not snooze). */
	int64_t last_due_utc;
	bool snoozed;
	uint64_t snooze_deadline_ms;
};

struct clock_event {
	uint64_t id;
	enum clock_event_kind kind;
	/* Alarm ID, or zero for the single timer. IDs never denote row indices. */
	uint32_t source_id;
	uint32_t generation;
	char label[CLOCK_TEXT_BYTES + 1];
	int64_t due_wall_utc;
	uint64_t due_monotonic_ms;
	uint64_t ring_started_ms;
	bool snoozed;
	enum clock_missed_reason missed_reason;
};

struct clock_timer {
	enum clock_timer_state state;
	uint32_t generation;
	unsigned int duration_seconds;
	uint64_t remaining_ms;
	uint64_t deadline_ms;
};

struct clock_lap {
	uint64_t elapsed_ms;
	uint64_t difference_ms;
};

struct clock_stopwatch {
	bool running;
	uint64_t elapsed_ms;
	struct clock_lap laps[CLOCK_LAP_LIMIT];
	size_t lap_count;
};

struct clock_world_entry {
	uint32_t id;
	int offset_minutes;
};

struct clock_local_time {
	int year;
	unsigned int month;
	unsigned int day;
	unsigned int weekday; /* Monday = 0, Sunday = 6. */
	unsigned int hour;
	unsigned int minute;
	unsigned int second;
};

/* One session owner retains this model when the app is hidden or closed.
 * Fields are readable snapshots; consumers mutate state only through this API.
 * All time is injected. The model performs no clock, filesystem, IPC or audio IO.
 */
struct clock_model {
	int64_t wall_utc;
	uint64_t monotonic_ms;
	enum clock_view view;
	unsigned int snooze_minutes;
	unsigned int last_alarm_hour;
	unsigned int last_alarm_minute;
	struct clock_alarm alarms[CLOCK_ALARM_LIMIT];
	size_t alarm_count;
	struct clock_event active;
	struct clock_event queue[CLOCK_QUEUE_LIMIT];
	size_t queue_count;
	struct clock_event missed[CLOCK_MISSED_LIMIT];
	size_t missed_count;
	uint64_t missed_total;
	struct clock_timer timer;
	struct clock_stopwatch stopwatch;
	struct clock_world_entry world[CLOCK_WORLD_LIMIT];
	size_t world_count;
	uint32_t next_alarm_id;
	uint32_t next_world_id;
	uint64_t next_event_id;
};

/* Empty normal state, digital view and five-minute snooze. */
enum clock_status clock_model_init(struct clock_model *model, int64_t wall_utc,
				   uint64_t monotonic_ms);
/* Monotonic regression rejects the whole tick. Wall time may move backwards.
 * An occurrence <= 60 seconds late rings. Larger jumps record the oldest missed
 * occurrence per alarm, then skip to its next future occurrence (no flood).
 * Existing occurrences never replay after a backwards wall adjustment.
 * Expiry, countdowns and snoozes use monotonic time. At most one event rings;
 * additional events queue FIFO. A full queue records missed events. Missed
 * history retains its newest eight entries and missed_total counts all misses.
 * Within a tick: expire/promote the current ring, scheduled alarms by due UTC
 * then ID, snoozes by deadline then ID, and the timer. Saving/enabling an alarm
 * chooses a strictly future occurrence after current wall or its last consumed
 * occurrence, whichever is later. Weekly schedules use the system UTC policy.
 */
enum clock_status clock_model_tick(struct clock_model *model, int64_t wall_utc,
				   uint64_t monotonic_ms);
enum clock_status clock_model_set_view(struct clock_model *model,
				       enum clock_view view);
enum clock_status clock_model_set_snooze(struct clock_model *model,
					 unsigned int minutes);

enum clock_status clock_model_alarm_add(struct clock_model *model,
					const struct clock_alarm_spec *spec,
					uint32_t *id);
/* Editing/deleting/disabling a ringing alarm requires cancel_active=true;
 * otherwise CLOCK_ACTIVE_EVENT has no effect. Accepted schedule changes cancel
 * that alarm's queued/snoozed events. Its stable ID survives editing/reordering.
 */
enum clock_status clock_model_alarm_edit(struct clock_model *model, uint32_t id,
					 const struct clock_alarm_spec *spec,
					 bool cancel_active);
enum clock_status clock_model_alarm_enable(struct clock_model *model,
					   uint32_t id, bool enabled,
					   bool cancel_active);
enum clock_status clock_model_alarm_delete(struct clock_model *model,
					   uint32_t id, bool cancel_active);
/* Cancel only the pending snooze; retain the alarm's regular schedule. */
enum clock_status clock_model_alarm_cancel_snooze(struct clock_model *model,
						  uint32_t id);
const struct clock_alarm *
clock_model_alarm_find(const struct clock_model *model, uint32_t id);

/* Event IDs reject late actions from an older ringing overlay. Snooze only
 * accepts alarm events; minutes=0 chooses the configured default (1..30).
 * Both actions immediately promote the oldest queued event.
 */
enum clock_status clock_model_dismiss(struct clock_model *model,
				      uint64_t event_id);
enum clock_status clock_model_snooze(struct clock_model *model,
				     uint64_t event_id, unsigned int minutes);
void clock_model_clear_missed(struct clock_model *model);

/* Setting a duration or stopping cancels this timer's active/queued event.
 * Zero is a valid stored duration but cannot start. Restart uses that duration.
 */
enum clock_status clock_model_timer_set(struct clock_model *model,
					unsigned int seconds);
enum clock_status clock_model_timer_start(struct clock_model *model);
enum clock_status clock_model_timer_pause(struct clock_model *model);
enum clock_status clock_model_timer_resume(struct clock_model *model);
enum clock_status clock_model_timer_stop(struct clock_model *model);
enum clock_status clock_model_timer_restart(struct clock_model *model);
uint64_t clock_model_timer_remaining(const struct clock_model *model);

enum clock_status clock_model_stopwatch_start(struct clock_model *model);
enum clock_status clock_model_stopwatch_pause(struct clock_model *model);
enum clock_status clock_model_stopwatch_reset(struct clock_model *model);
enum clock_status clock_model_stopwatch_lap(struct clock_model *model);
uint64_t clock_model_stopwatch_elapsed(const struct clock_model *model);

bool clock_model_world_offset_valid(int offset_minutes);
enum clock_status clock_model_world_add(struct clock_model *model,
					int offset_minutes, uint32_t *id);
/* Editing retains the entry's ID and position. Duplicate offsets are invalid. */
enum clock_status clock_model_world_edit(struct clock_model *model, uint32_t id,
					 int offset_minutes);
enum clock_status clock_model_world_delete(struct clock_model *model,
					   uint32_t id);
/* Printable, valid UTF-8, <=32 codepoints and <=128 bytes. Embedded NUL and
 * control characters are rejected. Empty alarm labels are accepted. The
 * explicit-length validator never guesses the input length.
 */
enum clock_status clock_model_validate_text(const char *text, size_t length,
					    bool allow_empty);
/* Calendar conversion is UTC, checked by a libc round trip, within 1980..2099.
 * weekday is output only; an invalid date never normalizes to another date.
 */
enum clock_status clock_model_utc_time(int64_t wall_utc,
				       struct clock_local_time *calendar);
enum clock_status
clock_model_utc_from_calendar(const struct clock_local_time *calendar,
			      int64_t *wall_utc);
/* Returns INVALID for malformed schedules or no strictly future occurrence. */
enum clock_status
clock_model_alarm_next_due(const struct clock_alarm_spec *spec, int64_t after,
			   int64_t *due_utc);
const char *clock_model_status_name(enum clock_status status);

#endif
