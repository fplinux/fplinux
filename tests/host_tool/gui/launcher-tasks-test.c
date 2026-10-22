/* SPDX-License-Identifier: GPL-2.0-only */
/* Production Tasks callbacks with controlled native widgets and IPC. */
#include "launcher-tasks.h"
#include "fplinux-ui-screen.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct row {
	char text[128];
	char detail[128];
	const void *icon;
};

struct fplinux_ui_screen {
	char title[128];
	char text[256];
	char select_label[128];
	struct row rows[8];
	size_t count;
	size_t focus;
	struct fplinux_ui_command commands[8];
	size_t command_count;
	fplinux_ui_command_cb command;
	fplinux_ui_change_cb changed;
	void *context;
};

struct fplinux_ui {
	struct fplinux_ui_screen base;
	struct fplinux_ui_screen *stack[8];
	size_t depth;
	struct fplinux_ui_session_message sent[32];
	size_t sent_count;
	bool fail_send;
	struct fplinux_launcher_tasks *tasks;
	bool tasks_open;
};

static const unsigned char notes_icon, editor_icon, generic_icon;

static const void *task_icon(const char *app_id, void *context)
{
	(void)context;
	if (!strcmp(app_id, "org.example.notes"))
		return &notes_icon;
	if (!strcmp(app_id, "org.example.editor"))
		return &editor_icon;
	return &generic_icon;
}

static void view_changed(void *context)
{
	struct fplinux_ui *ui = context;

	ui->tasks_open = fplinux_launcher_tasks_is_open(ui->tasks);
}

bool fplinux_ui_send(struct fplinux_ui *ui,
		     const struct fplinux_ui_session_message *message)
{
	if (ui->fail_send)
		return false;
	assert(ui->sent_count < 32);
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
	assert(ui->depth < 8);
	ui->stack[ui->depth++] = screen;
}

bool fplinux_ui_pop(struct fplinux_ui *ui)
{
	if (ui->depth == 1)
		return false;
	free(ui->stack[--ui->depth]);
	return true;
}

static struct fplinux_ui_screen *new_screen(const char *title)
{
	struct fplinux_ui_screen *screen = calloc(1, sizeof(*screen));

	assert(screen);
	snprintf(screen->title, sizeof(screen->title), "%s", title);
	screen->focus = SIZE_MAX;
	return screen;
}

struct fplinux_ui_screen *fplinux_ui_list_new(struct fplinux_ui *ui,
					      const char *title,
					      enum fplinux_ui_list_kind kind)
{
	(void)ui;
	assert(kind == FPLINUX_UI_LIST_IMPLICIT);
	return new_screen(title);
}

struct fplinux_ui_screen *fplinux_ui_alert_new(struct fplinux_ui *ui,
					       enum fplinux_ui_alert_kind kind,
					       const char *title,
					       const char *text,
					       unsigned int timeout_ms)
{
	struct fplinux_ui_screen *screen = new_screen(title);

	(void)ui;
	assert(kind == FPLINUX_UI_ALERT_ERROR ||
	       kind == FPLINUX_UI_ALERT_CONFIRMATION);
	assert(!timeout_ms);
	snprintf(screen->text, sizeof(screen->text), "%s", text);
	return screen;
}

void fplinux_ui_screen_set_listener(struct fplinux_ui_screen *screen,
				    fplinux_ui_command_cb command,
				    fplinux_ui_change_cb change, void *context)
{
	screen->command = command;
	screen->changed = change;
	screen->context = context;
}

bool fplinux_ui_screen_add_command(struct fplinux_ui_screen *screen,
				   const struct fplinux_ui_command *command)
{
	for (size_t i = 0; i < screen->command_count; ++i)
		if (screen->commands[i].id == command->id) {
			screen->commands[i] = *command;
			return true;
		}
	assert(screen->command_count < 8);
	screen->commands[screen->command_count++] = *command;
	return true;
}

void fplinux_ui_screen_remove_command(struct fplinux_ui_screen *screen, int id)
{
	for (size_t i = 0; i < screen->command_count; ++i) {
		if (screen->commands[i].id != id)
			continue;
		--screen->command_count;
		memmove(&screen->commands[i], &screen->commands[i + 1],
			(screen->command_count - i) *
				sizeof(*screen->commands));
		return;
	}
}

