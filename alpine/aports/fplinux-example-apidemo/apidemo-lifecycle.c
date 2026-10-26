/* SPDX-License-Identifier: 0BSD */
/*
 * Lifecycle: the screen stack, visibility, the display, time, a watched
 * file descriptor, the window title and the ways to leave.
 */
/* timerfd and poll() are POSIX.1-2008 and Linux interfaces. */
#define _POSIX_C_SOURCE 200809L
#include "apidemo.h"

#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#define TEXT_BYTES 256
/* The clock demo redraws ten times a second while it is shown. */
#define CLOCK_PERIOD_MS 100
#define MS_PER_SECOND 1000
/* The title text field takes up to this many characters. */
#define TITLE_CHARS 60

/* The screen stack */

/* The demo stops pushing at this depth. */
#define STACK_LIMIT 32

enum {
	COMMAND_PUSH = 1,
	COMMAND_REPLACE,
	COMMAND_POP,
};

/*
 * A screen keeps its number and actual predecessor as application data.
 * Replacement retains the predecessor, even after repeated replacements;
 * subtracting from the displayed number would name an already deleted screen.
 */
static struct stack_entry {
	int number;
	const struct stack_entry *below;
} stack_entries[STACK_LIMIT];

static const struct stack_entry *
stack_entry(const struct fplinux_ui_screen *screen)
{
	const void *data = screen ? fplinux_ui_screen_get_data(screen) : NULL;

	/* Other catalogue screens use unrelated data pointers. Equality is
	 * defined for those pointers; an array range comparison is not. */
	for (size_t i = 0; i < APIDEMO_COUNT(stack_entries); i++)
		if (data == &stack_entries[i])
			return &stack_entries[i];
	return NULL;
}

static int stack_number(const struct fplinux_ui_screen *screen)
{
	const struct stack_entry *entry = stack_entry(screen);

	return entry ? entry->number : 0;
}

static void open_stack_screen(struct apidemo *app, int number, bool replace);

static void stack_command(struct fplinux_ui *ui,
			  struct fplinux_ui_screen *screen, int command_id,
			  void *context)
{
	int number = stack_number(screen);

	switch (command_id) {
	case COMMAND_PUSH:
		open_stack_screen(context, number + 1, false);
		break;
	case COMMAND_REPLACE:
		/* The new screen takes this one's place; this one is deleted
		 * after the call. */
		open_stack_screen(context, number + 1, true);
		break;
	case COMMAND_POP:
		/* false only when this is the only screen. */
		if (!fplinux_ui_pop(ui))
			apidemo_show(context, _("Screen stack"),
				     _("fplinux_ui_pop() returned false: this "
				       "is the only screen."));
		break;
	case APIDEMO_COMMAND_BACK:
		/* Pops, or leaves the application from its first screen. */
		fplinux_ui_go_back(ui);
		break;
	default:
		break;
	}
}

