/* SPDX-License-Identifier: GPL-2.0-only */
#include "launcher-clock.h"
#include "fplinux-ui-locale.h"
#include "fplinux-ui-screen.h"

#include <libintl.h>
#include <stdio.h>
#include <string.h>

#define _(text) gettext(text)

enum clock_command {
	CLOCK_COMMAND_STOP = 100,
	CLOCK_COMMAND_SNOOZE,
};

void fplinux_launcher_clock_init(struct fplinux_launcher_clock *clock,
				 struct fplinux_ui *ui,
				 void (*changed)(void *context), void *context)
{
	memset(clock, 0, sizeof(*clock));
	clock->ui = ui;
	clock->changed = changed;
	clock->context = context;
}

bool fplinux_launcher_clock_is_ringing(
	const struct fplinux_launcher_clock *clock)
{
	return clock->ring.event.id != 0;
}

static bool alert_is_current(const struct fplinux_launcher_clock *clock)
{
	return clock->alert && fplinux_ui_current(clock->ui) == clock->alert;
}

bool fplinux_launcher_clock_is_visible(
	const struct fplinux_launcher_clock *clock)
{
	if (clock->pending || (clock->wait_event_id &&
			       clock->wait_event_id == clock->ring.event.id))
		return true;
	return alert_is_current(clock);
}

bool fplinux_launcher_clock_is_waiting(
	const struct fplinux_launcher_clock *clock)
{
	return clock->wait_event_id != 0;
}

bool fplinux_launcher_clock_update(struct fplinux_launcher_clock *clock,
				   const char *data)
{
	struct clock_ring ring = { 0 };
	uint64_t serial;

	if (data[0] && clock_wire_ring_decode(data, strlen(data), &ring,
					      &serial) != CLOCK_OK)
		return false;
	if (ring.event.id != clock->ring.event.id)
		clock->action_failed = false;
	clock->ring = ring;
	if (clock->pending && ring.event.id != clock->pending_event_id) {
		clock->pending = 0;
		clock->pending_event_id = 0;
	}
	return true;
}

static const char *ring_title(const struct fplinux_launcher_clock *clock)
{
	return clock->ring.event.kind == CLOCK_EVENT_ALARM ? _("Alarm") :
							     _("Timer");
}

static void alert_text(const struct fplinux_launcher_clock *clock, char *text,
		       size_t capacity)
{
	const struct clock_ring *ring = &clock->ring;
	const char *label = ring->event.label[0] ? ring->event.label :
						   _("Alarm");
	char time_text[32];
	char snooze_text[80];
	const char *status =
		clock->action_failed ? _("Action failed. Try again.") : "";

	if (ring->event.kind == CLOCK_EVENT_TIMER) {
		snprintf(text, capacity, "%s%s%s", _("Time is up"),
			 status[0] ? "\n" : "", status);
		return;
	}
	if (!fplinux_ui_format_date(clock->ui, (time_t)ring->event.due_wall_utc,
				    FPLINUX_UI_DATE_CLOCK, time_text,
				    sizeof(time_text)))
		time_text[0] = '\0';
	snprintf(snooze_text, sizeof(snooze_text), _("Snooze: %u min"),
		 ring->snooze_minutes);
	snprintf(text, capacity, "%s\n%s\n%s%s%s", time_text, label,
		 snooze_text, status[0] ? "\n" : "", status);
}

/* Never pop an unrelated screen or retain an Alert deleted by the runtime. */
static bool hide_alert(struct fplinux_launcher_clock *clock)
{
	if (!clock->alert)
		return true;
	if (!alert_is_current(clock))
		return false;
	clock->alert = NULL;
	clock->alert_event_id = 0;
	fplinux_ui_pop(clock->ui);
	return true;
}

static void forget_wait(struct fplinux_launcher_clock *clock)
{
	clock->wait_event_id = 0;
	clock->wait_alert = NULL;
	clock->wait_ending = false;
}

static void wait_ended(struct fplinux_ui *ui, void *context)
{
	struct fplinux_launcher_clock *clock = context;

	(void)ui;
	forget_wait(clock);
	if (clock->changed)
		clock->changed(clock->context);
}

/* The request was answered; done renders what follows. */
static void end_wait(struct fplinux_launcher_clock *clock)
{
	if (!clock->wait_event_id || clock->wait_ending)
		return;
	if (fplinux_ui_wait_end(clock->ui, wait_ended, clock))
		clock->wait_ending = true;
	else
		forget_wait(clock);
}

/*
 * A newer ring replaces the answered one at once, so that ACTIVE_ACK can
 * confirm it. Removing the busy alert or the answered ring below it ends
 * the wait without done.
 */
static bool leave_wait(struct fplinux_launcher_clock *clock)
{
	if (fplinux_ui_current(clock->ui) == clock->wait_alert) {
		if (!fplinux_ui_pop(clock->ui))
			return false;
		forget_wait(clock);
	}
	if (!hide_alert(clock))
		return false;
	forget_wait(clock);
	return true;
}

