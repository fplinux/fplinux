/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include <fplinux/fplinux-ui.h>
#include <fplinux/fplinux-ui-menu.h>
#include <fplinux/fplinux-ui-prefs.h>
#include <fplinux/fplinux-ui-screen.h>
#include <fplinux/fplinux-ui-symbol-picker.h>

#include "fplinux-terminal.h"
#include "terminal-pty.h"
#include "terminal-render.h"

#include <assert.h>
#include <errno.h>
#include <linux/input-event-codes.h>
#include <locale.h>
#include <limits.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon-keysyms.h>

struct lv_obj {
	lv_event_cb_t draw;
	void *context;
	struct lv_obj *parent;
	int width, height, x, y, align;
	char text[64];
	bool label;
};

struct lv_event {
	void *context;
	lv_layer_t layer;
};

struct lv_timer {
	lv_timer_cb_t callback;
	void *context;
	bool paused;
	bool deleted;
};

struct fplinux_ui_screen {
	fplinux_ui_canvas_key_filter_cb filter;
	void *filter_context;
	fplinux_ui_canvas_key_cb key;
	void *key_context;
	fplinux_ui_command_cb command;
	void *command_context;
	bool raw_keyboard;
	char title[64];
	char items[4][64];
	bool selected[4];
	size_t count, focused;
	int done_command;
	int right_command;
};

struct fplinux_ui_symbol_picker {
	bool open;
	unsigned int selected;
};

struct fplinux_ui {
	fplinux_ui_event_cb event;
	void *context;
	struct fplinux_ui_screen *current;
	fplinux_ui_fd_cb fd_callback;
	void *fd_context;
	int watched_fd;
	short watched_events;
	bool visible;
	bool menu_open;
	bool quit;
	bool closed;
};

static struct fplinux_ui ui = { .watched_fd = -1, .visible = true };
static struct fplinux_ui_screen screen;
static struct fplinux_ui_screen list_screen;
static struct fplinux_ui_screen alert_screen;
static struct lv_obj canvas;
static struct lv_obj status_objects[4];
static size_t status_object_count;
static struct lv_timer timer;
static const lv_font_t body_font = { .line_height = 12 };
static const lv_font_t title_font = { .line_height = 16 };
static const struct fplinux_ui_theme theme = {
	.font = &body_font,
	.font_large = &title_font,
};
static const struct fplinux_ui_metrics metrics = {
	.header_height = 16,
	.margin = 3,
	.spacing = 2,
};
static const char *scenario;
static uint64_t now_ms;
static bool advancing_clock;
static int shell_peer = -1;
static bool pty_opened;
static bool pty_closed;
static struct fplinux_terminal_grid *opened_grids[2];
static unsigned int grid_opened;
static unsigned int grid_closed;
static unsigned int painted_frames;
static char first_line[54];
static struct fplinux_ui_symbol_picker picker;
static int app_width = 128, app_height = 128;
static unsigned int expected_columns = 21, expected_rows = 9;
static unsigned int reported_columns, reported_rows;
static bool winsize_changed;
static char alert_text[128];

static void draw_frame(void)
{
	struct lv_event draw = { .context = canvas.context };

	canvas.draw(&draw);
}

static void command(int id)
{
	assert(ui.current == &screen);
	screen.command(&ui, &screen, id, screen.command_context);
}

static void choose_font(size_t index)
{
	command(FPLINUX_TERMINAL_COMMAND_FONT_SIZE);
	assert(ui.current == &list_screen);
	assert(!strcmp(list_screen.title, "Font size"));
	assert(list_screen.count == 2);
	assert(!strcmp(list_screen.items[0], "Compact (6×12)"));
	assert(!strcmp(list_screen.items[1], "Larger (8×16)"));
	assert(list_screen.focused == 0);
	list_screen.focused = index;
	list_screen.command(&ui, &list_screen, FPLINUX_UI_SELECT_COMMAND,
			    list_screen.command_context);
}

int fplinux_terminal_main(int argc, char **argv);

static void event(enum fplinux_ui_event_type type, bool visible)
{
	const struct fplinux_ui_event notification = {
		.type = type,
		.key = { .source = FPLINUX_UI_KEY_PHONE },
		.visible = visible,
	};

	ui.event(&ui, &notification, ui.context);
}

static void expect_shell_bytes(const char *expected)
{
	char bytes[128];
	size_t size = strlen(expected);
	ssize_t count = read(shell_peer, bytes, sizeof(bytes));

	assert(pty_opened && !pty_closed);
	if (!size) {
		assert(count == -1 &&
		       (errno == EAGAIN || errno == EWOULDBLOCK));
		return;
	}
	assert(count == (ssize_t)size);
	assert(!memcmp(bytes, expected, size));
}

static bool dispatch_ready(int timeout_ms)
{
	struct pollfd descriptor = {
		.fd = ui.watched_fd,
		.events = ui.watched_events,
	};
	int result;

	assert(ui.fd_callback && descriptor.fd >= 0);
	do {
		result = poll(&descriptor, 1, timeout_ms);
	} while (result < 0 && errno == EINTR);
	assert(result >= 0);
	if (!result)
		return false;
	ui.fd_callback(&ui, descriptor.fd, descriptor.revents, ui.fd_context);
	return true;
}

static void expect_quiet_watch(void)
{
	assert(ui.watched_events & POLLIN);
	assert(!(ui.watched_events & POLLOUT));
	assert(!dispatch_ready(0));
}

static void dispatch_key(const struct fplinux_ui_key *key)
{
	if (screen.filter &&
	    screen.filter(&ui, &screen, key, screen.filter_context))
		return;
	if (key->pressed) {
		if ((key->source == FPLINUX_UI_KEY_PHONE &&
		     key->code == KEY_F13) ||
		    (key->source == FPLINUX_UI_KEY_KEYBOARD &&
		     key->code == KEY_COMPOSE)) {
			ui.menu_open = true;
			return;
		}
		if (key->code == KEY_F14 || key->code == KEY_OK) {
			screen.command(&ui, &screen,
				       key->code == KEY_F14 ?
					       screen.right_command :
					       FPLINUX_TERMINAL_COMMAND_ENTER,
				       screen.command_context);
			return;
		}
	}
	if (screen.key)
		screen.key(&ui, &screen, key, screen.key_context);
}

