/* SPDX-License-Identifier: GPL-2.0-only */
/* Production ringing callbacks with controlled native Alerts and IPC. */
#include "launcher-clock.h"
#include "fplinux-ui-locale.h"
#include "fplinux-ui-screen.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct fplinux_ui_screen {
	char title[128];
	char text[512];
	struct fplinux_ui_command commands[2];
	size_t command_count;
	fplinux_ui_command_cb command;
	void *context;
};

/*
 * The runtime wait as its header describes it: nothing at first, a busy
 * alert once the test passes the threshold, done from the loop once the
 * origin or the shown alert is current.
 */
struct fake_wait {
	struct fplinux_ui_screen *origin;
	struct fplinux_ui_screen alert;
	bool shown;
	bool ending;
	fplinux_ui_wait_cb done;
	void *done_context;
};

struct fplinux_ui {
	struct fplinux_ui_screen base;
	struct fplinux_ui_screen other;
	struct fplinux_ui_screen *stack[4];
	size_t depth;
	struct fake_wait wait;
	/* Each one changes the legends. */
	unsigned int screen_changes;
	unsigned int busy_alerts;
	/* Waits that navigation removed before done could run. */
	unsigned int dropped_waits;
	struct fplinux_ui_session_message sent[8];
	size_t sent_count;
	bool fail_send;
	bool fail_alert;
	bool fail_command;
	bool fail_wait;
};

bool fplinux_ui_format_date(const struct fplinux_ui *ui, time_t when,
			    enum fplinux_ui_date_style style, char *out,
			    size_t size)
{
	assert(ui && when == 1704067200 && style == FPLINUX_UI_DATE_CLOCK);
	assert(size >= sizeof("00:00"));
	strcpy(out, "00:00");
	return true;
}

bool fplinux_ui_send(struct fplinux_ui *ui,
		     const struct fplinux_ui_session_message *message)
{
	if (ui->fail_send)
		return false;
	assert(ui->sent_count < 8);
	ui->sent[ui->sent_count++] = *message;
	return true;
}

struct fplinux_ui_screen *fplinux_ui_current(const struct fplinux_ui *ui)
{
	assert(ui->depth);
	return ui->stack[ui->depth - 1];
}

static bool stacked(const struct fplinux_ui *ui,
		    const struct fplinux_ui_screen *screen)
{
	for (size_t i = 0; i < ui->depth; ++i)
		if (ui->stack[i] == screen)
			return true;
	return false;
}

bool fplinux_ui_push(struct fplinux_ui *ui, struct fplinux_ui_screen *screen)
{
	assert(ui->depth < 4 && !stacked(ui, screen));
	ui->stack[ui->depth++] = screen;
	++ui->screen_changes;
	return true;
}

/* Removing the origin or the shown busy alert drops the wait without done. */
bool fplinux_ui_pop(struct fplinux_ui *ui)
{
	struct fplinux_ui_screen *top;

	if (ui->depth == 1)
		return false;
	top = fplinux_ui_current(ui);
	assert(top != &ui->other);
	--ui->depth;
	++ui->screen_changes;
	if (ui->wait.origin &&
	    (top == ui->wait.origin || top == &ui->wait.alert)) {
		ui->wait = (struct fake_wait){ 0 };
		++ui->dropped_waits;
	}
	if (top != &ui->wait.alert)
		free(top);
	return true;
}

struct fplinux_ui_screen *fplinux_ui_alert_new(struct fplinux_ui *ui,
					       enum fplinux_ui_alert_kind kind,
					       const char *title,
					       const char *text,
					       unsigned int timeout_ms)
{
	struct fplinux_ui_screen *screen;

	assert(kind == FPLINUX_UI_ALERT_CONFIRMATION && !timeout_ms);
	if (ui->fail_alert)
		return NULL;
	screen = calloc(1, sizeof(*screen));
	assert(screen);
	snprintf(screen->title, sizeof(screen->title), "%s", title);
	snprintf(screen->text, sizeof(screen->text), "%s", text);
	screen->context = ui;
	return screen;
}

