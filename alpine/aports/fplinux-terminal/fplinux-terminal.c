/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "fplinux-cli.h"
#include "fplinux-drm-session.h"
#include "fplinux-input-session.h"
#include "fplinux-keyboard-text.h"
#include "fplinux-terminal.h"
#include "terminal-diagnostic.h"
#include "terminal-keyboard.h"
#include "terminal-pty.h"
#include "terminal-render.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/random.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define FPLINUX_TERMINAL_REPEAT_DELAY_MS 400U
#define FPLINUX_TERMINAL_REPEAT_MS 50U
#define FPLINUX_TERMINAL_READ_BUDGET_BYTES 65536U
#define FPLINUX_TERMINAL_INPUT_REDRAW_MS 20U

struct terminal_app {
	struct fplinux_terminal terminal;
	struct fplinux_font font;
	struct fplinux_terminal_pty pty;
	struct fplinux_terminal_keyboard keyboard;
	struct fplinux_input_session input;
	struct fplinux_terminal_diagnostic diagnostic;
	struct fplinux_drm_session display;
	bool display_open;
	bool input_open;
	bool input_enabled;
	bool keyboard_open;
	uint64_t repeat_device;
	unsigned int repeat_code;
	enum fplinux_input_source repeat_source;
	uint64_t repeat_ms;
	uint64_t redraw_ms;
};

static volatile sig_atomic_t stop_signal;

static void request_stop(int number)
{
	stop_signal = number;
}

static uint64_t monotonic_ms(void)
{
	struct timespec time;

	clock_gettime(CLOCK_MONOTONIC, &time);
	return (uint64_t)time.tv_sec * 1000U +
	       (uint64_t)time.tv_nsec / 1000000U;
}

static bool generate_token(char token[33])
{
	unsigned char random[16];
	size_t used = 0;
	size_t index;

	while (used < sizeof(random)) {
		ssize_t size =
			getrandom(random + used, sizeof(random) - used, 0);

		if (size < 0) {
			if (errno == EINTR)
				continue;
			return false;
		}
		used += (size_t)size;
	}
	for (index = 0; index < sizeof(random); ++index)
		snprintf(token + index * 2, 3, "%02x", random[index]);
	return true;
}

static bool change_focus(struct fplinux_drm_session *display, bool active,
			 void *data)
{
	struct terminal_app *app = data;
	struct fplinux_input_event event;
	struct fplinux_terminal_key key;
	char error[256];

	(void)display;
	app->repeat_device = 0;
	app->redraw_ms = 0;
	fplinux_terminal_focus(&app->terminal, active, monotonic_ms());
	if (!active && app->input_enabled) {
		fplinux_input_session_suspend(&app->input);
		while (fplinux_input_session_next(&app->input, &event))
			fplinux_terminal_keyboard_event(&app->keyboard, &event,
							&key);
		app->input_enabled = false;
	} else if (active && !app->input_enabled) {
		fplinux_terminal_diagnostic_close(&app->diagnostic);
		if (!fplinux_input_session_resume(&app->input, error,
						  sizeof(error))) {
			fprintf(stderr, "fplinux-terminal: %s\n", error);
			errno = EIO;
			return false;
		}
		app->input_enabled = true;
	}
	if (!fplinux_terminal_keyboard_reset(&app->keyboard)) {
		errno = ENOMEM;
		return false;
	}
	return true;
}

static bool phone_repeats(unsigned int code)
{
	return code == KEY_UP || code == KEY_DOWN || code == KEY_LEFT ||
	       code == KEY_RIGHT;
}

static void keyboard_key(struct terminal_app *app,
			 const struct fplinux_terminal_key *key)
{
	fplinux_terminal_key(&app->terminal, key->keysym, key->ascii,
			     key->modifiers, key->unicode);
}

