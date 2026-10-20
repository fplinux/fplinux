/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include <fplinux/fplinux-ui.h>
#include <fplinux/fplinux-ui-menu.h>
#include <fplinux/fplinux-ui-screen.h>

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
#include <sys/random.h>
#include <termios.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#define TERMINAL_APP_ID "org.fplinux.terminal"
#ifndef TERMINAL_STARTUP_PATH
#define TERMINAL_STARTUP_PATH "/etc/fplinux/terminal.bashrc"
#endif
/* The tick drives multi-tap, held keys, the cursor and repaints. */
#define TERMINAL_TICK_MS 16U
#define TERMINAL_INPUT_REDRAW_MS 20U
#define TERMINAL_PENDING_KEYS 128U
#define TITLE_BYTES 128U
/* Commands of the other screens; the terminal's own are 0 to COUNT - 1. */
#define COMMAND_SCREEN_BACK 100
#define COMMAND_DONE 101

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

/* What a list pushed over the terminal chooses. */
enum picker {
	PICK_SYMBOL,
	PICK_SPECIAL,
	PICK_LANGUAGE,
};

enum input_action {
	INPUT_KEY,
	INPUT_COMMAND,
	INPUT_SYMBOL,
	INPUT_SPECIAL,
	INPUT_LANGUAGE,
	INPUT_MODIFIERS,
};

struct queued_input {
	enum input_action action;
	size_t value;
	struct fplinux_ui_key key;
};

struct gui_terminal {
	struct fplinux_ui *ui;
	struct fplinux_terminal terminal;
	struct fplinux_terminal_pty pty;
	struct fplinux_terminal_grid grid;
	struct fplinux_terminal_session session;
	/* The terminal screen, a canvas at the bottom of the screen stack. */
	struct fplinux_ui_screen *screen;
	lv_obj_t *canvas;
	lv_timer_t *timer;
	bool failed;
	bool closing;
	bool phone_pressed[KEY_CNT];
	/* The commands and the title the canvas has now. */
	bool shown[FPLINUX_TERMINAL_COMMAND_COUNT];
	char language_label[64];
	/* The title band's text width: the screen less its margins. */
	int32_t title_width;
	char title[TITLE_BYTES];
	uint64_t redraw_ms;
	struct queued_input pending_keys[TERMINAL_PENDING_KEYS];
	size_t pending_start;
	size_t pending_count;
};

static bool generate_token(char token[33])
{
	unsigned char random[16];
	size_t used = 0;

	while (used < sizeof(random)) {
		ssize_t count =
			getrandom(random + used, sizeof(random) - used, 0);

		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0)
			return false;
		used += (size_t)count;
	}
	for (size_t index = 0; index < sizeof(random); ++index)
		snprintf(token + index * 2, 3, "%02x", random[index]);
	return true;
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
	/* No more shell input or output while the error is being read. */
	fplinux_ui_watch_fd(app->ui, -1, 0, NULL, NULL);
	fplinux_terminal_pty_close(&app->pty);
	alert = fplinux_ui_current(app->ui) ?
			fplinux_ui_alert_new(app->ui, FPLINUX_UI_ALERT_ERROR,
					     _("Terminal error"), text, 0) :
			NULL;
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

static bool dispatch_pty(struct gui_terminal *app, short events)
{
	size_t pending = app->terminal.output_size;

	if (app->failed)
		return false;
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
	if (app->session.finished) {
		app->redraw_ms = 0;
		fplinux_ui_quit(app->ui);
	}
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
	for (int pass = 0; pass < 2; ++pass)
		for (int id = 0; id < FPLINUX_TERMINAL_COMMAND_COUNT; ++id) {
			bool adding = pass == 0;
			struct fplinux_terminal_command_info info;
			bool shown = fplinux_terminal_command(&app->terminal,
							      id, &info);
			bool relabel =
				shown &&
				id == FPLINUX_TERMINAL_COMMAND_LANGUAGE &&
				strcmp(info.label, app->language_label);

			if ((shown == app->shown[id] && !relabel) ||
			    shown != adding)
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
					   })) {
				fail(app,
				     _("The screen could not be updated. Open Terminal again."));
				return;
			}
			app->shown[id] = shown;
			if (shown && id == FPLINUX_TERMINAL_COMMAND_LANGUAGE)
				snprintf(app->language_label,
					 sizeof(app->language_label), "%s",
					 info.label);
		}
}