static void phone(unsigned int code, bool pressed, uint64_t time_ms)
{
	const struct fplinux_ui_key key = {
		.source = FPLINUX_UI_KEY_PHONE,
		.code = code,
		.pressed = pressed,
		.time_ms = time_ms,
	};

	now_ms = time_ms;
	dispatch_key(&key);
}

static void digit_two(uint64_t time_ms)
{
	phone(KEY_NUMERIC_2, true, time_ms);
	phone(KEY_NUMERIC_2, false, time_ms + 1);
}

static void pulse(uint64_t time_ms)
{
	now_ms = time_ms;
	if (!timer.paused)
		timer.callback(&timer);
}

static void cover(bool covered, uint64_t time_ms)
{
	now_ms = time_ms;
	if (!strcmp(scenario, "visibility")) {
		ui.visible = !covered;
		event(FPLINUX_UI_EVENT_VISIBILITY, ui.visible);
		assert(timer.paused == covered);
	} else {
		ui.menu_open = covered;
		pulse(time_ms);
		assert(!timer.paused);
	}
	assert(!pty_closed && !ui.quit);
}

static void hidden_input(void)
{
	const char output[] = "hidden\r\n";
	struct lv_event draw = { .context = canvas.context };

	digit_two(1000);
	cover(true, 1300);
	pulse(4000);
	expect_shell_bytes("");
	/* The watched connection still drains shell output under either cover. */
	assert(write(shell_peer, output, sizeof(output) - 1) ==
	       (ssize_t)sizeof(output) - 1);
	assert(dispatch_ready(1000));
	expect_shell_bytes("");
	cover(false, 5000);
	canvas.draw(&draw);
	assert(!strncmp(first_line, "hidden", 6));
	pulse(5999);
	expect_shell_bytes("");
	pulse(6000);
	expect_shell_bytes("a");

	phone(KEY_NUMERIC_2, true, 6100);
	cover(true, 6200);
	pulse(7000);
	cover(false, 8000);
	phone(KEY_NUMERIC_2, false, 8001);
	pulse(9000);
	/* Covering cancels the digit hold and preserves the pending letter. */
	expect_shell_bytes("a");
	phone(KEY_F14, true, 9100);
	expect_shell_bytes("\177");
	cover(true, 9200);
	pulse(10000);
	cover(false, 11000);
	phone(KEY_F14, false, 11001);
	expect_shell_bytes("");
}

static void keyboard(uint32_t keysym, unsigned int code, uint32_t character)
{
	const struct fplinux_ui_key key = {
		.source = FPLINUX_UI_KEY_KEYBOARD,
		.code = code,
		.keysym = keysym,
		.unicode = character,
		.ascii = character,
		.pressed = true,
		.time_ms = now_ms,
	};

	dispatch_key(&key);
}

static void resumed_input_uses_time_after_unpause(void)
{
	digit_two(1000);
	ui.menu_open = true;
	pulse(1300);
	expect_shell_bytes("");
	ui.menu_open = false;
	/* Pause synchronization reads the clock before tick's expiration sample. */
	advancing_clock = true;
	pulse(5000);
	expect_shell_bytes("");
	advancing_clock = false;
	pulse(5999);
	expect_shell_bytes("");
	pulse(6000);
	expect_shell_bytes("a");
}

static void keyboard_input(void)
{
	assert(screen.filter);
	keyboard(XKB_KEY_F13, KEY_F13, 0);
	keyboard(XKB_KEY_F14, KEY_F14, 0);
	expect_shell_bytes("\033[1;2P\033[1;2Q");
	assert(!ui.quit && !ui.menu_open && ui.current == &screen);
	digit_two(1000);
	now_ms = 1300;
	keyboard(XKB_KEY_Shift_L, KEY_LEFTSHIFT, 0);
	keyboard(XKB_KEY_Control_L, KEY_LEFTCTRL, 0);
	keyboard(XKB_KEY_ISO_Next_Group, KEY_RIGHTALT, 0);
	expect_shell_bytes("");
	pulse(1999);
	expect_shell_bytes("");
	pulse(2000);
	expect_shell_bytes("a");
	keyboard(XKB_KEY_x, KEY_X, 'x');
	expect_shell_bytes("x");
	keyboard(XKB_KEY_Menu, KEY_COMPOSE, 0);
	assert(ui.menu_open && !ui.quit);
	expect_shell_bytes("");
}

static void shared_picker_and_clear_consume_commands_and_receive_releases(void)
{
	digit_two(1000);
	phone(KEY_NUMERIC_STAR, true, 1100);
	phone(KEY_NUMERIC_STAR, false, 1101);
	expect_shell_bytes("a");
	assert(picker.open && picker.selected == 0 && !ui.menu_open);
	phone(KEY_RIGHT, true, 1200);
	phone(KEY_RIGHT, false, 1201);
	phone(KEY_RIGHT, true, 1300);
	phone(KEY_RIGHT, false, 1301);
	assert(picker.selected == 2);
	phone(KEY_OK, true, 1400);
	phone(KEY_OK, false, 1401);
	expect_shell_bytes("?");
	assert(!picker.open);
	phone(KEY_NUMERIC_STAR, true, 1500);
	phone(KEY_NUMERIC_STAR, false, 1501);
	assert(picker.open);
	phone(KEY_F14, true, 1600);
	phone(KEY_F14, false, 1601);
	expect_shell_bytes("");
	assert(!picker.open);
	phone(KEY_NUMERIC_POUND, true, 1700);
	phone(KEY_NUMERIC_POUND, false, 1701);
	digit_two(1800);
	pulse(2800);
	expect_shell_bytes("A");
	phone(KEY_F14, true, 3000);
	expect_shell_bytes("\177");
	pulse(3499);
	expect_shell_bytes("");
	pulse(3500);
	expect_shell_bytes("\177");
	pulse(3600);
	expect_shell_bytes("\177");
	phone(KEY_F14, false, 3601);
	pulse(4000);
	expect_shell_bytes("");
}

