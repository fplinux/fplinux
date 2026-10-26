/* SPDX-License-Identifier: GPL-2.0-only */
#include "clock-client.h"
#include "clock-presentation.h"
#include "fplinux-cli.h"

#include <fplinux/fplinux-ui-menu.h>
#include <fplinux/fplinux-ui-locale.h>
#include <fplinux/fplinux-ui-screen.h>
#include <libintl.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define APP_ID "org.fplinux.clock"
#define _(text) gettext(text)
#define COUNT(a) (sizeof(a) / sizeof((a)[0]))
#define VIEW_LIMIT 8

enum view_kind {
	VIEW_HOME,
	VIEW_FACE,
	VIEW_ALARMS,
	VIEW_ALARM_EDIT,
	VIEW_LABEL,
	VIEW_REPEAT,
	VIEW_DAYS,
	VIEW_TIMER,
	VIEW_TIMER_EDIT,
	VIEW_STOPWATCH,
	VIEW_LAPS,
	VIEW_WORLD,
	VIEW_OFFSET,
	VIEW_OFFSET_CHOICE,
	VIEW_SETTINGS,
	VIEW_FACE_CHOICE,
	VIEW_SNOOZE_CHOICE,
	VIEW_MISSED,
	VIEW_HELP,
};

enum command_id {
	CMD_BACK = 1,
	CMD_SAVE,
	CMD_EDIT,
	CMD_DELETE,
	CMD_TOGGLE,
	CMD_LABEL,
	CMD_REPEAT,
	CMD_START,
	CMD_PAUSE,
	CMD_RESUME,
	CMD_STOP,
	CMD_RESTART,
	CMD_RESET,
	CMD_LAP,
	CMD_LAPS,
	CMD_CLEAR,
	CMD_SHOW_FACE,
	CMD_SETTINGS,
	CMD_MISSED,
	CMD_HELP,
	CMD_CONFIRM,
	CMD_STAY,
	CMD_CLOSE,
	CMD_RETRY,
	CMD_CANCEL_SNOOZE,
};

enum confirmation {
	CONFIRM_NONE,
	CONFIRM_REQUEST,
	CONFIRM_ACTIVE,
	CONFIRM_LEAVE,
	CONFIRM_RETRY,
	CONFIRM_TIMER,
};

enum leave_destination {
	LEAVE_BACK,
	LEAVE_HOME,
	LEAVE_CLOSE,
};

struct clock_screen {
	struct fplinux_ui_screen *screen;
	enum view_kind kind;
	uint64_t subject;
	uint64_t token;
	uint64_t ids[CLOCK_LAP_LIMIT];
	size_t rows;
	size_t items[8];
	int command_state;
	bool commands_initialized;
	bool offset_saved;
	lv_obj_t *time_label;
	lv_obj_t *date_label;
	lv_obj_t *dial;
	lv_obj_t *hands[3];
	lv_point_precise_t points[3][2];
};

struct alarm_draft {
	struct clock_alarm_spec spec;
	time_t date;
	time_t time;
	bool date_chosen;
	bool dirty;
	uint64_t revision;
};

struct clock_app {
	struct fplinux_ui *ui;
	struct clock_client client;
	struct clock_screen views[VIEW_LIMIT];
	size_t depth;
	struct fplinux_ui_screen *alert;
	enum confirmation confirmation;
	struct clock_request confirmed_request;
	uint64_t confirmation_token;
	uint64_t confirmation_draft_revision;
	uint64_t next_view_token;
	uint64_t pending_view_token;
	uint64_t pending_draft_revision;
	uint64_t timer_draft_revision;
	uint64_t save_view_token;
	uint64_t save_draft_revision;
	bool pending_close;
	bool retrying;
	bool waiting_retry;
	bool save_failed;
	bool save_applied;
	struct alarm_draft alarm;
	uint32_t pending_first_alarm_id;
	uint32_t focus_alarm_id;
	uint32_t focus_world_id;
	char feedback[180];
	lv_timer_t *poll_timer;
	uint64_t last_poll_ms;
	bool closing;
	enum leave_destination leave_destination;
	bool home_requested;
};

static void command(struct fplinux_ui *, struct fplinux_ui_screen *, int,
		    void *);
static void changed(struct fplinux_ui *, struct fplinux_ui_screen *, size_t,
		    void *);
static void edit_accepted(struct fplinux_ui *, struct fplinux_ui_screen *,
			  size_t, void *);
static void refresh_current(struct clock_app *);
static void open_view(struct clock_app *, enum view_kind, uint64_t);
static void submit(struct clock_app *, const struct clock_request *, bool);
static void return_home(struct clock_app *);
static void open_home(struct clock_app *);

static struct clock_screen *top(struct clock_app *app)
{
	return app->depth ? &app->views[app->depth - 1] : NULL;
}

static const struct clock_model *model(const struct clock_app *app)
{
	return &app->client.snapshot;
}

static bool is_active(const struct clock_app *app)
{
	return !app->closing && fplinux_ui_is_visible(app->ui) &&
	       fplinux_ui_lifecycle(app->ui) == FPLINUX_UI_FOREGROUND;
}

static bool mutation_pending(const struct clock_app *app)
{
	return app->client.pending &&
	       app->client.request.op != CLOCK_REQUEST_SNAPSHOT &&
	       app->client.request.op != CLOCK_REQUEST_SAVE;
}

static void add_command(struct fplinux_ui *ui, struct clock_screen *view,
			int id, enum fplinux_ui_command_type type,
			const char *label, const char *glyph, int priority,
			bool keep_visible)
{
	const struct fplinux_ui_command value = {
		.id = id,
		.type = type,
		.label = label,
		.icon = fplinux_ui_glyph(ui, glyph, FPLINUX_UI_GLYPH_ROW),
		.priority = priority,
		.keep_visible = keep_visible,
	};

	fplinux_ui_screen_add_command(view->screen, &value);
}

static void item_command(struct fplinux_ui *ui, struct clock_screen *view,
			 size_t item, int id)
{
	const struct fplinux_ui_command value = {
		.id = id,
		.type = FPLINUX_UI_COMMAND_OK,
		.label = _("Edit"),
		.icon = fplinux_ui_glyph(ui, "edit", FPLINUX_UI_GLYPH_ROW),
	};

	fplinux_ui_form_set_appearance(view->screen, item,
				       FPLINUX_UI_APPEARANCE_BUTTON);
	fplinux_ui_form_add_item_command(view->screen, item, &value);
}

static void editor_commands(struct fplinux_ui *ui, struct clock_screen *view,
			    const char *save, const char *glyph)
{
	add_command(ui, view, CMD_SAVE, FPLINUX_UI_COMMAND_SCREEN, save, glyph,
		    0, true);
	add_command(ui, view, CMD_BACK, FPLINUX_UI_COMMAND_CANCEL, _("Cancel"),
		    "cancel", 0, true);
}

static void notice(struct clock_app *app, const char *title, const char *text)
{
	const struct fplinux_ui_command close = {
		.id = CMD_CLOSE,
		.type = FPLINUX_UI_COMMAND_BACK,
		.label = _("Close"),
		.icon = fplinux_ui_glyph(app->ui, "back", FPLINUX_UI_GLYPH_ROW),
	};

	if (app->alert || app->closing)
		return;
	app->confirmation = CONFIRM_NONE;
	app->alert = fplinux_ui_alert_new(app->ui, FPLINUX_UI_ALERT_INFO, title,
					  text, 0);
	if (!app->alert)
		return;
	fplinux_ui_screen_set_listener(app->alert, command, NULL, app);
	fplinux_ui_screen_add_command(app->alert, &close);
	fplinux_ui_push(app->ui, app->alert);
}

static void confirm(struct clock_app *app, const char *text,
		    const struct clock_request *request,
		    enum confirmation action)
{
	const struct fplinux_ui_command yes = {
		.id = CMD_CONFIRM,
		.type = FPLINUX_UI_COMMAND_SCREEN,
		.label = action == CONFIRM_LEAVE ? _("Leave") :
			 action == CONFIRM_RETRY ? _("Retry") :
						   _("Confirm"),
		.keep_visible = true,
		.icon = fplinux_ui_glyph(app->ui,
					 action == CONFIRM_LEAVE ? "exit" :
					 action == CONFIRM_RETRY ? "refresh" :
								   "check",
					 FPLINUX_UI_GLYPH_ROW),
	};
	const struct fplinux_ui_command stay = {
		.id = CMD_STAY,
		.type = FPLINUX_UI_COMMAND_OK,
		.label = action == CONFIRM_LEAVE ? _("Stay") : _("Confirm"),
		.icon = fplinux_ui_glyph(
			app->ui, action == CONFIRM_LEAVE ? "cancel" : "check",
			FPLINUX_UI_GLYPH_ROW),
	};
	const struct fplinux_ui_command cancel = {
		.id = CMD_CLOSE,
		.type = FPLINUX_UI_COMMAND_CANCEL,
		.label = _("Cancel"),
		.icon = fplinux_ui_glyph(app->ui, "cancel",
					 FPLINUX_UI_GLYPH_ROW),
	};

	if (app->alert || app->closing)
		return;
	app->confirmation = action;
	app->confirmation_token = top(app) ? top(app)->token : 0;
	if (action == CONFIRM_ACTIVE)
		app->confirmation_draft_revision = app->pending_draft_revision;
	else if (action == CONFIRM_TIMER)
		app->confirmation_draft_revision = app->timer_draft_revision;
	if (request)
		app->confirmed_request = *request;
	app->alert = fplinux_ui_alert_new(
		app->ui, FPLINUX_UI_ALERT_CONFIRMATION, _("Clock"), text, 0);
	if (!app->alert)
		return;
	fplinux_ui_screen_set_listener(app->alert, command, NULL, app);
	fplinux_ui_screen_add_command(app->alert, &yes);
	if (action == CONFIRM_LEAVE)
		fplinux_ui_screen_add_command(app->alert, &stay);
	fplinux_ui_screen_add_command(app->alert, &cancel);
	fplinux_ui_push(app->ui, app->alert);
}

static bool editor_dirty(const struct clock_app *app)
{
	if (mutation_pending(app))
		return true;
	for (size_t index = 0; index < app->depth; ++index) {
		const struct clock_screen *view = &app->views[index];

		if (fplinux_ui_form_is_editing(view->screen))
			return true;
		if (view->kind == VIEW_ALARM_EDIT && app->alarm.dirty)
			return true;
		if (app->save_failed &&
		    view->token == app->pending_view_token &&
		    (view->kind == VIEW_TIMER_EDIT ||
		     view->kind == VIEW_OFFSET_CHOICE ||
		     view->kind == VIEW_FACE_CHOICE ||
		     view->kind == VIEW_SNOOZE_CHOICE))
			return true;
		if (view->kind == VIEW_TIMER_EDIT &&
		    fplinux_ui_form_get_duration(view->screen,
						 view->items[0]) !=
			    app->client.snapshot.timer.duration_seconds)
			return true;
		if (view->kind == VIEW_LABEL) {
			const char *text = fplinux_ui_form_get_text(
				view->screen, view->items[0]);

			if (text && strcmp(text, app->alarm.spec.label))
				return true;
		}
		if (view->kind == VIEW_DAYS)
			for (size_t day = 0; day < 7; ++day)
				if (fplinux_ui_list_is_selected(view->screen,
								day) !=
				    !!(app->alarm.spec.repeat_days &
				       (1U << day)))
					return true;
	}
	return false;
}

