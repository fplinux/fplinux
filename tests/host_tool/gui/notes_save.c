/* SPDX-License-Identifier: 0BSD */
/* Controlled screen stack and persistence: no drawing or filesystem writes. */
#include <fplinux/fplinux-ui.h>
#include <fplinux/fplinux-ui-menu.h>
#include <fplinux/fplinux-ui-prefs.h>
#include <fplinux/fplinux-ui-screen.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct fplinux_ui_screen {
	char text[4097];
	char title[128];
	fplinux_ui_command_cb listener;
	fplinux_ui_change_cb changed;
	void *context;
	bool has_save;
	bool picker_open;
	size_t cursor;
	int save_command;
	int back_command;
	int retry_command;
	int stay_command;
	int discard_command;
	char discard_label[64];
	enum fplinux_ui_alert_kind kind;
	unsigned int timeout_ms;
};

struct fplinux_ui_prefs {
	char staged[4097];
	char saved[4097];
	bool fail_set;
	bool fail_save;
	bool fail_sync;
	bool closed;
	unsigned int writes;
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
	fplinux_ui_close_cb prepare_close;
	void *close_context;
	bool close_waiting;
	bool menu_open;
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
		 !strcmp(scenario, "clean_empty") ? "" : "Earlier note");
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

struct fplinux_storage_result
fplinux_ui_prefs_save(struct fplinux_ui_prefs *prefs)
{
	++prefs->writes;
	if (prefs->fail_save) {
		return (struct fplinux_storage_result){
			.state = FPLINUX_STORAGE_UNPUBLISHED, .error = ENOSPC
		};
	}
	snprintf(prefs->saved, sizeof(prefs->saved), "%s", prefs->staged);
	return (struct fplinux_storage_result){
		.state = prefs->fail_sync ? FPLINUX_STORAGE_PUBLISHED_UNSYNCED :
					    FPLINUX_STORAGE_DURABLE,
		.error = prefs->fail_sync ? EIO : 0
	};
}

const char *fplinux_ui_storage_error(struct fplinux_storage_result result)
{
	return result.state == FPLINUX_STORAGE_PUBLISHED_UNSYNCED ?
		       "Storage input/output error" :
		       "Storage write failed";
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
	snprintf(ui->editor->title, sizeof(ui->editor->title), "%s", title);
	return ui->editor;
}

const char *fplinux_ui_text_box_get(const struct fplinux_ui_screen *box)
{
	return box->text;
}

bool fplinux_ui_text_box_cancel_picker(struct fplinux_ui_screen *box)
{
	box->picker_open = false;
	return true;
}

void fplinux_ui_menu_close(struct fplinux_ui *ui)
{
	ui->menu_open = false;
}

const void *fplinux_ui_glyph(struct fplinux_ui *ui, const char *name,
			     enum fplinux_ui_glyph_size size)
{
	assert(ui == &runtime && size == FPLINUX_UI_GLYPH_ROW);
	return name;
}

bool fplinux_ui_screen_add_command(struct fplinux_ui_screen *screen,
				   const struct fplinux_ui_command *command)
{
	if (command->label && !strcmp(command->label, "Retry")) {
		assert(command->icon && !strcmp(command->icon, "refresh"));
		screen->retry_command = command->id;
	} else if (command->label && !strcmp(command->label, "Stay")) {
		assert(command->icon && !strcmp(command->icon, "cancel"));
		screen->stay_command = command->id;
	} else if (command->label &&
		   (!strcmp(command->label, "Discard changes") ||
		    !strcmp(command->label, "Close anyway"))) {
		assert(command->icon && !strcmp(command->icon, "delete"));
		screen->discard_command = command->id;
		snprintf(screen->discard_label, sizeof(screen->discard_label),
			 "%s", command->label);
	} else if (command->type == FPLINUX_UI_COMMAND_BACK) {
		assert(command->icon && !strcmp(command->icon, "back"));
		screen->back_command = command->id;
	} else {
		assert(command->type == FPLINUX_UI_COMMAND_SCREEN);
		assert(!strcmp(command->label, "Save"));
		assert(command->icon && !strcmp(command->icon, "save"));
		screen->save_command = command->id;
		screen->has_save = true;
	}
	return true;
}