static void hidden_releases_cancel_phone_gestures(bool cancelled)
{
	phone(KEY_NUMERIC_2, true, 1000);
	/* The UI publishes hidden state, sends held releases, then VISIBILITY. */
	if (cancelled) {
		now_ms = 2000;
		event(FPLINUX_UI_EVENT_INPUT_CANCEL, false);
	} else {
		ui.visible = false;
	}
	phone(KEY_NUMERIC_2, false, 2000);
	ui.visible = false;
	event(FPLINUX_UI_EVENT_VISIBILITY, false);
	expect_shell_bytes("");
	ui.visible = true;
	now_ms = 3000;
	event(FPLINUX_UI_EVENT_VISIBILITY, true);
	pulse(3999);
	expect_shell_bytes("");
	pulse(4000);
	expect_shell_bytes("a");
	phone(KEY_F14, true, 6000);
	expect_shell_bytes("\177");
	if (cancelled) {
		now_ms = 7000;
		event(FPLINUX_UI_EVENT_INPUT_CANCEL, false);
	} else {
		ui.visible = false;
	}
	phone(KEY_F14, false, 7000);
	ui.visible = false;
	event(FPLINUX_UI_EVENT_VISIBILITY, false);
	/* Synthetic release stops the held Clear without another shell byte. */
	expect_shell_bytes("");
}

static void input_backpressure(void)
{
	char bytes[512];
	size_t filler_size = 0;
	size_t received = 0;
	ssize_t count;
	int send_buffer = 4096;
	bool options = !strcmp(scenario, "backpressure-options");
	bool cancelled = !strcmp(scenario, "input-cancel");
	bool hidden = !strcmp(scenario, "backpressure-hidden");
	bool held = !strcmp(scenario, "backpressure");
	bool phone_input = held || options || cancelled;
	size_t expected_size = phone_input ? 8203 : 8202;

	assert(setsockopt(ui.watched_fd, SOL_SOCKET, SO_SNDBUF, &send_buffer,
			  sizeof(send_buffer)) == 0);
	memset(bytes, '!', sizeof(bytes));
	while ((count = write(ui.watched_fd, bytes, sizeof(bytes))) > 0)
		filler_size += (size_t)count;
	assert(filler_size && count == -1 && errno == EAGAIN);
	/* The shell is not reading while ordinary keys and Enter keep arriving. */
	for (size_t index = 0; index < 8200; ++index)
		keyboard(XKB_KEY_x, KEY_X, 'x');
	if (phone_input)
		phone(KEY_NUMERIC_2, true, 9000);
	if (held)
		phone(KEY_NUMERIC_2, false, 9600);
	keyboard(XKB_KEY_y, KEY_Y, 'y');
	screen.command(&ui, &screen, FPLINUX_TERMINAL_COMMAND_ENTER,
		       screen.command_context);
	assert(!ui.quit && !pty_closed);
	/* Open Options before a timer callback can observe the covered screen. */
	if (options)
		ui.menu_open = true;
	if (cancelled)
		event(FPLINUX_UI_EVENT_INPUT_CANCEL, false);
	if (hidden) {
		ui.visible = false;
		event(FPLINUX_UI_EVENT_VISIBILITY, false);
		assert(timer.paused);
	}
	while (filler_size) {
		size_t capacity = filler_size < sizeof(bytes) ? filler_size :
								sizeof(bytes);

		count = read(shell_peer, bytes, capacity);
		assert(count > 0);
		for (ssize_t index = 0; index < count; ++index)
			assert(bytes[index] == '!');
		filler_size -= (size_t)count;
	}
	for (unsigned int attempt = 0; attempt < 8 && received < expected_size;
	     ++attempt) {
		assert(dispatch_ready(1000));
		while ((count = read(shell_peer, bytes, sizeof(bytes))) > 0)
			for (ssize_t index = 0; index < count; ++index) {
				char expected = 'x';

				if (phone_input && received == 8200)
					expected = held ? '2' : 'a';
				else if (received == expected_size - 2)
					expected = 'y';
				else if (received == expected_size - 1)
					expected = '\r';

				assert(received < expected_size &&
				       bytes[index] == expected);
				++received;
			}
		assert(count == -1 && errno == EAGAIN);
	}
	assert(received == expected_size);
	expect_shell_bytes("");
	expect_quiet_watch();
	if (hidden)
		assert(timer.paused);
}

static void font_change_keeps_the_session_and_pending_input(void)
{
	const char output[] = "kept\r\n";
	int original_fd = ui.watched_fd;

	assert(write(shell_peer, output, sizeof(output) - 1) ==
	       (ssize_t)sizeof(output) - 1);
	assert(dispatch_ready(1000));
	digit_two(1000);
	expect_shell_bytes("");
	choose_font(1);
	expected_columns = 16;
	expected_rows = 6;
	assert(reported_columns == 16 && reported_rows == 6);
	assert(ui.watched_fd == original_fd && !pty_closed && !ui.quit);
	if (!strcmp(scenario, "font-save-failure") ||
	    !strcmp(scenario, "font-sync-failure")) {
		assert(ui.current == &alert_screen);
		assert(!strcmp(alert_text,
			       !strcmp(scenario, "font-save-failure") ?
				       "Storage is read-only" :
				       "Storage input/output error"));
		assert(fplinux_ui_pop(&ui));
	}
	assert(ui.current == &screen);
	draw_frame();
	assert(!strncmp(first_line, "kept", 4));
	pulse(1100);
	expect_shell_bytes("");
	pulse(2200);
	expect_shell_bytes("a");
	keyboard(XKB_KEY_Return, KEY_ENTER, '\r');
	expect_shell_bytes("\r");
}

