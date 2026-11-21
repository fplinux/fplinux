/* SPDX-License-Identifier: GPL-2.0-only */
#include "launcher-tasks.h"
#include "fplinux-ui-screen.h"

#include <libintl.h>
#include <stdio.h>
#include <string.h>

#define _(text) gettext(text)

enum task_command {
	TASK_COMMAND_CLOSE = 100,
	TASK_COMMAND_BACK,
	TASK_COMMAND_CONFIRM,
	TASK_COMMAND_CANCEL,
};

static void task_command(struct fplinux_ui *ui,
			 struct fplinux_ui_screen *screen, int command_id,
			 void *context);

void fplinux_launcher_tasks_init(
	struct fplinux_launcher_tasks *tasks, struct fplinux_ui *ui,
	const struct fplinux_launcher_tasks_listener *listener)
{
	memset(tasks, 0, sizeof(*tasks));
	tasks->ui = ui;
	if (listener)
		tasks->listener = *listener;
}

bool fplinux_launcher_tasks_is_open(const struct fplinux_launcher_tasks *tasks)
{
	return tasks->list || tasks->alert;
}

static void view_changed(struct fplinux_launcher_tasks *tasks)
{
	if (tasks->listener.view_changed)
		tasks->listener.view_changed(tasks->listener.context);
}

static void remember_focus(struct fplinux_launcher_tasks *tasks)
{
	if (!tasks->list)
		return;
	tasks->focused_row = fplinux_ui_list_get_focused(tasks->list);
	if (tasks->focused_row >= tasks->count)
		tasks->focused_row = 0;
	tasks->focused_token = tasks->focused_row < tasks->count ?
				       tasks->tasks[tasks->focused_row].token :
				       0;
}

void fplinux_launcher_tasks_close(struct fplinux_launcher_tasks *tasks)
{
	struct fplinux_ui_screen *list = tasks->list;
	struct fplinux_ui_screen *alert = tasks->alert;

	if (!tasks->list && !tasks->alert)
		return;
	remember_focus(tasks);
	tasks->list = NULL;
	tasks->alert = NULL;
	tasks->querying = false;
	memset(&tasks->confirmed, 0, sizeof(tasks->confirmed));
	if (alert && fplinux_ui_current(tasks->ui) == alert)
		fplinux_ui_pop(tasks->ui);
	if (list && fplinux_ui_current(tasks->ui) == list)
		fplinux_ui_pop(tasks->ui);
	view_changed(tasks);
}

void fplinux_launcher_tasks_set_blocked(struct fplinux_launcher_tasks *tasks,
					bool blocked)
{
	tasks->blocked = blocked;
	if (blocked)
		fplinux_launcher_tasks_close(tasks);
}

static bool query_tasks(struct fplinux_launcher_tasks *tasks)
{
	struct fplinux_ui_session_message message = {
		.type = FPLINUX_UI_SESSION_TASKS,
	};

	tasks->querying = fplinux_ui_send(tasks->ui, &message);
	return tasks->querying;
}

static void update_commands(struct fplinux_launcher_tasks *tasks)
{
	size_t focused;
	bool waiting;
	bool usable;
	struct fplinux_ui_command close = {
		.id = TASK_COMMAND_CLOSE,
		.type = FPLINUX_UI_COMMAND_SCREEN,
		.label = _("Quit"),
		.direct = true,
		.priority = 1,
		.icon = fplinux_ui_glyph(tasks->ui, "exit",
					 FPLINUX_UI_GLYPH_ROW),
	};

	if (!tasks->list)
		return;
	focused = fplinux_ui_list_get_focused(tasks->list);
	waiting = tasks->querying || tasks->request_pending;
	usable = !waiting && focused < tasks->count &&
		 tasks->tasks[focused].lifecycle != FPLINUX_UI_SESSION_CLOSING;
	fplinux_ui_list_set_select_label(tasks->list, waiting ? "" :
						      usable  ? _("Open") :
						      focused < tasks->count ?
							       _("Closing") :
							       "");
	if (usable)
		fplinux_ui_screen_add_command(tasks->list, &close);
	else
		fplinux_ui_screen_remove_command(tasks->list,
						 TASK_COMMAND_CLOSE);
}