static enum fplinux_ui_close_response prepare_close(struct fplinux_ui *ui,
						    void *context)
{
	struct clock_app *app = context;
	(void)ui;

	if (!editor_dirty(app))
		return FPLINUX_UI_CLOSE_READY;
	if (app->alert) {
		fplinux_ui_pop(app->ui);
		app->alert = NULL;
		app->confirmation = CONFIRM_NONE;
	}
	confirm(app, _("Leave without saving?"), NULL, CONFIRM_LEAVE);
	app->leave_destination = LEAVE_CLOSE;
	return app->alert ? FPLINUX_UI_CLOSE_WAIT : FPLINUX_UI_CLOSE_CANCEL;
}

static const char *request_error(enum clock_request_op op)
{
	switch (op) {
	case CLOCK_REQUEST_ALARM_ADD:
	case CLOCK_REQUEST_ALARM_EDIT:
	case CLOCK_REQUEST_ALARM_ENABLE:
		return _("Could not save the alarm. Try again.");
	case CLOCK_REQUEST_ALARM_DELETE:
		return _("Could not delete the alarm. Try again.");
	case CLOCK_REQUEST_ALARM_CANCEL_SNOOZE:
		return _("Could not cancel the snooze. Try again.");
	case CLOCK_REQUEST_TIMER_START:
	case CLOCK_REQUEST_TIMER_RESTART:
	case CLOCK_REQUEST_TIMER_RESUME:
		return _("Could not start the timer. Try again.");
	case CLOCK_REQUEST_TIMER_SET:
		return _("Could not save the duration. Try again.");
	case CLOCK_REQUEST_WORLD_ADD:
		return _("Could not add the UTC offset. Try again.");
	case CLOCK_REQUEST_WORLD_EDIT:
		return _("Could not save the UTC offset. Try again.");
	case CLOCK_REQUEST_TIMER_PAUSE:
		return _("Could not pause the timer. Try again.");
	case CLOCK_REQUEST_TIMER_STOP:
		return _("Could not stop the timer. Try again.");
	default:
		return _("Could not apply the change. Try again.");
	}
}

static void duration(uint64_t ms, bool hundredths, char *text, size_t capacity)
{
	unsigned long long seconds = (unsigned long long)(ms / 1000);

	if (hundredths && seconds < 3600)
		snprintf(text, capacity, "%02llu:%02llu.%02llu", seconds / 60,
			 seconds % 60, (unsigned long long)(ms / 10 % 100));
	else if (hundredths)
		snprintf(text, capacity, "%02llu:%02llu:%02llu.%02llu",
			 seconds / 3600, seconds / 60 % 60, seconds % 60,
			 (unsigned long long)(ms / 10 % 100));
	else
		snprintf(text, capacity, "%02llu:%02llu:%02llu", seconds / 3600,
			 seconds / 60 % 60, seconds % 60);
}

static const char *repeat_name(unsigned int days)
{
	switch (days) {
	case 0:
		return _("Once");
	case 127:
		return _("Daily");
	case 31:
		return _("Mon–Fri");
	case 96:
		return _("Sat–Sun");
	default:
		return _("Choose days");
	}
}

static void repeat_text(unsigned int days, char *text, size_t capacity)
{
	static const char *const names[] = {
		"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun",
	};
	size_t used = 0;

	if (days == 0 || days == 127 || days == 31 || days == 96) {
		snprintf(text, capacity, "%s", repeat_name(days));
		return;
	}
	text[0] = '\0';
	for (size_t i = 0; i < COUNT(names); ++i) {
		int written;

		if (!(days & (1U << i)))
			continue;
		written = snprintf(text + used, capacity - used, "%s%s",
				   used ? " " : "", _(names[i]));
		if (written < 0 || (size_t)written >= capacity - used)
			break;
		used += (size_t)written;
	}
}

static const struct clock_world_entry *
find_world_entry(const struct clock_model *clock, uint64_t id)
{
	for (size_t i = 0; i < clock->world_count; ++i)
		if (clock->world[i].id == id)
			return &clock->world[i];
	return NULL;
}

static void world_save_accepted(struct clock_app *app, uint32_t id,
				int offset_minutes, bool durable)
{
	const struct clock_model *clock = model(app);
	const struct clock_world_entry *entry = NULL;

	for (size_t i = 0; i < clock->world_count; ++i)
		if ((id && clock->world[i].id == id) ||
		    (!id && clock->world[i].offset_minutes == offset_minutes)) {
			entry = &clock->world[i];
			break;
		}
	if (!entry)
		return;
	app->focus_world_id = entry->id;
	for (size_t i = 0; i < app->depth; ++i)
		if (app->views[i].token == app->pending_view_token &&
		    app->views[i].kind == VIEW_OFFSET_CHOICE) {
			/* A late acknowledgement can arrive under a retry alert. */
			app->views[i].subject = entry->id;
			app->views[i].offset_saved = durable;
			break;
		}
}

static void alarm_save_accepted(struct clock_app *app,
				const struct clock_request *request,
				bool durable)
{
	app->focus_alarm_id = request->id;
	if (!request->id)
		for (size_t index = 0; index < model(app)->alarm_count; ++index)
			if (model(app)->alarms[index].id >=
			    app->pending_first_alarm_id)
				app->focus_alarm_id =
					model(app)->alarms[index].id;
	for (size_t index = 0; index < app->depth; ++index) {
		struct clock_screen *view = &app->views[index];

		if (view->token != app->pending_view_token ||
		    view->kind != VIEW_ALARM_EDIT)
			continue;
		view->subject = app->focus_alarm_id;
		fplinux_ui_screen_set_title(view->screen, _("Edit alarm"));
		if (durable &&
		    app->alarm.revision == app->pending_draft_revision)
			app->alarm.dirty = false;
		else
			app->pending_close = false;
	}
}

static void pop_view(struct clock_app *app, bool back)
{
	if (app->depth <= 1) {
		fplinux_ui_go_back(app->ui);
		return;
	}
	memset(&app->views[--app->depth], 0, sizeof(app->views[0]));
	if (back)
		fplinux_ui_go_back(app->ui);
	else
		fplinux_ui_pop(app->ui);
	refresh_current(app);
}

static void return_home(struct clock_app *app)
{
	fplinux_ui_menu_close(app->ui);
	while (app->depth > 1)
		pop_view(app, false);
	app->home_requested = false;
}

static void open_home(struct clock_app *app)
{
	if (app->alert)
		return;
	if (app->client.pending &&
	    app->client.request.op != CLOCK_REQUEST_SNAPSHOT) {
		app->home_requested = true;
		return;
	}
	if (editor_dirty(app)) {
		app->leave_destination = LEAVE_HOME;
		confirm(app, _("Return to the start without saving?"), NULL,
			CONFIRM_LEAVE);
		return;
	}
	return_home(app);
}

static struct clock_screen *new_view(struct clock_app *app, enum view_kind kind,
				     uint64_t subject, const char *title,
				     int list_kind, bool canvas)
{
	struct clock_screen *view;

	if (app->depth == VIEW_LIMIT)
		return NULL;
	view = &app->views[app->depth];
	memset(view, 0, sizeof(*view));
	view->kind = kind;
	view->subject = subject;
	view->token = ++app->next_view_token;
	view->screen = canvas ? fplinux_ui_canvas_new(app->ui, title) :
		       list_kind >= 0 ?
				fplinux_ui_list_new(app->ui, title, list_kind) :
				fplinux_ui_form_new(app->ui, title);
	if (!view->screen)
		return NULL;
	++app->depth;
	fplinux_ui_screen_set_listener(view->screen, command, changed, app);
	return view;
}

static size_t string_item(struct clock_screen *view, size_t slot,
			  const char *label, const char *text)
{
	view->items[slot] =
		fplinux_ui_form_append_string(view->screen, label, text);
	return view->items[slot];
}

static bool draft_calendar(struct clock_app *app, struct clock_alarm_spec *spec)
{
	struct clock_local_time date, time;

	*spec = app->alarm.spec;
	if (clock_model_utc_time(app->alarm.date, &date) != CLOCK_OK ||
	    clock_model_utc_time(app->alarm.time, &time) != CLOCK_OK)
		return false;
	if (spec->schedule == CLOCK_ALARM_WEEKLY) {
		spec->hour = time.hour;
		spec->minute = time.minute;
		spec->once_utc = 0;
		return spec->repeat_days != 0;
	}
	date.hour = time.hour;
	date.minute = time.minute;
	date.second = 0;
	spec->hour = 0;
	spec->minute = 0;
	spec->repeat_days = 0;
	return clock_model_utc_from_calendar(&date, &spec->once_utc) ==
	       CLOCK_OK;
}

static void nearest_date(struct clock_app *app)
{
	struct clock_local_time now, chosen;
	int64_t utc;

	if (clock_model_utc_time(model(app)->wall_utc, &now) != CLOCK_OK ||
	    clock_model_utc_time(app->alarm.time, &chosen) != CLOCK_OK)
		return;
	now.hour = chosen.hour;
	now.minute = chosen.minute;
	now.second = 0;
	if (clock_model_utc_from_calendar(&now, &utc) != CLOCK_OK)
		return;
	if (utc <= model(app)->wall_utc)
		utc += 86400;
	if (utc <= CLOCK_WALL_MAX)
		app->alarm.date = (time_t)utc;
}

static void draft_capture(struct clock_app *app, struct clock_screen *view)
{
	if (view->kind != VIEW_ALARM_EDIT)
		return;
	app->alarm.time =
		fplinux_ui_form_get_date(view->screen, view->items[0]);
	if (app->alarm.spec.schedule == CLOCK_ALARM_ONCE)
		app->alarm.date =
			fplinux_ui_form_get_date(view->screen, view->items[2]);
	app->alarm.spec.enabled =
		fplinux_ui_form_get_choice(view->screen, view->items[6]) == 1;
}

static void alarm_summary(struct clock_app *app, char *text, size_t capacity)
{
	struct clock_alarm_spec spec;
	int64_t due;
	char day[100], time[32];

	if (!app->alarm.spec.enabled) {
		snprintf(text, capacity, "%s",
			 _("Off. This alarm will not ring."));
		return;
	}

	if (!draft_calendar(app, &spec) ||
	    clock_model_alarm_next_due(&spec, model(app)->wall_utc, &due) !=
		    CLOCK_OK) {
		snprintf(text, capacity, "%s",
			 _("Choose a future date and time"));
		return;
	}
	clock_presentation_when(app->ui, due, model(app)->wall_utc, true, day,
				sizeof(day));
	clock_presentation_time(app->ui, due, false, time, sizeof(time));
	snprintf(text, capacity, "%s, %s", day, time);
}

