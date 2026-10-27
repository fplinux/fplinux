/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include <fplinux/fplinux-ui.h>
#include <fplinux/fplinux-ui-menu.h>
#include <fplinux/fplinux-ui-prefs.h>
#include <fplinux/fplinux-ui-screen.h>
#include <fplinux/fplinux-ui-session.h>
#include <fplinux/fplinux-ui-symbol-picker.h>

#include "fplinux-terminal.h"
#include "fplinux-cli.h"
#include "fplinux-keypad.h"
#include "terminal-help.h"
#include "terminal-pty.h"
#include "terminal-render.h"
#include "terminal-session.h"

#include <errno.h>
#include <poll.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#define TERMINAL_APP_ID "org.fplinux.terminal"
#ifndef TERMINAL_STARTUP_PATH
#define TERMINAL_STARTUP_PATH "/etc/fplinux/terminal.bashrc"
#endif
#ifndef TERMINAL_FONT_DIRECTORY
#define TERMINAL_FONT_DIRECTORY "/usr/share/fplinux/fonts"
#endif
#define TERMINAL_INPUT_REDRAW_MS 20U
#define TERMINAL_PENDING_KEYS 128U
#define TERMINAL_FONT_COUNT 2U
/* Commands of the other screens; the terminal's own are 0 to COUNT - 1. */
#define COMMAND_SCREEN_BACK 100
#define COMMAND_DONE 101

static const struct {
	const char *label;
	const char *value;
	const char *path;
} font_choices[TERMINAL_FONT_COUNT] = {
	{ N_("Compact (6×12)"), "6x12",
	  TERMINAL_FONT_DIRECTORY "/ter-u12n-terminal.psf" },
	{ N_("Larger (8×16)"), "8x16",
	  TERMINAL_FONT_DIRECTORY "/ter-u16n-terminal.psf" },
};

_Static_assert((int)FPLINUX_TERMINAL_TYPE_BACK == FPLINUX_UI_COMMAND_BACK &&
		       (int)FPLINUX_TERMINAL_TYPE_CANCEL ==
			       FPLINUX_UI_COMMAND_CANCEL &&
		       (int)FPLINUX_TERMINAL_TYPE_EXIT ==
			       FPLINUX_UI_COMMAND_EXIT &&
		       (int)FPLINUX_TERMINAL_TYPE_OK == FPLINUX_UI_COMMAND_OK &&
		       (int)FPLINUX_TERMINAL_TYPE_SCREEN ==
			       FPLINUX_UI_COMMAND_SCREEN &&
		       (int)FPLINUX_TERMINAL_TYPE_HELP ==
			       FPLINUX_UI_COMMAND_HELP,
	       "terminal command types are the runtime's");

enum input_action {
	INPUT_KEY,
	INPUT_COMMAND,
	INPUT_SPECIAL,
	INPUT_MODIFIERS,
};

struct queued_input {
	enum input_action action;
	size_t value;
	struct fplinux_ui_key key;
	bool hold_cancelled;
};

struct gui_terminal {
	struct fplinux_ui *ui;
	struct fplinux_ui_prefs *prefs;
	struct fplinux_terminal terminal;
	struct fplinux_terminal_pty pty;
	/* Both fonts outlive UI objects and their queued draw tasks. */
	struct fplinux_terminal_grid grids[TERMINAL_FONT_COUNT];
	size_t font_index;
	struct fplinux_terminal_session session;
	struct fplinux_ui_symbol_picker *symbols;
	/* The terminal screen, a canvas at the bottom of the screen stack. */
	struct fplinux_ui_screen *screen;
	struct fplinux_ui_screen *helper;
	lv_obj_t *canvas;
	lv_timer_t *timer;
	bool failed;
	bool closing;
	/* The commands the canvas has now. */
	bool shown[FPLINUX_TERMINAL_COMMAND_COUNT];
	uint64_t redraw_ms;
	uint64_t scheduled_ms;
	struct queued_input pending_keys[TERMINAL_PENDING_KEYS];
	size_t pending_start;
	size_t pending_count;
};

static void close_symbol_picker(struct gui_terminal *app)
{
	fplinux_ui_symbol_picker_delete(app->symbols);
	app->symbols = NULL;
	fplinux_text_input_close_picker(&app->terminal.input);
}

static void cancel_phone_holds(struct gui_terminal *app)
{
	size_t kept = 0;

	for (size_t index = 0; index < app->pending_count; ++index) {
		struct queued_input *input =
			&app->pending_keys[(app->pending_start + index) %
					   TERMINAL_PENDING_KEYS];
		bool control_gesture =
			(input->action == INPUT_KEY &&
			 input->key.source == FPLINUX_UI_KEY_PHONE &&
			 (input->key.code == KEY_NUMERIC_STAR ||
			  input->key.code == KEY_NUMERIC_POUND)) ||
			(input->action == INPUT_COMMAND &&
			 input->value == FPLINUX_TERMINAL_COMMAND_SYMBOLS);

		if (control_gesture)
			continue;
		if (input->action == INPUT_KEY &&
		    input->key.source == FPLINUX_UI_KEY_PHONE)
			input->hold_cancelled = true;
		app->pending_keys[(app->pending_start + kept) %
				  TERMINAL_PENDING_KEYS] = *input;
		++kept;
	}
	app->pending_count = kept;
	fplinux_terminal_cancel_held(&app->terminal);
	close_symbol_picker(app);
}

