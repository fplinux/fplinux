/* SPDX-License-Identifier: 0BSD */
/* UI peer for a normally linked Notes app and real preferences writer. */
#include <fplinux/fplinux-ui.h>
#include <fplinux/fplinux-ui-prefs.h>
#include <fplinux/fplinux-ui-screen.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct fplinux_ui_screen {
	char text[256];
	fplinux_ui_command_cb listener;
	void *context;
	int save_command;
};

struct fplinux_ui {
	struct fplinux_ui_screen *editor;
	fplinux_ui_event_cb event_callback;
	void *event_context;
	bool left;
};

static struct fplinux_ui runtime;
static const char *state_directory;
static const char *note = "Заметка\nКнига 📖\rC:\\notes";
static bool reopening;

int fplinux_notes_application_main(int argc, char **argv);

/* Directory creation and resolution belong to the controlled UI boundary. */
bool fplinux_ui_state_dir(const char *app_id, char *path, size_t capacity)
{
	assert(!strcmp(app_id, "org.fplinux.notes"));
	assert(strlen(state_directory) < capacity);
	snprintf(path, capacity, "%s", state_directory);
	return true;
}

struct fplinux_ui *fplinux_ui_open(const char *app_id,
				   fplinux_ui_event_cb callback, void *context)
{
	assert(!strcmp(app_id, "org.fplinux.notes"));
	memset(&runtime, 0, sizeof(runtime));
	runtime.event_callback = callback;
	runtime.event_context = context;
	return &runtime;
}

void fplinux_ui_close(struct fplinux_ui *ui)
{
	free(ui->editor);
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
	if (command->type == FPLINUX_UI_COMMAND_SCREEN)
		screen->save_command = command->id;
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
	(void)ui;
	(void)kind;
	(void)title;
	(void)text;
	(void)timeout_ms;
	assert(!"saving in the controlled state directory must succeed");
	return NULL;
}

void fplinux_ui_push(struct fplinux_ui *ui, struct fplinux_ui_screen *screen)
{
	assert(screen == ui->editor);
}

void fplinux_ui_go_back(struct fplinux_ui *ui)
{
	struct fplinux_ui_prefs *saved =
		fplinux_ui_prefs_open("org.fplinux.notes");

	assert(saved);
	assert(!strcmp(fplinux_ui_prefs_get(saved, "note", ""), note));
	fplinux_ui_prefs_close(saved);
	ui->left = true;
}

int fplinux_ui_run(struct fplinux_ui *ui)
{
	const struct fplinux_ui_event close = { .type = FPLINUX_UI_EVENT_CLOSE };

	if (reopening) {
		assert(!strcmp(ui->editor->text, note));
		ui->event_callback(ui, &close, ui->event_context);
	} else {
		assert(!strcmp(ui->editor->text, ""));
		snprintf(ui->editor->text, sizeof(ui->editor->text), "%s",
			 note);
		ui->editor->listener(ui, ui->editor, ui->editor->save_command,
				     ui->editor->context);
		assert(ui->left);
	}
	return 0;
}

int main(int argc, char **argv)
{
	char *application_argv[] = { "fplinux-notes", NULL };

	assert(argc == 2);
	state_directory = argv[1];
	assert(fplinux_notes_application_main(1, application_argv) == 0);
	reopening = true;
	assert(fplinux_notes_application_main(1, application_argv) == 0);
	puts("notes-storage: save and reopen passed");
	return 0;
}