static void input_event(struct terminal_app *app,
			const struct fplinux_input_event *event,
			uint64_t now_ms)
{
	struct fplinux_terminal_key key;
	bool repeats = false;

	if (event->type == FPLINUX_INPUT_EVENT_DEVICE_REMOVED ||
	    event->type == FPLINUX_INPUT_EVENT_DEVICE_ADDED) {
		fplinux_terminal_keyboard_event(&app->keyboard, event, &key);
		fplinux_terminal_reset_input(&app->terminal);
		if (app->repeat_device == event->device_id)
			app->repeat_device = 0;
		return;
	}
	if (event->type != FPLINUX_INPUT_EVENT_KEY)
		return;
	if (!event->pressed && app->repeat_device == event->device_id &&
	    app->repeat_code == event->code)
		app->repeat_device = 0;
	if (event->source == FPLINUX_INPUT_SOURCE_KEYPAD) {
		fplinux_terminal_phone(
			&app->terminal, event->code, event->pressed, false,
			event->time_ms,
			fplinux_terminal_pty_shell_foreground(&app->pty));
		repeats = event->pressed && phone_repeats(event->code);
	} else if (fplinux_terminal_keyboard_event(&app->keyboard, event,
						   &key)) {
		keyboard_key(app, &key);
		repeats = key.repeats;
	}
	if (repeats) {
		app->repeat_device = event->device_id;
		app->repeat_code = event->code;
		app->repeat_source = event->source;
		app->repeat_ms = now_ms + FPLINUX_TERMINAL_REPEAT_DELAY_MS;
	}
}

static void repeat_key(struct terminal_app *app, uint64_t now_ms)
{
	struct fplinux_terminal_key key;

	if (!app->repeat_device || now_ms < app->repeat_ms)
		return;
	if (app->repeat_source == FPLINUX_INPUT_SOURCE_KEYPAD) {
		fplinux_terminal_phone(
			&app->terminal, app->repeat_code, true, true, now_ms,
			fplinux_terminal_pty_shell_foreground(&app->pty));
	} else if (fplinux_terminal_keyboard_repeat(&app->keyboard,
						    app->repeat_device,
						    app->repeat_code, &key)) {
		keyboard_key(app, &key);
	} else {
		app->repeat_device = 0;
	}
	app->repeat_ms = now_ms + FPLINUX_TERMINAL_REPEAT_MS;
}

static bool flush_input(struct terminal_app *app)
{
	while (app->terminal.output_size) {
		ssize_t size = write(app->pty.fd, app->terminal.output,
				     app->terminal.output_size);

		if (size > 0) {
			/* Keep the displayed preedit until the PTY can echo it.
			 * A bounded wait also serves programs with echo disabled.
			 */
			if (!app->redraw_ms)
				app->redraw_ms =
					monotonic_ms() +
					FPLINUX_TERMINAL_INPUT_REDRAW_MS;
			fplinux_terminal_consume(&app->terminal, (size_t)size);
			continue;
		}
		if (size < 0 && errno == EINTR)
			continue;
		return size < 0 && errno == EAGAIN;
	}
	return true;
}

static bool read_output(struct terminal_app *app)
{
	char bytes[4096];
	size_t total = 0;

	while (total < FPLINUX_TERMINAL_READ_BUDGET_BYTES &&
	       app->terminal.output_size < FPLINUX_TERMINAL_OUTPUT_BYTES / 2) {
		ssize_t size = read(app->pty.fd, bytes, sizeof(bytes));

		if (size > 0) {
			fplinux_terminal_feed(&app->terminal, bytes,
					      (size_t)size);
			total += (size_t)size;
			continue;
		}
		if (size < 0 && errno == EINTR)
			continue;
		/* Linux returns EIO after the PTY slave has closed. */
		return size < 0 && errno == EAGAIN;
	}
	return true;
}

static bool present(struct terminal_app *app)
{
	unsigned int page = app->display.shown_page == 0 ? 1 : 0;
	struct fplinux_terminal_surface surface = {
		.pixels = (uint16_t *)(app->display.mapping +
				       page * app->display.page_bytes),
		.width = app->display.width,
		.height = app->display.height,
		.stride_bytes = (unsigned int)app->display.stride,
	};

	fplinux_terminal_render(&app->terminal, &app->font, &surface);
	return fplinux_drm_session_present(&app->display, page);
}

