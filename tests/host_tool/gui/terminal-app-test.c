/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include <fplinux/fplinux-ui.h>
#include <fplinux/fplinux-ui-menu.h>
#include <fplinux/fplinux-ui-screen.h>

#include "fplinux-terminal.h"
#include "terminal-pty.h"
#include "terminal-render.h"

#include <assert.h>
#include <errno.h>
#include <linux/input-event-codes.h>
#include <locale.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon-keysyms.h>

struct lv_obj {
	lv_event_cb_t draw;
	void *context;
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
	fplinux_ui_canvas_key_cb key;
	void *key_context;
	fplinux_ui_command_cb command;
	void *command_context;
	bool raw_keyboard;
};

struct fplinux_ui {
	fplinux_ui_event_cb event;
	void *context;
	struct fplinux_ui_screen *current;
	fplinux_ui_fd_cb fd_callback;
	void *fd_context;
	int watched_fd;
	bool visible;
	bool menu_open;
	bool quit;
	bool closed;
};

static struct fplinux_ui ui = { .watched_fd = -1, .visible = true };
static struct fplinux_ui_screen screen;
static struct lv_obj canvas;
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
};
static const char *scenario;
static uint64_t now_ms;
static bool advancing_clock;
static int shell_peer = -1;
static bool pty_opened;
static bool pty_closed;
static unsigned int painted_frames;
static char shell_token[33];
static char first_line[22];
static bool prompt_editing;

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

static void phone(unsigned int code, bool pressed, uint64_t time_ms)
{
	const struct fplinux_ui_key key = {
		.source = FPLINUX_UI_KEY_PHONE,
		.code = code,
		.pressed = pressed,
		.time_ms = time_ms,
	};

	now_ms = time_ms;
	screen.key(&ui, &screen, &key, screen.key_context);
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
	char output[80];
	int output_size;
	struct lv_event draw = { .context = canvas.context };

	digit_two(1000);
	cover(true, 1300);
	pulse(4000);
	expect_shell_bytes("");
	/* The watched connection still drains shell output under either cover. */
	output_size = snprintf(output, sizeof(output),
			       "hidden\r\n\033]777;%s;B\a", shell_token);
	assert(output_size > 0 && (size_t)output_size < sizeof(output));
	assert(write(shell_peer, output, (size_t)output_size) == output_size);
	assert(ui.fd_callback && ui.watched_fd >= 0);
	ui.fd_callback(&ui, ui.watched_fd, POLLIN, ui.fd_context);
	expect_shell_bytes("");
	cover(false, 5000);
	canvas.draw(&draw);
	assert(!strncmp(first_line, "hidden", 6) && prompt_editing);
	pulse(5699);
	expect_shell_bytes("");
	pulse(5700);
	expect_shell_bytes("a");

	phone(KEY_NUMERIC_STAR, true, 6000);
	cover(true, 6100);
	pulse(7000);
	cover(false, 8000);
	phone(KEY_NUMERIC_STAR, false, 8001);
	digit_two(8100);
	pulse(8800);
	/* A star released elsewhere must not switch the next letter to uppercase. */
	expect_shell_bytes("a");
	phone(KEY_NUMERIC_POUND, true, 9000);
	cover(true, 9100);
	pulse(10000);
	cover(false, 11000);
	phone(KEY_NUMERIC_POUND, false, 11001);
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

	screen.key(&ui, &screen, &key, screen.key_context);
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
	pulse(5699);
	expect_shell_bytes("");
	pulse(5700);
	expect_shell_bytes("a");
}

static void keyboard_input(void)
{
	assert(screen.raw_keyboard);
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
	pulse(1699);
	expect_shell_bytes("");
	pulse(1700);
	expect_shell_bytes("a");
	keyboard(XKB_KEY_x, KEY_X, 'x');
	expect_shell_bytes("x");
}

