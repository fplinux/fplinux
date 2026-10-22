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
	fplinux_ui_close_cb prepare_close;
	void *close_context;
};

static struct fplinux_ui runtime;
static const char *state_directory;
static const char *note = "Заметка\nКнига 📖\rC:\\notes";
static bool reopening;

int fplinux_notes_application_main(int argc, char **argv);

/* Resolve app storage at the UI boundary; retain the real file writer. */
struct fplinux_ui_prefs *fplinux_ui_prefs_open(const char *app_id)
{
	assert(!strcmp(app_id, "org.fplinux.notes"));
	return fplinux_ui_prefs_open_directory(state_directory);
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

const void *fplinux_ui_glyph(struct fplinux_ui *ui, const char *name,
			     enum fplinux_ui_glyph_size size)
{
	assert(ui == &runtime && size == FPLINUX_UI_GLYPH_ROW);
	return name;
}

bool fplinux_ui_screen_add_command(struct fplinux_ui_screen *screen,
				   const struct fplinux_ui_command *command)
{
	if (command->type == FPLINUX_UI_COMMAND_SCREEN)
		screen->save_command = command->id;
	return true;
}

bool fplinux_ui_screen_set_title(struct fplinux_ui_screen *screen,
				 const char *title)
{
	assert(screen && !strcmp(title, "Notes"));
	return true;
}

const char *fplinux_ui_storage_error(struct fplinux_storage_result result)
{
	(void)result;
	assert(!"controlled note storage must remain writable");
	return "Storage write failed";
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

struct fplinux_ui_screen *fplinux_ui_current(const struct fplinux_ui *ui)
{
	return ui->editor;
}

bool fplinux_ui_pop(struct fplinux_ui *ui)
{
	(void)ui;
	assert(!"no save alert is expected");
	return false;
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
	(void)ui;
	(void)response;
	assert(!"a successful close needs no delayed reply");
	return false;
}

void fplinux_ui_go_back(struct fplinux_ui *ui)
{
	struct fplinux_ui_prefs *saved =
		fplinux_ui_prefs_open("org.fplinux.notes");

	assert(saved);
	assert(ui->prepare_close(ui, ui->close_context) ==
	       FPLINUX_UI_CLOSE_READY);
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