/* The alert has already closed; EXIT asks the manager for a normal CLOSE. */
static void failure_command(struct fplinux_ui *ui,
			    struct fplinux_ui_screen *screen, int command_id,
			    void *context)
{
	struct gui_terminal *app = context;

	(void)screen;
	(void)command_id;
	if (fplinux_ui_back(ui))
		app->failed = false;
	fplinux_ui_quit(ui);
}

static void fail(struct gui_terminal *app, const char *text)
{
	struct fplinux_ui_screen *alert;
	bool close_added;

	if (app->failed)
		return;
	app->failed = true;
	if (app->timer)
		lv_timer_pause(app->timer);
	app->scheduled_ms = 0;
	close_symbol_picker(app);
	/* No more shell input or output while the error is being read. */
	fplinux_ui_watch_fd(app->ui, -1, 0, NULL, NULL);
	fplinux_terminal_pty_close(&app->pty);
	alert = fplinux_ui_alert_new(app->ui, FPLINUX_UI_ALERT_ERROR,
				     _("Terminal error"), text, 0);
	if (!alert) {
		fplinux_ui_quit(app->ui);
		return;
	}
	/* Default alert Close is a system command and does not call listeners. */
	close_added = fplinux_ui_screen_add_command(
		alert, &(struct fplinux_ui_command){
			       .id = COMMAND_SCREEN_BACK,
			       .type = FPLINUX_UI_COMMAND_BACK,
			       .label = _("Close"),
			       .icon = fplinux_ui_glyph(app->ui, "back",
							FPLINUX_UI_GLYPH_ROW),
		       });
	fplinux_ui_screen_set_listener(alert, failure_command, NULL, app);
	fplinux_ui_push(app->ui, alert);
	if (!close_added)
		fplinux_ui_quit(app->ui);
}

static void update_watch(struct gui_terminal *app);
static void update(struct gui_terminal *app);
static void drain_keys(struct gui_terminal *app);
static void queue_input(struct gui_terminal *app,
			const struct queued_input *input);
static bool resize_terminal(struct gui_terminal *app);
static void arm_timer(struct gui_terminal *app);

static void shell_finished(struct gui_terminal *app)
{
	app->redraw_ms = 0;
	fplinux_ui_watch_fd(app->ui, -1, 0, NULL, NULL);
	fplinux_terminal_pty_close(&app->pty);
	if (!fplinux_terminal_session_result(&app->session)) {
		fplinux_ui_quit(app->ui);
		return;
	}
	app->terminal.stopped = true;
	app->terminal.cursor_visible = false;
	app->terminal.modifier = 0;
	app->terminal.dirty = true;
	app->pending_start = 0;
	app->pending_count = 0;
	close_symbol_picker(app);
	fplinux_text_input_cancel(&app->terminal.input);
	if (app->timer)
		lv_timer_pause(app->timer);
	app->scheduled_ms = 0;
	/* Exit changes to Close after completion, even though its ID stays. */
	fplinux_ui_screen_remove_command(app->screen,
					 FPLINUX_TERMINAL_COMMAND_EXIT);
	app->shown[FPLINUX_TERMINAL_COMMAND_EXIT] = false;
}

static bool dispatch_pty(struct gui_terminal *app, short events)
{
	size_t pending = app->terminal.output_size;

	if (app->failed)
		return false;
	if (app->session.finished)
		return true;
	if (!fplinux_terminal_session_dispatch(&app->session, events)) {
		fail(app,
		     _("The shell connection was lost. Open Terminal again."));
		return false;
	}
	/* Keep preedit on screen briefly while the PTY echo catches up. */
	if (pending > app->terminal.output_size && !app->redraw_ms)
		app->redraw_ms = fplinux_ui_now_ms() + TERMINAL_INPUT_REDRAW_MS;
	if (events & POLLIN)
		app->redraw_ms = 0;
	if (app->session.finished)
		shell_finished(app);
	return true;
}

/* Hidden or not, the PTY is drained so that the shell never blocks. */
static void pty_event(struct fplinux_ui *ui, int fd, short events, void *data)
{
	struct gui_terminal *app = data;
	(void)ui;
	(void)fd;
	dispatch_pty(app, events);
	drain_keys(app);
	update_watch(app);
	update(app);
}

static void update_watch(struct gui_terminal *app)
{
	if (app->failed)
		return;
	if (app->session.finished) {
		fplinux_ui_watch_fd(app->ui, -1, 0, NULL, NULL);
		return;
	}
	fplinux_ui_watch_fd(app->ui, fplinux_terminal_session_fd(&app->session),
			    fplinux_terminal_session_events(&app->session),
			    pty_event, app);
}

/*
 * Context changes add and remove commands; the runtime sets the legends. New
 * commands come first, so that a key never shows a command on its way out.
 */