bool fplinux_ui_screen_set_title(struct fplinux_ui_screen *screen,
				 const char *title)
{
	snprintf(screen->title, sizeof(screen->title), "%s", title);
	return true;
}

bool fplinux_ui_alert_set_text(struct fplinux_ui_screen *screen,
			       const char *text)
{
	snprintf(screen->text, sizeof(screen->text), "%s", text);
	return true;
}

void fplinux_ui_screen_set_listener(struct fplinux_ui_screen *screen,
				    fplinux_ui_command_cb command,
				    fplinux_ui_change_cb change, void *context)
{
	assert(!change);
	screen->command = command;
	screen->context = context;
}

bool fplinux_ui_screen_add_command(struct fplinux_ui_screen *screen,
				   const struct fplinux_ui_command *command)
{
	struct fplinux_ui *ui = screen->context;

	if (ui->fail_command)
		return false;
	assert(screen->command_count < 2);
	screen->commands[screen->command_count++] = *command;
	return true;
}

struct fplinux_ui_screen *fplinux_ui_wait_begin(struct fplinux_ui *ui,
						const char *title,
						const char *text,
						fplinux_ui_wait_cb cancel,
						void *context)
{
	(void)context;
	/* Back on the origin stays ignored: no local cancel of a ring. */
	assert(!cancel);
	if (ui->fail_wait || ui->wait.origin) {
		errno = EBUSY;
		return NULL;
	}
	ui->wait = (struct fake_wait){ .origin = fplinux_ui_current(ui) };
	snprintf(ui->wait.alert.title, sizeof(ui->wait.alert.title), "%s",
		 title);
	snprintf(ui->wait.alert.text, sizeof(ui->wait.alert.text), "%s", text);
	return &ui->wait.alert;
}

bool fplinux_ui_wait_end(struct fplinux_ui *ui, fplinux_ui_wait_cb done,
			 void *context)
{
	if (!ui->wait.origin || ui->wait.ending) {
		errno = ENOENT;
		return false;
	}
	ui->wait.ending = true;
	ui->wait.done = done;
	ui->wait.done_context = context;
	return true;
}

/* The work has lasted longer than the threshold. */
static void pass_threshold(struct fplinux_ui *ui)
{
	assert(ui->wait.origin && !ui->wait.shown);
	if (fplinux_ui_current(ui) != ui->wait.origin)
		return;
	ui->wait.shown = true;
	++ui->busy_alerts;
	ui->stack[ui->depth++] = &ui->wait.alert;
	++ui->screen_changes;
}

/* One pass of the runtime loop; the shown alert has had its time. */
static void run_loop(struct fplinux_ui *ui)
{
	struct fake_wait wait = ui->wait;

	if (!wait.ending || (fplinux_ui_current(ui) != wait.origin &&
			     fplinux_ui_current(ui) != &ui->wait.alert))
		return;
	ui->wait = (struct fake_wait){ 0 };
	if (wait.shown) {
		--ui->depth;
		++ui->screen_changes;
	}
	if (wait.done)
		wait.done(ui, wait.done_context);
}

static bool waiting(const struct fplinux_ui *ui)
{
	return ui->wait.origin != NULL;
}

static const struct fplinux_ui_command *
find_command(const struct fplinux_ui_screen *screen,
	     enum fplinux_ui_command_type type)
{
	for (size_t i = 0; i < screen->command_count; ++i)
		if (screen->commands[i].type == type)
			return &screen->commands[i];
	return NULL;
}

static void alert_response(struct fplinux_ui *ui,
			   enum fplinux_ui_command_type type)
{
	struct fplinux_ui_screen *screen = fplinux_ui_current(ui);
	const struct fplinux_ui_command *command = find_command(screen, type);
	int command_id;

	assert(command && screen->command && ui->depth > 1);
	command_id = command->id;
	/*
	 * The native Alert detaches before its listener and is deleted after
	 * it unless the listener pushed it again.
	 */
	--ui->depth;
	++ui->screen_changes;
	screen->command(ui, screen, command_id, screen->context);
	if (!stacked(ui, screen))
		free(screen);
}

