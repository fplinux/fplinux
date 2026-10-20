/* SPDX-License-Identifier: 0BSD */
/* Controlled screen stack and persistence: no drawing or filesystem writes. */
#include <fplinux/fplinux-ui.h>
#include <fplinux/fplinux-ui-prefs.h>
#include <fplinux/fplinux-ui-screen.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct fplinux_ui_screen {
	char text[4097];
	fplinux_ui_command_cb listener;
	void *context;
	int save_command;
	int back_command;
	enum fplinux_ui_alert_kind kind;
	unsigned int timeout_ms;
};

struct fplinux_ui_prefs {
	char staged[4097];
	char saved[4097];
	bool fail_set;
	bool fail_save;
	bool closed;
};

struct fplinux_ui {
	struct fplinux_ui_screen *editor;
	struct fplinux_ui_screen *current;
	fplinux_ui_event_cb event_callback;
	void *event_context;
	unsigned int exits;
	char saved_at_exit[4097];
	bool fail_alert;
	bool closed;
};

static struct fplinux_ui_prefs preferences;
static struct fplinux_ui runtime;
static const char *scenario;
static bool resources_opened;

int fplinux_notes_application_main(int argc, char **argv);

struct fplinux_ui_prefs *fplinux_ui_prefs_open(const char *app_id)
{
	assert(!strcmp(app_id, "org.fplinux.notes"));
	resources_opened = true;
	snprintf(preferences.saved, sizeof(preferences.saved), "%s",
		 "Earlier note");
	return &preferences;
}

void fplinux_ui_prefs_close(struct fplinux_ui_prefs *prefs)
{
	assert(prefs == &preferences);
	prefs->closed = true;
}

const char *fplinux_ui_prefs_get(const struct fplinux_ui_prefs *prefs,
				 const char *key, const char *fallback)
{
	(void)fallback;
	assert(!strcmp(key, "note"));
	return prefs->saved;
}

bool fplinux_ui_prefs_set(struct fplinux_ui_prefs *prefs, const char *key,
			  const char *value)
{
	assert(!strcmp(key, "note"));
	if (prefs->fail_set) {
		errno = ENOMEM;
		return false;
	}
	assert(strlen(value) < sizeof(prefs->staged));
	snprintf(prefs->staged, sizeof(prefs->staged), "%s", value);
	return true;
}

bool fplinux_ui_prefs_save(struct fplinux_ui_prefs *prefs)
{
	if (prefs->fail_save) {
		errno = ENOSPC;
		return false;
	}
	snprintf(prefs->saved, sizeof(prefs->saved), "%s", prefs->staged);
	return true;
}

struct fplinux_ui *fplinux_ui_open(const char *app_id,
				   fplinux_ui_event_cb callback, void *context)
{
	assert(!strcmp(app_id, "org.fplinux.notes"));
	runtime.event_callback = callback;
	runtime.event_context = context;
	return &runtime;
}

void fplinux_ui_close(struct fplinux_ui *ui)
{
	assert(ui == &runtime);
	if (ui->current != ui->editor)
		free(ui->current);
	free(ui->editor);
	ui->closed = true;
}

struct fplinux_ui_screen *fplinux_ui_text_box_new(struct fplinux_ui *ui,
						  const char *title,
						  const char *text,
						  size_t max_chars)
{
	assert(!strcmp(title, "Notes"));
	assert(max_chars == 1000);
	ui->editor = calloc(1, sizeof(*ui->editor));
	assert(ui->editor);
	snprintf(ui->editor->text, sizeof(ui->editor->text), "%s", text);
	return ui->editor;
}

const char *fplinux_ui_text_box_get(const struct fplinux_ui_screen *box)
{
	return box->text;
}

bool fplinux_ui_screen_add_command(struct fplinux_ui_screen *screen,
				   const struct fplinux_ui_command *command)
{
	if (command->type == FPLINUX_UI_COMMAND_BACK) {
		screen->back_command = command->id;
	} else {
		assert(command->type == FPLINUX_UI_COMMAND_SCREEN);
		assert(!strcmp(command->label, "Save"));
		screen->save_command = command->id;
	}
	return true;
}

void fplinux_ui_screen_set_listener(struct fplinux_ui_screen *screen,
				    fplinux_ui_command_cb command,
				    fplinux_ui_change_cb change, void *context)
{
	(void)change;
	screen->listener = command;
	screen->context = context;
}

struct fplinux_ui_screen *fplinux_ui_alert_new(struct fplinux_ui *ui,
					       enum fplinux_ui_alert_kind kind,
					       const char *title,
					       const char *text,
					       unsigned int timeout_ms)
{
	struct fplinux_ui_screen *alert;

	(void)title;
	if (ui->fail_alert) {
		errno = ENOMEM;
		return NULL;
	}
	alert = calloc(1, sizeof(*alert));
	assert(alert);
	alert->kind = kind;
	alert->timeout_ms = timeout_ms;
	snprintf(alert->text, sizeof(alert->text), "%s", text);
	return alert;
}