static void sync_commands(struct gui_terminal *app)
{
	static const char *const glyphs[FPLINUX_TERMINAL_COMMAND_COUNT] = {
		[FPLINUX_TERMINAL_COMMAND_ENTER] = "enter",
		[FPLINUX_TERMINAL_COMMAND_CLEAR] = "backspace",
		[FPLINUX_TERMINAL_COMMAND_LIVE] = "back",
		[FPLINUX_TERMINAL_COMMAND_MODIFIERS] = "keyboard",
		[FPLINUX_TERMINAL_COMMAND_SYMBOLS] = "symbols",
		[FPLINUX_TERMINAL_COMMAND_SPECIAL] = "keyboard",
		[FPLINUX_TERMINAL_COMMAND_SCROLLBACK] = "history",
		[FPLINUX_TERMINAL_COMMAND_INTERRUPT] = "stop",
		[FPLINUX_TERMINAL_COMMAND_END_INPUT] = "exit",
		[FPLINUX_TERMINAL_COMMAND_SEARCH] = "search",
		[FPLINUX_TERMINAL_COMMAND_FONT_SIZE] = "text-size",
		[FPLINUX_TERMINAL_COMMAND_HELP] = "help",
		[FPLINUX_TERMINAL_COMMAND_EXIT] = "exit",
		[FPLINUX_TERMINAL_COMMAND_INSERT] = "add",
		[FPLINUX_TERMINAL_COMMAND_CANCEL] = "cancel",
	};

	for (int pass = 0; pass < 2; ++pass)
		for (int id = 0; id < FPLINUX_TERMINAL_COMMAND_COUNT; ++id) {
			bool adding = pass == 0;
			struct fplinux_terminal_command_info info;
			bool shown = fplinux_terminal_command(&app->terminal,
							      id, &info);
			if (shown == app->shown[id] || shown != adding)
				continue;
			if (!shown) {
				fplinux_ui_screen_remove_command(app->screen,
								 id);
			} else if (!fplinux_ui_screen_add_command(
					   app->screen,
					   &(struct fplinux_ui_command){
						   .id = id,
						   .type = (enum fplinux_ui_command_type)
								   info.type,
						   .label = info.label,
						   .priority = info.priority,
						   .icon = fplinux_ui_glyph(
							   app->ui, glyphs[id],
							   FPLINUX_UI_GLYPH_ROW),
					   })) {
				fail(app,
				     _("The screen could not be updated. Open Terminal again."));
				return;
			}
			app->shown[id] = shown;
		}
}

static void modifier_text(const struct fplinux_terminal *terminal,
			  bool abbreviated, char *text, size_t capacity)
{
	static const char *const initials[] = { "C", "A", "S" };
	size_t used = 0;

	text[0] = '\0';
	for (size_t index = 0; index < FPLINUX_TERMINAL_MODIFIER_COUNT; ++index)
		if (terminal->modifier & fplinux_terminal_modifier_masks[index])
			used += (size_t)snprintf(
				text + used, capacity - used,
				used ? "+%s" : "%s",
				abbreviated ?
					initials[index] :
					fplinux_terminal_modifier_names[index]);
}

static void update_status(struct gui_terminal *app)
{
	const char *mode = fplinux_text_input_indicator(&app->terminal.input);
	char indicator[FPLINUX_LANGUAGE_TEXT_BYTES];
	char modifiers[32];
	char full[sizeof(modifiers) + sizeof(indicator) + 2];
	char compact[sizeof(full)];

	if (app->terminal.history)
		snprintf(indicator, sizeof(indicator), "↑%u",
			 fplinux_terminal_history_offset(&app->terminal));
	else if (app->terminal.stopped)
		snprintf(
			indicator, sizeof(indicator),
			WIFSIGNALED(app->session.child_status) ?
				_("Signal %d") :
				_("Exit %d"),
			WIFSIGNALED(app->session.child_status) ?
				WTERMSIG(app->session.child_status) :
				fplinux_terminal_session_result(&app->session));
	else
		snprintf(indicator, sizeof(indicator), "%s", mode);
	modifier_text(&app->terminal, false, modifiers, sizeof(modifiers));
	snprintf(full, sizeof(full), "%s%s%s", modifiers,
		 *modifiers ? "  " : "", indicator);
	modifier_text(&app->terminal, true, modifiers, sizeof(modifiers));
	snprintf(compact, sizeof(compact), "%s%s%s", modifiers,
		 *modifiers ? "  " : "", indicator);
	if (fplinux_ui_screen_set_title(
		    app->screen, app->terminal.history ? _("Scrollback") :
				 app->terminal.stopped ? _("Terminal ended") :
							 _("Terminal")) &&
	    fplinux_ui_screen_set_header_status(app->screen, full, compact))
		return;
	fail(app, _("The screen could not be updated. Open Terminal again."));
}

static void sync_symbol_picker(struct gui_terminal *app)
{
	struct fplinux_text_input *input = &app->terminal.input;

	if (!input->picker_open || !fplinux_ui_is_visible(app->ui) ||
	    fplinux_ui_current(app->ui) != app->screen) {
		close_symbol_picker(app);
		return;
	}
	if (!fplinux_ui_symbol_picker_is_open(app->symbols)) {
		fplinux_ui_symbol_picker_delete(app->symbols);
		app->symbols = fplinux_ui_symbol_picker_create(
			app->ui, input->picker_index);
		if (!app->symbols) {
			fplinux_text_input_close_picker(input);
			return;
		}
		fplinux_text_input_set_picker_columns(
			input, fplinux_ui_symbol_picker_columns(app->symbols));
	}
	fplinux_ui_symbol_picker_select(app->symbols, input->picker_index);
}