static struct lv_obj *status_label(const char *text)
{
	for (size_t index = 0; index < status_object_count; ++index)
		if (status_objects[index].label &&
		    !strcmp(status_objects[index].text, text))
			return &status_objects[index];
	assert(false);
	return NULL;
}

static void status_keeps_all_states_visible(void)
{
	struct lv_obj *mode = status_label("абв");
	int right_edge = mode->parent->width + mode->x;
	char lines[1200];
	lv_area_t modifiers_area, history_area, mode_area;

	assert(mode->align == LV_ALIGN_RIGHT_MID && right_edge == 125);
	phone(KEY_NUMERIC_POUND, true, 100);
	phone(KEY_NUMERIC_POUND, false, 101);
	mode = status_label("АБВ");
	assert(mode->parent->width + mode->x == right_edge);
	command(FPLINUX_TERMINAL_COMMAND_MODIFIERS);
	assert(ui.current == &list_screen && list_screen.count == 3);
	for (size_t index = 0; index < 3; ++index)
		list_screen.selected[index] = true;
	list_screen.command(&ui, &list_screen, list_screen.done_command,
			    list_screen.command_context);
	assert(ui.current == &screen);
	assert(status_label("Ctrl+Alt+Shift"));
	for (size_t index = 0; index < sizeof(lines); index += 2) {
		lines[index] = '\r';
		lines[index + 1] = '\n';
	}
	assert(write(shell_peer, lines, sizeof(lines)) ==
	       (ssize_t)sizeof(lines));
	assert(dispatch_ready(1000));
	command(FPLINUX_TERMINAL_COMMAND_SCROLLBACK);
	for (unsigned int index = 0; index < 64; ++index) {
		phone(KEY_LEFT, true, 200 + index * 2);
		phone(KEY_LEFT, false, 201 + index * 2);
	}
	assert(status_label("↑512"));
	lv_obj_get_coords(status_label("C+A+S"), &modifiers_area);
	lv_obj_get_coords(status_label("↑512"), &history_area);
	lv_obj_get_coords(mode, &mode_area);
	assert(modifiers_area.x1 >= 0 && modifiers_area.x2 < history_area.x1);
	assert(history_area.x2 < mode_area.x1 && mode_area.x2 < app_width);
	assert(mode_area.x2 + 1 == right_edge);
	assert(modifiers_area.y1 >= 110 && modifiers_area.y2 <= 127);
	assert(history_area.y1 >= 110 && history_area.y2 <= 127);
	assert(mode_area.y1 >= 110 && mode_area.y2 <= 127);
	phone(KEY_F14, true, 400);
	phone(KEY_F14, false, 401);
	assert(status_label("Ctrl+Alt+Shift") && status_label(""));
	phone(KEY_F14, true, 500);
	phone(KEY_F14, false, 501);
	assert(status_label("АБВ"));
	phone(KEY_NUMERIC_POUND, true, 600);
	phone(KEY_NUMERIC_POUND, false, 601);
	assert(status_label("abc"));
	phone(KEY_NUMERIC_POUND, true, 700);
	phone(KEY_NUMERIC_POUND, false, 701);
	assert(status_label("ABC"));
	phone(KEY_NUMERIC_POUND, true, 800);
	phone(KEY_NUMERIC_POUND, false, 801);
	assert(status_label("123"));
	assert(!winsize_changed);
	draw_frame();
	expect_shell_bytes("");
}

struct fplinux_ui *fplinux_ui_open(const char *app_id,
				   fplinux_ui_event_cb callback, void *context)
{
	assert(strcmp(app_id, "org.fplinux.terminal") == 0);
	ui.event = callback;
	ui.context = context;
	if (!strcmp(scenario, "close-during-open"))
		event(FPLINUX_UI_EVENT_CLOSE, false);
	return &ui;
}

void fplinux_ui_close(struct fplinux_ui *instance)
{
	assert(instance == &ui);
	ui.closed = true;
}

int fplinux_ui_run(struct fplinux_ui *instance)
{
	struct lv_event draw = { .context = canvas.context };

	if (!strcmp(scenario, "prefs-open-failure")) {
		assert(instance == &ui && ui.current == &alert_screen);
		assert(!pty_opened && !canvas.draw && !timer.callback &&
		       !grid_opened);
		assert(!strcmp(
			alert_text,
			"The settings could not be loaded. Open Terminal again."));
		assert(alert_screen.command);
		alert_screen.command(&ui, &alert_screen, 0,
				     alert_screen.command_context);
		assert(ui.quit);
		return 0;
	}
	assert(instance == &ui && ui.current == &screen);
	assert(pty_opened && timer.callback && screen.filter && screen.command);
	expect_quiet_watch();
	canvas.draw(&draw);
	assert(painted_frames == 1);
	if (!strcmp(scenario, "keyboard"))
		keyboard_input();
	else if (!strcmp(scenario, "shared-keypad"))
		shared_picker_and_clear_consume_commands_and_receive_releases();
	else if (!strcmp(scenario, "hidden-releases"))
		hidden_releases_cancel_phone_gestures(false);
	else if (!strcmp(scenario, "input-cancel")) {
		hidden_releases_cancel_phone_gestures(true);
		ui.visible = true;
		now_ms = 8000;
		event(FPLINUX_UI_EVENT_VISIBILITY, true);
		input_backpressure();
	} else if (!strcmp(scenario, "resume-clock"))
		resumed_input_uses_time_after_unpause();
	else if (!strcmp(scenario, "backpressure") ||
		 !strcmp(scenario, "backpressure-hidden") ||
		 !strcmp(scenario, "backpressure-options"))
		input_backpressure();
	else if (!strcmp(scenario, "font-default")) {
		assert(reported_columns == expected_columns &&
		       reported_rows == expected_rows);
	} else if (!strcmp(scenario, "font-change") ||
		   !strcmp(scenario, "font-save-failure") ||
		   !strcmp(scenario, "font-sync-failure"))
		font_change_keeps_the_session_and_pending_input();
	else if (!strcmp(scenario, "status"))
		status_keeps_all_states_visible();
	else
		hidden_input();
	event(FPLINUX_UI_EVENT_CLOSE, false);
	assert(ui.quit);
	return 0;
}