static void changed(void *context)
{
	fplinux_launcher_clock_render(context);
}

static void ring_data(char *data, size_t capacity, uint64_t event_id,
		      enum clock_event_kind kind)
{
	struct clock_ring ring = {
		.event = {
			.id = event_id,
			.kind = kind,
			.source_id = kind == CLOCK_EVENT_ALARM ? 42 : 0,
			.generation = 1,
			.label = "Wake up",
			.due_wall_utc = 1704067200,
		},
		.snooze_minutes = 5,
		.missed_total = 2,
	};

	if (!event_id)
		memset(&ring.event, 0, sizeof(ring.event));
	assert(clock_wire_ring_encode(&ring, event_id, data, capacity) ==
	       CLOCK_OK);
}

static void update_ring(struct fplinux_launcher_clock *clock, uint64_t event_id,
			enum clock_event_kind kind)
{
	char data[CLOCK_WIRE_CAPACITY];

	ring_data(data, sizeof(data), event_id, kind);
	assert(fplinux_launcher_clock_update(clock, data));
	fplinux_launcher_clock_render(clock);
}

static void reply(struct fplinux_launcher_clock *clock, uint32_t serial,
		  uint64_t event_id, enum clock_event_kind kind,
		  const char *reason)
{
	struct fplinux_ui_session_message message = {
		.type = FPLINUX_UI_SESSION_CLOCK,
		.serial = serial,
	};

	snprintf(message.reason, sizeof(message.reason), "%s", reason);
	ring_data(message.clock_data, sizeof(message.clock_data), event_id,
		  kind);
	assert(fplinux_launcher_clock_receive(clock, &message));
}

/*
 * The ring answered stays current until the owner replies: the frame
 * after the key still shows it, and it takes no input meanwhile.
 */
static void assert_ring_waits(struct fplinux_ui *ui,
			      struct fplinux_launcher_clock *clock,
			      struct fplinux_ui_screen *ring)
{
	assert(fplinux_ui_current(ui) == ring && ui->depth == 2);
	assert(waiting(ui) && ui->wait.origin == ring);
	assert(fplinux_launcher_clock_is_waiting(clock));
	/* ACTIVE_ACK still confirms the ring the user answered. */
	assert(fplinux_launcher_clock_is_visible(clock));
}

static void assert_failed_ring(struct fplinux_ui *ui,
			       struct fplinux_ui_screen *ring)
{
	assert(ui->depth == 2 && fplinux_ui_current(ui) == ring);
	assert(ring->command_count == 2);
	assert(!strcmp(ring->text, "00:00\nWake up\nSnooze: 5 min\n"
				   "Action failed. Try again."));
}

/* A reply within the threshold: the ring leaves in one step, never busy. */
static void test_alarm(struct fplinux_ui *ui,
		       struct fplinux_launcher_clock *clock)
{
	update_ring(clock, UINT64_C(9007199254740993), CLOCK_EVENT_ALARM);
	struct fplinux_ui_screen *alert = fplinux_ui_current(ui);

