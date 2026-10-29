/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_LAUNCHER_CLOCK_H
#define FPLINUX_LAUNCHER_CLOCK_H

#include "clock-wire.h"
#include "fplinux-ui-session.h"

struct fplinux_ui;
struct fplinux_ui_screen;

struct fplinux_launcher_clock {
	struct fplinux_ui *ui;
	struct fplinux_ui_screen *alert;
	struct clock_ring ring;
	uint64_t alert_event_id;
	uint32_t serial;
	uint32_t pending;
	uint64_t pending_event_id;
	/*
	 * The event of the runtime wait, which began on its ring, until done
	 * runs or a newer ring removes it; 0 without one.
	 */
	uint64_t wait_event_id;
	/* Compared with the current screen only; NULL without a wait. */
	struct fplinux_ui_screen *wait_alert;
	bool wait_ending;
	bool action_failed;
	void (*changed)(void *context);
	void *context;
};

void fplinux_launcher_clock_init(struct fplinux_launcher_clock *clock,
				 struct fplinux_ui *ui,
				 void (*changed)(void *context), void *context);
bool fplinux_launcher_clock_is_ringing(
	const struct fplinux_launcher_clock *clock);
/* An ACTIVE update is authoritative; malformed data leaves the state unchanged. */
bool fplinux_launcher_clock_update(struct fplinux_launcher_clock *clock,
				   const char *data);
/* Receives CLOCK replies; stale serials and superseded events have no effect. */
bool fplinux_launcher_clock_receive(
	struct fplinux_launcher_clock *clock,
	const struct fplinux_ui_session_message *message);
void fplinux_launcher_clock_render(struct fplinux_launcher_clock *clock);
/*
 * ACTIVE_ACK must not acknowledge controls that failed to become visible.
 * After Stop or Snooze, the request for the drawn ring stands for them.
 */
bool fplinux_launcher_clock_is_visible(
	const struct fplinux_launcher_clock *clock);
/* The wait keeps the screen it began on; nothing else may navigate it away. */
bool fplinux_launcher_clock_is_waiting(
	const struct fplinux_launcher_clock *clock);

#endif