static void alarm_form_refresh(struct clock_app *app, struct clock_screen *view,
			       bool rebuild)
{
	char repeat[128], summary[160];

	repeat_text(app->alarm.spec.repeat_days, repeat, sizeof(repeat));
	alarm_summary(app, summary, sizeof(summary));
	if (rebuild) {
		const char *const states[] = { _("Disabled"), _("Enabled") };

		fplinux_ui_form_delete_all(view->screen);
		view->items[0] = fplinux_ui_form_append_date(view->screen,
							     _("Time"),
							     FPLINUX_UI_TIME,
							     app->alarm.time);
		string_item(view, 1, _("Repeat"), repeat);
		item_command(app->ui, view, view->items[1], CMD_REPEAT);
		view->items[2] = SIZE_MAX;
		if (app->alarm.spec.schedule == CLOCK_ALARM_ONCE)
			view->items[2] = fplinux_ui_form_append_date(
				view->screen, _("Date"), FPLINUX_UI_DATE,
				app->alarm.date);
		string_item(view, 3, NULL, summary);
		string_item(view, 4, _("Name"),
			    app->alarm.spec.label[0] ? app->alarm.spec.label :
						       _("Alarm"));
		item_command(app->ui, view, view->items[4], CMD_LABEL);
		string_item(view, 5, NULL, app->feedback);
		view->items[6] = fplinux_ui_form_append_choice(
			view->screen, _("Alarm state"), FPLINUX_UI_CHOICE_POPUP,
			states, COUNT(states));
		fplinux_ui_form_set_choice(view->screen, view->items[6],
					   app->alarm.spec.enabled ? 1 : 0);
	} else {
		fplinux_ui_form_set_string(view->screen, view->items[1],
					   repeat);
		fplinux_ui_form_set_string(view->screen, view->items[3],
					   summary);
		fplinux_ui_form_set_string(view->screen, view->items[4],
					   app->alarm.spec.label[0] ?
						   app->alarm.spec.label :
						   _("Alarm"));
		fplinux_ui_form_set_string(view->screen, view->items[5],
					   app->feedback);
	}
	add_command(app->ui, view, CMD_SAVE, FPLINUX_UI_COMMAND_SCREEN,
		    mutation_pending(app) ? app->waiting_retry ? _("Retry") :
					    app->retrying ? _("Trying again…") :
							    _("Saving…") :
		    app->save_failed	  ? _("Retry") :
					    _("Save"),
		    app->waiting_retry || app->retrying || app->save_failed ?
			    "refresh" :
			    "save",
		    0, true);
}

static void list_update(struct clock_screen *view, size_t count,
			const uint64_t *ids, char text[][180],
			char detail[][180])
{
	size_t previous = fplinux_ui_list_get_focused(view->screen);
	uint64_t selected = previous < view->rows ? view->ids[previous] : 0;
	bool rebuild = view->rows != count;
	size_t focus = previous == SIZE_MAX ? 0 : previous;

	for (size_t i = 0; i < count && !rebuild; ++i)
		if (view->ids[i] != ids[i])
			rebuild = true;
	if (rebuild)
		fplinux_ui_list_delete_all(view->screen);
	for (size_t i = 0; i < count; ++i) {
		if (rebuild)
			fplinux_ui_list_append(view->screen, text[i],
					       detail[i]);
		else
			fplinux_ui_list_set(view->screen, i, text[i],
					    detail[i]);
		view->ids[i] = ids[i];
		if (selected && selected == ids[i])
			focus = i;
	}
	view->rows = count;
	if (count && rebuild)
		fplinux_ui_list_set_focused(view->screen,
					    focus < count ? focus : count - 1);
}

static uint64_t selected_id(struct clock_screen *view)
{
	size_t index = fplinux_ui_list_get_focused(view->screen);

	return index < view->rows ? view->ids[index] : 0;
}

static void lists_refresh(struct clock_app *app, struct clock_screen *view)
{
	const struct clock_model *clock = model(app);
	char text[CLOCK_LAP_LIMIT][180] = { { 0 } };
	char detail[CLOCK_LAP_LIMIT][180] = { { 0 } };
	uint64_t ids[CLOCK_LAP_LIMIT] = { 0 };
	size_t count = 0;
	char time[48], day[100], repeat[128], delta[48];

	if (view->kind == VIEW_ALARMS) {
		count = clock->alarm_count + 1;
		snprintf(text[0], sizeof(text[0]), "%s", _("New alarm"));
		for (size_t i = 0; i < clock->alarm_count; ++i) {
			const struct clock_alarm *alarm = &clock->alarms[i];
			int64_t due = alarm->spec.schedule == CLOCK_ALARM_ONCE ?
					      alarm->spec.once_utc :
					      alarm->next_due_utc;
			size_t row = i + 1;

			ids[row] = alarm->id;
			if (alarm->spec.schedule == CLOCK_ALARM_WEEKLY) {
				snprintf(time, sizeof(time), "%02u:%02u",
					 alarm->spec.hour, alarm->spec.minute);
				repeat_text(alarm->spec.repeat_days, repeat,
					    sizeof(repeat));
			} else {
				clock_presentation_time(app->ui, due, false,
							time, sizeof(time));
				clock_presentation_when(app->ui, due,
							clock->wall_utc, false,
							repeat, sizeof(repeat));
			}
			snprintf(text[row], sizeof(text[row]), "%s  %s", time,
				 alarm->spec.label[0] ? alarm->spec.label :
							_("Alarm"));
			snprintf(detail[row], sizeof(detail[row]), "%s · %s",
				 repeat,
				 alarm->spec.enabled ? _("On") : _("Off"));
			if (alarm->snoozed) {
				uint64_t remaining_ms =
					alarm->snooze_deadline_ms >
							clock->monotonic_ms ?
						alarm->snooze_deadline_ms -
							clock->monotonic_ms :
						0;

				clock_presentation_time(
					app->ui,
					clock->wall_utc +
						(int64_t)((remaining_ms + 999) /
							  1000),
					false, time, sizeof(time));
				duration(remaining_ms, false, delta,
					 sizeof(delta));
				snprintf(text[row], sizeof(text[row]),
					 _("Snoozed until %s"), time);
				snprintf(detail[row], sizeof(detail[row]),
					 _("%s left%s%s"), delta,
					 alarm->spec.label[0] ? " · " : "",
					 alarm->spec.label);
			}
		}
	} else if (view->kind == VIEW_WORLD) {
		count = clock->world_count + 1;
		snprintf(text[0], sizeof(text[0]), "%s", _("Add UTC offset"));
		for (size_t i = 0; i < clock->world_count; ++i) {
			const struct clock_world_entry *entry =
				&clock->world[i];
			size_t row = i + 1;

			ids[row] = entry->id;
			clock_presentation_offset_name(entry->offset_minutes,
						       text[row],
						       sizeof(text[row]));
			if (!clock_presentation_offset(
				    app->ui, entry->offset_minutes,
				    clock->wall_utc, time, sizeof(time), day,
				    sizeof(day), repeat, sizeof(repeat))) {
				snprintf(detail[row], sizeof(detail[row]), "%s",
					 _("Time unavailable"));
				continue;
			}
			snprintf(detail[row], sizeof(detail[row]), "%s · %s",
				 time, repeat);
		}
	} else if (view->kind == VIEW_MISSED) {
		count = clock->missed_count;
		for (size_t i = 0; i < count; ++i) {
			const struct clock_event *event =
				&clock->missed[count - 1 - i];

			ids[i] = event->id;
			clock_presentation_time(app->ui, event->due_wall_utc,
						false, time, sizeof(time));
			clock_presentation_when(app->ui, event->due_wall_utc,
						clock->wall_utc, false, day,
						sizeof(day));
			snprintf(text[i], sizeof(text[i]), "%s, %s", day, time);
			snprintf(detail[i], sizeof(detail[i]), "%s",
				 event->kind == CLOCK_EVENT_TIMER ? _("Timer") :
				 event->label[0] ? event->label :
						   _("Alarm"));
		}
	} else if (view->kind == VIEW_LAPS) {
		count = clock->stopwatch.lap_count;
		for (size_t i = 0; i < count; ++i) {
			ids[i] = i + 1;
			duration(clock->stopwatch.laps[i].elapsed_ms, true,
				 time, sizeof(time));
			duration(clock->stopwatch.laps[i].difference_ms, true,
				 delta, sizeof(delta));
			snprintf(text[i], sizeof(text[i]), "%zu  %s", i + 1,
				 delta);
			snprintf(detail[i], sizeof(detail[i]), "%s %s",
				 _("Total"), time);
		}
	}
	if (!count) {
		count = 1;
		snprintf(text[0], sizeof(text[0]), "%s",
			 view->kind == VIEW_LAPS ? _("No laps yet") :
						   _("No missed alarms"));
	}
	list_update(view, count, ids, text, detail);
	if (view->kind == VIEW_ALARMS || view->kind == VIEW_WORLD)
		fplinux_ui_list_set_icon(view->screen, 0,
					 fplinux_ui_glyph(app->ui, "add",
							  FPLINUX_UI_GLYPH_ROW),
					 FPLINUX_UI_LIST_ICON_IMAGE);
	if (view->kind == VIEW_ALARMS && app->focus_alarm_id) {
		for (size_t i = 1; i < count; ++i)
			if (ids[i] == app->focus_alarm_id)
				fplinux_ui_list_set_focused(view->screen, i);
		app->focus_alarm_id = 0;
	}
	if (view->kind == VIEW_WORLD && app->focus_world_id) {
		for (size_t i = 1; i < count; ++i)
			if (ids[i] == app->focus_world_id)
				fplinux_ui_list_set_focused(view->screen, i);
		app->focus_world_id = 0;
	}
	if (view->kind == VIEW_ALARMS)
		for (size_t i = 1; i < count; ++i) {
			const struct clock_alarm *alarm =
				clock_model_alarm_find(clock, (uint32_t)ids[i]);
			const struct fplinux_ui_command toggle = {
				.id = CMD_TOGGLE,
				.type = FPLINUX_UI_COMMAND_SCREEN,
				.label = alarm->spec.enabled ? _("Disable") :
							       _("Enable"),
				.icon = fplinux_ui_glyph(app->ui,
							 alarm->spec.enabled ?
								 "cancel" :
								 "check",
							 FPLINUX_UI_GLYPH_ROW),
			};
			const struct fplinux_ui_command remove = {
				.id = CMD_DELETE,
				.type = FPLINUX_UI_COMMAND_SCREEN,
				.label = _("Delete"),
				.priority = 1,
				.icon = fplinux_ui_glyph(app->ui, "delete",
							 FPLINUX_UI_GLYPH_ROW),
			};
			const struct fplinux_ui_command cancel_snooze = {
				.id = CMD_CANCEL_SNOOZE,
				.type = FPLINUX_UI_COMMAND_SCREEN,
				.label = _("Cancel snooze"),
				.priority = -1,
				.icon = fplinux_ui_glyph(app->ui, "cancel",
							 FPLINUX_UI_GLYPH_ROW),
			};

			fplinux_ui_list_add_item_command(view->screen, i,
							 &toggle);
			fplinux_ui_list_add_item_command(view->screen, i,
							 &remove);
			if (alarm->snoozed)
				fplinux_ui_list_add_item_command(
					view->screen, i, &cancel_snooze);
			else
				fplinux_ui_screen_remove_item_command(
					view->screen, i, CMD_CANCEL_SNOOZE);
		}
}