void fplinux_ui_quit(struct fplinux_ui *instance)
{
	assert(instance == &ui);
	ui.quit = true;
}

void fplinux_ui_watch_fd(struct fplinux_ui *instance, int fd, short events,
			 fplinux_ui_fd_cb callback, void *context)
{
	assert(instance == &ui);
	ui.watched_fd = fd;
	ui.watched_events = events;
	ui.fd_callback = callback;
	ui.fd_context = context;
}

int fplinux_ui_width(const struct fplinux_ui *instance)
{
	assert(instance == &ui);
	return app_width;
}

int fplinux_ui_height(const struct fplinux_ui *instance)
{
	assert(instance == &ui);
	return app_height;
}

const struct fplinux_ui_theme *
fplinux_ui_theme(const struct fplinux_ui *instance)
{
	assert(instance == &ui);
	return &theme;
}

const struct fplinux_ui_metrics *
fplinux_ui_metrics(const struct fplinux_ui *instance)
{
	assert(instance == &ui);
	return &metrics;
}

uint64_t fplinux_ui_now_ms(void)
{
	return advancing_clock ? now_ms++ : now_ms;
}

const char *fplinux_ui_input_locale(const struct fplinux_ui *instance)
{
	assert(instance == &ui);
	return !strcmp(scenario, "status") ? "ru" : "en";
}

bool fplinux_ui_is_softkey(const struct fplinux_ui_key *key, bool left)
{
	return key->source == FPLINUX_UI_KEY_PHONE &&
	       key->code == (left ? KEY_F13 : KEY_F14);
}

bool fplinux_ui_is_visible(const struct fplinux_ui *instance)
{
	return instance->visible;
}

bool fplinux_ui_menu_is_open(const struct fplinux_ui *instance)
{
	return instance->menu_open;
}

bool fplinux_ui_set_title(struct fplinux_ui *instance, const char *title)
{
	assert(instance == &ui && title && *title);
	return true;
}

bool fplinux_ui_back(struct fplinux_ui *instance)
{
	assert(instance == &ui);
	event(FPLINUX_UI_EVENT_CLOSE, false);
	return true;
}

struct fplinux_ui_screen *fplinux_ui_current(const struct fplinux_ui *instance)
{
	return instance->current;
}

void fplinux_ui_push(struct fplinux_ui *instance,
		     struct fplinux_ui_screen *new_screen)
{
	assert(instance == &ui &&
	       (new_screen == &screen || new_screen == &list_screen ||
		new_screen == &alert_screen));
	instance->current = new_screen;
}

void fplinux_ui_navigate(struct fplinux_ui *instance,
			 struct fplinux_ui_screen *new_screen)
{
	fplinux_ui_push(instance, new_screen);
}

bool fplinux_ui_pop(struct fplinux_ui *instance)
{
	assert(instance == &ui);
	assert(instance->current != &screen);
	instance->current = &screen;
	return true;
}

void fplinux_ui_go_back(struct fplinux_ui *instance)
{
	fplinux_ui_pop(instance);
}

struct fplinux_ui_screen *fplinux_ui_canvas_new(struct fplinux_ui *instance,
						const char *title)
{
	assert(instance == &ui && !title);
	return &screen;
}

lv_obj_t *fplinux_ui_canvas_object(struct fplinux_ui_screen *instance)
{
	assert(instance == &screen);
	return &canvas;
}

void fplinux_ui_canvas_set_key_handler(struct fplinux_ui_screen *instance,
				       fplinux_ui_canvas_key_cb handler,
				       void *context)
{
	instance->key = handler;
	instance->key_context = context;
}

void fplinux_ui_canvas_set_key_filter(struct fplinux_ui_screen *instance,
				      fplinux_ui_canvas_key_filter_cb filter,
				      void *context)
{
	instance->filter = filter;
	instance->filter_context = context;
}

void fplinux_ui_canvas_set_keyboard_keys(struct fplinux_ui_screen *instance,
					 bool raw)
{
	instance->raw_keyboard = raw;
}

void fplinux_ui_canvas_set_key_repeat(struct fplinux_ui_screen *instance,
				      enum fplinux_ui_canvas_repeat policy)
{
	assert(instance == &screen &&
	       policy == FPLINUX_UI_CANVAS_REPEAT_KEYMAP);
}

struct fplinux_ui_symbol_picker *fplinux_ui_symbol_picker_create(
	struct fplinux_ui *instance, lv_obj_t *anchor,
	enum fplinux_ui_symbol_picker_placement placement,
	unsigned int selected)
{
	assert(instance == &ui && anchor == &canvas && !picker.open);
	assert(placement == FPLINUX_UI_SYMBOL_PICKER_CENTERED);
	picker.open = true;
	picker.selected = selected;
	return &picker;
}

void fplinux_ui_symbol_picker_delete(struct fplinux_ui_symbol_picker *instance)
{
	if (!instance)
		return;
	assert(instance == &picker);
	picker.open = false;
}

void fplinux_ui_symbol_picker_select(struct fplinux_ui_symbol_picker *instance,
				     unsigned int selected)
{
	assert(instance == &picker && picker.open);
	picker.selected = selected;
}

bool fplinux_ui_symbol_picker_refresh(struct fplinux_ui_symbol_picker *instance)
{
	assert(instance == &picker);
	return picker.open;
}

bool fplinux_ui_symbol_picker_is_open(
	const struct fplinux_ui_symbol_picker *instance)
{
	if (!instance)
		return false;
	assert(instance == &picker);
	return picker.open;
}

unsigned int fplinux_ui_symbol_picker_columns(
	const struct fplinux_ui_symbol_picker *instance)
{
	assert(instance == &picker && picker.open);
	return 6;
}

void fplinux_ui_screen_set_listener(struct fplinux_ui_screen *instance,
				    fplinux_ui_command_cb command,
				    fplinux_ui_change_cb change, void *context)
{
	(void)change;
	instance->command = command;
	instance->command_context = context;
}