static void hidden_releases_cancel_phone_gestures(bool cancelled)
{
	char prompt[80];
	int size = snprintf(prompt, sizeof(prompt), "\033]777;%s;B\a",
			    shell_token);

	assert(size > 0 && (size_t)size < sizeof(prompt));
	assert(write(shell_peer, prompt, (size_t)size) == size);
	ui.fd_callback(&ui, ui.watched_fd, POLLIN, ui.fd_context);
	if (cancelled)
		digit_two(900);
	phone(KEY_NUMERIC_STAR, true, 1000);
	/* The UI publishes hidden state, sends held releases, then VISIBILITY. */
	if (cancelled) {
		now_ms = 2000;
		event(FPLINUX_UI_EVENT_INPUT_CANCEL, false);
	} else {
		ui.visible = false;
	}
	phone(KEY_NUMERIC_STAR, false, 2000);
	ui.visible = false;
	event(FPLINUX_UI_EVENT_VISIBILITY, false);
	expect_shell_bytes("");
	ui.visible = true;
	now_ms = 3000;
	event(FPLINUX_UI_EVENT_VISIBILITY, true);
	if (cancelled) {
		pulse(3699);
		expect_shell_bytes("");
		pulse(3700);
		expect_shell_bytes("a");
	}
	digit_two(3800);
	expect_shell_bytes("");
	pulse(4500);
	expect_shell_bytes("a");
	phone(KEY_NUMERIC_POUND, true, 6000);
	if (cancelled) {
		now_ms = 7000;
		event(FPLINUX_UI_EVENT_INPUT_CANCEL, false);
	} else {
		ui.visible = false;
	}
	phone(KEY_NUMERIC_POUND, false, 7000);
	ui.visible = false;
	event(FPLINUX_UI_EVENT_VISIBILITY, false);
	/* The authenticated Bash prompt would otherwise accept the Clear action. */
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

	if (options || cancelled) {
		char prompt[80];
		int size = snprintf(prompt, sizeof(prompt), "\033]777;%s;B\a",
				    shell_token);
		struct lv_event draw = { .context = canvas.context };

		assert(size > 0 && (size_t)size < sizeof(prompt));
		assert(write(shell_peer, prompt, (size_t)size) == size);
		ui.fd_callback(&ui, ui.watched_fd, POLLIN, ui.fd_context);
		canvas.draw(&draw);
		assert(prompt_editing);
	}

	assert(setsockopt(ui.watched_fd, SOL_SOCKET, SO_SNDBUF, &send_buffer,
			  sizeof(send_buffer)) == 0);
	memset(bytes, '!', sizeof(bytes));
	while ((count = write(ui.watched_fd, bytes, sizeof(bytes))) > 0)
		filler_size += (size_t)count;
	assert(filler_size && count == -1 && errno == EAGAIN);
	/* The shell is not reading while ordinary keys and Enter keep arriving. */
	for (size_t index = 0; index < 8200; ++index)
		keyboard(XKB_KEY_x, KEY_X, 'x');
	if (options || cancelled) {
		phone(KEY_NUMERIC_POUND, true, 9000);
		phone(KEY_NUMERIC_POUND, false, 10000);
	}
	keyboard(XKB_KEY_y, KEY_Y, 'y');
	screen.command(&ui, &screen, FPLINUX_TERMINAL_COMMAND_ENTER,
		       screen.command_context);
	assert(!ui.quit && !pty_closed);
	/* Open Options before a timer callback can observe the covered screen. */
	if (options)
		ui.menu_open = true;
	if (cancelled)
		event(FPLINUX_UI_EVENT_INPUT_CANCEL, false);
	while (filler_size) {
		size_t capacity = filler_size < sizeof(bytes) ? filler_size :
								sizeof(bytes);

		count = read(shell_peer, bytes, capacity);
		assert(count > 0);
		for (ssize_t index = 0; index < count; ++index)
			assert(bytes[index] == '!');
		filler_size -= (size_t)count;
	}
	for (unsigned int attempt = 0; attempt < 8 && received < 8202;
	     ++attempt) {
		ui.fd_callback(&ui, ui.watched_fd, POLLOUT, ui.fd_context);
		while ((count = read(shell_peer, bytes, sizeof(bytes))) > 0)
			for (ssize_t index = 0; index < count; ++index) {
				char expected = 'x';

				if (received == 8200)
					expected = 'y';
				else if (received == 8201)
					expected = '\r';

				assert(received < 8202 &&
				       bytes[index] == expected);
				++received;
			}
		assert(count == -1 && errno == EAGAIN);
	}
	assert(received == 8202);
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

	assert(instance == &ui && ui.current == &screen);
	assert(pty_opened && timer.callback && screen.key && screen.command);
	canvas.draw(&draw);
	assert(painted_frames == 1);
	if (!strcmp(scenario, "keyboard"))
		keyboard_input();
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
		 !strcmp(scenario, "backpressure-options"))
		input_backpressure();
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
	(void)events;
	ui.watched_fd = fd;
	ui.fd_callback = callback;
	ui.fd_context = context;
}

int fplinux_ui_width(const struct fplinux_ui *instance)
{
	assert(instance == &ui);
	return 128;
}

int fplinux_ui_height(const struct fplinux_ui *instance)
{
	assert(instance == &ui);
	return 128;
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
	return "en";
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
	assert(instance == &ui && new_screen == &screen);
	instance->current = new_screen;
}

bool fplinux_ui_pop(struct fplinux_ui *instance)
{
	assert(instance == &ui);
	return false;
}