/* Paints only while the terminal is seen; a hidden one stays dirty. */
static void update(struct gui_terminal *app)
{
	if (app->failed)
		return;
	sync_symbol_picker(app);
	sync_commands(app);
	update_status(app);
	if (app->failed || !app->terminal.dirty ||
	    !fplinux_ui_is_visible(app->ui) ||
	    fplinux_ui_current(app->ui) != app->screen ||
	    fplinux_ui_now_ms() < app->redraw_ms) {
		arm_timer(app);
		return;
	}
	lv_obj_invalidate(app->canvas);
	arm_timer(app);
}

static void arm_timer(struct gui_terminal *app)
{
	uint64_t deadline = UINT64_MAX;
	uint64_t now = fplinux_ui_now_ms();
	uint64_t delay;

	if (!app->timer)
		return;
	if (!app->failed && !app->terminal.paused) {
		fplinux_terminal_deadline(
			&app->terminal,
			fplinux_terminal_session_can_input(&app->session),
			&deadline);
		if (app->terminal.dirty && app->redraw_ms > now &&
		    app->redraw_ms < deadline)
			deadline = app->redraw_ms;
	}
	if (deadline == UINT64_MAX) {
		lv_timer_pause(app->timer);
		app->scheduled_ms = 0;
		return;
	}
	if (deadline == app->scheduled_ms)
		return;
	delay = deadline > now ? deadline - now : 1;
	lv_timer_set_period(app->timer,
			    delay > UINT32_MAX ? UINT32_MAX : (uint32_t)delay);
	lv_timer_reset(app->timer);
	lv_timer_resume(app->timer);
	app->scheduled_ms = deadline;
}

static void after_input(struct gui_terminal *app)
{
	dispatch_pty(app, 0);
	update_watch(app);
	update(app);
}

static void special_command(struct fplinux_ui *ui,
			    struct fplinux_ui_screen *screen, int command_id,
			    void *context)
{
	struct gui_terminal *app = context;
	size_t index = fplinux_ui_list_get_focused(screen);

	app->helper = NULL;
	if (command_id == FPLINUX_UI_SELECT_COMMAND) {
		fplinux_ui_pop(ui);
		queue_input(app,
			    &(struct queued_input){ .action = INPUT_SPECIAL,
						    .value = index });
	} else {
		fplinux_ui_go_back(ui);
	}
	after_input(app);
}

static bool add_command(struct fplinux_ui *ui, struct fplinux_ui_screen *screen,
			int id, enum fplinux_ui_command_type type,
			const char *label, const char *glyph, int priority)
{
	return fplinux_ui_screen_add_command(
		screen, &(struct fplinux_ui_command){
				.id = id,
				.type = type,
				.label = label,
				.icon = fplinux_ui_glyph(ui, glyph,
							 FPLINUX_UI_GLYPH_ROW),
				.priority = priority });
}

static void open_special_keys(struct gui_terminal *app)
{
	struct fplinux_ui_screen *list = fplinux_ui_list_new(
		app->ui, _("Special keys"), FPLINUX_UI_LIST_IMPLICIT);
	char full[32], compact[32];

	if (!list) {
		perror("terminal: open list");
		return;
	}
	modifier_text(&app->terminal, false, full, sizeof(full));
	modifier_text(&app->terminal, true, compact, sizeof(compact));
	if (!fplinux_ui_screen_set_header_status(list, full, compact)) {
		fail(app,
		     _("The screen could not be updated. Open Terminal again."));
		return;
	}
	fplinux_ui_list_set_select_label(list, _("Send"));
	for (size_t index = 0; index < fplinux_terminal_special_count();
	     ++index)
		fplinux_ui_list_append(list, fplinux_terminal_special(index),
				       NULL);
	add_command(app->ui, list, COMMAND_SCREEN_BACK, FPLINUX_UI_COMMAND_BACK,
		    NULL, "back", 0);
	fplinux_ui_screen_set_listener(list, special_command, NULL, app);
	app->helper = list;
	fplinux_ui_navigate(app->ui, list);
}

static void modifiers_command(struct fplinux_ui *ui,
			      struct fplinux_ui_screen *screen, int command_id,
			      void *context)
{
	struct gui_terminal *app = context;
	unsigned int modifiers = 0;

	for (size_t index = 0; index < FPLINUX_TERMINAL_MODIFIER_COUNT; ++index)
		if (fplinux_ui_list_is_selected(screen, index))
			modifiers |= fplinux_terminal_modifier_masks[index];
	app->helper = NULL;
	if (command_id == COMMAND_DONE) {
		fplinux_ui_pop(ui);
		queue_input(app,
			    &(struct queued_input){ .action = INPUT_MODIFIERS,
						    .value = modifiers });
	} else {
		fplinux_ui_go_back(ui);
	}
	after_input(app);
}