bool fplinux_ui_list_set_select_label(struct fplinux_ui_screen *screen,
				      const char *label)
{
	snprintf(screen->select_label, sizeof(screen->select_label), "%s",
		 label);
	return true;
}

size_t fplinux_ui_list_get_focused(const struct fplinux_ui_screen *screen)
{
	return screen->focus;
}

void fplinux_ui_list_set_focused(struct fplinux_ui_screen *screen, size_t index)
{
	assert(index < screen->count);
	screen->focus = index;
}

void fplinux_ui_list_delete_all(struct fplinux_ui_screen *screen)
{
	screen->count = 0;
	screen->focus = SIZE_MAX;
}

size_t fplinux_ui_list_append(struct fplinux_ui_screen *screen,
			      const char *text, const char *detail)
{
	size_t index = screen->count++;

	assert(index < 8);
	screen->rows[index].icon = NULL;
	snprintf(screen->rows[index].text, sizeof(screen->rows[index].text),
		 "%s", text);
	snprintf(screen->rows[index].detail, sizeof(screen->rows[index].detail),
		 "%s", detail ? detail : "");
	if (!index)
		screen->focus = 0;
	return index;
}

bool fplinux_ui_list_set_icon(struct fplinux_ui_screen *screen, size_t index,
			      const void *icon)
{
	assert(index < screen->count);
	screen->rows[index].icon = icon;
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

static void invoke(struct fplinux_ui *ui, enum fplinux_ui_command_type type)
{
	struct fplinux_ui_screen *screen = fplinux_ui_current(ui);
	const struct fplinux_ui_command *command = find_command(screen, type);

	assert(command && screen->command);
	screen->command(ui, screen, command->id, screen->context);
}

static void select_row(struct fplinux_ui *ui)
{
	struct fplinux_ui_screen *screen = fplinux_ui_current(ui);

	assert(screen->command);
	screen->command(ui, screen, FPLINUX_UI_SELECT_COMMAND, screen->context);
}

static void alert_response(struct fplinux_ui *ui,
			   enum fplinux_ui_command_type type)
{
	struct fplinux_ui_screen *screen = fplinux_ui_current(ui);
	const struct fplinux_ui_command *command = find_command(screen, type);
	int command_id;

	assert(command && screen->command && ui->depth > 1);
	command_id = command->id;
	/* The native Alert removes itself, invokes the listener, then deletes. */
	--ui->depth;
	screen->command(ui, screen, command_id, screen->context);
	free(screen);
}

static void focus_row(struct fplinux_ui *ui, size_t index)
{
	struct fplinux_ui_screen *screen = fplinux_ui_current(ui);

	fplinux_ui_list_set_focused(screen, index);
	assert(screen->changed);
	screen->changed(ui, screen, index, screen->context);
}

static void snapshot(struct fplinux_launcher_tasks *tasks,
		     uint32_t editor_token)
{
	struct fplinux_ui_session_message message = {
		.type = FPLINUX_UI_SESSION_TASKS,
		.has_tasks = true,
		.task_count = 2,
		.tasks = {
			{ .app_id = "org.example.notes", .title = "Notes",
			  .lifecycle = FPLINUX_UI_SESSION_BACKGROUND, .token = 101 },
			{ .app_id = "org.example.editor", .title = "Editor",
			  .lifecycle = FPLINUX_UI_SESSION_FOREGROUND,
			  .token = editor_token },
		},
	};

	assert(fplinux_launcher_tasks_receive(tasks, &message));
}

static void test_confirm(struct fplinux_ui *ui,
			 struct fplinux_launcher_tasks *tasks)
{
	fplinux_launcher_tasks_open(tasks);
	assert(ui->sent_count == 1 &&
	       ui->sent[0].type == FPLINUX_UI_SESSION_TASKS &&
	       !ui->sent[0].has_tasks);
	assert(!fplinux_ui_current(ui)->count);
	select_row(ui);
	assert(ui->sent_count == 1);
	snapshot(tasks, 202);
	focus_row(ui, 1);
	assert(!strcmp(fplinux_ui_current(ui)->rows[1].detail, "Foreground"));
	invoke(ui, FPLINUX_UI_COMMAND_SCREEN);
	assert(!strcmp(fplinux_ui_current(ui)->title, "Close app"));
	assert(!strcmp(fplinux_ui_current(ui)->text,
		       "Close Editor? Unsaved changes may be lost."));
	/* A replacement arrives while the old instance's confirmation is open. */
	snapshot(tasks, 303);
	alert_response(ui, FPLINUX_UI_COMMAND_OK);
	assert(ui->sent_count == 3);
	assert(ui->sent[1].type == FPLINUX_UI_SESSION_CLOSE_TASK &&
	       !strcmp(ui->sent[1].app_id, "org.example.editor") &&
	       ui->sent[1].token == 202);
	assert(ui->sent[2].type == FPLINUX_UI_SESSION_TASKS);
	snapshot(tasks, 303);
	focus_row(ui, 1);
	invoke(ui, FPLINUX_UI_COMMAND_SCREEN);
	alert_response(ui, FPLINUX_UI_COMMAND_CANCEL);
	assert(ui->sent_count == 4 &&
	       ui->sent[3].type == FPLINUX_UI_SESSION_TASKS);
}

static void test_resume(struct fplinux_ui *ui,
			struct fplinux_launcher_tasks *tasks)
{
	fplinux_launcher_tasks_open(tasks);
	snapshot(tasks, 202);
	focus_row(ui, 1);
	select_row(ui);
	assert(ui->sent_count == 2 &&
	       ui->sent[1].type == FPLINUX_UI_SESSION_ACTIVATE &&
	       !strcmp(ui->sent[1].app_id, "org.example.editor") &&
	       ui->sent[1].token == 202);
	assert(ui->depth == 1);
	assert(!ui->tasks_open);
	struct fplinux_ui_session_message error = {
		.type = FPLINUX_UI_SESSION_ERROR,
		.reason = "task-unavailable",
	};

	assert(fplinux_launcher_tasks_receive(tasks, &error));
	assert(ui->tasks_open);
	assert(!strcmp(
		fplinux_ui_current(ui)->text,
		"This application is no longer available. Choose another."));
	alert_response(ui, FPLINUX_UI_COMMAND_BACK);
	assert(ui->depth == 1);
	assert(!ui->tasks_open);
}

static void test_states(struct fplinux_ui *ui,
			struct fplinux_launcher_tasks *tasks)
{
	struct fplinux_ui_session_message message = {
		.type = FPLINUX_UI_SESSION_TASKS,
		.has_tasks = true,
	};

	fplinux_launcher_tasks_open(tasks);
	assert(fplinux_launcher_tasks_receive(tasks, &message));
	struct fplinux_ui_screen *list = fplinux_ui_current(ui);

	assert(list->count == 1 && !strcmp(list->rows[0].text, "No open apps"));
	assert(!list->select_label[0] &&
	       !find_command(list, FPLINUX_UI_COMMAND_SCREEN));
	select_row(ui);
	assert(ui->sent_count == 1);
	snapshot(tasks, 202);
	focus_row(ui, 1);
	message.task_count = 1;
	message.tasks[0] = (struct fplinux_ui_session_task){
		.app_id = "org.example.editor",
		.title = "Editor",
		.lifecycle = FPLINUX_UI_SESSION_CLOSING,
		.token = 202,
	};
	assert(fplinux_launcher_tasks_receive(tasks, &message));
	assert(list->focus == 0 && !strcmp(list->rows[0].text, "Editor"));
	assert(!strcmp(list->rows[0].detail, "Closing") &&
	       !strcmp(list->select_label, "Closing"));
	assert(!find_command(list, FPLINUX_UI_COMMAND_SCREEN));
	select_row(ui);
	assert(ui->sent_count == 1);
}

static void test_refresh(struct fplinux_ui *ui,
			 struct fplinux_launcher_tasks *tasks)
{
	fplinux_launcher_tasks_open(tasks);
	assert(ui->tasks_open);
	struct fplinux_ui_screen *list = fplinux_ui_current(ui);

	assert(!list->select_label[0]);
	assert(!list->count);
	assert(find_command(list, FPLINUX_UI_COMMAND_BACK));
	select_row(ui);
	assert(ui->sent_count == 1);
	snapshot(tasks, 202);
	assert(!strcmp(list->select_label, "Open"));
	fplinux_launcher_tasks_close(tasks);
	assert(!ui->tasks_open);
	fplinux_launcher_tasks_open(tasks);
	list = fplinux_ui_current(ui);
	assert(list->count == 2 && !strcmp(list->rows[0].text, "Notes"));
	assert(!list->select_label[0]);
	select_row(ui);
	assert(ui->sent_count == 2);
	snapshot(tasks, 202);
	assert(!strcmp(list->select_label, "Open"));
}

static void test_icons(struct fplinux_ui *ui,
		       struct fplinux_launcher_tasks *tasks)
{
	fplinux_launcher_tasks_open(tasks);
	snapshot(tasks, 202);
	struct fplinux_ui_screen *list = fplinux_ui_current(ui);

	assert(list->rows[0].icon == &notes_icon);
	assert(list->rows[1].icon == &editor_icon);
	struct fplinux_ui_session_message message = {
		.type = FPLINUX_UI_SESSION_TASKS,
		.has_tasks = true,
		.task_count = 2,
		.tasks = {
			{ .app_id = "org.example.editor", .title = "Draft",
			  .lifecycle = FPLINUX_UI_SESSION_BACKGROUND, .token = 202 },
			{ .app_id = "org.example.hidden", .title = "Hidden app",
			  .lifecycle = FPLINUX_UI_SESSION_BACKGROUND, .token = 303 },
		},
	};

	assert(fplinux_launcher_tasks_receive(tasks, &message));
	assert(!strcmp(list->rows[0].text, "Draft"));
	assert(list->rows[0].icon == &editor_icon);
	assert(list->rows[1].icon == &generic_icon);
}

static void test_interruptions(struct fplinux_ui *ui,
			       struct fplinux_launcher_tasks *tasks)
{
	fplinux_launcher_tasks_open(tasks);
	snapshot(tasks, 202);
	invoke(ui, FPLINUX_UI_COMMAND_SCREEN);
	fplinux_launcher_tasks_set_blocked(tasks, true);
	assert(ui->depth == 1);
	assert(!ui->tasks_open);
	fplinux_launcher_tasks_open(tasks);
	assert(ui->depth == 1 && ui->sent_count == 1);
	fplinux_launcher_tasks_set_blocked(tasks, false);
	fplinux_launcher_tasks_open(tasks);
	assert(ui->depth == 2 && ui->sent_count == 2);
	fplinux_launcher_tasks_close(tasks);
	assert(ui->depth == 1);
}

static void test_errors(struct fplinux_ui *ui,
			struct fplinux_launcher_tasks *tasks)
{
	ui->fail_send = true;
	fplinux_launcher_tasks_open(tasks);
	assert(!strcmp(fplinux_ui_current(ui)->text,
		       "Cannot read open applications. Try again."));
	alert_response(ui, FPLINUX_UI_COMMAND_BACK);
	assert(ui->depth == 1 && !ui->sent_count);
}

int main(int argc, char **argv)
{
	struct fplinux_ui ui = { .depth = 1 };
	struct fplinux_launcher_tasks tasks;

	assert(argc == 2);
	ui.stack[0] = &ui.base;
	ui.tasks = &tasks;
	fplinux_launcher_tasks_init(
		&tasks, &ui, &ui.base,
		&(const struct fplinux_launcher_tasks_listener){
			.icon = task_icon,
			.view_changed = view_changed,
			.context = &ui,
		});
	if (!strcmp(argv[1], "confirm"))
		test_confirm(&ui, &tasks);
	else if (!strcmp(argv[1], "resume"))
		test_resume(&ui, &tasks);
	else if (!strcmp(argv[1], "states"))
		test_states(&ui, &tasks);
	else if (!strcmp(argv[1], "refresh"))
		test_refresh(&ui, &tasks);
	else if (!strcmp(argv[1], "icons"))
		test_icons(&ui, &tasks);
	else if (!strcmp(argv[1], "interruptions"))
		test_interruptions(&ui, &tasks);
	else if (!strcmp(argv[1], "errors"))
		test_errors(&ui, &tasks);
	else
		assert(false);
	fplinux_launcher_tasks_close(&tasks);
	assert(ui.depth == 1);
	puts("launcher-tasks: scenario passed");
	return 0;
}