const void *fplinux_ui_glyph(struct fplinux_ui *instance, const char *name,
			     enum fplinux_ui_glyph_size size)
{
	assert(instance == &ui && size == FPLINUX_UI_GLYPH_ROW);
	return name;
}

bool fplinux_ui_screen_add_command(struct fplinux_ui_screen *instance,
				   const struct fplinux_ui_command *command)
{
	assert((instance == &screen || instance == &list_screen ||
		instance == &alert_screen) &&
	       command);
	if (instance == &screen) {
		if (command->id == FPLINUX_TERMINAL_COMMAND_SEARCH)
			assert(command->icon &&
			       !strcmp(command->icon, "search"));
		if (command->id == FPLINUX_TERMINAL_COMMAND_INTERRUPT)
			assert(command->icon && !strcmp(command->icon, "stop"));
		if (command->id == FPLINUX_TERMINAL_COMMAND_FONT_SIZE)
			assert(command->icon &&
			       !strcmp(command->icon, "text-size"));
	}
	if (instance == &list_screen && command->type == FPLINUX_UI_COMMAND_OK)
		instance->done_command = command->id;
	if (instance == &screen &&
	    (command->id == FPLINUX_TERMINAL_COMMAND_CLEAR ||
	     command->id == FPLINUX_TERMINAL_COMMAND_LIVE))
		instance->right_command = command->id;
	return true;
}

void fplinux_ui_screen_remove_command(struct fplinux_ui_screen *instance,
				      int id)
{
	assert(instance == &screen);
	(void)id;
}

bool fplinux_ui_screen_set_title(struct fplinux_ui_screen *instance,
				 const char *title)
{
	assert(instance == &screen && title && *title);
	return true;
}

bool fplinux_ui_screen_get_content_size(
	const struct fplinux_ui_screen *instance, int *width, int *height)
{
	assert(instance == &screen);
	*width = app_width;
	*height = app_height;
	return true;
}

struct fplinux_ui_screen *fplinux_ui_list_new(struct fplinux_ui *instance,
					      const char *title,
					      enum fplinux_ui_list_kind kind)
{
	assert(instance == &ui && title && *title);
	assert(kind == FPLINUX_UI_LIST_IMPLICIT ||
	       kind == FPLINUX_UI_LIST_MULTIPLE);
	memset(&list_screen, 0, sizeof(list_screen));
	assert(snprintf(list_screen.title, sizeof(list_screen.title), "%s",
			title) < (int)sizeof(list_screen.title));
	return &list_screen;
}

struct fplinux_ui_screen *fplinux_ui_form_new(struct fplinux_ui *instance,
					      const char *title)
{
	(void)instance;
	(void)title;
	assert(false);
	return NULL;
}

struct fplinux_ui_screen *fplinux_ui_alert_new(struct fplinux_ui *instance,
					       enum fplinux_ui_alert_kind kind,
					       const char *title,
					       const char *text,
					       unsigned int timeout_ms)
{
	assert(instance == &ui && kind == FPLINUX_UI_ALERT_ERROR);
	assert(!strcmp(title, !strcmp(scenario, "prefs-open-failure") ?
				      "Terminal error" :
			      !strcmp(scenario, "font-sync-failure") ?
				      "Saving not confirmed" :
				      "Font size") &&
	       timeout_ms == 0);
	assert(snprintf(alert_text, sizeof(alert_text), "%s", text) <
	       (int)sizeof(alert_text));
	return &alert_screen;
}

size_t fplinux_ui_list_append(struct fplinux_ui_screen *list, const char *text,
			      const char *detail)
{
	assert(list == &list_screen && list->count < 4 && !detail);
	assert(snprintf(list->items[list->count], sizeof(list->items[0]), "%s",
			text) < (int)sizeof(list->items[0]));
	return list->count++;
}

bool fplinux_ui_list_set_select_label(struct fplinux_ui_screen *list,
				      const char *label)
{
	(void)list;
	(void)label;
	assert(false);
	return false;
}

size_t fplinux_ui_list_get_focused(const struct fplinux_ui_screen *list)
{
	assert(list == &list_screen);
	return list->focused;
}

void fplinux_ui_list_set_focused(struct fplinux_ui_screen *list, size_t index)
{
	assert(list == &list_screen && index < list->count);
	list->focused = index;
}

bool fplinux_ui_list_is_selected(const struct fplinux_ui_screen *list,
				 size_t index)
{
	assert(list == &list_screen && index < list->count);
	return list->selected[index];
}

void fplinux_ui_list_set_selected(struct fplinux_ui_screen *list, size_t index,
				  bool selected)
{
	assert(list == &list_screen && index < list->count);
	list->selected[index] = selected;
}

void fplinux_ui_screen_set_data(struct fplinux_ui_screen *instance, void *data)
{
	(void)instance;
	(void)data;
	assert(false);
}

void *fplinux_ui_screen_get_data(const struct fplinux_ui_screen *instance)
{
	(void)instance;
	assert(false);
	return NULL;
}

size_t fplinux_ui_form_append_string(struct fplinux_ui_screen *form,
				     const char *label, const char *text)
{
	(void)form;
	(void)label;
	(void)text;
	assert(false);
	return 0;
}

lv_obj_t *lv_obj_create(lv_obj_t *parent)
{
	struct lv_obj *object;

	assert(parent && status_object_count < 4);
	object = &status_objects[status_object_count++];
	object->parent = parent;
	return object;
}

lv_obj_t *lv_label_create(lv_obj_t *parent)
{
	lv_obj_t *object = lv_obj_create(parent);

	object->label = true;
	object->height = body_font.line_height;
	return object;
}

void lv_obj_remove_style_all(lv_obj_t *object)
{
	assert(object->parent == &canvas);
}

void lv_obj_set_size(lv_obj_t *object, int width, int height)
{
	assert(object && width > 0 && height > 0);
	object->width = width;
	object->height = height;
}