	assert(fplinux_launcher_clock_is_ringing(clock));
	assert(fplinux_launcher_clock_is_visible(clock));
	assert(!strcmp(alert->title, "Alarm"));
	assert(!strcmp(alert->text, "00:00\nWake up\nSnooze: 5 min"));
	assert(alert->command_count == 2);
	assert(!strcmp(find_command(alert, FPLINUX_UI_COMMAND_STOP)->label,
		       "Stop"));
	assert(!strcmp(find_command(alert, FPLINUX_UI_COMMAND_SCREEN)->label,
		       "Snooze"));
	assert(find_command(alert, FPLINUX_UI_COMMAND_SCREEN)->keep_visible);
	alert_response(ui, FPLINUX_UI_COMMAND_SCREEN);
	assert(ui->sent_count == 1 && ui->sent[0].serial != 0);
	assert(ui->sent[0].type == FPLINUX_UI_SESSION_CLOCK);
	assert(!strcmp(ui->sent[0].name, "snooze"));
	assert(!strcmp(
		ui->sent[0].clock_data,
		"{\"op\":\"snooze\",\"event_id\":9007199254740993,\"minutes\":0}"));
	assert_ring_waits(ui, clock, alert);
	assert(fplinux_launcher_clock_is_ringing(clock));
	reply(clock, ui->sent[0].serial, 0, CLOCK_EVENT_NONE, "");
	assert(!fplinux_launcher_clock_is_ringing(clock));
	/* done presents the result; until then the ring stays. */
	assert(fplinux_ui_current(ui) == alert && ui->wait.ending);
	run_loop(ui);
	assert(!waiting(ui) && !fplinux_launcher_clock_is_waiting(clock));
	assert(ui->depth == 1 && !ui->busy_alerts && !ui->dropped_waits);
}

/* A slow reply: one busy alert over the ring, without Cancel. */
static void test_timer(struct fplinux_ui *ui,
		       struct fplinux_launcher_clock *clock)
{
	update_ring(clock, 7, CLOCK_EVENT_TIMER);
	struct fplinux_ui_screen *alert = fplinux_ui_current(ui);
	unsigned int changes;

	assert(!strcmp(alert->title, "Timer"));
	assert(!strcmp(alert->text, "Time is up"));
	assert(alert->command_count == 1);
	assert(!find_command(alert, FPLINUX_UI_COMMAND_SCREEN));
	alert_response(ui, FPLINUX_UI_COMMAND_STOP);
	assert(ui->sent_count == 1 && !strcmp(ui->sent[0].name, "dismiss"));
	assert(!strcmp(ui->sent[0].clock_data,
		       "{\"op\":\"dismiss\",\"event_id\":7}"));
	assert_ring_waits(ui, clock, alert);
	assert(!ui->busy_alerts);
	pass_threshold(ui);
	assert(fplinux_ui_current(ui) == &ui->wait.alert);
	assert(ui->depth == 3 && ui->stack[1] == alert);
	assert(!strcmp(ui->wait.alert.title, "Timer"));
	assert(!strcmp(ui->wait.alert.text, "Waiting…"));
	assert(!ui->wait.alert.command_count);
	assert(fplinux_launcher_clock_is_visible(clock));
	/* Another ACTIVE for the same ring keeps the wait as it is. */
	changes = ui->screen_changes;
	update_ring(clock, 7, CLOCK_EVENT_TIMER);
	assert(fplinux_ui_current(ui) == &ui->wait.alert);
	assert(ui->screen_changes == changes);
	update_ring(clock, 0, CLOCK_EVENT_NONE);
	assert(fplinux_ui_current(ui) == &ui->wait.alert);
	assert(ui->screen_changes == changes);
	/* The busy alert and the ring leave together. */
	run_loop(ui);
	assert(!waiting(ui) && ui->depth == 1 && ui->busy_alerts == 1);
	assert(!ui->dropped_waits);
}

/*
 * A new event before the busy alert replaces the answered ring at once, so
 * that ACTIVE_ACK can confirm it; the old wait ends with that ring.
 */
static void test_stale(struct fplinux_ui *ui,
		       struct fplinux_launcher_clock *clock)
{
	update_ring(clock, 7, CLOCK_EVENT_ALARM);
	alert_response(ui, FPLINUX_UI_COMMAND_STOP);
	uint32_t first = ui->sent[0].serial;