void fplinux_ui_push(struct fplinux_ui *ui, struct fplinux_ui_screen *screen)
{
	ui->current = screen;
}

void fplinux_ui_go_back(struct fplinux_ui *ui)
{
	ui->exits++;
	snprintf(ui->saved_at_exit, sizeof(ui->saved_at_exit), "%s",
		 preferences.saved);
}

static void choose(struct fplinux_ui *ui, int command)
{
	assert(ui->current == ui->editor);
	ui->editor->listener(ui, ui->editor, command, ui->editor->context);
}

static void send_event(struct fplinux_ui *ui, enum fplinux_ui_event_type type,
		       bool visible)
{
	const struct fplinux_ui_event event = { .type = type,
						.visible = visible };

	ui->event_callback(ui, &event, ui->event_context);
}

static void check_failed_save(struct fplinux_ui *ui, const char *draft)
{
	preferences.fail_set = !strcmp(scenario, "set_failure");
	preferences.fail_save = !preferences.fail_set;
	ui->fail_alert = !strcmp(scenario, "alert_failure");
	choose(ui, ui->editor->save_command);
	assert(ui->exits == 0);
	assert(!strcmp(preferences.saved, "Earlier note"));
	assert(!strcmp(ui->editor->text, draft));
	if (ui->fail_alert) {
		assert(ui->current == ui->editor);
	} else {
		assert(ui->current != ui->editor);
		assert(ui->current->kind == FPLINUX_UI_ALERT_ERROR);
		assert(ui->current->timeout_ms == 0);
		assert(!strcmp(
			ui->current->text,
			"The note was not saved. Close and try Save again."));
		/* Closing the modal returns to the existing editor. */
		free(ui->current);
		ui->current = ui->editor;
	}
	preferences.fail_set = preferences.fail_save = false;
	choose(ui, ui->editor->save_command);
	assert(ui->exits == 1);
	assert(!strcmp(ui->saved_at_exit, draft));
}

int fplinux_ui_run(struct fplinux_ui *ui)
{
	const char *draft = "Call at 18:00\nBring the book";

	assert(!strcmp(ui->editor->text, "Earlier note"));
	if (!strcmp(scenario, "empty"))
		draft = "";
	else if (!strcmp(scenario, "unicode"))
		draft = "Позвонить в 18:00\n📖 взять книгу";
	snprintf(ui->editor->text, sizeof(ui->editor->text), "%s", draft);
	if (!strcmp(scenario, "set_failure") ||
	    !strcmp(scenario, "save_failure") ||
	    !strcmp(scenario, "alert_failure")) {
		check_failed_save(ui, draft);
	} else if (!strcmp(scenario, "success") || !strcmp(scenario, "empty") ||
		   !strcmp(scenario, "unicode")) {
		choose(ui, ui->editor->save_command);
		assert(ui->exits == 1);
		assert(!strcmp(ui->saved_at_exit, draft));
		assert(ui->current == ui->editor);
	} else if (!strcmp(scenario, "autosave")) {
		send_event(ui, FPLINUX_UI_EVENT_VISIBILITY, true);
		assert(!strcmp(preferences.saved, "Earlier note"));
		send_event(ui, FPLINUX_UI_EVENT_VISIBILITY, false);
		assert(!strcmp(preferences.saved, draft));
		assert(ui->exits == 0 && ui->current == ui->editor);
		snprintf(ui->editor->text, sizeof(ui->editor->text), "%s",
			 "Second edit");
		choose(ui, ui->editor->back_command);
		assert(ui->exits == 1);
		send_event(ui, FPLINUX_UI_EVENT_CLOSE, false);
		assert(!strcmp(preferences.saved, "Second edit"));
	} else {
		assert(!strcmp(scenario, "implicit_failure"));
		preferences.fail_save = true;
		send_event(ui, FPLINUX_UI_EVENT_VISIBILITY, false);
		assert(!strcmp(preferences.saved, "Earlier note"));
		assert(!strcmp(ui->editor->text, draft));
		assert(ui->current == ui->editor && ui->exits == 0);
		preferences.fail_save = false;
		send_event(ui, FPLINUX_UI_EVENT_CLOSE, false);
		assert(!strcmp(preferences.saved, draft));
		assert(ui->current == ui->editor);
	}
	return 0;
}

int main(int argc, char **argv)
{
	char *application_argv[] = { "fplinux-notes", NULL };
	int result;

	if (argc > 1 && !strcmp(argv[1], "cli")) {
		result = fplinux_notes_application_main(argc - 1, argv + 1);
		assert(!resources_opened && !runtime.event_callback);
		return result;
	}
	assert(argc == 2);
	scenario = argv[1];
	assert(fplinux_notes_application_main(1, application_argv) == 0);
	assert(runtime.closed && preferences.closed);
	puts("notes-listeners: scenario passed");
	return 0;
}