/* Proportional text: the title is measured in pixels in the band's font. */
static bool title_fits(const char *text, void *context)
{
	const struct gui_terminal *app = context;
	lv_point_t size;

	lv_text_get_size(&size, text, fplinux_ui_theme(app->ui)->font_large, 0,
			 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
	return size.x <= app->title_width;
}

static void update_title(struct gui_terminal *app)
{
	char title[TITLE_BYTES];

	if (app->failed)
		return;
	fplinux_terminal_title(&app->terminal, title_fits, app, title,
			       sizeof(title));
	if (!strcmp(title, app->title))
		return;
	if (!fplinux_ui_screen_set_title(app->screen, title)) {
		fail(app,
		     _("The screen could not be updated. Open Terminal again."));
		return;
	}
	snprintf(app->title, sizeof(app->title), "%s", title);
}

/* Paints only while the terminal is seen; a hidden one stays dirty. */
static void update(struct gui_terminal *app)
{
	if (app->failed)
		return;
	sync_commands(app);
	update_title(app);
	if (app->failed || !app->terminal.dirty ||
	    !fplinux_ui_is_visible(app->ui) ||
	    fplinux_ui_current(app->ui) != app->screen ||
	    fplinux_ui_now_ms() < app->redraw_ms)
		return;
	lv_obj_invalidate(app->canvas);
}

static void after_input(struct gui_terminal *app)
{
	dispatch_pty(app, 0);
	update_watch(app);
	update(app);
}

static void picker_command(struct fplinux_ui *ui,
			   struct fplinux_ui_screen *screen, int command_id,
			   void *context)
{
	struct gui_terminal *app = context;
	enum picker kind =
		(enum picker)(uintptr_t)fplinux_ui_screen_get_data(screen);
	size_t index = fplinux_ui_list_get_focused(screen);

	fplinux_ui_pop(ui);
	if (command_id == FPLINUX_UI_SELECT_COMMAND) {
		if (kind == PICK_SYMBOL)
			queue_input(app, &(struct queued_input){
						 .action = INPUT_SYMBOL,
						 .value = index });
		else if (kind == PICK_SPECIAL)
			queue_input(app, &(struct queued_input){
						 .action = INPUT_SPECIAL,
						 .value = index });
	}
	after_input(app);
}

/* OK on a language marks it; the choice applies and the list closes. */
static void language_chosen(struct fplinux_ui *ui,
			    struct fplinux_ui_screen *screen, size_t index,
			    void *context)
{
	struct gui_terminal *app = context;

	(void)screen;
	fplinux_ui_pop(ui);
	queue_input(app, &(struct queued_input){ .action = INPUT_LANGUAGE,
						 .value = index });
	after_input(app);
}

static bool add_command(struct fplinux_ui_screen *screen, int id,
			enum fplinux_ui_command_type type, const char *label,
			int priority)
{
	return fplinux_ui_screen_add_command(
		screen, &(struct fplinux_ui_command){ .id = id,
						      .type = type,
						      .label = label,
						      .priority = priority });
}

static void open_picker(struct gui_terminal *app, enum picker kind)
{
	static const char *const titles[] = {
		[PICK_SYMBOL] = N_("Symbols"),
		[PICK_SPECIAL] = N_("Special keys"),
		[PICK_LANGUAGE] = N_("Input language"),
	};
	struct fplinux_ui_screen *list = fplinux_ui_list_new(
		app->ui, _(titles[kind]),
		kind == PICK_LANGUAGE ? FPLINUX_UI_LIST_EXCLUSIVE :
					FPLINUX_UI_LIST_IMPLICIT);
	size_t count = kind == PICK_SYMBOL  ? fplinux_terminal_symbol_count() :
		       kind == PICK_SPECIAL ? fplinux_terminal_special_count() :
					      app->terminal.languages.count;

	if (!list) {
		perror("terminal: open list");
		return;
	}
	if (kind != PICK_LANGUAGE)
		fplinux_ui_list_set_select_label(
			list, kind == PICK_SYMBOL ? _("Insert") : _("Send"));
	for (size_t index = 0; index < count; ++index)
		fplinux_ui_list_append(
			list,
			kind == PICK_SYMBOL ?
				fplinux_terminal_symbol(index) :
			kind == PICK_SPECIAL ?
				fplinux_terminal_special(index) :
				app->terminal.languages.language[index].name,
			NULL);
	if (kind == PICK_LANGUAGE) {
		fplinux_ui_list_set_selected(list, app->terminal.language,
					     true);
		fplinux_ui_list_set_focused(list, app->terminal.language);
	}
	fplinux_ui_screen_set_data(list, (void *)(uintptr_t)kind);
	add_command(list, COMMAND_SCREEN_BACK, FPLINUX_UI_COMMAND_BACK, NULL,
		    0);
	fplinux_ui_screen_set_listener(
		list, picker_command,
		kind == PICK_LANGUAGE ? language_chosen : NULL, app);
	fplinux_ui_push(app->ui, list);
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
	fplinux_ui_pop(ui);
	if (command_id == COMMAND_DONE)
		queue_input(app,
			    &(struct queued_input){ .action = INPUT_MODIFIERS,
						    .value = modifiers });
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
	add_command(list, COMMAND_DONE, FPLINUX_UI_COMMAND_OK, _("Done"), 0);
	add_command(list, COMMAND_SCREEN_BACK, FPLINUX_UI_COMMAND_BACK, NULL,
		    1);
	fplinux_ui_screen_set_listener(list, modifiers_command, NULL, app);
	fplinux_ui_push(app->ui, list);
}

static void help_command(struct fplinux_ui *ui,
			 struct fplinux_ui_screen *screen, int command_id,
			 void *context)
{
	(void)screen;
	(void)command_id;
	fplinux_ui_pop(ui);
	after_input(context);
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
	add_command(form, COMMAND_SCREEN_BACK, FPLINUX_UI_COMMAND_BACK, NULL,
		    0);
	fplinux_ui_screen_set_listener(form, help_command, NULL, app);
	fplinux_ui_push(app->ui, form);
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
	case FPLINUX_TERMINAL_COMMAND_SYMBOLS:
		open_picker(app, PICK_SYMBOL);
		break;
	case FPLINUX_TERMINAL_COMMAND_SPECIAL:
		open_picker(app, PICK_SPECIAL);
		break;
	case FPLINUX_TERMINAL_COMMAND_LANGUAGE:
		open_picker(app, PICK_LANGUAGE);
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
static void terminal_key(struct gui_terminal *app,
			 const struct fplinux_ui_key *key)
{
	if (key->source == FPLINUX_UI_KEY_PHONE) {
		bool repeat = key->code < KEY_CNT &&
			      app->phone_pressed[key->code] && key->pressed;

		/* A queued hold cannot outlive the screen that received its press. */
		if (app->terminal.paused && (key->code == KEY_NUMERIC_STAR ||
					     key->code == KEY_NUMERIC_POUND))
			return;
		if (key->code < KEY_CNT)
			app->phone_pressed[key->code] = key->pressed;
		fplinux_terminal_phone(
			&app->terminal, key->code, key->pressed, repeat,
			key->time_ms,
			fplinux_terminal_pty_shell_foreground(&app->pty));
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
			return;
		}
		fplinux_terminal_key(&app->terminal, key->keysym, ascii,
				     modifiers, unicode);
	}
}

static void sync_input_pause(struct gui_terminal *app)
{
	bool covered = !fplinux_ui_is_visible(app->ui) ||
		       fplinux_ui_menu_is_open(app->ui) ||
		       fplinux_ui_current(app->ui) != app->screen;

	/* A key held into another screen is released there, unseen here. */
	if (covered)
		memset(app->phone_pressed, 0, sizeof(app->phone_pressed));
	fplinux_terminal_pause(&app->terminal, covered, fplinux_ui_now_ms());
}

static void drain_keys(struct gui_terminal *app)
{
	if (app->failed)
		return;
	sync_input_pause(app);
	while (app->pending_count &&
	       fplinux_terminal_session_can_input(&app->session)) {
		const struct queued_input *input =
			&app->pending_keys[app->pending_start];

		switch (input->action) {
		case INPUT_KEY:
			terminal_key(app, &input->key);
			break;
		case INPUT_COMMAND:
			fplinux_terminal_run(
				&app->terminal,
				(enum fplinux_terminal_command)input->value);
			break;
		case INPUT_SYMBOL:
			fplinux_terminal_type_symbol(&app->terminal,
						     input->value);
			break;
		case INPUT_SPECIAL:
			fplinux_terminal_send_special(&app->terminal,
						      input->value);
			break;
		case INPUT_LANGUAGE:
			fplinux_terminal_select_language(&app->terminal,
							 input->value);
			break;
		case INPUT_MODIFIERS:
			fplinux_terminal_set_modifiers(
				&app->terminal, (unsigned int)input->value);
			break;
		}
		app->pending_start =
			(app->pending_start + 1) % TERMINAL_PENDING_KEYS;
		--app->pending_count;
	}
}

static void queue_input(struct gui_terminal *app,
			const struct queued_input *input)
{
	size_t index;

	if (app->failed || app->session.input_closed)
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

static void canvas_key(struct fplinux_ui *ui, struct fplinux_ui_screen *canvas,
		       const struct fplinux_ui_key *key, void *context)
{
	struct gui_terminal *app = context;

	(void)ui;
	(void)canvas;
	queue_input(app,
		    &(struct queued_input){ .action = INPUT_KEY, .key = *key });
	after_input(app);
}

static void tick(lv_timer_t *timer)
{
	struct gui_terminal *app = lv_timer_get_user_data(timer);
	uint64_t now;

	if (app->failed)
		return;
	drain_keys(app);
	now = fplinux_ui_now_ms();
	if (app->terminal.failed) {
		fail(app,
		     _("The shell connection was lost. Open Terminal again."));
		return;
	}
	if (fplinux_terminal_session_can_input(&app->session))
		fplinux_terminal_tick(
			&app->terminal, now,
			fplinux_terminal_pty_shell_foreground(&app->pty));
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
	if (!visible) {
		lv_timer_pause(app->timer);
		return;
	}
	lv_timer_resume(app->timer);
	app->terminal.dirty = true;
	update(app);
}

static bool resize_terminal(struct gui_terminal *app)
{
	struct winsize size = { 0 };
	int width;
	int height;
	unsigned int columns;
	unsigned int rows;

	if (!fplinux_ui_screen_get_content_size(app->screen, &width, &height) ||
	    width <= 0 || height <= 0 ||
	    !fplinux_terminal_grid_init(&app->grid,
					fplinux_ui_theme(app->ui)->font))
		return false;
	columns = (unsigned int)width / app->grid.cell_width;
	rows = (unsigned int)height / app->grid.cell_height;
	if (!columns || columns > USHRT_MAX || rows < 2 || rows > USHRT_MAX)
		return false;
	if (tsm_screen_resize(app->terminal.screen, columns, rows) < 0)
		return false;
	size.ws_col = (unsigned short)columns;
	size.ws_row = (unsigned short)rows;
	if (ioctl(app->pty.fd, TIOCSWINSZ, &size) < 0)
		return false;
	app->title_width = width - 2 * fplinux_ui_metrics(app->ui)->margin;
	app->terminal.dirty = true;
	update(app);
	return true;
}

static void cancel_phone_holds(struct gui_terminal *app)
{
	size_t kept = 0;

	for (size_t index = 0; index < app->pending_count; ++index) {
		const struct queued_input *input =
			&app->pending_keys[(app->pending_start + index) %
					   TERMINAL_PENDING_KEYS];
		bool gesture = input->action == INPUT_KEY &&
			       input->key.source == FPLINUX_UI_KEY_PHONE &&
			       (input->key.code == KEY_NUMERIC_STAR ||
				input->key.code == KEY_NUMERIC_POUND);

		if (gesture)
			continue;
		app->pending_keys[(app->pending_start + kept) %
				  TERMINAL_PENDING_KEYS] = *input;
		++kept;
	}
	app->pending_count = kept;
	memset(app->phone_pressed, 0, sizeof(app->phone_pressed));
	fplinux_terminal_cancel_held(&app->terminal);
}

static void ui_event(struct fplinux_ui *ui,
		     const struct fplinux_ui_event *event, void *data)
{
	struct gui_terminal *app = data;

	if (event->type == FPLINUX_UI_EVENT_INPUT_CANCEL) {
		if (event->key.source == FPLINUX_UI_KEY_PHONE)
			cancel_phone_holds(app);
	} else if (event->type == FPLINUX_UI_EVENT_CLOSE) {
		/* The manager requested this exit, including Home over the alert. */
		app->failed = false;
		app->closing = true;
		fplinux_ui_quit(ui);
	} else if (event->type == FPLINUX_UI_EVENT_VISIBILITY)
		set_visible(app, event->visible);
	else if (event->type == FPLINUX_UI_EVENT_RESIZE && app->screen &&
		 !resize_terminal(app))
		fail(app,
		     _("The screen could not be updated. Open Terminal again."));
}

static void draw_terminal(lv_event_t *event)
{
	struct gui_terminal *app = lv_event_get_user_data(event);
	lv_area_t bounds;

	lv_obj_get_coords(app->canvas, &bounds);
	fplinux_terminal_render(&app->terminal, &app->grid,
				lv_event_get_layer(event), &bounds);
}

/* The canvas below the title band, and the width of the title's text. */
static bool create_screen(struct gui_terminal *app, const char *title)
{
	app->title_width = fplinux_ui_width(app->ui) -
			   2 * fplinux_ui_metrics(app->ui)->margin;
	app->screen = fplinux_ui_canvas_new(app->ui, title);
	if (!app->screen)
		return false;
	app->canvas = fplinux_ui_canvas_object(app->screen);
	if (!app->canvas)
		return false;
	lv_obj_set_scrollable(app->canvas, false);
	lv_obj_add_event_cb(app->canvas, draw_terminal, LV_EVENT_DRAW_MAIN,
			    app);
	fplinux_ui_canvas_set_key_handler(app->screen, canvas_key, app);
	/* A shell needs the keyboard's Escape and Enter as keys. */
	fplinux_ui_canvas_set_keyboard_keys(app->screen, true);
	fplinux_ui_canvas_set_key_repeat(app->screen, true);
	fplinux_ui_screen_set_listener(app->screen, canvas_command, NULL, app);
	snprintf(app->title, sizeof(app->title), "%s", title);
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
	unsigned int columns;
	unsigned int rows;
	int height;
	const char *title;
	char shell_token[33];
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
	if (!fplinux_terminal_grid_init(&app.grid,
					fplinux_ui_theme(app.ui)->font)) {
		fprintf(stderr,
			"fplinux-terminal: cannot measure the GUI font\n");
		goto done;
	}
	title = _("Terminal");
	/* The grid and the PTY size fill the canvas below the title band. */
	height = fplinux_ui_height(app.ui) -
		 fplinux_ui_metrics(app.ui)->header_height;
	if (height <= 0 || fplinux_ui_width(app.ui) <= 0) {
		fprintf(stderr,
			"fplinux-terminal: assigned viewport is too small\n");
		goto done;
	}
	columns = (unsigned int)fplinux_ui_width(app.ui) / app.grid.cell_width;
	rows = (unsigned int)height / app.grid.cell_height;
	if (rows < 2 || rows > USHRT_MAX || !columns || columns > USHRT_MAX) {
		fprintf(stderr,
			"fplinux-terminal: assigned viewport is too small\n");
		goto done;
	}
	if (!generate_token(shell_token) ||
	    !fplinux_terminal_init(&app.terminal, columns, rows, shell_token) ||
	    !fplinux_terminal_pty_open(&app.pty, columns, rows, "/bin/bash",
				       TERMINAL_STARTUP_PATH, shell_token)) {
		perror("fplinux-terminal: start Bash PTY");
		goto done;
	}
	fplinux_terminal_load_languages(&app.terminal,
					FPLINUX_LANGUAGE_DIRECTORY,
					fplinux_ui_input_locale(app.ui));
	fplinux_terminal_session_init(&app.session, &app.terminal, &app.pty);
	if (!create_screen(&app, title)) {
		perror("terminal: create the terminal screen");
		goto done;
	}
	if (!fplinux_ui_set_title(app.ui, title))
		goto done;
	update_watch(&app);
	app.timer = lv_timer_create(tick, TERMINAL_TICK_MS, &app);
	if (!app.timer)
		goto done;
	if (!fplinux_ui_is_visible(app.ui))
		lv_timer_pause(app.timer);
	result = fplinux_ui_run(app.ui) == 0 && !app.failed ? EXIT_SUCCESS :
							      EXIT_FAILURE;
	if (!result && !app.closing && app.session.finished)
		result = fplinux_terminal_session_result(&app.session);

done:
	if (app.timer)
		lv_timer_delete(app.timer);
	/* Every exit path closes the master, terminates its job and reaps Bash. */
	fplinux_terminal_pty_close(&app.pty);
	fplinux_terminal_destroy(&app.terminal);
	if (app.ui)
		fplinux_ui_close(app.ui);
	return result;
}