	reply(clock, first + 1, 0, CLOCK_EVENT_NONE, "");
	assert(fplinux_launcher_clock_is_ringing(clock));
	assert(waiting(ui) && !ui->wait.ending);
	update_ring(clock, 8, CLOCK_EVENT_ALARM);
	assert(!waiting(ui) && ui->dropped_waits == 1);
	assert(!fplinux_launcher_clock_is_waiting(clock));
	assert(ui->depth == 2 && fplinux_ui_current(ui)->command_count == 2);
	assert(fplinux_launcher_clock_is_visible(clock));
	/* The old request's reply changes nothing. */
	reply(clock, first, 0, CLOCK_EVENT_NONE, "");
	assert(fplinux_launcher_clock_is_ringing(clock) && ui->depth == 2);
	alert_response(ui, FPLINUX_UI_COMMAND_STOP);
	assert(ui->sent_count == 2 && ui->sent[1].serial != first);
	assert(!strcmp(ui->sent[1].clock_data,
		       "{\"op\":\"dismiss\",\"event_id\":8}"));
	assert_ring_waits(ui, clock, fplinux_ui_current(ui));
	reply(clock, ui->sent[1].serial, 0, CLOCK_EVENT_NONE, "");
	run_loop(ui);
	assert(!waiting(ui) && ui->depth == 1 && !ui->busy_alerts);
	assert(!fplinux_launcher_clock_is_waiting(clock));
	assert(ui->dropped_waits == 1);
}

/*
 * A new event after the busy alert, also while a failed reply's wait still
 * ends, replaces both the busy alert and the answered ring at once.
 */
static void test_superseded(struct fplinux_ui *ui,
			    struct fplinux_launcher_clock *clock)
{
	update_ring(clock, 7, CLOCK_EVENT_ALARM);
	alert_response(ui, FPLINUX_UI_COMMAND_STOP);
	pass_threshold(ui);
	assert(ui->depth == 3);
	update_ring(clock, 8, CLOCK_EVENT_ALARM);
	assert(!waiting(ui) && ui->dropped_waits == 1);
	assert(ui->depth == 2 && fplinux_ui_current(ui)->command_count == 2);
	assert(fplinux_launcher_clock_is_visible(clock));
	assert(!fplinux_launcher_clock_is_waiting(clock));
	alert_response(ui, FPLINUX_UI_COMMAND_STOP);
	assert(!strcmp(ui->sent[1].clock_data,
		       "{\"op\":\"dismiss\",\"event_id\":8}"));
	pass_threshold(ui);
	reply(clock, ui->sent[1].serial, 8, CLOCK_EVENT_ALARM, "clock-failed");
	assert(waiting(ui) && ui->wait.ending);
	update_ring(clock, 9, CLOCK_EVENT_ALARM);
	assert(!waiting(ui) && ui->dropped_waits == 2);
	assert(ui->depth == 2 && fplinux_ui_current(ui)->command_count == 2);
	assert(!strstr(fplinux_ui_current(ui)->text, "Action failed"));
	assert(fplinux_launcher_clock_is_visible(clock));
	/* Nothing is left of either request: the next answer waits as usual. */
	alert_response(ui, FPLINUX_UI_COMMAND_STOP);
	assert(ui->sent_count == 3);
	assert(!strcmp(ui->sent[2].clock_data,
		       "{\"op\":\"dismiss\",\"event_id\":9}"));
	assert_ring_waits(ui, clock, fplinux_ui_current(ui));
	reply(clock, ui->sent[2].serial, 0, CLOCK_EVENT_NONE, "");
	run_loop(ui);
	assert(!waiting(ui) && ui->depth == 1 && ui->busy_alerts == 2);
}

/* A failure keeps the ring in place and adds its error to the text. */
static void test_errors(struct fplinux_ui *ui,
			struct fplinux_launcher_clock *clock)
{
	update_ring(clock, 7, CLOCK_EVENT_ALARM);
	struct fplinux_ui_screen *alert = fplinux_ui_current(ui);