void lv_obj_align(lv_obj_t *object, int align, int x, int y)
{
	assert(object && object != &canvas);
	object->align = align;
	object->x = x;
	object->y = y;
}

void lv_obj_set_style_bg_color(lv_obj_t *object, lv_color_t color, int selector)
{
	assert(object && color == theme.surface && !selector);
}

void lv_obj_set_style_bg_opa(lv_obj_t *object, lv_opa_t opacity, int selector)
{
	assert(object && opacity == LV_OPA_COVER && !selector);
}

void lv_obj_set_style_border_color(lv_obj_t *object, lv_color_t color,
				   int selector)
{
	assert(object && color == theme.border && !selector);
}

void lv_obj_set_style_border_width(lv_obj_t *object, int width, int selector)
{
	assert(object && width == 1 && !selector);
}

void lv_obj_set_style_border_side(lv_obj_t *object, int side, int selector)
{
	assert(object && side == LV_BORDER_SIDE_TOP && !selector);
}

void lv_obj_set_style_text_font(lv_obj_t *object, const lv_font_t *font,
				int selector)
{
	assert(object && font == &body_font && !selector);
}

void lv_obj_set_style_text_color(lv_obj_t *object, lv_color_t color,
				 int selector)
{
	assert(object && color == theme.foreground && !selector);
}

void lv_label_set_text(lv_obj_t *object, const char *text)
{
	lv_point_t size;

	assert(object->label && strlen(text) < sizeof(object->text));
	strcpy(object->text, text);
	lv_text_get_size(&size, text, &body_font, 0, 0, LV_COORD_MAX,
			 LV_TEXT_FLAG_NONE);
	object->width = size.x;
}

const char *lv_label_get_text(const lv_obj_t *object)
{
	assert(object->label);
	return object->text;
}

void lv_obj_set_scrollable(lv_obj_t *object, bool scrollable)
{
	assert(object && !scrollable);
}

void lv_obj_add_event_cb(lv_obj_t *object, lv_event_cb_t callback, int type,
			 void *context)
{
	assert(object == &canvas && type == LV_EVENT_DRAW_MAIN);
	object->draw = callback;
	object->context = context;
}

void lv_obj_get_coords(lv_obj_t *object, lv_area_t *area)
{
	int x = object->x;
	int y = object->y;

	if (object == &canvas) {
		*area = (lv_area_t){ 0, 0, app_width - 1, app_height - 1 };
		return;
	}
	assert(object->parent);
	if (object->align == LV_ALIGN_BOTTOM_MID)
		y += app_height - object->height;
	else {
		assert(object->parent->parent == &canvas);
		y += app_height - object->parent->height +
		     (object->parent->height - object->height) / 2;
		if (object->align == LV_ALIGN_RIGHT_MID)
			x += object->parent->width - object->width;
		else
			assert(object->align == LV_ALIGN_LEFT_MID);
	}
	*area = (lv_area_t){ x, y, x + object->width - 1,
			     y + object->height - 1 };
}

void lv_obj_invalidate(lv_obj_t *object)
{
	assert(object == &canvas);
}

void *lv_event_get_user_data(lv_event_t *draw)
{
	return draw->context;
}

lv_layer_t *lv_event_get_layer(lv_event_t *draw)
{
	return &draw->layer;
}

void lv_text_get_size(lv_point_t *size, const char *text, const lv_font_t *font,
		      int letter_space, int line_space, int width, int flags)
{
	int characters = 0;

	assert(font == &body_font);
	(void)letter_space;
	(void)line_space;
	(void)width;
	(void)flags;
	for (const unsigned char *byte = (const unsigned char *)text; *byte;
	     ++byte)
		if ((*byte & 0xc0) != 0x80)
			++characters;
	size->x = characters * 6;
	size->y = font->line_height;
}

lv_timer_t *lv_timer_create(lv_timer_cb_t callback, unsigned int period,
			    void *context)
{
	assert(period > 0);
	timer.callback = callback;
	timer.context = context;
	return &timer;
}

void lv_timer_pause(lv_timer_t *instance)
{
	instance->paused = true;
}

void lv_timer_resume(lv_timer_t *instance)
{
	instance->paused = false;
}

void lv_timer_delete(lv_timer_t *instance)
{
	instance->deleted = true;
}

void *lv_timer_get_user_data(lv_timer_t *instance)
{
	return instance->context;
}

bool fplinux_terminal_grid_open(struct fplinux_terminal_grid *grid,
				const char *path)
{
	bool compact =
		!strcmp(path, "/usr/share/fplinux/fonts/ter-u12n-terminal.psf");

	assert(grid_opened < 2 && !grid_closed);
	assert(compact ||
	       !strcmp(path, "/usr/share/fplinux/fonts/ter-u16n-terminal.psf"));
	*grid = (struct fplinux_terminal_grid){
		.font = { .line_height = compact ? 12 : 16 },
		.cell_width = compact ? 6 : 8,
		.cell_height = compact ? 12 : 16,
	};
	opened_grids[grid_opened++] = grid;
	return true;
}

void fplinux_terminal_grid_close(struct fplinux_terminal_grid *grid)
{
	assert(ui.closed && grid_closed < 2);
	if (grid->cell_width)
		assert(grid == opened_grids[0] || grid == opened_grids[1]);
	memset(grid, 0, sizeof(*grid));
	++grid_closed;
}

void fplinux_terminal_render(struct fplinux_terminal *terminal,
			     const struct fplinux_terminal_grid *grid,
			     lv_layer_t *layer, const lv_area_t *bounds)
{
	const struct tsm_screen_cell *cells =
		tsm_screen_draw2(terminal->screen);

	assert(grid_opened == 2 && !grid_closed && layer);
	assert(grid == opened_grids[0] || grid == opened_grids[1]);
	assert(bounds->x1 == 0 && bounds->y1 == 0);
	assert(bounds->x2 == app_width - 1 &&
	       bounds->y2 == app_height - (app_width < 240 ? 18 : 22) - 1);
	assert(cells &&
	       tsm_screen_get_width(terminal->screen) == expected_columns);
	assert(tsm_screen_get_height(terminal->screen) == expected_rows);
	for (size_t index = 0; index < expected_columns; ++index)
		first_line[index] =
			cells[index].ch < 128 ? (char)cells[index].ch : '?';
	first_line[expected_columns] = '\0';
	++painted_frames;
}