static void open_stack_screen(struct apidemo *app, int number, bool replace)
{
	const struct fplinux_ui_command commands[] = {
		{ .id = COMMAND_PUSH,
		  .type = FPLINUX_UI_COMMAND_OK,
		  .label = _("Push"),
		  .priority = 0,
		  .icon = apidemo_glyph(app, "add") },
		{ .id = COMMAND_REPLACE,
		  .type = FPLINUX_UI_COMMAND_SCREEN,
		  .label = _("Replace"),
		  .priority = 1,
		  .icon = apidemo_glyph(app, "refresh") },
		{ .id = COMMAND_POP,
		  .type = FPLINUX_UI_COMMAND_SCREEN,
		  .label = _("Pop"),
		  .priority = 2,
		  .icon = apidemo_glyph(app, "back") },
	};
	/* The top screen now: the one that pushes, or the one replaced. */
	struct fplinux_ui_screen *top = fplinux_ui_current(app->ui);
	const struct stack_entry *previous = stack_entry(top);
	const struct stack_entry *below =
		replace && previous ? previous->below : previous;
	struct fplinux_ui_screen *form;
	char title[TEXT_BYTES];
	char text[TEXT_BYTES];

	if (number > STACK_LIMIT) {
		apidemo_show(app, _("Screen stack"),
			     _("This demo stops at 32 screens."));
		return;
	}
	/* TRANSLATORS: %d numbers the screens this demo stacks. */
	snprintf(title, sizeof(title), _("Screen %d"), number);
	form = fplinux_ui_form_new(app->ui, title);
	if (!form) {
		apidemo_error(app, "fplinux_ui_form_new");
		return;
	}
	stack_entries[number - 1] = (struct stack_entry){
		.number = number,
		.below = below,
	};
	fplinux_ui_screen_set_data(form, &stack_entries[number - 1]);
	if (below)
		snprintf(text, sizeof(text), _("Screen %d"), below->number);
	else
		snprintf(text, sizeof(text), "%s", _("The list of demos"));
	fplinux_ui_form_append_string(form, _("The screen below"), text);
	if (replace) {
		/* TRANSLATORS: %d is the number of the replaced screen. */
		snprintf(text, sizeof(text),
			 _("Screen %d was replaced and is gone: Back skips "
			   "it."),
			 stack_number(top));
		fplinux_ui_form_append_string(form, NULL, text);
	}
	fplinux_ui_form_append_string(
		form, NULL,
		_("OK pushes a new screen on top. Replace swaps this screen "
		  "for a new one. Pop deletes the top screen, as Back does "
		  "here; from an application's first screen Back leaves the "
		  "application instead."));
	for (size_t i = 0; i < APIDEMO_COUNT(commands); i++)
		fplinux_ui_screen_add_command(form, &commands[i]);
	apidemo_add_back(form);
	fplinux_ui_screen_set_listener(form, stack_command, NULL, app);
	if (replace)
		fplinux_ui_replace(app->ui, form);
	else
		fplinux_ui_push(app->ui, form);
}

static void open_stack(struct apidemo *app, const struct apidemo_entry *entry)
{
	(void)entry;
	open_stack_screen(app, 1, false);
}

/* Visibility and the display */

static struct {
	struct fplinux_ui_screen *form;
	size_t visible;
	size_t lifecycle;
	size_t hidden_lifecycle;
	size_t changes;
	enum fplinux_ui_lifecycle hidden_state;
	unsigned int hidden;
	unsigned int shown;
} visibility_demo;

static const char *lifecycle_name(enum fplinux_ui_lifecycle lifecycle)
{
	switch (lifecycle) {
	case FPLINUX_UI_FOREGROUND:
		return _("Foreground");
	case FPLINUX_UI_BACKGROUND:
		return _("Background");
	case FPLINUX_UI_CLOSING:
		return _("Closing");
	default:
		return "";
	}
}

static void show_visibility(struct apidemo *app)
{
	char text[TEXT_BYTES];

	if (!visibility_demo.form)
		return;
	fplinux_ui_form_set_string(
		visibility_demo.form, visibility_demo.visible,
		fplinux_ui_is_visible(app->ui) ? "true" : "false");
	fplinux_ui_form_set_string(
		visibility_demo.form, visibility_demo.lifecycle,
		lifecycle_name(fplinux_ui_lifecycle(app->ui)));
	fplinux_ui_form_set_string(
		visibility_demo.form, visibility_demo.hidden_lifecycle,
		visibility_demo.hidden ?
			lifecycle_name(visibility_demo.hidden_state) :
			"");
	/* TRANSLATORS: how often the application was hidden and shown. */
	snprintf(text, sizeof(text), _("Hidden %u times, shown %u times"),
		 visibility_demo.hidden, visibility_demo.shown);
	fplinux_ui_form_set_string(visibility_demo.form,
				   visibility_demo.changes, text);
}

static void visibility_command(struct fplinux_ui *ui,
			       struct fplinux_ui_screen *form, int command_id,
			       void *context)
{
	(void)form;
	(void)context;
	if (command_id == APIDEMO_COMMAND_BACK) {
		visibility_demo.form = NULL;
		fplinux_ui_go_back(ui);
	}
}