static void task_changed(struct fplinux_ui *ui,
			 struct fplinux_ui_screen *screen, size_t index,
			 void *context)
{
	struct fplinux_launcher_tasks *tasks = context;

	(void)ui;
	(void)index;
	if (screen == tasks->list)
		update_commands(tasks);
}

static void task_error(struct fplinux_launcher_tasks *tasks, const char *text)
{
	const struct fplinux_ui_command close = {
		.id = TASK_COMMAND_CANCEL,
		.type = FPLINUX_UI_COMMAND_BACK,
		.label = _("Close"),
	};

	if (tasks->alert || tasks->blocked)
		return;
	tasks->alert = fplinux_ui_alert_new(tasks->ui, FPLINUX_UI_ALERT_ERROR,
					    _("Open apps"), text, 0);
	if (!tasks->alert)
		return;
	fplinux_ui_screen_add_command(tasks->alert, &close);
	fplinux_ui_screen_set_listener(tasks->alert, task_command, NULL, tasks);
	fplinux_ui_push(tasks->ui, tasks->alert);
	view_changed(tasks);
}

static void task_command(struct fplinux_ui *ui,
			 struct fplinux_ui_screen *screen, int command_id,
			 void *context)
{
	struct fplinux_launcher_tasks *tasks = context;
	struct fplinux_ui_session_message request = { 0 };
	size_t focused;
	const struct fplinux_ui_session_task *task;

	if (screen == tasks->alert) {
		/* Native alerts leave the stack before their listener runs. */
		tasks->alert = NULL;
		if (command_id == TASK_COMMAND_CONFIRM && !tasks->blocked) {
			request.type = FPLINUX_UI_SESSION_CLOSE_TASK;
			strcpy(request.app_id, tasks->confirmed.app_id);
			request.token = tasks->confirmed.token;
			tasks->request_pending = fplinux_ui_send(ui, &request);
			if (!tasks->request_pending)
				task_error(
					tasks,
					_("Cannot close the application. Try again."));
		}
		memset(&tasks->confirmed, 0, sizeof(tasks->confirmed));
		if (tasks->list)
			query_tasks(tasks);
		update_commands(tasks);
		view_changed(tasks);
		return;
	}
	if (screen != tasks->list)
		return;
	if (command_id == TASK_COMMAND_BACK) {
		fplinux_launcher_tasks_close(tasks);
		return;
	}
	focused = fplinux_ui_list_get_focused(screen);
	if (tasks->blocked || tasks->querying || tasks->request_pending ||
	    focused >= tasks->count ||
	    tasks->tasks[focused].lifecycle == FPLINUX_UI_SESSION_CLOSING)
		return;
	task = &tasks->tasks[focused];
	if (command_id == FPLINUX_UI_SELECT_COMMAND) {
		request.type = FPLINUX_UI_SESSION_ACTIVATE;
		strcpy(request.app_id, task->app_id);
		request.token = task->token;
		if (fplinux_ui_send(ui, &request)) {
			fplinux_launcher_tasks_close(tasks);
			tasks->request_pending = true;
		} else {
			task_error(tasks,
				   _("Cannot switch applications. Try again."));
		}
	} else if (command_id == TASK_COMMAND_CLOSE) {
		char text[2 * FPLINUX_UI_SESSION_TEXT_BYTES];
		const struct fplinux_ui_command yes = {
			.id = TASK_COMMAND_CONFIRM,
			.type = FPLINUX_UI_COMMAND_OK,
			.label = _("Close"),
			.priority = 1,
		};
		const struct fplinux_ui_command no = {
			.id = TASK_COMMAND_CANCEL,
			.type = FPLINUX_UI_COMMAND_CANCEL,
			.label = _("Cancel"),
		};

		snprintf(text, sizeof(text),
			 _("Close “%s”? Unsaved changes may be lost."),
			 task->title);
		tasks->alert =
			fplinux_ui_alert_new(ui, FPLINUX_UI_ALERT_CONFIRMATION,
					     _("Close app"), text, 0);
		if (!tasks->alert)
			return;
		tasks->confirmed = *task;
		fplinux_ui_screen_add_command(tasks->alert, &yes);
		fplinux_ui_screen_add_command(tasks->alert, &no);
		fplinux_ui_screen_set_listener(tasks->alert, task_command, NULL,
					       tasks);
		fplinux_ui_push(ui, tasks->alert);
	}
}

