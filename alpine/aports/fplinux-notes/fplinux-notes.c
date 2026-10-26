/* SPDX-License-Identifier: 0BSD */
#define _POSIX_C_SOURCE 200809L
#include "fplinux-cli.h"
#include <fplinux/fplinux-ui.h>
#include <fplinux/fplinux-ui-menu.h>
#include <fplinux/fplinux-ui-prefs.h>
#include <fplinux/fplinux-ui-screen.h>
#include <libintl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define APP_ID "org.fplinux.notes"
#define NOTE_CHARS 1000
#define _(text) gettext(text)

enum command {
	COMMAND_SAVE,
	COMMAND_BACK,
	COMMAND_RETRY,
	COMMAND_STAY,
	COMMAND_DISCARD,
};

struct notes_app {
	struct fplinux_ui *ui;
	struct fplinux_ui_prefs *prefs;
	struct fplinux_ui_screen *box;
	struct fplinux_ui_screen *save_alert;
	struct fplinux_storage_result saved;
	char *durable_text;
	bool close_pending;
	bool discard;
};

static bool needs_save(const struct notes_app *app)
{
	return app->box &&
	       (app->saved.state != FPLINUX_STORAGE_DURABLE ||
		strcmp(fplinux_ui_text_box_get(app->box), app->durable_text));
}

static void refresh_commands(struct notes_app *app)
{
	const struct fplinux_ui_command save = {
		.id = COMMAND_SAVE,
		.type = FPLINUX_UI_COMMAND_SCREEN,
		.label = _("Save"),
		.icon = fplinux_ui_glyph(app->ui, "save", FPLINUX_UI_GLYPH_ROW),
	};
	bool unsaved = needs_save(app);

	if (unsaved)
		fplinux_ui_screen_add_command(app->box, &save);
	else
		fplinux_ui_screen_remove_command(app->box, COMMAND_SAVE);
	fplinux_ui_screen_set_title(app->box, unsaved ? _("Notes (unsaved)") :
							_("Notes"));
}

/* The preferences keep the note in memory; save writes it to the file. */
static bool save_note(struct notes_app *app)
{
	struct fplinux_storage_result result = {
		.state = FPLINUX_STORAGE_DURABLE,
	};
	char *saved_text;

	if (!needs_save(app))
		return true;
	/* Own the replacement before writing so DURABLE always has a copy. */
	saved_text = strdup(fplinux_ui_text_box_get(app->box));
	if (!saved_text ||
	    !fplinux_ui_prefs_set(app->prefs, "note", saved_text)) {
		result.state = FPLINUX_STORAGE_UNPUBLISHED;
		result.error = errno;
	} else {
		result = fplinux_ui_prefs_save(app->prefs);
	}
	if (result.state != FPLINUX_STORAGE_DURABLE &&
	    (app->saved.state != result.state ||
	     app->saved.error != result.error)) {
		errno = result.error;
		perror("fplinux-notes: save note");
	} else if (result.state == FPLINUX_STORAGE_DURABLE &&
		   app->saved.error) {
		fputs("fplinux-notes: note saved\n", stderr);
	}
	app->saved = result;
	if (result.state == FPLINUX_STORAGE_DURABLE) {
		free(app->durable_text);
		app->durable_text = saved_text;
	} else {
		free(saved_text);
	}
	refresh_commands(app);
	return result.state == FPLINUX_STORAGE_DURABLE;
}

static bool show_save_error(struct notes_app *app);

static void save_error_command(struct fplinux_ui *ui,
			       struct fplinux_ui_screen *screen, int command_id,
			       void *context)
{
	struct notes_app *app = context;

	(void)screen;
	app->save_alert = NULL;
	if (command_id == COMMAND_RETRY) {
		if (!save_note(app)) {
			if (!show_save_error(app) && app->close_pending) {
				app->close_pending = false;
				fplinux_ui_close_reply(ui,
						       FPLINUX_UI_CLOSE_CANCEL);
			}
			return;
		}
		if (app->close_pending) {
			app->close_pending = false;
			fplinux_ui_close_reply(ui, FPLINUX_UI_CLOSE_READY);
		}
	} else if (command_id == COMMAND_DISCARD && app->close_pending) {
		app->discard = true;
		app->close_pending = false;
		fplinux_ui_close_reply(ui, FPLINUX_UI_CLOSE_READY);
	} else if (app->close_pending) {
		app->close_pending = false;
		fplinux_ui_close_reply(ui, FPLINUX_UI_CLOSE_CANCEL);
	}
}