static void digits_refresh(struct clock_app *app, struct clock_screen *view,
			   const char *time, const char *caption)
{
	const struct fplinux_ui_theme *theme = fplinux_ui_theme(app->ui);
	const struct fplinux_ui_metrics *metrics = fplinux_ui_metrics(app->ui);
	lv_obj_t *parent = fplinux_ui_canvas_object(view->screen);
	const lv_font_t *fonts[] = { theme->font_clock, theme->font_large,
				     theme->font };
	const lv_font_t *font = theme->font;
	int width, height;
	int line = lv_font_get_line_height(theme->font);
	int caption_height;
	int digits_height;

	if (!fplinux_ui_screen_get_content_size(view->screen, &width, &height))
		return;
	caption_height = caption[0] ? 2 * line : 0;
	digits_height = height - caption_height - metrics->gap_small;
	if (!view->time_label) {
		view->time_label = lv_label_create(parent);
		view->date_label = lv_label_create(parent);
		lv_obj_set_style_text_color(view->time_label, theme->foreground,
					    0);
		lv_obj_set_style_text_align(view->time_label,
					    LV_TEXT_ALIGN_CENTER, 0);
		lv_obj_set_style_text_font(view->date_label, theme->font, 0);
		lv_obj_set_style_text_color(view->date_label, theme->muted, 0);
		lv_obj_set_style_text_align(view->date_label,
					    LV_TEXT_ALIGN_CENTER, 0);
	}
	for (size_t i = 0; i < COUNT(fonts); ++i) {
		lv_point_t size;

		lv_text_get_size(&size, time, fonts[i], 0, 0, LV_COORD_MAX,
				 LV_TEXT_FLAG_NONE);
		if (size.x <= width - 2 * metrics->margin &&
		    size.y <= digits_height) {
			font = fonts[i];
			break;
		}
	}
	lv_label_set_text(view->time_label, time);
	lv_obj_set_style_text_font(view->time_label, font, 0);
	lv_obj_set_size(view->time_label, width, lv_font_get_line_height(font));
	lv_obj_set_pos(view->time_label, 0,
		       (digits_height - lv_font_get_line_height(font)) / 2);
	lv_label_set_text(view->date_label, caption);
	lv_obj_set_size(view->date_label, width, caption_height);
	lv_obj_set_pos(view->date_label, 0, height - caption_height);
}

