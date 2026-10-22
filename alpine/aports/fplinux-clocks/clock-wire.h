/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_CLOCK_WIRE_H
#define FPLINUX_CLOCK_WIRE_H

#include "clock-model.h"
#include "fplinux-storage.h"

/* Maximum encoded JSON size, including the terminating NUL. */
#define CLOCK_WIRE_CAPACITY 16384

/* Application and persistence are independent: a published change is accepted
 * even when its directory synchronization must be retried.
 */
struct clock_result {
	enum clock_status status;
	bool applied;
	struct fplinux_storage_result storage;
};

enum clock_request_op {
	CLOCK_REQUEST_SNAPSHOT,
	CLOCK_REQUEST_ALARM_ADD,
	CLOCK_REQUEST_ALARM_EDIT,
	CLOCK_REQUEST_ALARM_ENABLE,
	CLOCK_REQUEST_ALARM_DELETE,
	CLOCK_REQUEST_DISMISS,
	CLOCK_REQUEST_SNOOZE,
	CLOCK_REQUEST_TIMER_SET,
	CLOCK_REQUEST_TIMER_START,
	CLOCK_REQUEST_TIMER_PAUSE,
	CLOCK_REQUEST_TIMER_RESUME,
	CLOCK_REQUEST_TIMER_STOP,
	CLOCK_REQUEST_TIMER_RESTART,
	CLOCK_REQUEST_STOPWATCH_START,
	CLOCK_REQUEST_STOPWATCH_PAUSE,
	CLOCK_REQUEST_STOPWATCH_RESET,
	CLOCK_REQUEST_STOPWATCH_LAP,
	CLOCK_REQUEST_WORLD_ADD,
	CLOCK_REQUEST_WORLD_EDIT,
	CLOCK_REQUEST_WORLD_DELETE,
	CLOCK_REQUEST_SET_VIEW,
	CLOCK_REQUEST_SET_SNOOZE,
	CLOCK_REQUEST_CLEAR_MISSED,
	CLOCK_REQUEST_SAVE,
};

/* Only fields belonging to op are encoded. The decoder initializes all fields
 * and rejects extra members, wrong JSON types, duplicate keys and bad bounds.
 * Alarm/world IDs are stable uint32 IDs; event IDs remain exact uint64 values.
 */
struct clock_request {
	enum clock_request_op op;
	struct clock_alarm_spec alarm;
	uint32_t id;
	uint64_t event_id;
	bool cancel_active;
	bool enabled;
	unsigned int seconds;
	unsigned int minutes;
	int offset_minutes;
	enum clock_view view;
};

/* Launcher receives only the current event and presentation settings. NONE is
 * represented by an all-zero event. Ring actions use event.id, never row IDs.
 */
struct clock_ring {
	struct clock_event event;
	unsigned int snooze_minutes;
	uint64_t missed_total;
};

enum clock_status clock_wire_request_encode(const struct clock_request *request,
					    char *json, size_t capacity);
enum clock_status clock_wire_request_decode(const char *json, size_t length,
					    struct clock_request *request);
/* Apply at the owner's already-injected current time. Authorization belongs to
 * the session manager. No clock or IPC IO occurs here.
 */
enum clock_status clock_wire_request_apply(struct clock_model *model,
					   const struct clock_request *request);
const char *clock_wire_request_name(enum clock_request_op op);

/* Response acceptance and persistence are separate; codec success is CLOCK_OK.
 * A storage error with applied=true retains the accepted mutation's identity.
 * Decode is transactional: malformed input leaves every output unchanged.
 * Snapshot data is read-only presentation state, never an owner replacement.
 */
enum clock_status clock_wire_snapshot_encode(const struct clock_model *model,
					     const struct clock_result *result,
					     uint64_t serial, char *json,
					     size_t capacity);
enum clock_status clock_wire_snapshot_decode(const char *json, size_t length,
					     struct clock_model *model,
					     struct clock_result *result,
					     uint64_t *serial);
enum clock_status clock_wire_ring_encode(const struct clock_ring *ring,
					 uint64_t serial, char *json,
					 size_t capacity);
enum clock_status clock_wire_ring_decode(const char *json, size_t length,
					 struct clock_ring *ring,
					 uint64_t *serial);
void clock_wire_ring_from_model(const struct clock_model *model,
				struct clock_ring *ring);

/* Durable configuration only. Decode replaces configuration transactionally in
 * an initialized model, retains its current injected time, and resets transient
 * activity. Elapsed once alarms stay disabled; no stored occurrence is replayed.
 */
enum clock_status clock_wire_preferences_encode(const struct clock_model *model,
						char *json, size_t capacity);
enum clock_status clock_wire_preferences_decode(const char *json, size_t length,
						struct clock_model *model);

#endif