static void clock_command(struct fplinux_ui *ui,
			  struct fplinux_ui_screen *alert, int command_id,
			  void *context)
{
	struct fplinux_launcher_clock *clock = context;
	struct clock_request request = { 0 };
	struct fplinux_ui_session_message message = {
		.type = FPLINUX_UI_SESSION_CLOCK,
	};

	if (alert != clock->alert)
		return;
	/*
	 * Native Alerts leave the stack before their listener runs. Pushed
	 * again, the ring stays until the owner's reply decides what follows.
	 */
	if (!fplinux_ui_push(ui, alert)) {
		clock->alert = NULL;
		clock->alert_event_id = 0;
		goto changed;
	}
	request.event_id = clock->alert_event_id;
	if (clock->pending || !request.event_id ||
	    request.event_id != clock->ring.event.id ||
	    (command_id != CLOCK_COMMAND_STOP &&
	     (command_id != CLOCK_COMMAND_SNOOZE ||
	      clock->ring.event.kind != CLOCK_EVENT_ALARM)))
		goto changed;
	request.op = command_id == CLOCK_COMMAND_SNOOZE ? CLOCK_REQUEST_SNOOZE :
							  CLOCK_REQUEST_DISMISS;
	/* minutes=0 uses the owner's current snooze duration. */
	if (!++clock->serial)
		++clock->serial;
	message.serial = clock->serial;
	snprintf(message.name, sizeof(message.name), "%s",
		 clock_wire_request_name(request.op));
	clock->pending = message.serial;
	clock->pending_event_id = request.event_id;
	clock->action_failed = false;
	if (clock_wire_request_encode(&request, message.clock_data,
				      sizeof(message.clock_data)) != CLOCK_OK ||
	    !fplinux_ui_send(ui, &message)) {
		clock->pending = 0;
		clock->pending_event_id = 0;
		clock->action_failed = true;
	} else {
		/* No local Cancel: hiding a ring is the owner's decision. */
		clock->wait_alert = fplinux_ui_wait_begin(
			ui, ring_title(clock), _("Waiting…"), NULL, clock);
		if (clock->wait_alert) {
			clock->wait_event_id = request.event_id;
			clock->wait_ending = false;
		}
	}
changed:
	if (clock->changed)
		clock->changed(clock->context);
}

void fplinux_launcher_clock_render(struct fplinux_launcher_clock *clock)
{
	const struct clock_ring *ring = &clock->ring;
	const char *title = ring_title(clock);
	const struct fplinux_ui_command stop = {
		.id = CLOCK_COMMAND_STOP,
		.type = FPLINUX_UI_COMMAND_STOP,
		.label = _("Stop"),
	};
	const struct fplinux_ui_command snooze = {
		.id = CLOCK_COMMAND_SNOOZE,
		.type = FPLINUX_UI_COMMAND_SCREEN,
		.label = _("Snooze"),
		.keep_visible = true,
	};
	bool ready;
	char text[512];

	if (clock->wait_event_id) {
		if (fplinux_launcher_clock_is_ringing(clock) &&
		    ring->event.id != clock->wait_event_id) {
			if (!leave_wait(clock))
				return;
		} else {
			/* done hides the ring or shows its failure in place. */
			if (!clock->pending)
				end_wait(clock);
			if (clock->wait_event_id)
				return;
		}
	}
	if (!fplinux_launcher_clock_is_ringing(clock)) {
		hide_alert(clock);
		return;
	}
	if (clock->pending)
		return;
	alert_text(clock, text, sizeof(text));
	if (clock->alert && clock->alert_event_id == ring->event.id) {
		if (alert_is_current(clock)) {
			fplinux_ui_screen_set_title(clock->alert, title);
			fplinux_ui_alert_set_text(clock->alert, text);
		}
		return;
	}
	if (!hide_alert(clock))
		return;
	clock->alert = fplinux_ui_alert_new(
		clock->ui, FPLINUX_UI_ALERT_CONFIRMATION, title, text, 0);
	if (!clock->alert)
		return;
	clock->alert_event_id = ring->event.id;
	ready = fplinux_ui_screen_add_command(clock->alert, &stop) &&
		(ring->event.kind != CLOCK_EVENT_ALARM ||
		 fplinux_ui_screen_add_command(clock->alert, &snooze));
	fplinux_ui_screen_set_listener(clock->alert, clock_command, NULL,
				       clock);
	fplinux_ui_push(clock->ui, clock->alert);
	if (!ready)
		hide_alert(clock);
}

bool fplinux_launcher_clock_receive(
	struct fplinux_launcher_clock *clock,
	const struct fplinux_ui_session_message *message)
{
	if (message->type != FPLINUX_UI_SESSION_CLOCK)
		return false;
	if (!clock->pending || message->serial != clock->pending ||
	    clock->pending_event_id != clock->ring.event.id)
		return true;
	clock->pending = 0;
	clock->pending_event_id = 0;
	clock->action_failed = message->reason[0] != '\0';
	if (!message->clock_data[0] ||
	    !fplinux_launcher_clock_update(clock, message->clock_data))
		clock->action_failed = true;
	if (clock->changed)
		clock->changed(clock->context);
	return true;
}