void fplinux_ui_screen_remove_command(struct fplinux_ui_screen *screen, int id)
{
	assert(id == screen->save_command);
	screen->has_save = false;
}

bool fplinux_ui_screen_set_title(struct fplinux_ui_screen *screen,
				 const char *title)
{
	assert(screen && title && *title);
	snprintf(screen->title, sizeof(screen->title), "%s", title);
	return true;
}

void fplinux_ui_screen_set_listener(struct fplinux_ui_screen *screen,
				    fplinux_ui_command_cb command,
				    fplinux_ui_change_cb change, void *context)
{
	screen->listener = command;
	screen->changed = change;
	screen->context = context;
}

struct fplinux_ui_screen *fplinux_ui_alert_new(struct fplinux_ui *ui,
					       enum fplinux_ui_alert_kind kind,
					       const char *title,
					       const char *text,
					       unsigned int timeout_ms)
{
	struct fplinux_ui_screen *alert;

	if (ui->fail_alert) {
		errno = ENOMEM;
		return NULL;
	}
	alert = calloc(1, sizeof(*alert));
	assert(alert);
	alert->kind = kind;
	alert->timeout_ms = timeout_ms;
	snprintf(alert->title, sizeof(alert->title), "%s", title ? title : "");
	snprintf(alert->text, sizeof(alert->text), "%s", text);
	return alert;
}

bool fplinux_ui_push(struct fplinux_ui *ui, struct fplinux_ui_screen *screen)
{
	ui->current = screen;
	return true;
}

struct fplinux_ui_screen *fplinux_ui_current(const struct fplinux_ui *ui)
{
	return ui->current;
}

bool fplinux_ui_pop(struct fplinux_ui *ui)
{
	assert(ui->current != ui->editor);
	free(ui->current);
	ui->current = ui->editor;
	return true;
}

static void finish_close(struct fplinux_ui *ui)
{
	ui->exits++;
	snprintf(ui->saved_at_exit, sizeof(ui->saved_at_exit), "%s",
		 preferences.saved);
}

void fplinux_ui_set_close_handler(struct fplinux_ui *ui,
				  fplinux_ui_close_cb callback, void *context)
{
	ui->prepare_close = callback;
	ui->close_context = context;
}

bool fplinux_ui_close_reply(struct fplinux_ui *ui,
			    enum fplinux_ui_close_response response)
{
	assert(ui->close_waiting);
	ui->close_waiting = response == FPLINUX_UI_CLOSE_WAIT;
	if (response == FPLINUX_UI_CLOSE_READY)
		finish_close(ui);
	return true;
}

void fplinux_ui_go_back(struct fplinux_ui *ui)
{
	enum fplinux_ui_close_response response =
		ui->prepare_close(ui, ui->close_context);

	ui->close_waiting = response == FPLINUX_UI_CLOSE_WAIT;
	if (response == FPLINUX_UI_CLOSE_READY)
		finish_close(ui);
}

static void choose(struct fplinux_ui *ui, int command)
{
	struct fplinux_ui_screen *screen = ui->current;

	if (screen != ui->editor)
		ui->current = ui->editor;
	screen->listener(ui, screen, command, screen->context);
	if (screen != ui->editor)
		free(screen);
}

static void send_event(struct fplinux_ui *ui, enum fplinux_ui_event_type type,
		       bool visible)
{
	const struct fplinux_ui_event event = { .type = type,
						.visible = visible };

	ui->event_callback(ui, &event, ui->event_context);
}

static void activate(struct fplinux_ui *ui,
		     enum fplinux_ui_activation activation)
{
	const struct fplinux_ui_event event = {
		.type = FPLINUX_UI_EVENT_ACTIVATION,
		.activation = activation,
	};

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
		assert(!strcmp(ui->current->text, "Storage write failed"));
		/* Alert commands return to the existing editor before the callback. */
		choose(ui, ui->current->stay_command);
	}
	preferences.fail_set = preferences.fail_save = false;
	choose(ui, ui->editor->save_command);
	assert(ui->exits == 0);
	assert(!strcmp(preferences.saved, draft));
	assert(!ui->editor->has_save);
}

static void edit(struct fplinux_ui *ui, const char *text, bool notify)
{
	snprintf(ui->editor->text, sizeof(ui->editor->text), "%s", text);
	ui->editor->cursor = strlen(text);
	if (notify) {
		assert(ui->editor->changed);
		ui->editor->changed(ui, ui->editor, 0, ui->editor->context);
	}
}