/* Marks Ctrl, Alt and Shift for the next phone key; Back keeps the old. */
static void open_modifiers(struct gui_terminal *app)
{
	struct fplinux_ui_screen *list = fplinux_ui_list_new(
		app->ui, _("Modifiers"), FPLINUX_UI_LIST_MULTIPLE);

	if (!list) {
		perror("terminal: open modifiers");
		return;
	}
	for (size_t index = 0; index < FPLINUX_TERMINAL_MODIFIER_COUNT;
	     ++index) {
		fplinux_ui_list_append(
			list, fplinux_terminal_modifier_names[index], NULL);
		fplinux_ui_list_set_selected(
			list, index,
			app->terminal.modifier &
				fplinux_terminal_modifier_masks[index]);
	}
	add_command(app->ui, list, COMMAND_DONE, FPLINUX_UI_COMMAND_OK,
		    _("Done"), "check", 0);
	add_command(app->ui, list, COMMAND_SCREEN_BACK, FPLINUX_UI_COMMAND_BACK,
		    NULL, "back", 1);
	fplinux_ui_screen_set_listener(list, modifiers_command, NULL, app);
	app->helper = list;
	fplinux_ui_navigate(app->ui, list);
}

static void font_changed(struct fplinux_ui *ui,
			 struct fplinux_ui_screen *screen, size_t index,
			 void *context)
{
	struct gui_terminal *app = context;
	struct fplinux_ui_screen *alert;
	struct fplinux_storage_result saved;

	(void)screen;
	if (index >= TERMINAL_FONT_COUNT)
		return;
	app->helper = NULL;
	fplinux_ui_pop(ui);
	if (index != app->font_index) {
		app->font_index = index;
		if (!resize_terminal(app)) {
			fail(app,
			     _("The screen could not be updated. Open Terminal again."));
			return;
		}
	}
	if (!fplinux_ui_prefs_set(app->prefs, "font_size",
				  font_choices[index].value)) {
		saved.state = FPLINUX_STORAGE_UNPUBLISHED;
		saved.error = errno;
	} else {
		saved = fplinux_ui_prefs_save(app->prefs);
	}
	if (saved.state != FPLINUX_STORAGE_DURABLE) {
		errno = saved.error;
		perror("terminal: save font size");
		alert = fplinux_ui_alert_new(
			ui, FPLINUX_UI_ALERT_ERROR,
			saved.state == FPLINUX_STORAGE_PUBLISHED_UNSYNCED ?
				_("Saving not confirmed") :
				_("Font size"),
			fplinux_ui_storage_error(saved), 0);
		if (alert)
			fplinux_ui_push(ui, alert);
	}
	fplinux_ui_set_title(ui, saved.state == FPLINUX_STORAGE_DURABLE ?
					 _("Terminal") :
					 _("Terminal (unsaved)"));
	after_input(app);
}

static void screen_back(struct fplinux_ui *ui, struct fplinux_ui_screen *screen,
			int command_id, void *context)
{
	struct gui_terminal *app = context;

	(void)screen;
	(void)command_id;
	app->helper = NULL;
	fplinux_ui_go_back(ui);
	after_input(app);
}

static void open_font_size(struct gui_terminal *app)
{
	struct fplinux_ui_screen *list = fplinux_ui_list_new(
		app->ui, _("Font size"), FPLINUX_UI_LIST_EXCLUSIVE);

	if (!list) {
		perror("terminal: open font sizes");
		return;
	}
	for (size_t index = 0; index < TERMINAL_FONT_COUNT; ++index)
		fplinux_ui_list_append(list, _(font_choices[index].label),
				       NULL);
	fplinux_ui_list_set_selected(list, app->font_index, true);
	fplinux_ui_list_set_focused(list, app->font_index);
	add_command(app->ui, list, COMMAND_SCREEN_BACK, FPLINUX_UI_COMMAND_BACK,
		    NULL, "back", 0);
	fplinux_ui_screen_set_listener(list, screen_back, font_changed, app);
	app->helper = list;
	fplinux_ui_navigate(app->ui, list);
}

/* One read-only item per section; Up and Down scroll, Back returns. */
static void open_help(struct gui_terminal *app)
{
	struct fplinux_ui_screen *form =
		fplinux_ui_form_new(app->ui, _("Help"));

	if (!form) {
		perror("terminal: open help");
		return;
	}
	for (size_t index = 0; index < fplinux_terminal_help_count(); ++index) {
		const char *title;
		const char *text;

		fplinux_terminal_help_section(index, &title, &text);
		fplinux_ui_form_append_string(form, title, text);
	}
	add_command(app->ui, form, COMMAND_SCREEN_BACK, FPLINUX_UI_COMMAND_BACK,
		    NULL, "back", 0);
	fplinux_ui_screen_set_listener(form, screen_back, NULL, app);
	app->helper = form;
	fplinux_ui_navigate(app->ui, form);
}

