/* SPDX-License-Identifier: GPL-2.0-only */
/* Production ringing callbacks with controlled native Alerts and IPC. */
#include "launcher-clock.h"
#include "fplinux-ui-locale.h"
#include "fplinux-ui-screen.h"

#include <assert.h>
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
	bool busy;
};

struct fplinux_ui {
	struct fplinux_ui_screen base;
	struct fplinux_ui_screen other;
	struct fplinux_ui_screen *stack[4];
	size_t depth;
	struct fplinux_ui_session_message sent[8];
	size_t sent_count;
	bool fail_send;
	bool fail_alert;
	bool fail_command;
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

void fplinux_ui_push(struct fplinux_ui *ui, struct fplinux_ui_screen *screen)
{
	assert(ui->depth < 4);
	ui->stack[ui->depth++] = screen;
}

bool fplinux_ui_pop(struct fplinux_ui *ui)
{
	if (ui->depth == 1)
		return false;
	assert(fplinux_ui_current(ui) != &ui->other);
	free(ui->stack[--ui->depth]);
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

bool fplinux_ui_alert_set_progress(struct fplinux_ui_screen *screen, int value,
				   int maximum)
{
	assert(!value && !maximum);
	screen->busy = true;
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
	/* The native Alert detaches before its listener and is deleted after it. */
	--ui->depth;
	screen->command(ui, screen, command_id, screen->context);
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
	alert = fplinux_ui_current(ui);
	assert(alert->busy && !alert->command_count);
	assert(strstr(alert->text, "Waiting…"));
	assert(fplinux_launcher_clock_is_ringing(clock));
	reply(clock, ui->sent[0].serial, 0, CLOCK_EVENT_NONE, "");
	assert(!fplinux_launcher_clock_is_ringing(clock));
	assert(ui->depth == 1);
}

static void test_timer(struct fplinux_ui *ui,
		       struct fplinux_launcher_clock *clock)
{
	update_ring(clock, 7, CLOCK_EVENT_TIMER);
	struct fplinux_ui_screen *alert = fplinux_ui_current(ui);

	assert(!strcmp(alert->title, "Timer"));
	assert(!strcmp(alert->text, "Time is up"));
	assert(alert->command_count == 1);
	assert(!find_command(alert, FPLINUX_UI_COMMAND_SCREEN));
	alert_response(ui, FPLINUX_UI_COMMAND_STOP);
	assert(ui->sent_count == 1 && !strcmp(ui->sent[0].name, "dismiss"));
	assert(!strcmp(ui->sent[0].clock_data,
		       "{\"op\":\"dismiss\",\"event_id\":7}"));
	assert(fplinux_ui_current(ui)->busy);
	update_ring(clock, 0, CLOCK_EVENT_NONE);
	assert(ui->depth == 1);
}

static void test_stale(struct fplinux_ui *ui,
		       struct fplinux_launcher_clock *clock)
{
	update_ring(clock, 7, CLOCK_EVENT_ALARM);
	alert_response(ui, FPLINUX_UI_COMMAND_STOP);
	uint32_t first = ui->sent[0].serial;

	reply(clock, first + 1, 0, CLOCK_EVENT_NONE, "");
	assert(fplinux_launcher_clock_is_ringing(clock));
	assert(fplinux_ui_current(ui)->busy);
	/* A new event supersedes the old request before its reply arrives. */
	update_ring(clock, 8, CLOCK_EVENT_ALARM);
	assert(fplinux_ui_current(ui)->command_count == 2);
	reply(clock, first, 0, CLOCK_EVENT_NONE, "");
	assert(fplinux_launcher_clock_is_ringing(clock));
	assert(fplinux_ui_current(ui)->command_count == 2);
	alert_response(ui, FPLINUX_UI_COMMAND_STOP);
	assert(ui->sent_count == 2 && ui->sent[1].serial != first);
	assert(!strcmp(ui->sent[1].clock_data,
		       "{\"op\":\"dismiss\",\"event_id\":8}"));
	reply(clock, first, 0, CLOCK_EVENT_NONE, "");
	assert(fplinux_ui_current(ui)->busy);
	reply(clock, ui->sent[1].serial, 0, CLOCK_EVENT_NONE, "");
	assert(ui->depth == 1);
}

static void test_errors(struct fplinux_ui *ui,
			struct fplinux_launcher_clock *clock)
{
	update_ring(clock, 7, CLOCK_EVENT_ALARM);
	ui->fail_send = true;
	alert_response(ui, FPLINUX_UI_COMMAND_STOP);
	assert(!ui->sent_count);
	assert(fplinux_ui_current(ui)->command_count == 2);
	assert(strstr(fplinux_ui_current(ui)->text,
		      "Action failed. Try again."));
	ui->fail_send = false;
	alert_response(ui, FPLINUX_UI_COMMAND_STOP);
	reply(clock, ui->sent[0].serial, 7, CLOCK_EVENT_ALARM, "clock-failed");
	assert(fplinux_ui_current(ui)->command_count == 2);
	assert(strstr(fplinux_ui_current(ui)->text,
		      "Action failed. Try again."));
	assert(!fplinux_launcher_clock_update(clock, "{\"op\":\"invalid\"}"));
	assert(fplinux_launcher_clock_is_ringing(clock));
	alert_response(ui, FPLINUX_UI_COMMAND_STOP);
	struct fplinux_ui_session_message bad = {
		.type = FPLINUX_UI_SESSION_CLOCK,
		.serial = ui->sent[1].serial,
		.clock_data = "{\"op\":\"invalid\"}",
	};

	assert(fplinux_launcher_clock_receive(clock, &bad));
	assert(fplinux_launcher_clock_is_ringing(clock));
	assert(fplinux_ui_current(ui)->command_count == 2);
	assert(strstr(fplinux_ui_current(ui)->text,
		      "Action failed. Try again."));
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
	else if (!strcmp(argv[1], "errors"))
		test_errors(&ui, &clock);
	else if (!strcmp(argv[1], "visibility"))
		test_visibility(&ui, &clock);
	else
		assert(false);
	assert(fplinux_launcher_clock_update(&clock, ""));
	fplinux_launcher_clock_render(&clock);
	assert(ui.depth == 1);
	puts("launcher-clock: scenario passed");
	return 0;
}