static void face_refresh(struct clock_app *app, struct clock_screen *view)
{
	const struct clock_model *clock = model(app);
	const struct fplinux_ui_theme *theme = fplinux_ui_theme(app->ui);
	const struct fplinux_ui_metrics *metrics = fplinux_ui_metrics(app->ui);
	lv_obj_t *parent = fplinux_ui_canvas_object(view->screen);
	struct clock_local_time local;
	char time[48], date[100];
	int width, height, line = lv_font_get_line_height(theme->font);
	int radius, cx, cy;

	clock_presentation_time(app->ui, clock->wall_utc,
				clock->view == CLOCK_VIEW_DIGITAL, time,
				sizeof(time));
	fplinux_ui_format_date(app->ui, (time_t)clock->wall_utc,
			       FPLINUX_UI_DATE_LONG, date, sizeof(date));
	if (clock->view == CLOCK_VIEW_DIGITAL) {
		digits_refresh(app, view, time, date);
		if (view->dial)
			lv_obj_set_hidden(view->dial, true);
		return;
	}
	if (!fplinux_ui_screen_get_content_size(view->screen, &width, &height))
		return;
	digits_refresh(app, view, time, date);
	lv_obj_set_style_text_font(view->time_label, theme->font, 0);
	lv_obj_set_size(view->time_label, width, line);
	lv_obj_set_pos(view->time_label, 0, height - 3 * line);
	lv_obj_set_pos(view->date_label, 0, height - 2 * line);
	radius = (height - 3 * line - 2 * metrics->gap_small) / 2;
	if (radius > (width - 2 * metrics->margin) / 2)
		radius = (width - 2 * metrics->margin) / 2;
	if (radius < 8)
		return;
	cx = width / 2;
	cy = radius + metrics->gap_small;
	if (!view->dial) {
		view->dial = lv_obj_create(parent);
		lv_obj_remove_style_all(view->dial);
		lv_obj_set_scrollable(view->dial, false);
		lv_obj_set_style_radius(view->dial, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_border_width(view->dial, metrics->focus_width,
					      0);
		lv_obj_set_style_border_color(view->dial, theme->border, 0);
		for (size_t i = 0; i < 3; ++i) {
			view->hands[i] = lv_line_create(view->dial);
			lv_obj_set_style_line_color(
				view->hands[i],
				i == 2 ? theme->accent : theme->foreground, 0);
			lv_obj_set_style_line_width(
				view->hands[i],
				i == 2 ? 1 : metrics->focus_width, 0);
		}
	}
	lv_obj_set_hidden(view->dial, false);
	lv_obj_set_pos(view->dial, cx - radius, cy - radius);
	lv_obj_set_size(view->dial, 2 * radius, 2 * radius);
	if (clock_model_utc_time(clock->wall_utc, &local) != CLOCK_OK)
		return;
	for (size_t i = 0; i < 3; ++i) {
		double units =
			i == 0 ?
				(local.hour % 12 + local.minute / 60.0) / 12.0 :
			i == 1 ? (local.minute + local.second / 60.0) / 60.0 :
				 local.second / 60.0;
		double angle = units * 6.283185307179586;
		int length = radius * (i == 0 ? 5 : 8) / 10;

		view->points[i][0] = (lv_point_precise_t){ radius, radius };
		view->points[i][1] = (lv_point_precise_t){
			radius + (int)(sin(angle) * length),
			radius - (int)(cos(angle) * length)
		};
		lv_line_set_points(view->hands[i], view->points[i], 2);
	}
}

static void timer_commands_refresh(struct fplinux_ui *ui,
				   struct clock_screen *view,
				   enum clock_timer_state state)
{
	if (view->commands_initialized && view->command_state == (int)state)
		return;
	for (int id = CMD_EDIT; id <= CMD_LAPS; ++id)
		fplinux_ui_screen_remove_command(view->screen, id);
	add_command(ui, view,
		    state == CLOCK_TIMER_RUNNING  ? CMD_PAUSE :
		    state == CLOCK_TIMER_PAUSED	  ? CMD_RESUME :
		    state == CLOCK_TIMER_FINISHED ? CMD_RESTART :
						    CMD_START,
		    FPLINUX_UI_COMMAND_OK,
		    state == CLOCK_TIMER_RUNNING  ? _("Pause") :
		    state == CLOCK_TIMER_PAUSED	  ? _("Resume") :
		    state == CLOCK_TIMER_FINISHED ? _("Repeat") :
						    _("Start"),
		    state == CLOCK_TIMER_RUNNING  ? "pause" :
		    state == CLOCK_TIMER_FINISHED ? "refresh" :
						    "play",
		    0, false);
	if (state == CLOCK_TIMER_RUNNING)
		add_command(ui, view, CMD_STOP, FPLINUX_UI_COMMAND_SCREEN,
			    _("Stop"), "stop", 0, true);
	else {
		add_command(ui, view, CMD_EDIT, FPLINUX_UI_COMMAND_SCREEN,
			    _("Edit"), "edit", 0, state != CLOCK_TIMER_PAUSED);
		if (state == CLOCK_TIMER_PAUSED) {
			add_command(ui, view, CMD_RESTART,
				    FPLINUX_UI_COMMAND_SCREEN, _("Start over"),
				    "refresh", 1, false);
			add_command(ui, view, CMD_STOP,
				    FPLINUX_UI_COMMAND_SCREEN, _("Reset"),
				    "refresh", 2, false);
		}
	}
	view->command_state = (int)state;
	view->commands_initialized = true;
}

static void stopwatch_commands_refresh(struct fplinux_ui *ui,
				       struct clock_screen *view,
				       const struct clock_stopwatch *watch)
{
	int state = watch->running ? 2 : watch->elapsed_ms ? 1 : 0;

	if (view->commands_initialized && view->command_state == state)
		return;
	for (int id = CMD_START; id <= CMD_LAPS; ++id)
		fplinux_ui_screen_remove_command(view->screen, id);
	add_command(ui, view, watch->running ? CMD_PAUSE : CMD_START,
		    FPLINUX_UI_COMMAND_OK,
		    watch->running    ? _("Pause") :
		    watch->elapsed_ms ? _("Resume") :
					_("Start"),
		    watch->running ? "pause" : "play", 0, false);
	if (watch->running)
		add_command(ui, view, CMD_LAP, FPLINUX_UI_COMMAND_SCREEN,
			    _("Lap"), "flag", 0, true);
	else if (watch->elapsed_ms) {
		add_command(ui, view, CMD_LAPS, FPLINUX_UI_COMMAND_SCREEN,
			    _("Laps"), "list", 0, false);
		add_command(ui, view, CMD_RESET, FPLINUX_UI_COMMAND_SCREEN,
			    _("Reset"), "refresh", 1, false);
	}
	view->command_state = state;
	view->commands_initialized = true;
}

static void refresh_current(struct clock_app *app)
{
	struct clock_screen *view = top(app);
	const struct clock_model *clock = model(app);
	char time[64], date[240], relative[100], text[180];

	if (!view || !is_active(app) || fplinux_ui_menu_is_open(app->ui) ||
	    fplinux_ui_current(app->ui) != view->screen)
		return;
	if (app->waiting_retry && view->kind != VIEW_ALARM_EDIT)
		add_command(app->ui, view, CMD_RETRY, FPLINUX_UI_COMMAND_SCREEN,
			    _("Retry"), "refresh", -1, true);
	else if (app->client.received &&
		 app->client.result.storage.state != FPLINUX_STORAGE_DURABLE &&
		 view->kind != VIEW_ALARM_EDIT &&
		 view->kind != VIEW_TIMER_EDIT && view->kind != VIEW_TIMER &&
		 view->kind != VIEW_STOPWATCH)
		add_command(app->ui, view, CMD_RETRY, FPLINUX_UI_COMMAND_SCREEN,
			    _("Retry saving"), "refresh", 5, false);
	else
		fplinux_ui_screen_remove_command(view->screen, CMD_RETRY);
	switch (view->kind) {
	case VIEW_HOME:
		if (clock->missed_count)
			add_command(app->ui, view, CMD_MISSED,
				    FPLINUX_UI_COMMAND_SCREEN,
				    _("Missed alarms"), "alarm", 4, false);
		else
			fplinux_ui_screen_remove_command(view->screen,
							 CMD_MISSED);
		break;
	case VIEW_FACE:
		face_refresh(app, view);
		break;
	case VIEW_ALARMS:
	case VIEW_WORLD:
	case VIEW_MISSED:
	case VIEW_LAPS:
		lists_refresh(app, view);
		break;
	case VIEW_ALARM_EDIT:
		alarm_form_refresh(app, view, false);
		break;
	case VIEW_TIMER_EDIT:
		if (mutation_pending(app)) {
			fplinux_ui_form_set_string(
				view->screen, view->items[1],
				app->waiting_retry ?
					_("Could not confirm the change. Retry?") :
				app->retrying ? _("Trying again…") :
						_("Saving…"));
			add_command(app->ui, view, CMD_SAVE,
				    FPLINUX_UI_COMMAND_SCREEN,
				    app->waiting_retry ? _("Retry") :
							 _("Saving…"),
				    app->waiting_retry ? "refresh" : "save", 0,
				    true);
		} else {
			fplinux_ui_form_set_string(view->screen, view->items[1],
						   app->feedback);
			add_command(app->ui, view, CMD_SAVE,
				    FPLINUX_UI_COMMAND_SCREEN,
				    app->save_failed ? _("Retry") : _("Done"),
				    app->save_failed ? "refresh" : "check", 0,
				    true);
		}
		break;
	case VIEW_TIMER:
		duration(clock_model_timer_remaining(clock), false, time,
			 sizeof(time));
		digits_refresh(app, view, time,
			       clock->timer.state == CLOCK_TIMER_PAUSED ?
				       _("Paused") :
			       clock->timer.state == CLOCK_TIMER_FINISHED ?
				       _("Time is up") :
				       "");
		timer_commands_refresh(app->ui, view, clock->timer.state);
		break;
	case VIEW_STOPWATCH:
		duration(clock_model_stopwatch_elapsed(clock), true, time,
			 sizeof(time));
		text[0] = '\0';
		if (clock->stopwatch.lap_count) {
			size_t last = clock->stopwatch.lap_count - 1;

			duration(clock->stopwatch.laps[last].difference_ms,
				 true, relative, sizeof(relative));
			snprintf(text, sizeof(text), _("Lap %zu · %s"),
				 last + 1, relative);
		}
		if (clock->stopwatch.running)
			snprintf(date, sizeof(date), "%s%s%s", text,
				 text[0] ? "\n" : "", _("Laps →"));
		else if (clock->stopwatch.elapsed_ms)
			snprintf(date, sizeof(date), "%s\n%s", _("Paused"),
				 text);
		else
			snprintf(date, sizeof(date), "%s", text);
		digits_refresh(app, view, time, date);
		stopwatch_commands_refresh(app->ui, view, &clock->stopwatch);
		break;
	case VIEW_OFFSET_CHOICE:
		if (view->offset_saved) {
			pop_view(app, false);
			return;
		}
		break;
	case VIEW_OFFSET: {
		const struct clock_world_entry *entry =
			find_world_entry(clock, view->subject);

		if (!entry) {
			pop_view(app, false);
			return;
		}
		clock_presentation_offset_name(entry->offset_minutes, text,
					       sizeof(text));
		fplinux_ui_screen_set_title(view->screen, text);
		if (!clock_presentation_offset(app->ui, entry->offset_minutes,
					       clock->wall_utc, time,
					       sizeof(time), date, sizeof(date),
					       relative, sizeof(relative))) {
			snprintf(time, sizeof(time), "%s",
				 _("Time unavailable"));
			date[0] = '\0';
		}
		fplinux_ui_form_set_string(view->screen, view->items[0], time);
		fplinux_ui_form_set_string(view->screen, view->items[1], date);
		break;
	}
	case VIEW_SETTINGS:
		fplinux_ui_list_set(view->screen, 0, _("Clock face"),
				    clock->view == CLOCK_VIEW_ANALOG ?
					    _("Analog") :
					    _("Digital"));
		snprintf(text, sizeof(text), _("%u min"),
			 clock->snooze_minutes);
		fplinux_ui_list_set(view->screen, 1, _("Snooze"), text);
		break;
	default:
		break;
	}
}

static void stopwatch_key(struct fplinux_ui *ui,
			  struct fplinux_ui_screen *screen,
			  const struct fplinux_ui_key *key, void *context)
{
	struct clock_app *app = context;
	struct clock_screen *view = top(app);

	(void)ui;
	if (key->pressed && key->code == KEY_RIGHT && view &&
	    view->screen == screen && view->kind == VIEW_STOPWATCH &&
	    model(app)->stopwatch.running)
		open_view(app, VIEW_LAPS, 0);
}

static void open_alarm(struct clock_app *app, uint32_t id)
{
	const struct clock_alarm *saved =
		clock_model_alarm_find(model(app), id);
	struct clock_local_time civil;
	int64_t utc;

	if (id && !saved)
		return;
	memset(&app->alarm, 0, sizeof(app->alarm));
	app->alarm.spec = saved ? saved->spec :
				  (struct clock_alarm_spec){
					  .schedule = CLOCK_ALARM_ONCE,
					  .enabled = true,
				  };
	app->alarm.date =
		(time_t)(saved && saved->spec.schedule == CLOCK_ALARM_ONCE ?
				 saved->spec.once_utc :
				 model(app)->wall_utc);
	if (saved && saved->spec.schedule == CLOCK_ALARM_ONCE)
		app->alarm.time = (time_t)saved->spec.once_utc;
	else {
		if (clock_model_utc_time(model(app)->wall_utc, &civil) !=
		    CLOCK_OK)
			return;
		civil.hour = saved ? saved->spec.hour :
				     model(app)->last_alarm_hour;
		civil.minute = saved ? saved->spec.minute :
				       model(app)->last_alarm_minute;
		civil.second = 0;
		if (clock_model_utc_from_calendar(&civil, &utc) != CLOCK_OK)
			return;
		app->alarm.time = (time_t)utc;
	}
	app->alarm.date_chosen = saved &&
				 saved->spec.schedule == CLOCK_ALARM_ONCE;
	if (!saved)
		nearest_date(app);
	app->feedback[0] = '\0';
	app->save_failed = false;
	app->save_applied = false;
	open_view(app, VIEW_ALARM_EDIT, id);
}

static void open_view(struct clock_app *app, enum view_kind kind,
		      uint64_t subject)
{
	const struct clock_model *clock = model(app);
	struct clock_screen *view = NULL;
	static const char *const weekdays[] = { "Monday",    "Tuesday",
						"Wednesday", "Thursday",
						"Friday",    "Saturday",
						"Sunday" };

	if (app->closing || app->alert)
		return;
	if (!app->client.received && kind != VIEW_HOME && kind != VIEW_HELP) {
		notice(app, _("Clock"),
		       _("Clocks are loading. Try again in a moment."));
		return;
	}
	switch (kind) {
	case VIEW_HOME: {
		static const struct {
			const char *label;
			const char *glyph;
		} entries[] = {
			{ "Alarms", "alarm" },
			{ "Timer", "timer" },
			{ "Stopwatch", "stopwatch" },
			{ "World clock", "world" },
		};

		view = new_view(app, kind, 0, _("Clock"),
				FPLINUX_UI_LIST_IMPLICIT, false);
		if (!view)
			return;
		for (size_t i = 0; i < COUNT(entries); ++i) {
			fplinux_ui_list_append(view->screen,
					       _(entries[i].label), NULL);
			fplinux_ui_list_set_icon(
				view->screen, i,
				fplinux_ui_glyph(app->ui, entries[i].glyph,
						 FPLINUX_UI_GLYPH_ROW),
				FPLINUX_UI_LIST_ICON_IMAGE);
		}
		add_command(app->ui, view, CMD_SHOW_FACE,
			    FPLINUX_UI_COMMAND_SCREEN, _("Show clock"),
			    "date-time", 0, false);
		add_command(app->ui, view, CMD_SETTINGS,
			    FPLINUX_UI_COMMAND_SCREEN, _("Settings"),
			    "settings", 1, false);
		add_command(app->ui, view, CMD_HELP, FPLINUX_UI_COMMAND_HELP,
			    _("Help"), "help", 5, false);
		break;
	}
	case VIEW_FACE:
	case VIEW_TIMER:
	case VIEW_STOPWATCH:
		view = new_view(app, kind, 0,
				kind == VIEW_FACE  ? _("Clock") :
				kind == VIEW_TIMER ? _("Timer") :
						     _("Stopwatch"),
				0, true);
		if (view && kind == VIEW_STOPWATCH)
			fplinux_ui_canvas_set_key_handler(view->screen,
							  stopwatch_key, app);
		break;
	case VIEW_ALARMS:
	case VIEW_WORLD:
	case VIEW_MISSED:
	case VIEW_LAPS:
		view = new_view(app, kind, 0,
				kind == VIEW_ALARMS ? _("Alarms") :
				kind == VIEW_WORLD  ? _("World clock") :
				kind == VIEW_MISSED ? _("Missed alarms") :
						      _("Laps"),
				FPLINUX_UI_LIST_IMPLICIT, false);
		if (view && kind == VIEW_MISSED)
			add_command(app->ui, view, CMD_CLEAR,
				    FPLINUX_UI_COMMAND_SCREEN,
				    _("Clear history"), "delete", 0, false);
		break;
	case VIEW_ALARM_EDIT:
		view = new_view(app, kind, subject,
				subject ? _("Edit alarm") : _("New alarm"), -1,
				false);
		if (!view)
			return;
		alarm_form_refresh(app, view, true);
		editor_commands(app->ui, view, _("Save"), "save");
		fplinux_ui_form_set_edit_listener(view->screen, edit_accepted,
						  app);
		break;
	case VIEW_LABEL:
		view = new_view(app, kind, 0, _("Alarm name"), -1, false);
		if (!view)
			return;
		view->items[0] = fplinux_ui_form_append_text_field(
			view->screen, _("Name"), app->alarm.spec.label,
			CLOCK_TEXT_CHARACTERS, FPLINUX_UI_TEXT_ANY);
		add_command(app->ui, view, CMD_SAVE, FPLINUX_UI_COMMAND_OK,
			    _("Done"), "check", 0, false);
		add_command(app->ui, view, CMD_BACK, FPLINUX_UI_COMMAND_CANCEL,
			    _("Cancel"), "cancel", 0, true);
		fplinux_ui_form_set_focus(view->screen, view->items[0]);
		break;
	case VIEW_REPEAT:
		view = new_view(app, kind, 0, _("Repeat"),
				FPLINUX_UI_LIST_IMPLICIT, false);
		if (!view)
			return;
		for (size_t i = 0; i < 5; ++i) {
			static const unsigned int masks[] = { 0, 127, 31, 96,
							      1 };

			fplinux_ui_list_append(view->screen,
					       repeat_name(masks[i]), NULL);
		}
		add_command(app->ui, view, CMD_BACK, FPLINUX_UI_COMMAND_CANCEL,
			    _("Cancel"), "cancel", 0, true);
		break;
	case VIEW_DAYS:
		view = new_view(app, kind, 0, _("Choose days"),
				FPLINUX_UI_LIST_MULTIPLE, false);
		if (!view)
			return;
		for (size_t i = 0; i < COUNT(weekdays); ++i) {
			fplinux_ui_list_append(view->screen, _(weekdays[i]),
					       NULL);
			fplinux_ui_list_set_selected(
				view->screen, i,
				(app->alarm.spec.repeat_days & (1U << i)) != 0);
		}
		editor_commands(app->ui, view, _("Done"), "check");
		break;
	case VIEW_TIMER_EDIT:
		app->feedback[0] = '\0';
		app->save_failed = false;
		app->save_applied = false;
		view = new_view(app, kind, 0, _("Timer duration"), -1, false);
		if (!view)
			return;
		view->items[0] = fplinux_ui_form_append_duration(
			view->screen, _("Duration"),
			clock->timer.duration_seconds);
		string_item(view, 1, NULL, "");
		editor_commands(app->ui, view, _("Done"), "check");
		break;
	case VIEW_OFFSET_CHOICE: {
		const struct clock_world_entry *entry =
			subject ? find_world_entry(clock, subject) : NULL;
		int selected_offset = entry ? entry->offset_minutes : 0;
		size_t focus = (size_t)((selected_offset -
					 CLOCK_WORLD_MIN_OFFSET_MINUTES) /
					CLOCK_WORLD_STEP_MINUTES);

		if (subject && !entry)
			return;
		view = new_view(app, kind, subject, _("UTC offset"),
				FPLINUX_UI_LIST_IMPLICIT, false);
		if (!view)
			return;
		for (int offset_minutes = CLOCK_WORLD_MIN_OFFSET_MINUTES;
		     offset_minutes <= CLOCK_WORLD_MAX_OFFSET_MINUTES;
		     offset_minutes += CLOCK_WORLD_STEP_MINUTES) {
			char text[32];

			clock_presentation_offset_name(offset_minutes, text,
						       sizeof(text));
			fplinux_ui_list_append(view->screen, text, NULL);
		}
		fplinux_ui_list_set_focused(view->screen, focus);
		add_command(app->ui, view, CMD_BACK, FPLINUX_UI_COMMAND_CANCEL,
			    _("Cancel"), "cancel", 0, true);
		break;
	}
	case VIEW_OFFSET: {
		const struct clock_world_entry *entry =
			find_world_entry(clock, subject);
		char name[32];

		if (!entry ||
		    !clock_presentation_offset_name(entry->offset_minutes, name,
						    sizeof(name)))
			return;
		view = new_view(app, kind, subject, name, -1, false);
		if (!view)
			return;
		string_item(view, 0, _("Time"), "");
		string_item(view, 1, _("Date"), "");
		add_command(app->ui, view, CMD_EDIT, FPLINUX_UI_COMMAND_OK,
			    _("Edit"), "edit", 0, false);
		add_command(app->ui, view, CMD_DELETE,
			    FPLINUX_UI_COMMAND_SCREEN, _("Delete"), "delete", 0,
			    false);
		break;
	}
	case VIEW_SETTINGS:
		view = new_view(app, kind, 0, _("Clock settings"),
				FPLINUX_UI_LIST_IMPLICIT, false);
		if (!view)
			return;
		fplinux_ui_list_append(view->screen, _("Clock face"), NULL);
		fplinux_ui_list_append(view->screen, _("Snooze"), NULL);
		fplinux_ui_list_set_icon(view->screen, 0,
					 fplinux_ui_glyph(app->ui, "date-time",
							  FPLINUX_UI_GLYPH_ROW),
					 FPLINUX_UI_LIST_ICON_IMAGE);
		fplinux_ui_list_set_icon(view->screen, 1,
					 fplinux_ui_glyph(app->ui, "snooze",
							  FPLINUX_UI_GLYPH_ROW),
					 FPLINUX_UI_LIST_ICON_IMAGE);
		break;
	case VIEW_FACE_CHOICE:
	case VIEW_SNOOZE_CHOICE:
		view = new_view(app, kind, 0,
				kind == VIEW_FACE_CHOICE ? _("Clock face") :
							   _("Snooze"),
				FPLINUX_UI_LIST_IMPLICIT, false);
		if (!view)
			return;
		if (kind == VIEW_FACE_CHOICE) {
			fplinux_ui_list_append(view->screen, _("Digital"),
					       NULL);
			fplinux_ui_list_append(view->screen, _("Analog"), NULL);
			fplinux_ui_list_set_focused(
				view->screen,
				clock->view == CLOCK_VIEW_ANALOG ? 1 : 0);
		} else {
			for (unsigned int i = 1; i <= 30; ++i) {
				char text[32];

				snprintf(text, sizeof(text), _("%u min"), i);
				fplinux_ui_list_append(view->screen, text,
						       NULL);
			}
			fplinux_ui_list_set_focused(view->screen,
						    clock->snooze_minutes - 1);
		}
		add_command(app->ui, view, CMD_BACK, FPLINUX_UI_COMMAND_CANCEL,
			    _("Cancel"), "cancel", 0, true);
		break;
	case VIEW_HELP:
		view = new_view(app, kind, 0, _("Clock help"), -1, false);
		if (!view)
			return;
		string_item(
			view, 0, _("Alarms"),
			_("Choose a time and date, or repeat on selected days. Save sets the alarm; Cancel leaves it unchanged."));
		string_item(
			view, 1, _("Timer and stopwatch"),
			_("You can leave this screen while time is running. Right opens stopwatch laps without pausing."));
		string_item(
			view, 2, _("Power"),
			_("Alarms ring while the phone is on. They cannot turn on a powered-off phone."));
		break;
	}
	if (!view)
		return;
	if (kind != VIEW_ALARM_EDIT && kind != VIEW_LABEL &&
	    kind != VIEW_REPEAT && kind != VIEW_DAYS &&
	    kind != VIEW_TIMER_EDIT && kind != VIEW_OFFSET_CHOICE &&
	    kind != VIEW_FACE_CHOICE && kind != VIEW_SNOOZE_CHOICE)
		add_command(app->ui, view, CMD_BACK, FPLINUX_UI_COMMAND_BACK,
			    _("Back"), "back", 0, false);
	fplinux_ui_navigate(app->ui, view->screen);
	refresh_current(app);
}

static void submit(struct clock_app *app, const struct clock_request *request,
		   bool close_editor)
{
	if (mutation_pending(app))
		return;
	app->pending_view_token = top(app) ? top(app)->token : 0;
	app->pending_close = close_editor;
	app->pending_draft_revision =
		request->op == CLOCK_REQUEST_TIMER_SET ||
				(request->op == CLOCK_REQUEST_SAVE &&
				 top(app) &&
				 top(app)->kind == VIEW_TIMER_EDIT) ?
			app->timer_draft_revision :
			app->alarm.revision;
	app->retrying = false;
	app->waiting_retry = false;
	app->pending_first_alarm_id = model(app)->next_alarm_id;
	app->feedback[0] = '\0';
	app->save_failed = false;
	app->save_applied = false;
	if (!clock_client_send(&app->client, app->ui, request,
			       fplinux_ui_now_ms())) {
		app->pending_close = false;
		notice(app, _("Clock"), request_error(request->op));
	}
	refresh_current(app);
}

static void save_editor(struct clock_app *app, struct clock_screen *view)
{
	struct clock_request request = { 0 };
	uint64_t revision = view->kind == VIEW_TIMER_EDIT ?
				    app->timer_draft_revision :
				    app->alarm.revision;

	if (app->save_applied && app->save_view_token == view->token &&
	    app->save_draft_revision == revision &&
	    !fplinux_ui_form_is_editing(view->screen)) {
		request.op = CLOCK_REQUEST_SAVE;
		submit(app, &request, true);
		return;
	}

	if (view->kind == VIEW_LABEL) {
		const char *name =
			fplinux_ui_form_get_text(view->screen, view->items[0]);

		if (!name || clock_model_validate_text(name, strlen(name),
						       true) != CLOCK_OK) {
			notice(app, _("Alarm name"),
			       _("Use a name of up to 32 characters."));
			return;
		}
		if (strcmp(app->alarm.spec.label, name)) {
			app->alarm.dirty = true;
			++app->alarm.revision;
		}
		snprintf(app->alarm.spec.label, sizeof(app->alarm.spec.label),
			 "%s", name);
		pop_view(app, false);
		return;
	}
	if (view->kind == VIEW_DAYS) {
		unsigned int mask = 0;

		for (size_t i = 0; i < 7; ++i)
			if (fplinux_ui_list_is_selected(view->screen, i))
				mask |= 1U << i;
		if (!mask) {
			notice(app, _("Choose days"),
			       _("Select at least one day."));
			return;
		}
		app->alarm.spec.schedule = CLOCK_ALARM_WEEKLY;
		app->alarm.spec.repeat_days = mask;
		app->alarm.dirty = true;
		++app->alarm.revision;
		pop_view(app, false);
		alarm_form_refresh(app, top(app), true);
		return;
	}
	if (mutation_pending(app))
		return;
	if (view->kind == VIEW_ALARM_EDIT) {
		draft_capture(app, view);
		if (!draft_calendar(app, &request.alarm)) {
			notice(app, _("Alarm"),
			       _("Check the date, time and repeat days."));
			return;
		}
		if (request.alarm.enabled &&
		    request.alarm.schedule == CLOCK_ALARM_ONCE &&
		    request.alarm.once_utc <= model(app)->wall_utc) {
			app->feedback[0] = '\0';
			fplinux_ui_form_set_focus(view->screen, view->items[2]);
			notice(app, _("Alarm"),
			       _("This time has passed. Choose another date or time."));
			return;
		}
		request.op = view->subject ? CLOCK_REQUEST_ALARM_EDIT :
					     CLOCK_REQUEST_ALARM_ADD;
		request.id = (uint32_t)view->subject;
		app->alarm.dirty = true;
	} else if (view->kind == VIEW_TIMER_EDIT) {
		request.seconds = fplinux_ui_form_get_duration(view->screen,
							       view->items[0]);
		if (!request.seconds ||
		    request.seconds > CLOCK_TIMER_MAX_SECONDS) {
			snprintf(
				app->feedback, sizeof(app->feedback), "%s",
				request.seconds ?
					_("Use a duration up to 99:59:59.") :
					_("Enter a duration greater than zero."));
			fplinux_ui_form_set_string(view->screen, view->items[1],
						   app->feedback);
			return;
		}
		if (request.seconds == model(app)->timer.duration_seconds) {
			pop_view(app, false);
			return;
		}
		request.op = CLOCK_REQUEST_TIMER_SET;
		if (model(app)->timer.state != CLOCK_TIMER_STOPPED) {
			confirm(app,
				_("Replace the current countdown with this duration?"),
				&request, CONFIRM_TIMER);
			return;
		}
	} else
		return;
	submit(app, &request, true);
}

static void edit_accepted(struct fplinux_ui *ui,
			  struct fplinux_ui_screen *screen, size_t index,
			  void *context)
{
	struct clock_app *app = context;
	struct clock_screen *view = top(app);
	(void)ui;

	if (view && screen == view->screen && view->kind == VIEW_ALARM_EDIT &&
	    index == view->items[2])
		app->alarm.date_chosen = true;
}

static void changed(struct fplinux_ui *ui, struct fplinux_ui_screen *screen,
		    size_t index, void *context)
{
	struct clock_app *app = context;
	struct clock_screen *view = top(app);
	(void)ui;

	if (!view || screen != view->screen)
		return;
	if (view->kind == VIEW_ALARM_EDIT) {
		draft_capture(app, view);
		if (index == view->items[2])
			app->alarm.date_chosen = true;
		if (index == view->items[0] && !app->alarm.date_chosen) {
			nearest_date(app);
			if (view->items[2] != SIZE_MAX)
				fplinux_ui_form_set_date(view->screen,
							 view->items[2],
							 app->alarm.date);
		}
		app->alarm.dirty = true;
		++app->alarm.revision;
		app->feedback[0] = '\0';
		app->save_failed = false;
		refresh_current(app);
	} else if (view->kind == VIEW_TIMER_EDIT && index == view->items[0]) {
		++app->timer_draft_revision;
		app->feedback[0] = '\0';
		app->save_failed = false;
		fplinux_ui_form_set_string(view->screen, view->items[1], "");
	} else if (view->kind == VIEW_ALARMS)
		refresh_current(app);
}

static void select_row(struct clock_app *app, struct clock_screen *view)
{
	size_t selected = fplinux_ui_list_get_focused(view->screen);
	uint64_t id = selected_id(view);
	struct clock_request request = { 0 };

	switch (view->kind) {
	case VIEW_HOME: {
		static const enum view_kind destinations[] = {
			VIEW_ALARMS, VIEW_TIMER, VIEW_STOPWATCH, VIEW_WORLD
		};

		if (selected < COUNT(destinations))
			open_view(app, destinations[selected], 0);
		break;
	}
	case VIEW_ALARMS:
		open_alarm(app, (uint32_t)id);
		break;
	case VIEW_WORLD:
		open_view(app, id ? VIEW_OFFSET : VIEW_OFFSET_CHOICE, id);
		break;
	case VIEW_REPEAT: {
		static const unsigned int masks[] = { 0, 127, 31, 96 };

		if (selected >= COUNT(masks)) {
			pop_view(app, false);
			open_view(app, VIEW_DAYS, 0);
			break;
		}
		app->alarm.spec.schedule = selected ? CLOCK_ALARM_WEEKLY :
						      CLOCK_ALARM_ONCE;
		app->alarm.spec.repeat_days = masks[selected];
		app->alarm.dirty = true;
		++app->alarm.revision;
		pop_view(app, false);
		alarm_form_refresh(app, top(app), true);
		break;
	}
	case VIEW_OFFSET_CHOICE:
		if (selected <= (size_t)((CLOCK_WORLD_MAX_OFFSET_MINUTES -
					  CLOCK_WORLD_MIN_OFFSET_MINUTES) /
					 CLOCK_WORLD_STEP_MINUTES)) {
			request.op = view->subject ? CLOCK_REQUEST_WORLD_EDIT :
						     CLOCK_REQUEST_WORLD_ADD;
			request.id = (uint32_t)view->subject;
			request.offset_minutes =
				CLOCK_WORLD_MIN_OFFSET_MINUTES +
				(int)selected * CLOCK_WORLD_STEP_MINUTES;
			submit(app, &request, true);
		}
		break;
	case VIEW_SETTINGS:
		open_view(app, selected ? VIEW_SNOOZE_CHOICE : VIEW_FACE_CHOICE,
			  0);
		break;
	case VIEW_FACE_CHOICE:
		request.op = CLOCK_REQUEST_SET_VIEW;
		request.view = selected ? CLOCK_VIEW_ANALOG :
					  CLOCK_VIEW_DIGITAL;
		submit(app, &request, true);
		break;
	case VIEW_SNOOZE_CHOICE:
		request.op = CLOCK_REQUEST_SET_SNOOZE;
		request.minutes = (unsigned int)selected + 1;
		submit(app, &request, true);
		break;
	default:
		break;
	}
}

static void command(struct fplinux_ui *ui, struct fplinux_ui_screen *screen,
		    int id, void *context)
{
	struct clock_app *app = context;
	struct clock_screen *view = top(app);
	const struct clock_model *clock = model(app);
	struct clock_request request = { 0 };
	(void)ui;

	if (app->closing)
		return;
	if (screen == app->alert) {
		enum confirmation action = app->confirmation;
		uint64_t token = app->confirmation_token;
		uint64_t revision = app->confirmation_draft_revision;

		request = app->confirmed_request;
		app->alert = NULL;
		app->confirmation = CONFIRM_NONE;
		if (action == CONFIRM_LEAVE) {
			enum leave_destination destination =
				app->leave_destination;

			app->leave_destination = LEAVE_BACK;
			if (destination == LEAVE_CLOSE) {
				fplinux_ui_close_reply(
					app->ui,
					id == CMD_CONFIRM ?
						FPLINUX_UI_CLOSE_READY :
						FPLINUX_UI_CLOSE_CANCEL);
				return;
			}
			if (id == CMD_CONFIRM && view && view->token == token) {
				if (destination == LEAVE_HOME)
					return_home(app);
				else
					pop_view(app, true);
			}
			refresh_current(app);
			return;
		}
		if (id == CMD_CONFIRM && view && view->token == token) {
			if (action == CONFIRM_RETRY && mutation_pending(app)) {
				clock_client_retry(&app->client, app->ui,
						   fplinux_ui_now_ms());
				app->waiting_retry = false;
			} else if (action == CONFIRM_REQUEST ||
				   action == CONFIRM_ACTIVE ||
				   action == CONFIRM_TIMER) {
				request.cancel_active = action ==
							CONFIRM_ACTIVE;
				submit(app, &request,
				       action == CONFIRM_TIMER ||
					       (action == CONFIRM_ACTIVE &&
						app->pending_close));
				if (action == CONFIRM_ACTIVE ||
				    action == CONFIRM_TIMER)
					app->pending_draft_revision = revision;
			}
		}
		refresh_current(app);
		return;
	}
	if (!view || screen != view->screen)
		return;
	if (id == CMD_BACK) {
		if (view->kind == VIEW_ALARM_EDIT && app->alarm.dirty)
			confirm(app, _("Leave without saving?"), NULL,
				CONFIRM_LEAVE);
		else
			pop_view(app, true);
		return;
	}
	if (mutation_pending(app)) {
		if (app->waiting_retry && (id == CMD_RETRY || id == CMD_SAVE)) {
			clock_client_retry(&app->client, app->ui,
					   fplinux_ui_now_ms());
			app->waiting_retry = false;
			refresh_current(app);
		}
		return;
	}
	if (id == CMD_SAVE) {
		save_editor(app, view);
		return;
	}
	if (id == CMD_RETRY) {
		request.op = CLOCK_REQUEST_SAVE;
		submit(app, &request, false);
		return;
	}
	if (id == FPLINUX_UI_SELECT_COMMAND) {
		select_row(app, view);
		return;
	}
	if (id == CMD_SHOW_FACE || id == CMD_SETTINGS || id == CMD_HELP ||
	    id == CMD_MISSED || id == CMD_LAPS) {
		open_view(app,
			  id == CMD_SHOW_FACE ? VIEW_FACE :
			  id == CMD_SETTINGS  ? VIEW_SETTINGS :
			  id == CMD_HELP      ? VIEW_HELP :
			  id == CMD_MISSED    ? VIEW_MISSED :
						VIEW_LAPS,
			  0);
		return;
	}
	if (id == CMD_LABEL || id == CMD_REPEAT) {
		draft_capture(app, view);
		open_view(app, id == CMD_LABEL ? VIEW_LABEL : VIEW_REPEAT, 0);
		return;
	}
	if (id == CMD_EDIT) {
		open_view(app,
			  view->kind == VIEW_OFFSET ? VIEW_OFFSET_CHOICE :
						      VIEW_TIMER_EDIT,
			  view->kind == VIEW_OFFSET ? view->subject : 0);
		return;
	}
	if (id == CMD_DELETE) {
		request.op = view->kind == VIEW_ALARMS ?
				     CLOCK_REQUEST_ALARM_DELETE :
				     CLOCK_REQUEST_WORLD_DELETE;
		request.id = (uint32_t)(view->kind == VIEW_ALARMS ?
						selected_id(view) :
						view->subject);
		if (request.id)
			confirm(app,
				view->kind == VIEW_ALARMS ?
					_("Delete this alarm?") :
					_("Delete this UTC offset?"),
				&request, CONFIRM_REQUEST);
		return;
	}
	if (id == CMD_CANCEL_SNOOZE) {
		request.op = CLOCK_REQUEST_ALARM_CANCEL_SNOOZE;
		request.id = (uint32_t)selected_id(view);
		if (!request.id)
			return;
	} else if (id == CMD_TOGGLE) {
		const struct clock_alarm *alarm = clock_model_alarm_find(
			clock, (uint32_t)selected_id(view));

		if (!alarm)
			return;
		if (!alarm->spec.enabled &&
		    alarm->spec.schedule == CLOCK_ALARM_ONCE &&
		    alarm->spec.once_utc <= clock->wall_utc) {
			open_alarm(app, alarm->id);
			app->alarm.spec.enabled = true;
			app->alarm.date_chosen = false;
			app->alarm.dirty = true;
			++app->alarm.revision;
			nearest_date(app);
			snprintf(
				app->feedback, sizeof(app->feedback), "%s",
				_("Choose the new date, then save to enable."));
			alarm_form_refresh(app, top(app), true);
			return;
		}
		request.op = CLOCK_REQUEST_ALARM_ENABLE;
		request.id = alarm->id;
		request.enabled = !alarm->spec.enabled;
	} else if (id == CMD_CLEAR) {
		request.op = CLOCK_REQUEST_CLEAR_MISSED;
		confirm(app, _("Clear missed alarms?"), &request,
			CONFIRM_REQUEST);
		return;
	} else if (id == CMD_RESET) {
		request.op = CLOCK_REQUEST_STOPWATCH_RESET;
		if (clock_model_stopwatch_elapsed(clock)) {
			confirm(app, _("Reset the stopwatch and delete laps?"),
				&request, CONFIRM_REQUEST);
			return;
		}
	} else if (view->kind == VIEW_TIMER) {
		switch (id) {
		case CMD_START:
			request.op = CLOCK_REQUEST_TIMER_START;
			break;
		case CMD_PAUSE:
			request.op = CLOCK_REQUEST_TIMER_PAUSE;
			break;
		case CMD_RESUME:
			request.op = CLOCK_REQUEST_TIMER_RESUME;
			break;
		case CMD_STOP:
			request.op = CLOCK_REQUEST_TIMER_STOP;
			break;
		case CMD_RESTART:
			request.op = CLOCK_REQUEST_TIMER_RESTART;
			break;
		default:
			return;
		}
		if ((id == CMD_START || id == CMD_RESTART) &&
		    !clock->timer.duration_seconds) {
			open_view(app, VIEW_TIMER_EDIT, 0);
			return;
		}
	} else if (view->kind == VIEW_STOPWATCH) {
		request.op = id == CMD_START ? CLOCK_REQUEST_STOPWATCH_START :
			     id == CMD_PAUSE ? CLOCK_REQUEST_STOPWATCH_PAUSE :
					       CLOCK_REQUEST_STOPWATCH_LAP;
	} else
		return;
	submit(app, &request, false);
}

static void poll(lv_timer_t *timer)
{
	struct clock_app *app = lv_timer_get_user_data(timer);
	struct clock_screen *view = top(app);
	uint64_t now = fplinux_ui_now_ms();
	unsigned int period = view && view->kind == VIEW_STOPWATCH ? 200 : 1000;
	struct clock_request request = { .op = CLOCK_REQUEST_SNAPSHOT };

	if (!is_active(app) || app->alert)
		return;
	if (mutation_pending(app) && !app->waiting_retry &&
	    now >= app->client.deadline_ms) {
		if (!app->retrying) {
			clock_client_retry(&app->client, app->ui, now);
			app->retrying = true;
		} else {
			app->waiting_retry = true;
			confirm(app, _("Could not confirm the change. Retry?"),
				NULL, CONFIRM_RETRY);
		}
		refresh_current(app);
	} else if (!mutation_pending(app) &&
		   clock_client_expire(&app->client, now)) {
		refresh_current(app);
	}
	if (app->client.pending || now - app->last_poll_ms < period)
		return;
	app->last_poll_ms = now;
	clock_client_send(&app->client, app->ui, &request, now);
}

static void event(struct fplinux_ui *ui, const struct fplinux_ui_event *event,
		  void *context)
{
	struct clock_app *app = context;
	struct clock_screen *view;
	(void)ui;

	if (event->type == FPLINUX_UI_EVENT_CLOSE) {
		app->closing = true;
		if (app->poll_timer)
			lv_timer_pause(app->poll_timer);
		return;
	}
	if (app->closing)
		return;
	if (event->type == FPLINUX_UI_EVENT_ACTIVATION) {
		if (event->activation == FPLINUX_UI_ACTIVATION_OPEN)
			open_home(app);
		return;
	}
	if (event->type == FPLINUX_UI_EVENT_VISIBILITY ||
	    event->type == FPLINUX_UI_EVENT_LIFECYCLE) {
		if (app->poll_timer) {
			if (is_active(app)) {
				app->last_poll_ms = 0;
				lv_timer_resume(app->poll_timer);
			} else
				lv_timer_pause(app->poll_timer);
		}
		if (is_active(app))
			refresh_current(app);
		return;
	}
	if (event->type == FPLINUX_UI_EVENT_RESIZE) {
		refresh_current(app);
		return;
	}
	if (event->type != FPLINUX_UI_EVENT_SESSION || !event->session)
		return;
	enum clock_status status = CLOCK_INVALID;
	struct clock_request request = app->client.request;
	enum clock_client_result result =
		clock_client_receive(&app->client, event->session, &status);

	if (result == CLOCK_CLIENT_IGNORED)
		return;
	if (result == CLOCK_CLIENT_ERROR && mutation_pending(app)) {
		refresh_current(app);
		return;
	}
	app->retrying = false;
	app->waiting_retry = false;
	view = top(app);
	if (result == CLOCK_CLIENT_SNAPSHOT &&
	    request.op == CLOCK_REQUEST_SNAPSHOT && app->save_applied &&
	    app->client.result.storage.state == FPLINUX_STORAGE_DURABLE) {
		app->feedback[0] = '\0';
		app->save_failed = false;
		if (view && view->kind == VIEW_ALARM_EDIT &&
		    view->token == app->save_view_token &&
		    app->alarm.revision == app->save_draft_revision &&
		    !fplinux_ui_form_is_editing(view->screen))
			app->alarm.dirty = false;
	}
	if (request.op != CLOCK_REQUEST_SNAPSHOT) {
		bool current = view && view->token == app->pending_view_token &&
			       !app->alert &&
			       fplinux_ui_current(app->ui) == view->screen;

		if (result == CLOCK_CLIENT_ERROR || status != CLOCK_OK)
			app->home_requested = false;
		if (result == CLOCK_CLIENT_ERROR || status != CLOCK_OK) {
			bool accepted = result == CLOCK_CLIENT_SNAPSHOT &&
					app->client.result.applied;
			enum view_kind kind = view ? view->kind : VIEW_HOME;

			if (accepted) {
				if (request.op == CLOCK_REQUEST_ALARM_ADD ||
				    request.op == CLOCK_REQUEST_ALARM_EDIT)
					alarm_save_accepted(app, &request,
							    false);
				if (request.op == CLOCK_REQUEST_WORLD_ADD ||
				    request.op == CLOCK_REQUEST_WORLD_EDIT)
					world_save_accepted(
						app, request.id,
						request.offset_minutes, false);
				if (current && kind == VIEW_OFFSET &&
				    request.op == CLOCK_REQUEST_WORLD_DELETE)
					pop_view(app, false);
			}
			if (status == CLOCK_STORAGE_ERROR &&
			    result == CLOCK_CLIENT_SNAPSHOT) {
				const char *title =
					accepted ? _("Saving not confirmed") :
						   _("Change not saved");
				const char *cause = fplinux_ui_storage_error(
					app->client.result.storage);

				app->save_failed = true;
				app->save_applied = accepted;
				app->save_view_token = app->pending_view_token;
				app->save_draft_revision =
					app->pending_draft_revision;
				snprintf(app->feedback, sizeof(app->feedback),
					 "%s\n%s", title, cause);
				if (current)
					notice(app, title, cause);
			} else if (current && kind == VIEW_TIMER_EDIT)
				fplinux_ui_form_set_string(view->screen,
							   view->items[1], "");
			if (current && status == CLOCK_ACTIVE_EVENT)
				confirm(app,
					_("This alarm is ringing. Stop it and apply the change?"),
					&request, CONFIRM_ACTIVE);
			else if (current && status == CLOCK_INVALID &&
				 kind == VIEW_ALARM_EDIT &&
				 request.alarm.schedule == CLOCK_ALARM_ONCE) {
				app->feedback[0] = '\0';
				fplinux_ui_form_set_focus(view->screen,
							  view->items[2]);
				notice(app, _("Alarm"),
				       _("This time has passed. Choose another date or time."));
			} else if (current && status != CLOCK_STORAGE_ERROR) {
				const char *text = request_error(request.op);

				if (status == CLOCK_FULL)
					text = request.op == CLOCK_REQUEST_STOPWATCH_LAP ?
						       _("Lap history is full. Pause and reset to record more.") :
					       request.op == CLOCK_REQUEST_WORLD_ADD ?
						       _("The list is full. Delete a UTC offset first.") :
						       _("The list is full. Delete an item first.");
				if (status == CLOCK_INVALID &&
				    (request.op == CLOCK_REQUEST_WORLD_ADD ||
				     request.op == CLOCK_REQUEST_WORLD_EDIT))
					text = _(
						"This UTC offset is already in your list.");
				notice(app, _("Clock"), text);
			}
			if (status != CLOCK_ACTIVE_EVENT)
				app->pending_close = false;
		} else {
			if (current && request.op == CLOCK_REQUEST_SAVE &&
			    app->pending_close) {
				uint64_t revision =
					view->kind == VIEW_TIMER_EDIT ?
						app->timer_draft_revision :
						app->alarm.revision;

				if (revision != app->pending_draft_revision)
					app->pending_close = false;
				else if (view->kind == VIEW_ALARM_EDIT)
					app->alarm.dirty = false;
			}
			if (request.op == CLOCK_REQUEST_ALARM_ADD ||
			    request.op == CLOCK_REQUEST_ALARM_EDIT)
				alarm_save_accepted(app, &request, true);
			if (request.op == CLOCK_REQUEST_WORLD_ADD ||
			    request.op == CLOCK_REQUEST_WORLD_EDIT)
				world_save_accepted(app, request.id,
						    request.offset_minutes,
						    true);
			if (current && request.op == CLOCK_REQUEST_TIMER_SET &&
			    app->timer_draft_revision !=
				    app->pending_draft_revision)
				app->pending_close = false;
			if (current && fplinux_ui_form_is_editing(view->screen))
				app->pending_close = false;
			if (current && app->pending_close)
				pop_view(app, false);
			else if (current &&
				 request.op == CLOCK_REQUEST_WORLD_DELETE &&
				 view->kind == VIEW_OFFSET)
				pop_view(app, false);
			app->pending_close = false;
		}
	}
	refresh_current(app);
	if (app->home_requested && request.op != CLOCK_REQUEST_SNAPSHOT &&
	    result == CLOCK_CLIENT_SNAPSHOT && status == CLOCK_OK) {
		app->home_requested = false;
		open_home(app);
	}
}

int main(int argc, char **argv)
{
	struct fplinux_cli cli = {
		.program = "fplinux-clocks",
		.description =
			"Alarms, countdown timer, stopwatch and world clocks",
	};
	struct clock_app app = { 0 };
	struct clock_request request = { .op = CLOCK_REQUEST_SNAPSHOT };
	int result;

	result = fplinux_cli_parse(&cli, argc, argv);
	if (result != FPLINUX_CLI_READY)
		return result;
	app.ui = fplinux_ui_open(APP_ID, event, &app);
	if (!app.ui) {
		perror("fplinux-clocks: open UI");
		return 1;
	}
	fplinux_ui_set_close_handler(app.ui, prepare_close, &app);
	open_view(&app, VIEW_HOME, 0);
	if (!app.depth) {
		perror("fplinux-clocks: home screen");
		fplinux_ui_close(app.ui);
		return 1;
	}
	app.poll_timer = lv_timer_create(poll, 200, &app);
	if (!app.poll_timer) {
		perror("fplinux-clocks: poll timer");
		fplinux_ui_close(app.ui);
		return 1;
	}
	if (!is_active(&app))
		lv_timer_pause(app.poll_timer);
	clock_client_send(&app.client, app.ui, &request, fplinux_ui_now_ms());
	result = fplinux_ui_run(app.ui);
	app.closing = true;
	lv_timer_delete(app.poll_timer);
	fplinux_ui_close(app.ui);
	return result < 0;
}