static bool show_save_error(struct notes_app *app)
{
	const struct fplinux_ui_command commands[] = {
		{ .id = COMMAND_RETRY,
		  .type = FPLINUX_UI_COMMAND_OK,
		  .label = _("Retry"),
		  .icon = fplinux_ui_glyph(app->ui, "refresh",
					   FPLINUX_UI_GLYPH_ROW) },
		{ .id = COMMAND_STAY,
		  .type = FPLINUX_UI_COMMAND_BACK,
		  .label = _("Stay"),
		  .icon = fplinux_ui_glyph(app->ui, "cancel",
					   FPLINUX_UI_GLYPH_ROW) },
		{ .id = COMMAND_DISCARD,
		  .type = FPLINUX_UI_COMMAND_SCREEN,
		  .label = app->saved.state ==
					   FPLINUX_STORAGE_PUBLISHED_UNSYNCED ?
				   _("Close anyway") :
				   _("Discard changes"),
		  .icon = fplinux_ui_glyph(app->ui, "delete",
					   FPLINUX_UI_GLYPH_ROW) },
	};
	struct fplinux_ui_screen *alert;

	if (app->save_alert && fplinux_ui_current(app->ui) == app->save_alert)
		fplinux_ui_pop(app->ui);
	app->save_alert = NULL;
	alert = fplinux_ui_alert_new(
		app->ui, FPLINUX_UI_ALERT_ERROR,
		app->saved.state == FPLINUX_STORAGE_PUBLISHED_UNSYNCED ?
			_("Saving not confirmed") :
			_("Note not saved"),
		fplinux_ui_storage_error(app->saved), 0);
	if (!alert)
		return false;
	for (size_t index = 0; index < (app->close_pending ? 3U : 2U);
	     ++index) {
		if (fplinux_ui_screen_add_command(alert, &commands[index]))
			continue;
		fplinux_ui_push(app->ui, alert);
		fplinux_ui_pop(app->ui);
		return false;
	}
	fplinux_ui_screen_set_listener(alert, save_error_command, NULL, app);
	app->save_alert = alert;
	fplinux_ui_push(app->ui, alert);
	return true;
}

static enum fplinux_ui_close_response prepare_close(struct fplinux_ui *ui,
						    void *context)
{
	struct notes_app *app = context;

	(void)ui;
	if (app->close_pending)
		return FPLINUX_UI_CLOSE_WAIT;
	if (save_note(app))
		return FPLINUX_UI_CLOSE_READY;
	app->close_pending = true;
	if (show_save_error(app))
		return FPLINUX_UI_CLOSE_WAIT;
	app->close_pending = false;
	return FPLINUX_UI_CLOSE_CANCEL;
}

/* Save stays reachable in Options while the right soft key belongs to Clear. */
static void handle_command(struct fplinux_ui *ui,
			   struct fplinux_ui_screen *screen, int command_id,
			   void *context)
{
	struct notes_app *app = context;

	(void)screen;
	if (command_id == COMMAND_BACK) {
		fplinux_ui_go_back(ui);
	} else if (command_id == COMMAND_SAVE) {
		if (!save_note(app))
			show_save_error(app);
	}
}

static void note_changed(struct fplinux_ui *ui,
			 struct fplinux_ui_screen *screen, size_t index,
			 void *context)
{
	struct notes_app *app = context;

	(void)ui;
	(void)screen;
	(void)index;
	refresh_commands(app);
}

/* Save when the note leaves the screen as well as on CLOSE. */
static void handle_event(struct fplinux_ui *ui,
			 const struct fplinux_ui_event *event, void *context)
{
	struct notes_app *app = context;

	if (event->type == FPLINUX_UI_EVENT_ACTIVATION &&
	    event->activation == FPLINUX_UI_ACTIVATION_OPEN) {
		fplinux_ui_menu_close(ui);
		if (!app->save_alert && app->box)
			fplinux_ui_text_box_cancel_picker(app->box);
		return;
	}
	if ((event->type == FPLINUX_UI_EVENT_CLOSE && !app->discard) ||
	    (event->type == FPLINUX_UI_EVENT_VISIBILITY && !event->visible &&
	     !app->discard))
		save_note(app);
}

static bool create_box(struct notes_app *app)
{
	const struct fplinux_ui_command back = {
		.id = COMMAND_BACK,
		.type = FPLINUX_UI_COMMAND_BACK,
		.label = NULL,
		.priority = 0,
		.icon = fplinux_ui_glyph(app->ui, "back", FPLINUX_UI_GLYPH_ROW),
	};

	app->box = fplinux_ui_text_box_new(
		app->ui, _("Notes"),
		fplinux_ui_prefs_get(app->prefs, "note", ""), NOTE_CHARS);
	if (!app->box)
		return false;
	app->durable_text = strdup(fplinux_ui_text_box_get(app->box));
	if (!app->durable_text)
		return false;
	fplinux_ui_screen_add_command(app->box, &back);
	fplinux_ui_screen_set_listener(app->box, handle_command, note_changed,
				       app);
	fplinux_ui_push(app->ui, app->box);
	return true;
}

int main(int argc, char **argv)
{
	struct fplinux_cli cli = {
		.program = "fplinux-notes",
		.description = "Write and save a note",
	};
	struct notes_app app = {
		.saved = { .state = FPLINUX_STORAGE_DURABLE },
	};
	int result;

	result = fplinux_cli_parse(&cli, argc, argv);
	if (result != FPLINUX_CLI_READY)
		return result;
	app.prefs = fplinux_ui_prefs_open(APP_ID);
	if (!app.prefs) {
		perror("fplinux-notes: preferences");
		return 1;
	}
	app.ui = fplinux_ui_open(APP_ID, handle_event, &app);
	if (!app.ui) {
		perror("fplinux-notes: open UI");
		fplinux_ui_prefs_close(app.prefs);
		return 1;
	}
	if (!create_box(&app)) {
		perror("fplinux-notes: text box");
		fplinux_ui_close(app.ui);
		fplinux_ui_prefs_close(app.prefs);
		free(app.durable_text);
		return 1;
	}
	fplinux_ui_set_close_handler(app.ui, prepare_close, &app);
	result = fplinux_ui_run(app.ui);
	fplinux_ui_close(app.ui);
	fplinux_ui_prefs_close(app.prefs);
	free(app.durable_text);
	return result < 0;
}