	/* Nothing was sent, so there is nothing to wait for. */
	ui->fail_send = true;
	alert_response(ui, FPLINUX_UI_COMMAND_STOP);
	assert(!ui->sent_count && !waiting(ui));
	assert_failed_ring(ui, alert);
	ui->fail_send = false;
	/* A failed reply within the threshold. */
	alert_response(ui, FPLINUX_UI_COMMAND_STOP);
	assert_ring_waits(ui, clock, alert);
	reply(clock, ui->sent[0].serial, 7, CLOCK_EVENT_ALARM, "clock-failed");
	assert(waiting(ui) && fplinux_ui_current(ui) == alert);
	run_loop(ui);
	assert(!waiting(ui) && !fplinux_launcher_clock_is_waiting(clock));
	assert_failed_ring(ui, alert);
	assert(!ui->busy_alerts);
	assert(!fplinux_launcher_clock_update(clock, "{\"op\":\"invalid\"}"));
	assert(fplinux_launcher_clock_is_ringing(clock));
	/* A malformed reply after the threshold. */
	alert_response(ui, FPLINUX_UI_COMMAND_STOP);
	pass_threshold(ui);
	struct fplinux_ui_session_message bad = {
		.type = FPLINUX_UI_SESSION_CLOCK,
		.serial = ui->sent[1].serial,
		.clock_data = "{\"op\":\"invalid\"}",
	};

	assert(fplinux_launcher_clock_receive(clock, &bad));
	assert(fplinux_launcher_clock_is_ringing(clock));
	assert(fplinux_ui_current(ui) == &ui->wait.alert);
	run_loop(ui);
	assert_failed_ring(ui, alert);
	assert(ui->busy_alerts == 1);
	/* Without a wait the request still runs and its result still shows. */
	ui->fail_wait = true;
	alert_response(ui, FPLINUX_UI_COMMAND_STOP);
	assert(ui->sent_count == 3 && !waiting(ui));
	assert(fplinux_ui_current(ui) == alert);
	assert(fplinux_launcher_clock_is_visible(clock));
	reply(clock, ui->sent[2].serial, 7, CLOCK_EVENT_ALARM, "clock-failed");
	assert_failed_ring(ui, alert);
	ui->fail_wait = false;
	assert(!ui->dropped_waits);
}

static void test_visibility(struct fplinux_ui *ui,
			    struct fplinux_launcher_clock *clock)
{
	ui->fail_alert = true;
	update_ring(clock, 7, CLOCK_EVENT_ALARM);
	assert(ui->depth == 1);
	assert(fplinux_launcher_clock_is_ringing(clock));
	assert(!fplinux_launcher_clock_is_visible(clock));
	ui->fail_alert = false;
	ui->fail_command = true;
	fplinux_launcher_clock_render(clock);
	assert(ui->depth == 1);
	assert(!fplinux_launcher_clock_is_visible(clock));
	ui->fail_command = false;
	fplinux_launcher_clock_render(clock);
	assert(fplinux_launcher_clock_is_visible(clock));
	fplinux_ui_push(ui, &ui->other);
	update_ring(clock, 0, CLOCK_EVENT_NONE);
	assert(ui->depth == 3 && fplinux_ui_current(ui) == &ui->other);
	assert(!fplinux_launcher_clock_is_visible(clock));
	--ui->depth;
	fplinux_launcher_clock_render(clock);
	assert(ui->depth == 1);
}

int main(int argc, char **argv)
{
	struct fplinux_ui ui = { .depth = 1 };
	struct fplinux_launcher_clock clock;

	assert(argc == 2);
	ui.stack[0] = &ui.base;
	fplinux_launcher_clock_init(&clock, &ui, changed, &clock);
	if (!strcmp(argv[1], "alarm"))
		test_alarm(&ui, &clock);
	else if (!strcmp(argv[1], "timer"))
		test_timer(&ui, &clock);
	else if (!strcmp(argv[1], "stale"))
		test_stale(&ui, &clock);
	else if (!strcmp(argv[1], "superseded"))
		test_superseded(&ui, &clock);
	else if (!strcmp(argv[1], "errors"))
		test_errors(&ui, &clock);
	else if (!strcmp(argv[1], "visibility"))
		test_visibility(&ui, &clock);
	else
		assert(false);
	assert(fplinux_launcher_clock_update(&clock, ""));
	fplinux_launcher_clock_render(&clock);
	assert(ui.depth == 1 && !waiting(&ui));
	puts("launcher-clock: scenario passed");
	return 0;
}