static int run_terminal(struct terminal_app *app)
{
	int child_status = 0;

	while (!stop_signal) {
		uint64_t now_ms = monotonic_ms();
		int timeout = fplinux_terminal_timeout(&app->terminal, now_ms);
		bool can_input = app->terminal.output_size <
				 FPLINUX_TERMINAL_OUTPUT_BYTES / 2;
		struct pollfd
			descriptors[3 + FPLINUX_INPUT_SESSION_DEVICE_COUNT] = {
				{ .fd = app->pty.fd,
				  .events = can_input ? POLLIN : 0 },
				{ .fd = fplinux_drm_session_fd(&app->display),
				  .events = POLLIN },
				{ .fd = app->input_enabled && can_input ?
						fplinux_input_session_get_fd(
							&app->input) :
						-1,
				  .events = POLLIN },
			};
		nfds_t descriptor_count = 3;
		struct fplinux_input_event event;

		if (!app->input_enabled) {
			for (size_t index = 0; index < app->diagnostic.count;
			     ++index)
				descriptors[descriptor_count++] =
					(struct pollfd){
						.fd = app->diagnostic.fds[index],
						.events = POLLIN,
					};
		}
		if (app->terminal.failed) {
			fprintf(stderr,
				"fplinux-terminal: PTY input queue exhausted\n");
			return 1;
		}
		if (app->terminal.output_size)
			descriptors[0].events |= POLLOUT;
		if (app->repeat_device) {
			int repeat_timeout =
				now_ms >= app->repeat_ms ?
					0 :
					(int)(app->repeat_ms - now_ms);

			if (timeout < 0 || repeat_timeout < timeout)
				timeout = repeat_timeout;
		}
		if (app->terminal.active && app->terminal.dirty) {
			int redraw_timeout =
				now_ms >= app->redraw_ms ?
					0 :
					(int)(app->redraw_ms - now_ms);

			if (timeout < 0 || redraw_timeout < timeout)
				timeout = redraw_timeout;
		}
		if (poll(descriptors, descriptor_count, timeout) < 0) {
			if (errno == EINTR)
				continue;
			return 1;
		}
		if (descriptors[1].revents &&
		    !fplinux_drm_session_dispatch(&app->display))
			return 1;
		/* Dispatch may have closed the watched fds while reacquiring this VT. */
		if (!app->input_enabled &&
		    !fplinux_terminal_diagnostic_dispatch(&app->diagnostic,
							  app->display.control,
							  app->display.vt))
			return 1;
		if (descriptors[0].revents & POLLIN)
			if (!read_output(app))
				break;
		if ((descriptors[0].revents & POLLOUT) && !flush_input(app))
			break;
		now_ms = monotonic_ms();
		if (app->input_enabled && can_input) {
			while (app->terminal.output_size <
				       FPLINUX_TERMINAL_OUTPUT_BYTES / 2 &&
			       fplinux_input_session_next(&app->input, &event))
				input_event(app, &event, now_ms);
			/* Dispatch can produce events newer than the pre-drain sample. */
			now_ms = monotonic_ms();
			repeat_key(app, now_ms);
			fplinux_terminal_tick(
				&app->terminal, now_ms,
				fplinux_terminal_pty_shell_foreground(
					&app->pty));
		}
		if (app->terminal.diagnostic_requested) {
			app->terminal.diagnostic_requested = false;
			if (!fplinux_terminal_diagnostic_begin(
				    &app->diagnostic, &app->input,
				    app->display.control))
				return 1;
		}
		if (!flush_input(app) ||
		    fplinux_terminal_pty_finished(&app->pty, &child_status))
			break;
		if (app->terminal.active && app->terminal.dirty &&
		    monotonic_ms() >= app->redraw_ms) {
			if (!present(app))
				return 1;
			app->redraw_ms = 0;
		}
		if (descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL))
			break;
	}
	if (stop_signal)
		return 128 + stop_signal;
	return WIFEXITED(child_status) ? WEXITSTATUS(child_status) : 1;
}