static const char *task_state(const struct fplinux_ui_session_task *task)
{
	if (task->lifecycle == FPLINUX_UI_SESSION_CLOSING)
		return _("Closing");
	switch (task->playback) {
	case FPLINUX_UI_SESSION_PLAYBACK_PLAYING:
		return _("Playing");
	case FPLINUX_UI_SESSION_PLAYBACK_PAUSED:
		return _("Paused");
	case FPLINUX_UI_SESSION_PLAYBACK_HELD:
		return _("Audio interrupted");
	case FPLINUX_UI_SESSION_PLAYBACK_UNAVAILABLE:
		return _("Audio unavailable");
	case FPLINUX_UI_SESSION_PLAYBACK_NONE:
		return task->lifecycle == FPLINUX_UI_SESSION_FOREGROUND ?
			       _("Foreground") :
			       _("Background");
	}
	return "";
}

static void update_list(struct fplinux_launcher_tasks *tasks)
{
	size_t focus = tasks->focused_row;

	if (!tasks->list)
		return;
	if (focus >= tasks->count)
		focus = tasks->count ? tasks->count - 1 : 0;
	fplinux_ui_list_delete_all(tasks->list);
	for (size_t i = 0; i < tasks->count; ++i) {
		const struct fplinux_ui_session_task *task = &tasks->tasks[i];
		size_t row = fplinux_ui_list_append(tasks->list, task->title,
						    task_state(task));

		if (tasks->listener.icon) {
			struct fplinux_launcher_task_icon icon =
				tasks->listener.icon(task->app_id,
						     tasks->listener.context);

			fplinux_ui_list_set_icon(tasks->list, row, icon.source,
						 icon.kind);
		}
		if (task->token == tasks->focused_token)
			focus = i;
	}
	if (!tasks->count && tasks->known)
		fplinux_ui_list_append(tasks->list, _("No open apps"), NULL);
	if (tasks->count || tasks->known)
		fplinux_ui_list_set_focused(tasks->list, focus);
	update_commands(tasks);
}

bool fplinux_launcher_tasks_receive(
	struct fplinux_launcher_tasks *tasks,
	const struct fplinux_ui_session_message *message)
{
	if (message->type == FPLINUX_UI_SESSION_TASKS) {
		if (!message->has_tasks)
			return true;
		remember_focus(tasks);
		tasks->count = message->task_count;
		memcpy(tasks->tasks, message->tasks, sizeof(tasks->tasks));
		tasks->known = true;
		tasks->querying = false;
		tasks->request_pending = false;
		update_list(tasks);
		return true;
	}
	if (message->type == FPLINUX_UI_SESSION_ERROR &&
	    (tasks->request_pending || tasks->list)) {
		tasks->request_pending = false;
		tasks->querying = false;
		update_commands(tasks);
		task_error(
			tasks,
			_("This application is no longer available. Choose another."));
		return true;
	}
	return false;
}

void fplinux_launcher_tasks_open(struct fplinux_launcher_tasks *tasks)
{
	const struct fplinux_ui_command back = {
		.id = TASK_COMMAND_BACK,
		.type = FPLINUX_UI_COMMAND_BACK,
		.label = _("Back"),
	};

	if (tasks->list || tasks->alert || tasks->blocked)
		return;
	tasks->list = fplinux_ui_list_new(tasks->ui, _("Open apps"),
					  FPLINUX_UI_LIST_IMPLICIT);
	if (!tasks->list)
		return;
	fplinux_ui_screen_add_command(tasks->list, &back);
	fplinux_ui_screen_set_listener(tasks->list, task_command, NULL, tasks);
	fplinux_ui_list_set_focus_listener(tasks->list, task_changed, tasks);
	update_list(tasks);
	fplinux_ui_push(tasks->ui, tasks->list);
	view_changed(tasks);
	if (!query_tasks(tasks)) {
		fplinux_launcher_tasks_close(tasks);
		task_error(tasks,
			   _("Cannot read open applications. Try again."));
	}
	update_commands(tasks);
}