static void canvas_command(struct fplinux_ui *ui,
			   struct fplinux_ui_screen *screen, int command_id,
			   void *context)
{
	struct gui_terminal *app = context;

	(void)screen;
	switch (command_id) {
	case FPLINUX_TERMINAL_COMMAND_MODIFIERS:
		open_modifiers(app);
		break;
	case FPLINUX_TERMINAL_COMMAND_SPECIAL:
		open_special_keys(app);
		break;
	case FPLINUX_TERMINAL_COMMAND_FONT_SIZE:
		open_font_size(app);
		break;
	case FPLINUX_TERMINAL_COMMAND_HELP:
		open_help(app);
		break;
	case FPLINUX_TERMINAL_COMMAND_EXIT:
		/* The manager answers with CLOSE; the shell ends on the way out. */
		if (!fplinux_ui_back(ui))
			fail(app,
			     _("The terminal could not close. Open Terminal again."));
		break;
	default:
		queue_input(app, &(struct queued_input){
					 .action = INPUT_COMMAND,
					 .value = (size_t)command_id });
		break;
	}
	after_input(app);
}

/* Every key that no command uses: digits, * and #, the D-pad, keyboards. */
static bool terminal_key(struct gui_terminal *app,
			 const struct fplinux_ui_key *key, bool hold_cancelled)
{
	if (key->source == FPLINUX_UI_KEY_PHONE) {
		if (!fplinux_terminal_phone(&app->terminal, key->code,
					    key->pressed, key->time_ms))
			return false;
		/* Retain queued text, but a departed gesture cannot start a hold. */
		if (hold_cancelled || app->terminal.paused) {
			fplinux_terminal_cancel_held(&app->terminal);
		}
	} else if (key->pressed) {
		unsigned int modifiers = (key->control ? TSM_CONTROL_MASK : 0) |
					 (key->alt ? TSM_ALT_MASK : 0) |
					 (key->shift ? TSM_SHIFT_MASK : 0);
		uint32_t unicode = key->unicode ? key->unicode :
						  TSM_VTE_INVALID;
		uint32_t ascii = key->ascii ? key->ascii : TSM_VTE_INVALID;

		/* Modifier-only events do not commit a pending phone composition. */
		switch (key->keysym) {
		case XKB_KEY_NoSymbol:
		case XKB_KEY_Shift_L:
		case XKB_KEY_Shift_R:
		case XKB_KEY_Control_L:
		case XKB_KEY_Control_R:
		case XKB_KEY_Alt_L:
		case XKB_KEY_Alt_R:
		case XKB_KEY_Super_L:
		case XKB_KEY_Super_R:
		case XKB_KEY_Caps_Lock:
		case XKB_KEY_Num_Lock:
		case XKB_KEY_ISO_Level3_Shift:
		case XKB_KEY_ISO_Next_Group:
		case XKB_KEY_ISO_Prev_Group:
			return true;
		}
		return fplinux_terminal_key(&app->terminal, key->keysym, ascii,
					    modifiers, unicode);
	}
	return true;
}

static void sync_input_pause(struct gui_terminal *app)
{
	bool covered = !fplinux_ui_is_visible(app->ui) ||
		       fplinux_ui_menu_is_open(app->ui) ||
		       fplinux_ui_current(app->ui) != app->screen;

	if (covered && !app->terminal.paused)
		cancel_phone_holds(app);
	fplinux_terminal_pause(&app->terminal, covered, fplinux_ui_now_ms());
	if (covered)
		close_symbol_picker(app);
}

static void drain_keys(struct gui_terminal *app)
{
	if (app->failed)
		return;
	sync_input_pause(app);
	while (app->pending_count &&
	       (app->terminal.stopped ||
		fplinux_terminal_session_can_input(&app->session))) {
		const struct queued_input *input =
			&app->pending_keys[app->pending_start];
		bool accepted = true;

		switch (input->action) {
		case INPUT_KEY:
			accepted = terminal_key(app, &input->key,
						input->hold_cancelled);
			break;
		case INPUT_COMMAND:
			accepted = fplinux_terminal_run(
				&app->terminal,
				(enum fplinux_terminal_command)input->value);
			break;
		case INPUT_SPECIAL:
			accepted = fplinux_terminal_send_special(&app->terminal,
								 input->value);
			break;
		case INPUT_MODIFIERS:
			fplinux_terminal_set_modifiers(
				&app->terminal, (unsigned int)input->value);
			break;
		}
		if (!accepted)
			break;
		app->pending_start =
			(app->pending_start + 1) % TERMINAL_PENDING_KEYS;
		--app->pending_count;
	}
}

static void queue_input(struct gui_terminal *app,
			const struct queued_input *input)
{
	size_t index;

	if (app->failed ||
	    (app->session.input_closed && !app->terminal.stopped))
		return;
	/* Wayland keeps delivering keys while a child temporarily stops reading. */
	if (app->pending_count == TERMINAL_PENDING_KEYS) {
		fail(app,
		     _("The shell is not accepting input. Open Terminal again."));
		return;
	}
	index = (app->pending_start + app->pending_count) %
		TERMINAL_PENDING_KEYS;
	app->pending_keys[index] = *input;
	++app->pending_count;
	drain_keys(app);
}