static void open_visibility(struct apidemo *app,
			    const struct apidemo_entry *entry)
{
	struct fplinux_ui_screen *form =
		fplinux_ui_form_new(app->ui, _(entry->title));

	if (!form) {
		apidemo_error(app, "fplinux_ui_form_new");
		return;
	}
	visibility_demo.form = form;
	visibility_demo.visible = fplinux_ui_form_append_string(
		form, "fplinux_ui_is_visible()", NULL);
	visibility_demo.lifecycle = fplinux_ui_form_append_string(
		form, "fplinux_ui_lifecycle()", NULL);
	visibility_demo.hidden_lifecycle = fplinux_ui_form_append_string(
		form, _("Last hidden state"), NULL);
	visibility_demo.changes = fplinux_ui_form_append_string(
		form, "FPLINUX_UI_EVENT_VISIBILITY", NULL);
	fplinux_ui_form_append_string(
		form, NULL,
		_("Lock the keypad, then return: the application stays foreground. "
		  "Home keeps it in background; Open apps resumes this screen. "
		  "Opening API Demos from the catalogue returns to its main list. "
		  "While hidden, it draws nothing and should pause its UI timers."));
	show_visibility(app);
	apidemo_add_back(form);
	fplinux_ui_screen_set_listener(form, visibility_command, NULL, app);
	fplinux_ui_push(app->ui, form);
}

/* Keep the screen on */

static struct {
	struct fplinux_ui_screen *form;
	size_t choice;
	size_t result;
	bool on;
} screen_on_demo;

static void set_screen_on(struct apidemo *app, bool on)
{
	bool sent = fplinux_ui_keep_screen_on(app->ui, on);
	char text[TEXT_BYTES];

	screen_on_demo.on = sent && on;
	if (!screen_on_demo.form)
		return;
	snprintf(text, sizeof(text), "fplinux_ui_keep_screen_on(%s): %s",
		 on ? "true" : "false", sent ? "true" : "false");
	fplinux_ui_form_set_string(screen_on_demo.form, screen_on_demo.result,
				   text);
}

static void screen_on_changed(struct fplinux_ui *ui,
			      struct fplinux_ui_screen *form, size_t item,
			      void *context)
{
	(void)ui;
	if (item == screen_on_demo.choice)
		set_screen_on(context,
			      fplinux_ui_form_get_choice(form, item) == 1);
}

static void screen_on_command(struct fplinux_ui *ui,
			      struct fplinux_ui_screen *form, int command_id,
			      void *context)
{
	(void)form;
	if (command_id != APIDEMO_COMMAND_BACK)
		return;
	/* Keep the request only while the user watches this screen. */
	if (screen_on_demo.on)
		set_screen_on(context, false);
	screen_on_demo.form = NULL;
	fplinux_ui_go_back(ui);
}

static void open_screen_on(struct apidemo *app,
			   const struct apidemo_entry *entry)
{
	const char *choices[] = { _("Off"), _("On") };
	struct fplinux_ui_screen *form =
		fplinux_ui_form_new(app->ui, _(entry->title));

	if (!form) {
		apidemo_error(app, "fplinux_ui_form_new");
		return;
	}
	screen_on_demo.form = form;
	screen_on_demo.choice = fplinux_ui_form_append_choice(
		form, _("Keep the screen on"), FPLINUX_UI_CHOICE_EXCLUSIVE,
		choices, APIDEMO_COUNT(choices));
	fplinux_ui_form_set_choice(form, screen_on_demo.choice,
				   screen_on_demo.on ? 1 : 0);
	screen_on_demo.result = fplinux_ui_form_append_string(
		form, _("Result"), _("Nothing sent yet."));
	fplinux_ui_form_append_string(
		form, NULL,
		_("While on, the display does not dim, turn off or lock "
		  "while this application is visible. The red key still "
		  "offers Minimize or Close. Back releases the request."));
	apidemo_add_back(form);
	fplinux_ui_screen_set_listener(form, screen_on_command,
				       screen_on_changed, app);
	fplinux_ui_push(app->ui, form);
}

/* Time and timers */

static struct {
	struct fplinux_ui_screen *form;
	lv_timer_t *timer;
	size_t now;
	size_t elapsed;
	uint64_t opened_ms;
} clock_demo;

/*
 * fplinux_ui_now_ms() is CLOCK_MONOTONIC: it measures intervals, also
 * across a hidden period in which the timer pauses.
 */
