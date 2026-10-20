/* SPDX-License-Identifier: 0BSD */
#include "fplinux-cli.h"
#include <fplinux/fplinux-ui.h>
#include <fplinux/fplinux-ui-prefs.h>
#include <fplinux/fplinux-ui-screen.h>
#include <libintl.h>
#include <stdio.h>

#define APP_ID "org.fplinux.notes"
#define NOTE_CHARS 1000
#define _(text) gettext(text)

enum command {
	COMMAND_SAVE,
	COMMAND_BACK,
};

struct notes_app {
	struct fplinux_ui *ui;
	struct fplinux_ui_prefs *prefs;
	struct fplinux_ui_screen *box;
};

/* The preferences keep the note in memory; save writes it to the file. */
static bool save_note(struct notes_app *app)
{
	if (!app->box)
		return false;
	if (!fplinux_ui_prefs_set(app->prefs, "note",
				  fplinux_ui_text_box_get(app->box)) ||
	    !fplinux_ui_prefs_save(app->prefs)) {
		perror("fplinux-notes: save note");
		return false;
	}
	return true;
}

/*
 * The runtime gives OK to the text (a new line), the right soft key to Clear
 * while there is text and to Back when empty. Options keeps Save and Back
 * reachable while Clear owns the right key. Save leaves only after writing;
 * a failed Save keeps the editor so the user can try again.
 */
static void handle_command(struct fplinux_ui *ui,
			   struct fplinux_ui_screen *screen, int command_id,
			   void *context)
{
	struct notes_app *app = context;
	struct fplinux_ui_screen *alert;

	(void)screen;
	if (command_id == COMMAND_BACK) {
		fplinux_ui_go_back(ui);
	} else if (command_id == COMMAND_SAVE) {
		if (save_note(app)) {
			fplinux_ui_go_back(ui);
			return;
		}
		alert = fplinux_ui_alert_new(
			ui, FPLINUX_UI_ALERT_ERROR, NULL,
			_("The note was not saved. Close and try Save again."),
			0);
		if (alert)
			fplinux_ui_push(ui, alert);
		else
			perror("fplinux-notes: save error");
	}
}

/* Save when the note leaves the screen as well as on CLOSE. */
static void handle_event(struct fplinux_ui *ui,
			 const struct fplinux_ui_event *event, void *context)
{
	struct notes_app *app = context;

	(void)ui;
	if (event->type == FPLINUX_UI_EVENT_CLOSE ||
	    (event->type == FPLINUX_UI_EVENT_VISIBILITY && !event->visible))
		save_note(app);
}

static bool create_box(struct notes_app *app)
{
	const struct fplinux_ui_command save = {
		.id = COMMAND_SAVE,
		.type = FPLINUX_UI_COMMAND_SCREEN,
		.label = _("Save"),
		.priority = 0,
	};
	const struct fplinux_ui_command back = {
		.id = COMMAND_BACK,
		.type = FPLINUX_UI_COMMAND_BACK,
		.label = NULL,
		.priority = 0,
	};

	app->box = fplinux_ui_text_box_new(
		app->ui, _("Notes"),
		fplinux_ui_prefs_get(app->prefs, "note", ""), NOTE_CHARS);
	if (!app->box)
		return false;
	fplinux_ui_screen_add_command(app->box, &save);
	fplinux_ui_screen_add_command(app->box, &back);
	fplinux_ui_screen_set_listener(app->box, handle_command, NULL, app);
	fplinux_ui_push(app->ui, app->box);
	return true;
}

int main(int argc, char **argv)
{
	struct fplinux_cli cli = {
		.program = "fplinux-notes",
		.description = "Write and save a note",
	};
	struct notes_app app = { 0 };
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
		return 1;
	}
	result = fplinux_ui_run(app.ui);
	fplinux_ui_close(app.ui);
	fplinux_ui_prefs_close(app.prefs);
	return result < 0;
}