static bool canvas_key(struct fplinux_ui *ui, struct fplinux_ui_screen *canvas,
		       const struct fplinux_ui_key *key, void *context)
{
	struct gui_terminal *app = context;

	(void)ui;
	(void)canvas;
	if (!app->terminal.input.picker_open) {
		if (key->source == FPLINUX_UI_KEY_PHONE &&
		    (key->code == KEY_OK || fplinux_ui_is_softkey(key, true) ||
		     key->code == KEY_POWER ||
		     (app->terminal.stopped && !app->terminal.history &&
		      fplinux_ui_is_softkey(key, false))))
			return false;
		if (key->source == FPLINUX_UI_KEY_KEYBOARD &&
		    (key->code == KEY_COMPOSE ||
		     (app->terminal.stopped && key->code == KEY_ESC)))
			return false;
	}
	queue_input(app,
		    &(struct queued_input){ .action = INPUT_KEY, .key = *key });
	after_input(app);
	return true;
}

static void tick(lv_timer_t *timer)
{
	struct gui_terminal *app = lv_timer_get_user_data(timer);
	uint64_t now;

	lv_timer_pause(timer);
	app->scheduled_ms = 0;
	if (app->failed || app->terminal.stopped)
		return;
	drain_keys(app);
	now = fplinux_ui_now_ms();
	if (app->terminal.failed) {
		fail(app,
		     _("The shell connection was lost. Open Terminal again."));
		return;
	}
	fplinux_terminal_tick(
		&app->terminal, now,
		fplinux_terminal_session_can_input(&app->session));
	after_input(app);
}

/*
 * Hidden, the terminal neither ticks nor paints, so the cursor stops
 * blinking; the watched PTY is still read, and EIO still ends the program.
 */
static void set_visible(struct gui_terminal *app, bool visible)
{
	if (!app->timer)
		return;
	sync_input_pause(app);
	sync_symbol_picker(app);
	if (visible)
		app->terminal.dirty = true;
	if (visible && app->symbols &&
	    !fplinux_ui_symbol_picker_refresh(app->symbols))
		close_symbol_picker(app);
	update(app);
}

static void canvas_active(struct fplinux_ui *ui,
			  struct fplinux_ui_screen *screen, bool active,
			  void *context)
{
	struct gui_terminal *app = context;

	(void)ui;
	(void)screen;
	if (!active)
		cancel_phone_holds(app);
	fplinux_terminal_pause(&app->terminal, !active, fplinux_ui_now_ms());
	if (active)
		app->terminal.dirty = true;
	arm_timer(app);
}

static bool terminal_size(const struct gui_terminal *app, struct winsize *size)
{
	const struct fplinux_terminal_grid *grid = &app->grids[app->font_index];
	int width;
	int height;
	unsigned int columns;
	unsigned int rows;

	if (!fplinux_ui_screen_get_content_size(app->screen, &width, &height) ||
	    width <= 0 || height <= 0)
		return false;
	columns = (unsigned int)width / grid->cell_width;
	rows = (unsigned int)height / grid->cell_height;
	if (!columns || columns > USHRT_MAX || rows < 2 || rows > USHRT_MAX)
		return false;
	*size = (struct winsize){
		.ws_col = (unsigned short)columns,
		.ws_row = (unsigned short)rows,
	};
	return true;
}

static bool resize_terminal(struct gui_terminal *app)
{
	struct winsize size;

	if (!terminal_size(app, &size) ||
	    tsm_screen_resize(app->terminal.screen, size.ws_col, size.ws_row) <
		    0)
		return false;
	if (!app->terminal.stopped && ioctl(app->pty.fd, TIOCSWINSZ, &size) < 0)
		return false;
	app->terminal.dirty = true;
	if (app->symbols && !fplinux_ui_symbol_picker_refresh(app->symbols))
		close_symbol_picker(app);
	update(app);
	return true;
}

static void ui_event(struct fplinux_ui *ui,
		     const struct fplinux_ui_event *event, void *data)
{
	struct gui_terminal *app = data;

	if (event->type == FPLINUX_UI_EVENT_ACTIVATION &&
	    event->activation == FPLINUX_UI_ACTIVATION_OPEN) {
		fplinux_ui_menu_close(ui);
		cancel_phone_holds(app);
		/* Error alerts keep their decision; helpers own no shell state. */
		if (app->helper && fplinux_ui_current(ui) == app->helper) {
			app->helper = NULL;
			fplinux_ui_pop(ui);
		}
		after_input(app);
	} else if (event->type == FPLINUX_UI_EVENT_INPUT_CANCEL) {
		if (event->key.source == FPLINUX_UI_KEY_PHONE)
			cancel_phone_holds(app);
	} else if (event->type == FPLINUX_UI_EVENT_CLOSE) {
		/* The manager requested this exit, including Home over the alert. */
		app->failed = false;
		app->closing = true;
		fplinux_ui_quit(ui);
	} else if (event->type == FPLINUX_UI_EVENT_VISIBILITY)
		set_visible(app, event->visible);
	else if (event->type == FPLINUX_UI_EVENT_RESIZE &&
		 app->terminal.screen && !resize_terminal(app))
		fail(app,
		     _("The screen could not be updated. Open Terminal again."));
}

static void draw_terminal(lv_event_t *event)
{
	struct gui_terminal *app = lv_event_get_user_data(event);
	const struct fplinux_ui_theme *theme = fplinux_ui_theme(app->ui);
	lv_area_t bounds;

	lv_obj_get_coords(app->canvas, &bounds);
	fplinux_terminal_render(&app->terminal, &app->grids[app->font_index],
				lv_event_get_layer(event), &bounds,
				theme->accent, theme->accent_text);
}