static void clock_tick(lv_timer_t *timer)
{
	struct apidemo *app = lv_timer_get_user_data(timer);
	uint64_t now = fplinux_ui_now_ms();
	uint64_t elapsed = now - clock_demo.opened_ms;
	char text[TEXT_BYTES];

	snprintf(text, sizeof(text), "%llu", (unsigned long long)now);
	fplinux_ui_form_set_string(clock_demo.form, clock_demo.now, text);
	if (!fplinux_ui_format_number(app->ui, (double)elapsed / MS_PER_SECOND,
				      1, text, sizeof(text)))
		text[0] = '\0';
	fplinux_ui_form_set_string(clock_demo.form, clock_demo.elapsed, text);
}

static void stop_clock(void)
{
	if (clock_demo.timer)
		lv_timer_delete(clock_demo.timer);
	clock_demo.timer = NULL;
	clock_demo.form = NULL;
}

static void clock_command(struct fplinux_ui *ui, struct fplinux_ui_screen *form,
			  int command_id, void *context)
{
	(void)form;
	(void)context;
	if (command_id == APIDEMO_COMMAND_BACK) {
		stop_clock();
		fplinux_ui_go_back(ui);
	}
}

static void open_clock(struct apidemo *app, const struct apidemo_entry *entry)
{
	struct fplinux_ui_screen *form =
		fplinux_ui_form_new(app->ui, _(entry->title));
	char text[TEXT_BYTES];

	if (!form) {
		apidemo_error(app, "fplinux_ui_form_new");
		return;
	}
	clock_demo.form = form;
	clock_demo.opened_ms = fplinux_ui_now_ms();
	clock_demo.now = fplinux_ui_form_append_string(
		form, "fplinux_ui_now_ms()", NULL);
	clock_demo.elapsed = fplinux_ui_form_append_string(
		form, _("Seconds on this screen"), NULL);
	/* The time of day comes from time(), not from the monotonic clock. */
	if (!fplinux_ui_format_date(app->ui, time(NULL), FPLINUX_UI_DATE_CLOCK,
				    text, sizeof(text)))
		text[0] = '\0';
	fplinux_ui_form_append_string(form, _("Opened at"), text);
	fplinux_ui_form_append_string(
		form, NULL,
		_("An LVGL timer updates this screen ten times a second. It "
		  "pauses while the application is hidden and is deleted on "
		  "Back, so that an idle application never wakes up."));
	clock_demo.timer = lv_timer_create(clock_tick, CLOCK_PERIOD_MS, app);
	if (!clock_demo.timer)
		apidemo_error(app, "lv_timer_create");
	else if (!app->visible)
		lv_timer_pause(clock_demo.timer);
	apidemo_add_back(form);
	fplinux_ui_screen_set_listener(form, clock_command, NULL, app);
	fplinux_ui_push(app->ui, form);
	if (clock_demo.timer)
		clock_tick(clock_demo.timer);
}

/* A watched file descriptor */

enum { COMMAND_TOGGLE = 1 };

static struct {
	struct fplinux_ui_screen *form;
	int fd;
	bool watching;
	size_t ticks_item;
	unsigned long long ticks;
} watch_demo = { .fd = -1 };

/*
 * Called on every loop iteration in which the descriptor has events: read
 * the expirations, or the loop spins.
 */
static void timer_ready(struct fplinux_ui *ui, int fd, short events,
			void *context)
{
	uint64_t expirations = 0;
	char text[TEXT_BYTES];

	(void)ui;
	(void)context;
	if (events & (POLLERR | POLLHUP | POLLNVAL)) {
		fplinux_ui_watch_fd(ui, -1, 0, NULL, NULL);
		watch_demo.watching = false;
		return;
	}
	if (read(fd, &expirations, sizeof(expirations)) !=
	    (ssize_t)sizeof(expirations))
		return;
	watch_demo.ticks += expirations;
	if (!watch_demo.form)
		return;
	snprintf(text, sizeof(text), "%llu", watch_demo.ticks);
	fplinux_ui_form_set_string(watch_demo.form, watch_demo.ticks_item,
				   text);
}