static void close_app(struct terminal_app *app)
{
	fplinux_terminal_pty_close(&app->pty);
	fplinux_terminal_diagnostic_close(&app->diagnostic);
	if (app->input_open)
		fplinux_input_session_close(&app->input);
	if (app->keyboard_open)
		fplinux_terminal_keyboard_close(&app->keyboard);
	if (app->display_open) {
		fplinux_drm_session_set_active_handler(&app->display, NULL,
						       NULL);
		fplinux_drm_session_close(&app->display);
	}
	fplinux_font_close(&app->font);
	fplinux_terminal_destroy(&app->terminal);
}

int main(int argc, char **argv)
{
	struct fplinux_cli_option options[] = {
		{ .name = "drm",
		  .metavar = "PATH",
		  .help = "DRM device (default /dev/dri/card0)" },
		{ .name = "tty",
		  .metavar = "PATH",
		  .help = "VT control device (default /dev/tty0)" },
		{ .name = "font",
		  .metavar = "PATH",
		  .help = "PSF2 Unicode font" },
	};
	struct fplinux_cli cli = {
		.program = "fplinux-terminal",
		.description =
			"Interactive terminal for the phone display and keyboards",
		.options = options,
		.option_count = sizeof(options) / sizeof(options[0]),
	};
	struct terminal_app app = { .pty = { .fd = -1, .shell = -1 } };
	struct sigaction action = { .sa_handler = request_stop };
	char token[33];
	char error[256] = "cannot initialize terminal";
	const char *font;
	unsigned int columns;
	unsigned int rows;
	int result = fplinux_cli_parse(&cli, argc, argv);

	if (result != FPLINUX_CLI_READY)
		return result;
	result = 1;
	sigaction(SIGTERM, &action, NULL);
	sigaction(SIGINT, &action, NULL);
	if (!generate_token(token))
		goto fail;
	if (!fplinux_drm_session_open(
		    &app.display,
		    options[0].value ? options[0].value : "/dev/dri/card0",
		    options[1].value, DRM_FORMAT_RGB565, error, sizeof(error)))
		goto fail;
	app.display_open = true;
	font = options[2].value;
	if (font && !fplinux_font_open(&app.font, font)) {
		snprintf(error, sizeof(error), "cannot load font %s", font);
		goto fail;
	}
	if (!font && !fplinux_font_open_default(&app.font, app.display.width,
						error, sizeof(error)))
		goto fail;
	columns = app.display.width / app.font.width;
	rows = app.display.height / app.font.height;
	if (columns < 16 || rows < 4) {
		snprintf(error, sizeof(error),
			 "font leaves too few terminal cells");
		goto fail;
	}
	--rows;
	if (!fplinux_terminal_init(&app.terminal, columns, rows, token))
		goto fail;
	if (!fplinux_terminal_keyboard_open(&app.keyboard,
					    FPLINUX_KEYBOARD_TEXT_DATA_ROOT,
					    error, sizeof(error)))
		goto fail;
	app.keyboard_open = true;
	if (!fplinux_input_session_open(
		    &app.input,
		    FPLINUX_INPUT_SOURCE_MASK(FPLINUX_INPUT_SOURCE_KEYPAD) |
			    FPLINUX_INPUT_SOURCE_MASK(
				    FPLINUX_INPUT_SOURCE_KEYBOARD),
		    error, sizeof(error)))
		goto fail;
	app.input_open = true;
	app.input_enabled = true;
	if (!fplinux_drm_session_set_active_handler(&app.display, change_focus,
						    &app))
		goto fail;
	if (!fplinux_terminal_pty_open(&app.pty, columns, rows, "/bin/bash",
				       "/etc/fplinux/terminal.bashrc", token)) {
		snprintf(error, sizeof(error), "cannot start Bash PTY: %s",
			 strerror(errno));
		goto fail;
	}
	result = run_terminal(&app);
	if (result == 1)
		fprintf(stderr, "fplinux-terminal: session failed: %s\n",
			strerror(errno));
	close_app(&app);
	return result;
fail:
	fprintf(stderr, "fplinux-terminal: %s\n", error);
	close_app(&app);
	return result;
}