int fplinux_ui_run(struct fplinux_ui *ui)
{
	const char *draft = "Call at 18:00\nBring the book";

	assert(!ui->editor->has_save);
	if (!strcmp(scenario, "clean_empty") ||
	    !strcmp(scenario, "clean_loaded")) {
		assert(!strcmp(ui->editor->text,
			       !strcmp(scenario, "clean_empty") ?
				       "" :
				       "Earlier note"));
		choose(ui, ui->editor->back_command);
		assert(ui->exits == 1 && preferences.writes == 0);
		send_event(ui, FPLINUX_UI_EVENT_VISIBILITY, false);
		send_event(ui, FPLINUX_UI_EVENT_CLOSE, false);
		assert(preferences.writes == 0);
		return 0;
	}
	assert(!strcmp(ui->editor->text, "Earlier note"));
	if (!strcmp(scenario, "empty"))
		draft = "";
	else if (!strcmp(scenario, "unicode"))
		draft = "Позвонить в 18:00\n📖 взять книгу";
	edit(ui, draft, strcmp(scenario, "last_key_home") != 0);
	if (strcmp(scenario, "last_key_home")) {
		assert(ui->editor->has_save);
		assert(!strcmp(ui->editor->title, "Notes (unsaved)"));
	}
	if (!strcmp(scenario, "activation")) {
		struct fplinux_ui_screen *alert;
		unsigned int writes;

		ui->editor->cursor = 4;
		ui->editor->picker_open = true;
		ui->menu_open = true;
		activate(ui, FPLINUX_UI_ACTIVATION_RESUME);
		assert(ui->menu_open && ui->editor->picker_open);
		activate(ui, FPLINUX_UI_ACTIVATION_OPEN);
		assert(!ui->menu_open && !ui->editor->picker_open);
		assert(ui->editor->cursor == 4 &&
		       !strcmp(ui->editor->text, draft));
		assert(preferences.writes == 0 && ui->editor->has_save);
		preferences.fail_save = true;
		choose(ui, ui->editor->back_command);
		assert(ui->close_waiting && ui->current != ui->editor);
		alert = ui->current;
		writes = preferences.writes;
		activate(ui, FPLINUX_UI_ACTIVATION_OPEN);
		activate(ui, FPLINUX_UI_ACTIVATION_RESUME);
		assert(ui->current == alert && ui->close_waiting && !ui->exits);
		assert(preferences.writes == writes);
		assert(ui->editor->cursor == 4 &&
		       !strcmp(ui->editor->text, draft));
		choose(ui, alert->stay_command);
		assert(!ui->close_waiting && ui->current == ui->editor);
	} else if (!strcmp(scenario, "set_failure") ||
		   !strcmp(scenario, "save_failure") ||
		   !strcmp(scenario, "alert_failure")) {
		check_failed_save(ui, draft);
	} else if (!strcmp(scenario, "success") || !strcmp(scenario, "empty") ||
		   !strcmp(scenario, "unicode")) {
		choose(ui, ui->editor->save_command);
		assert(ui->exits == 0);
		assert(!strcmp(preferences.saved, draft));
		assert(ui->current == ui->editor);
		assert(!ui->editor->has_save);
		assert(!strcmp(ui->editor->title, "Notes"));
		assert(ui->editor->cursor == strlen(draft));
		choose(ui, ui->editor->back_command);
		send_event(ui, FPLINUX_UI_EVENT_CLOSE, false);
		assert(ui->exits == 1 && preferences.writes == 1);
	} else if (!strcmp(scenario, "autosave") ||
		   !strcmp(scenario, "last_key_home")) {
		send_event(ui, FPLINUX_UI_EVENT_VISIBILITY, true);
		assert(!strcmp(preferences.saved, "Earlier note"));
		send_event(ui, FPLINUX_UI_EVENT_VISIBILITY, false);
		assert(!strcmp(preferences.saved, draft));
		assert(ui->exits == 0 && ui->current == ui->editor);
		assert(!ui->editor->has_save && preferences.writes == 1);
		edit(ui, "Second edit", false);
		choose(ui, ui->editor->back_command);
		assert(ui->exits == 1);
		send_event(ui, FPLINUX_UI_EVENT_CLOSE, false);
		assert(!strcmp(preferences.saved, "Second edit"));
		assert(preferences.writes == 2);
	} else if (!strcmp(scenario, "back_failure")) {
		preferences.fail_save = true;
		choose(ui, ui->editor->back_command);
		/* An unsuccessful implicit save must not discard this draft. */
		assert(ui->exits == 0);
		assert(!strcmp(preferences.saved, "Earlier note"));
		assert(!strcmp(ui->editor->text, draft));
		assert(ui->current != ui->editor);
		assert(ui->close_waiting);
		choose(ui, ui->current->stay_command);
		assert(!ui->close_waiting && ui->current == ui->editor);
		assert(!strcmp(ui->editor->text, draft));
		send_event(ui, FPLINUX_UI_EVENT_VISIBILITY, false);
		send_event(ui, FPLINUX_UI_EVENT_VISIBILITY, true);
		assert(ui->current == ui->editor && ui->exits == 0);
		preferences.fail_save = false;
		choose(ui, ui->editor->back_command);
		assert(ui->exits == 1 && !strcmp(preferences.saved, draft));
	} else if (!strcmp(scenario, "close_retry") ||
		   !strcmp(scenario, "close_discard")) {
		preferences.fail_save = true;
		choose(ui, ui->editor->back_command);
		assert(ui->close_waiting && ui->current != ui->editor);
		assert(!strcmp(ui->editor->text, draft));
		if (!strcmp(scenario, "close_retry")) {
			choose(ui, ui->current->retry_command);
			assert(ui->close_waiting && ui->exits == 0);
			preferences.fail_save = false;
			choose(ui, ui->current->retry_command);
			assert(ui->exits == 1 &&
			       !strcmp(preferences.saved, draft));
		} else {
			assert(!strcmp(ui->current->discard_label,
				       "Discard changes"));
			choose(ui, ui->current->discard_command);
			assert(ui->exits == 1);
			/* A final hide/CLOSE must not retry an explicit Discard. */
			preferences.fail_save = false;
			send_event(ui, FPLINUX_UI_EVENT_VISIBILITY, false);
			send_event(ui, FPLINUX_UI_EVENT_CLOSE, false);
			assert(!strcmp(preferences.saved, "Earlier note"));
		}
	} else if (!strcmp(scenario, "published_unsynced") ||
		   !strcmp(scenario, "unsynced_close")) {
		preferences.fail_sync = true;
		choose(ui, !strcmp(scenario, "unsynced_close") ?
				   ui->editor->back_command :
				   ui->editor->save_command);
		assert(ui->exits == 0 && ui->current != ui->editor);
		assert(!strcmp(preferences.saved, draft));
		assert(!strcmp(ui->current->title, "Saving not confirmed"));
		assert(!strcmp(ui->current->text,
			       "Storage input/output error"));
		assert(ui->editor->has_save);
		assert(!strcmp(ui->editor->title, "Notes (unsaved)"));
		if (!strcmp(scenario, "unsynced_close")) {
			assert(!strcmp(ui->current->discard_label,
				       "Close anyway"));
			choose(ui, ui->current->discard_command);
			send_event(ui, FPLINUX_UI_EVENT_CLOSE, false);
			assert(ui->exits == 1 && preferences.writes == 1);
			return 0;
		}
		preferences.fail_sync = false;
		choose(ui, ui->current->retry_command);
		assert(ui->exits == 0 && !strcmp(preferences.saved, draft));
		assert(!ui->editor->has_save);
	} else if (!strcmp(scenario, "save_retry")) {
		preferences.fail_save = true;
		choose(ui, ui->editor->save_command);
		assert(ui->current != ui->editor && ui->exits == 0);
		preferences.fail_save = false;
		choose(ui, ui->current->retry_command);
		assert(ui->current == ui->editor && ui->exits == 0);
		assert(!strcmp(preferences.saved, draft));
		assert(!ui->editor->has_save);
	} else if (!strcmp(scenario, "reverted")) {
		edit(ui, "Earlier note", true);
		assert(!ui->editor->has_save);
		assert(!strcmp(ui->editor->title, "Notes"));
		choose(ui, ui->editor->back_command);
		assert(ui->exits == 1 && preferences.writes == 0);
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