static void relabel_watch(struct apidemo *app)
{
	const struct fplinux_ui_command toggle = {
		.id = COMMAND_TOGGLE,
		.type = FPLINUX_UI_COMMAND_OK,
		.label = watch_demo.watching ? _("Stop") : _("Start"),
		.priority = 0,
		.icon = apidemo_glyph(app,
				      watch_demo.watching ? "stop" : "play"),
	};

	/* Adding the same id again replaces the command and its legend. */
	fplinux_ui_screen_add_command(watch_demo.form, &toggle);
}

/* One watch per application: a new call replaces it, -1 stops it. */
static void set_watching(struct apidemo *app, bool watching)
{
	if (watching && watch_demo.fd >= 0) {
		fplinux_ui_watch_fd(app->ui, watch_demo.fd, POLLIN, timer_ready,
				    app);
		watch_demo.watching = true;
	} else {
		fplinux_ui_watch_fd(app->ui, -1, 0, NULL, NULL);
		watch_demo.watching = false;
	}
}

/* Stop watching before closing: the descriptor stays the caller's. */
static void close_watch(struct apidemo *app)
{
	if (watch_demo.fd >= 0) {
		set_watching(app, false);
		close(watch_demo.fd);
	}
	watch_demo.fd = -1;
	watch_demo.form = NULL;
}

static void watch_command(struct fplinux_ui *ui, struct fplinux_ui_screen *form,
			  int command_id, void *context)
{
	(void)form;
	if (command_id == COMMAND_TOGGLE) {
		set_watching(context, !watch_demo.watching);
		relabel_watch(context);
	} else if (command_id == APIDEMO_COMMAND_BACK) {
		close_watch(context);
		fplinux_ui_go_back(ui);
	}
}

static void open_watch(struct apidemo *app, const struct apidemo_entry *entry)
{
	const struct itimerspec second = {
		.it_interval = { .tv_sec = 1 },
		.it_value = { .tv_sec = 1 },
	};
	struct fplinux_ui_screen *form =
		fplinux_ui_form_new(app->ui, _(entry->title));

	if (!form) {
		apidemo_error(app, "fplinux_ui_form_new");
		return;
	}
	close_watch(app);
	watch_demo.fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
	if (watch_demo.fd < 0 ||
	    timerfd_settime(watch_demo.fd, 0, &second, NULL) != 0) {
		apidemo_error(app, "timerfd_create");
		if (watch_demo.fd >= 0)
			close(watch_demo.fd);
		watch_demo.fd = -1;
	}
	watch_demo.form = form;
	watch_demo.ticks = 0;
	watch_demo.ticks_item =
		fplinux_ui_form_append_string(form, _("Ticks"), "0");
	fplinux_ui_form_append_string(
		form, NULL,
		_("A timerfd expires every second; fplinux_ui_watch_fd() adds "
		  "it to the event loop with POLLIN. Unlike LVGL timers, the "
		  "watch keeps running while the application is hidden."));
	set_watching(app, true);
	relabel_watch(app);
	apidemo_add_back(form);
	fplinux_ui_screen_set_listener(form, watch_command, NULL, app);
	fplinux_ui_push(app->ui, form);
}

/* The window title */

enum { COMMAND_SEND = 1 };

static struct {
	struct fplinux_ui_screen *form;
	size_t field;
	size_t result;
} title_demo;

/* Sends the chosen title and shows what fplinux_ui_set_title() returned. */
static void send_title(struct fplinux_ui *ui)
{
	const char *title =
		fplinux_ui_form_get_text(title_demo.form, title_demo.field);
	char text[TEXT_BYTES];
	bool sent = fplinux_ui_set_title(ui, title ? title : "");

	snprintf(text, sizeof(text), "%s%s%s", sent ? "true" : "false",
		 sent ? "" : ", ", sent ? "" : apidemo_errno_name(errno));
	fplinux_ui_form_set_string(title_demo.form, title_demo.result, text);
}