bool fplinux_terminal_pty_open(struct fplinux_terminal_pty *pty,
			       unsigned int columns, unsigned int rows,
			       const char *shell, const char *startup)
{
	int sockets[2];

	assert(!pty_opened && columns == expected_columns &&
	       rows == expected_rows);
	reported_columns = columns;
	reported_rows = rows;
	assert(!strcmp(shell, "/bin/bash") && startup && *startup);
	assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
			  0, sockets) == 0);
	pty->fd = sockets[0];
	shell_peer = sockets[1];
	pty_opened = true;
	return true;
}

bool fplinux_terminal_pty_finished(struct fplinux_terminal_pty *pty,
				   int *status)
{
	assert(pty->fd >= 0 && !pty_closed);
	(void)status;
	return false;
}

void fplinux_terminal_pty_close(struct fplinux_terminal_pty *pty)
{
	if (pty->fd < 0)
		return;
	assert(pty_opened && !pty_closed);
	assert(close(pty->fd) == 0);
	pty->fd = -1;
	pty_closed = true;
}

size_t __real_fplinux_language_list(const char *directory,
				    char codes[][FPLINUX_LANGUAGE_CODE_BYTES],
				    size_t capacity);
bool __real_fplinux_language_load(const char *directory, const char *code,
				  struct fplinux_language *language);

/* Installed definitions are replaced with the test-owned language directory. */
size_t __wrap_fplinux_language_list(const char *directory,
				    char codes[][FPLINUX_LANGUAGE_CODE_BYTES],
				    size_t capacity)
{
	(void)directory;
	(void)codes;
	(void)capacity;
	if (!strcmp(scenario, "status"))
		return __real_fplinux_language_list(
			FPLINUX_TERMINAL_TEST_LANGUAGE_DIRECTORY, codes,
			capacity);
	return 0;
}

bool __wrap_fplinux_language_load(const char *directory, const char *code,
				  struct fplinux_language *language)
{
	(void)directory;
	return __real_fplinux_language_load(
		FPLINUX_TERMINAL_TEST_LANGUAGE_DIRECTORY, code, language);
}

struct fplinux_ui_prefs *fplinux_ui_prefs_open(const char *app_id)
{
	const char *directory = getenv("FPLINUX_TERMINAL_TEST_STATE");

	assert(!strcmp(app_id, "org.fplinux.terminal") && directory);
	if (!strcmp(scenario, "prefs-open-failure")) {
		errno = EACCES;
		return NULL;
	}
	return fplinux_ui_prefs_open_directory(directory);
}

struct fplinux_storage_result
__real_fplinux_ui_prefs_save(struct fplinux_ui_prefs *prefs);

struct fplinux_storage_result
__wrap_fplinux_ui_prefs_save(struct fplinux_ui_prefs *prefs)
{
	if (!strcmp(scenario, "font-save-failure")) {
		return (struct fplinux_storage_result){
			.state = FPLINUX_STORAGE_UNPUBLISHED, .error = EROFS
		};
	}
	if (!strcmp(scenario, "font-sync-failure")) {
		struct fplinux_storage_result result =
			__real_fplinux_ui_prefs_save(prefs);

		assert(result.state == FPLINUX_STORAGE_DURABLE);
		return (struct fplinux_storage_result){
			.state = FPLINUX_STORAGE_PUBLISHED_UNSYNCED,
			.error = EIO
		};
	}
	return __real_fplinux_ui_prefs_save(prefs);
}

const char *fplinux_ui_storage_error(struct fplinux_storage_result result)
{
	assert(result.error == EROFS || result.error == EIO);
	return result.state == FPLINUX_STORAGE_PUBLISHED_UNSYNCED ?
		       "Storage input/output error" :
		       "Storage is read-only";
}

int __wrap_ioctl(int fd, unsigned long request, ...)
{
	const struct winsize *size;
	va_list arguments;

	assert(fd == ui.watched_fd && request == TIOCSWINSZ);
	va_start(arguments, request);
	size = va_arg(arguments, const struct winsize *);
	reported_columns = size->ws_col;
	reported_rows = size->ws_row;
	winsize_changed = true;
	va_end(arguments);
	return 0;
}

int main(int argc, char **argv)
{
	char *application_argv[] = { "fplinux-terminal", NULL };
	int result;

	assert(argc >= 2 && setlocale(LC_ALL, "C"));
	scenario = argv[1];
	if (!strcmp(scenario, "cli")) {
		result = fplinux_terminal_main(argc - 1, argv + 1);
		assert(!ui.event && !pty_opened && !timer.callback &&
		       !grid_opened && !grid_closed);
		return result;
	}
	if (!strcmp(scenario, "font-default")) {
		assert(argc == 6);
		app_width = atoi(argv[2]);
		app_height = atoi(argv[3]);
		expected_columns = (unsigned int)atoi(argv[4]);
		expected_rows = (unsigned int)atoi(argv[5]);
	} else
		assert(argc == 2);
	result = fplinux_terminal_main(1, application_argv);
	assert(result == (!strcmp(scenario, "prefs-open-failure") ? 1 : 0));
	assert(ui.closed && ui.quit && grid_closed == 2);
	if (!strcmp(scenario, "close-during-open") ||
	    !strcmp(scenario, "prefs-open-failure")) {
		assert(!pty_opened && !timer.callback && !screen.filter &&
		       !grid_opened);
	} else {
		char byte;

		assert(pty_closed && timer.deleted);
		assert(read(shell_peer, &byte, 1) == 0);
		assert(close(shell_peer) == 0);
	}
	printf("terminal-app %s: OK\n", scenario);
	return 0;
}