struct fplinux_ui_screen *fplinux_ui_canvas_new(struct fplinux_ui *instance,
						const char *title)
{
	assert(instance == &ui && title && *title);
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

void fplinux_ui_canvas_set_keyboard_keys(struct fplinux_ui_screen *instance,
					 bool raw)
{
	instance->raw_keyboard = raw;
}

void fplinux_ui_canvas_set_key_repeat(struct fplinux_ui_screen *instance,
				      bool enabled)
{
	assert(instance == &screen && enabled);
}

void fplinux_ui_screen_set_listener(struct fplinux_ui_screen *instance,
				    fplinux_ui_command_cb command,
				    fplinux_ui_change_cb change, void *context)
{
	(void)change;
	instance->command = command;
	instance->command_context = context;
}

bool fplinux_ui_screen_add_command(struct fplinux_ui_screen *instance,
				   const struct fplinux_ui_command *command)
{
	assert(instance == &screen && command);
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
	*width = 128;
	*height = 112;
	return true;
}

/* Picker, help and alert screens are outside these lifecycle scenarios. */
struct fplinux_ui_screen *fplinux_ui_list_new(struct fplinux_ui *instance,
					      const char *title,
					      enum fplinux_ui_list_kind kind)
{
	(void)instance;
	(void)title;
	(void)kind;
	assert(false);
	return NULL;
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
	(void)instance;
	(void)kind;
	(void)title;
	(void)text;
	(void)timeout_ms;
	assert(false);
	return NULL;
}

size_t fplinux_ui_list_append(struct fplinux_ui_screen *list, const char *text,
			      const char *detail)
{
	(void)list;
	(void)text;
	(void)detail;
	assert(false);
	return 0;
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
	(void)list;
	assert(false);
	return 0;
}

void fplinux_ui_list_set_focused(struct fplinux_ui_screen *list, size_t index)
{
	(void)list;
	(void)index;
	assert(false);
}

bool fplinux_ui_list_is_selected(const struct fplinux_ui_screen *list,
				 size_t index)
{
	(void)list;
	(void)index;
	assert(false);
	return false;
}

void fplinux_ui_list_set_selected(struct fplinux_ui_screen *list, size_t index,
				  bool selected)
{
	(void)list;
	(void)index;
	(void)selected;
	assert(false);
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

void lv_obj_set_scrollable(lv_obj_t *object, bool scrollable)
{
	assert(object == &canvas && !scrollable);
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
	assert(object == &canvas);
	*area = (lv_area_t){ .x1 = 0, .y1 = 16, .x2 = 127, .y2 = 127 };
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
	assert(font == &title_font);
	(void)letter_space;
	(void)line_space;
	(void)width;
	(void)flags;
	size->x = (int)strlen(text) * 6;
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

bool fplinux_terminal_grid_init(struct fplinux_terminal_grid *grid,
				const lv_font_t *font)
{
	assert(font == &body_font);
	*grid = (struct fplinux_terminal_grid){
		.font = font,
		.cell_width = 6,
		.cell_height = 12,
	};
	return true;
}

void fplinux_terminal_render(struct fplinux_terminal *terminal,
			     const struct fplinux_terminal_grid *grid,
			     lv_layer_t *layer, const lv_area_t *bounds)
{
	const struct tsm_screen_cell *cells =
		tsm_screen_draw2(terminal->screen);

	assert(grid->font == &body_font && layer);
	assert(bounds->x1 == 0 && bounds->y1 == 16);
	assert(bounds->x2 == 127 && bounds->y2 == 127);
	assert(cells && tsm_screen_get_width(terminal->screen) == 21);
	for (size_t index = 0; index < sizeof(first_line) - 1; ++index)
		first_line[index] =
			cells[index].ch < 128 ? (char)cells[index].ch : '?';
	first_line[sizeof(first_line) - 1] = '\0';
	prompt_editing = terminal->editing;
	++painted_frames;
}

bool fplinux_terminal_pty_open(struct fplinux_terminal_pty *pty,
			       unsigned int columns, unsigned int rows,
			       const char *shell, const char *startup,
			       const char *token)
{
	int sockets[2];

	assert(!pty_opened && columns == 21 && rows == 9);
	assert(!strcmp(shell, "/bin/bash") && startup && *startup);
	assert(token && strlen(token) == 32);
	memcpy(shell_token, token, sizeof(shell_token));
	assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
			  0, sockets) == 0);
	pty->fd = sockets[0];
	shell_peer = sockets[1];
	pty_opened = true;
	return true;
}

bool fplinux_terminal_pty_shell_foreground(
	const struct fplinux_terminal_pty *pty)
{
	assert(pty->fd >= 0 && !pty_closed);
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

/* Keep installed language files outside this callback/lifecycle boundary. */
size_t __wrap_fplinux_language_list(const char *directory,
				    char codes[][FPLINUX_LANGUAGE_CODE_BYTES],
				    size_t capacity)
{
	(void)directory;
	(void)codes;
	(void)capacity;
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
		assert(!ui.event && !pty_opened && !timer.callback);
		return result;
	}
	assert(argc == 2);
	result = fplinux_terminal_main(1, application_argv);
	assert(result == 0 && ui.closed && ui.quit);
	if (!strcmp(scenario, "close-during-open")) {
		assert(!pty_opened && !timer.callback && !screen.key);
	} else {
		char byte;

		assert(pty_closed && timer.deleted);
		assert(read(shell_peer, &byte, 1) == 0);
		assert(close(shell_peer) == 0);
	}
	printf("terminal-app %s: OK\n", scenario);
	return 0;
}