static void title_command(struct fplinux_ui *ui, struct fplinux_ui_screen *form,
			  int command_id, void *context)
{
	(void)form;
	(void)context;
	if (command_id == COMMAND_SEND) {
		send_title(ui);
	} else if (command_id == APIDEMO_COMMAND_BACK) {
		title_demo.form = NULL;
		fplinux_ui_go_back(ui);
	}
}

static void open_title(struct apidemo *app, const struct apidemo_entry *entry)
{
	const struct fplinux_ui_command send = {
		.id = COMMAND_SEND,
		.type = FPLINUX_UI_COMMAND_SCREEN,
		.label = _("Send"),
		.priority = 0,
		.icon = apidemo_glyph(app, "send"),
	};
	struct fplinux_ui_screen *form =
		fplinux_ui_form_new(app->ui, _(entry->title));

	if (!form) {
		apidemo_error(app, "fplinux_ui_form_new");
		return;
	}
	title_demo.form = form;
	title_demo.field = fplinux_ui_form_append_text_field(
		form, _("Title"), _("API Demos"), TITLE_CHARS,
		FPLINUX_UI_TEXT_ANY);
	title_demo.result = fplinux_ui_form_append_string(
		form, "fplinux_ui_set_title()", _("Nothing sent yet."));
	fplinux_ui_form_append_string(
		form, NULL,
		_("Edit the window title, then Send. The system receives the "
		  "title; the application's own screen title stays separate."));
	fplinux_ui_screen_add_command(form, &send);
	apidemo_add_back(form);
	fplinux_ui_screen_set_listener(form, title_command, NULL, app);
	fplinux_ui_push(app->ui, form);
}

/* Leaving the application */

enum leave_row {
	LEAVE_BACK,
	LEAVE_HOME,
	LEAVE_QUIT,
	LEAVE_COUNT,
};

enum {
	COMMAND_LEAVE = 1,
	COMMAND_STAY,
};

static const struct {
	const char *text;
	const char *detail;
	const char *question;
} leave_rows[LEAVE_COUNT] = {
	[LEAVE_BACK] = { N_("Back to the catalogue"), "fplinux_ui_back",
			 N_("Leave for the launcher's catalogue? The "
			    "application gets CLOSE and saves.") },
	[LEAVE_HOME] = { N_("Home"), "fplinux_ui_home",
			 N_("Return to the Home view? "
			    "The application stays open in background.") },
	[LEAVE_QUIT] = { N_("Quit without CLOSE"), "fplinux_ui_quit",
			 N_("End fplinux_ui_run() without a CLOSE event? "
			    "Applications use this after a fatal error.") },
};

static enum leave_row leaving;

/* The confirmation has gone when its listener runs. */
static void leave_command(struct fplinux_ui *ui,
			  struct fplinux_ui_screen *alert, int command_id,
			  void *context)
{
	struct apidemo *app = context;

	(void)alert;
	app->decision = NULL;
	if (command_id != COMMAND_LEAVE)
		return;
	switch (leaving) {
	case LEAVE_BACK:
		if (!fplinux_ui_back(ui))
			apidemo_error(app, "fplinux_ui_back");
		break;
	case LEAVE_HOME:
		if (!fplinux_ui_home(ui))
			apidemo_error(app, "fplinux_ui_home");
		break;
	case LEAVE_QUIT:
		/* No CLOSE follows: save before fplinux_ui_run() returns. */
		if (app->prefs)
			apidemo_save_prefs(app, false);
		fplinux_ui_quit(ui);
		break;
	case LEAVE_COUNT:
	default:
		break;
	}
}