static bool create_screen(struct gui_terminal *app)
{
	app->screen = fplinux_ui_canvas_new(app->ui, _("Terminal"));
	if (!app->screen)
		return false;
	app->canvas = fplinux_ui_canvas_object(app->screen);
	if (!app->canvas)
		return false;
	lv_obj_set_scrollable(app->canvas, false);
	return true;
}

static bool activate_screen(struct gui_terminal *app)
{
	lv_obj_add_event_cb(app->canvas, draw_terminal, LV_EVENT_DRAW_MAIN,
			    app);
	fplinux_ui_screen_set_key_filter(app->screen, canvas_key, app);
	fplinux_ui_canvas_set_active_listener(app->screen, canvas_active, app);
	fplinux_ui_canvas_set_key_repeat(app->screen,
					 FPLINUX_UI_CANVAS_REPEAT_KEYMAP);
	fplinux_ui_screen_set_listener(app->screen, canvas_command, NULL, app);
	sync_commands(app);
	if (app->failed)
		return false;
	fplinux_ui_push(app->ui, app->screen);
	update(app);
	return !app->failed;
}

int main(int argc, char **argv)
{
	struct fplinux_cli cli = {
		.program = "fplinux-terminal",
		.description = "Terminal for the graphical phone session",
	};
	struct gui_terminal app = {
		.pty = { .fd = -1,
			 .shell = -1,
			 .supervisor = -1,
			 .owner_fd = -1,
			 .status_fd = -1 },
	};
	struct winsize size;
	const char *title;
	const char *font_size;
	int result = fplinux_cli_parse(&cli, argc, argv);

	if (result != FPLINUX_CLI_READY)
		return result;
	result = EXIT_FAILURE;
	signal(SIGPIPE, SIG_IGN);
	app.ui = fplinux_ui_open(TERMINAL_APP_ID, ui_event, &app);
	if (!app.ui) {
		perror("terminal: open UI session");
		goto done;
	}
	if (app.closing) {
		result = EXIT_SUCCESS;
		goto done;
	}
	app.prefs = fplinux_ui_prefs_open(TERMINAL_APP_ID);
	if (!app.prefs) {
		perror("terminal: load preferences");
		fail(&app,
		     _("The settings could not be loaded. Open Terminal again."));
		fplinux_ui_run(app.ui);
		goto done;
	}
	app.font_index = fplinux_ui_width(app.ui) < 240 ? 0 : 1;
	font_size = fplinux_ui_prefs_get(app.prefs, "font_size", NULL);
	for (size_t index = 0; index < TERMINAL_FONT_COUNT; ++index) {
		if (font_size && !strcmp(font_size, font_choices[index].value))
			app.font_index = index;
		if (!fplinux_terminal_grid_open(&app.grids[index],
						font_choices[index].path)) {
			fprintf(stderr,
				"fplinux-terminal: cannot load font %s\n",
				font_choices[index].value);
			goto done;
		}
	}
	title = _("Terminal");
	if (!create_screen(&app)) {
		perror("terminal: create the terminal screen");
		goto done;
	}
	if (!terminal_size(&app, &size)) {
		fprintf(stderr,
			"fplinux-terminal: assigned viewport is too small\n");
		goto done;
	}
	if (!fplinux_terminal_init(&app.terminal, size.ws_col, size.ws_row) ||
	    !fplinux_terminal_pty_open(&app.pty, size.ws_col, size.ws_row,
				       "/bin/bash", TERMINAL_STARTUP_PATH)) {
		perror("fplinux-terminal: start Bash PTY");
		goto done;
	}
	fplinux_terminal_load_languages(&app.terminal,
					FPLINUX_LANGUAGE_DIRECTORY,
					fplinux_ui_input_locale(app.ui));
	fplinux_terminal_session_init(&app.session, &app.terminal, &app.pty);
	app.terminal.paused = true;
	app.timer = lv_timer_create(tick, 1, &app);
	if (!app.timer)
		goto done;
	lv_timer_pause(app.timer);
	if (!activate_screen(&app)) {
		perror("terminal: create the terminal screen");
		goto done;
	}
	if (!fplinux_ui_set_title(app.ui, title))
		goto done;
	update_watch(&app);
	arm_timer(&app);
	result = fplinux_ui_run(app.ui) == 0 && !app.failed ? EXIT_SUCCESS :
							      EXIT_FAILURE;
	if (!result && !app.closing && app.session.finished)
		result = fplinux_terminal_session_result(&app.session);

done:
	if (app.timer)
		lv_timer_delete(app.timer);
	/* Every exit path closes the master, terminates its job and reaps Bash. */
	close_symbol_picker(&app);
	fplinux_terminal_pty_close(&app.pty);
	fplinux_terminal_destroy(&app.terminal);
	if (app.ui)
		fplinux_ui_close(app.ui);
	for (size_t index = 0; index < TERMINAL_FONT_COUNT; ++index)
		fplinux_terminal_grid_close(&app.grids[index]);
	fplinux_ui_prefs_close(app.prefs);
	return result;
}