static void leave_list_command(struct fplinux_ui *ui,
			       struct fplinux_ui_screen *list, int command_id,
			       void *context)
{
	const struct fplinux_ui_command leave = {
		.id = COMMAND_LEAVE,
		.type = FPLINUX_UI_COMMAND_OK,
		.label = _("Leave"),
		.priority = 0,
		.icon = apidemo_glyph(context, "exit"),
	};
	const struct fplinux_ui_command stay = {
		.id = COMMAND_STAY,
		.type = FPLINUX_UI_COMMAND_CANCEL,
		.label = NULL,
		.priority = 0,
	};
	size_t row = fplinux_ui_list_get_focused(list);
	struct fplinux_ui_screen *alert;

	if (command_id == APIDEMO_COMMAND_BACK) {
		fplinux_ui_go_back(ui);
		return;
	}
	if (command_id != FPLINUX_UI_SELECT_COMMAND || row >= LEAVE_COUNT)
		return;
	leaving = (enum leave_row)row;
	alert = fplinux_ui_alert_new(ui, FPLINUX_UI_ALERT_CONFIRMATION,
				     _(leave_rows[row].text),
				     _(leave_rows[row].question), 0);
	if (!alert) {
		apidemo_error(context, "fplinux_ui_alert_new");
		return;
	}
	fplinux_ui_screen_add_command(alert, &leave);
	fplinux_ui_screen_add_command(alert, &stay);
	fplinux_ui_screen_set_listener(alert, leave_command, NULL, context);
	((struct apidemo *)context)->decision = alert;
	fplinux_ui_push(ui, alert);
}

static void open_leave(struct apidemo *app, const struct apidemo_entry *entry)
{
	struct fplinux_ui_screen *list = fplinux_ui_list_new(
		app->ui, _(entry->title), FPLINUX_UI_LIST_IMPLICIT);

	if (!list) {
		apidemo_error(app, "fplinux_ui_list_new");
		return;
	}
	for (size_t i = 0; i < LEAVE_COUNT; i++)
		fplinux_ui_list_append(list, _(leave_rows[i].text),
				       leave_rows[i].detail);
	apidemo_add_back(list);
	fplinux_ui_screen_set_listener(list, leave_list_command, NULL, app);
	fplinux_ui_push(app->ui, list);
}

/* Hooks */

static void lifecycle_visibility(struct apidemo *app, bool visible)
{
	if (visible)
		visibility_demo.shown++;
	else {
		visibility_demo.hidden++;
		visibility_demo.hidden_state = fplinux_ui_lifecycle(app->ui);
	}
	show_visibility(app);
	/* Pause timers while hidden; the clock catches up from now_ms. */
	if (clock_demo.timer) {
		if (visible) {
			lv_timer_resume(clock_demo.timer);
			clock_tick(clock_demo.timer);
		} else {
			lv_timer_pause(clock_demo.timer);
		}
	}
}

static void lifecycle_cleanup(struct apidemo *app)
{
	stop_clock();
	close_watch(app);
	if (screen_on_demo.on)
		set_screen_on(app, false);
	visibility_demo.form = NULL;
	screen_on_demo.form = NULL;
	title_demo.form = NULL;
}

static const struct apidemo_entry entries[] = {
	{ N_("Screen stack"),
	  "fplinux_ui_push · fplinux_ui_replace · fplinux_ui_pop · fplinux_ui_go_back",
	  open_stack, 0, N_("Push, replace and return between screens") },
	{ N_("Visibility"),
	  "fplinux_ui_is_visible · FPLINUX_UI_EVENT_VISIBILITY · "
	  "FPLINUX_UI_EVENT_ACTIVATION · FPLINUX_UI_ACTIVATION_OPEN · "
	  "FPLINUX_UI_ACTIVATION_RESUME",
	  open_visibility, 0, N_("Observe Home and return to the task") },
	{ N_("Keep the screen on"), "fplinux_ui_keep_screen_on", open_screen_on,
	  0, N_("Request wakefulness while visible") },
	{ N_("Time and timers"), "fplinux_ui_now_ms · lv_timer · time",
	  open_clock, 0, N_("Pause updates while hidden") },
	{ N_("Watched descriptor"), "fplinux_ui_watch_fd · timerfd · POLLIN",
	  open_watch, 0, N_("Watch and release a timer descriptor") },
	{ N_("Window title"), "fplinux_ui_set_title", open_title, 0,
	  N_("Send a title to the system") },
	{ N_("Leaving the application"),
	  "fplinux_ui_back · fplinux_ui_home · fplinux_ui_quit", open_leave, 0,
	  N_("Try Home, return and ordinary closing") },
};

const struct apidemo_category apidemo_lifecycle = {
	.entries = entries,
	.count = APIDEMO_COUNT(entries),
	.visibility = lifecycle_visibility,
	.cleanup = lifecycle_cleanup,
};
