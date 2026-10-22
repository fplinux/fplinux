/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "fplinux-ui-fonts.h"
#include "fplinux-ui-profile.h"
#include "screen-frame.h"
#include "ui-frame-peer.h"

#include <assert.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static void wait_frame(struct fplinux_ui_frame_peer *peer, int observer)
{
	unsigned int previous = peer->commits;
	char frame = 'F';

	assert(send(observer, &frame, 1, 0) == 1);
	fplinux_ui_frame_wait_readable(peer, observer);
	assert(recv(observer, &frame, 1, 0) == 1 && frame == 'F');
	assert(peer->commits > previous);
}

static void measure_status(const struct fplinux_ui_profile_spec *profile,
			   int width)
{
	const lv_font_t *font = fplinux_ui_builtin_font(profile->font_body);
	lv_point_t modifiers, compact, history, mode;

	assert(font);
	lv_text_get_size(&modifiers, "Ctrl+Alt+Shift", font, 0, 0, LV_COORD_MAX,
			 LV_TEXT_FLAG_NONE);
	lv_text_get_size(&compact, "C+A+S", font, 0, 0, LV_COORD_MAX,
			 LV_TEXT_FLAG_NONE);
	lv_text_get_size(&history, "↑512", font, 0, 0, LV_COORD_MAX,
			 LV_TEXT_FLAG_NONE);
	lv_text_get_size(&mode, "АБВ", font, 0, 0, LV_COORD_MAX,
			 LV_TEXT_FLAG_NONE);
	assert(modifiers.x + mode.x + 2 * profile->margin + profile->spacing <=
	       width);
	assert(compact.x + history.x + mode.x + 2 * profile->margin +
		       2 * profile->spacing <=
	       width);
	printf("terminal-status widths %d: full=%d compact=%d history=%d mode=%d margins=%d gaps=%d\n",
	       width, (int)modifiers.x, (int)compact.x, (int)history.x,
	       (int)mode.x, 2 * profile->margin, 2 * profile->spacing);
}

static void press(struct fplinux_ui_frame_peer *peer, int observer,
		  unsigned int code)
{
	fplinux_ui_frame_send_key(&peer->keyboards[1], code, true);
	fplinux_ui_frame_send_key(&peer->keyboards[1], code, false);
	wait_frame(peer, observer);
}

static void wait_shell(struct fplinux_ui_frame_peer *peer, int control,
		       char expected)
{
	char byte;

	fplinux_ui_frame_wait_readable(peer, control);
	assert(read(control, &byte, 1) == 1 && byte == expected);
}

static void read_shell_size(struct fplinux_ui_frame_peer *peer, int control,
			    const char *phase, unsigned int columns,
			    unsigned int rows)
{
	struct winsize size;
	unsigned char *bytes = (unsigned char *)&size;

	for (size_t index = 0; index < sizeof(size); ++index) {
		fplinux_ui_frame_wait_readable(peer, control);
		assert(read(control, bytes + index, 1) == 1);
	}
	assert(size.ws_col == columns && size.ws_row == rows);
	printf("terminal-pty %s %dx%d: %ux%u\n", phase, peer->width,
	       peer->height, size.ws_col, size.ws_row);
}

static void wait_legends(struct fplinux_ui_frame_peer *peer, int session,
			 const char *left, const char *centre,
			 const char *right)
{
	uint64_t deadline = fplinux_ui_frame_now_ms() + 3000;

	for (;;) {
		struct fplinux_ui_session_message message =
			fplinux_ui_frame_wait_message(peer, session);

		assert(fplinux_ui_frame_now_ms() < deadline);
		if (message.type == FPLINUX_UI_SESSION_SOFTKEYS &&
		    !strcmp(message.left, left) &&
		    !strcmp(message.centre, centre) &&
		    !strcmp(message.right, right))
			return;
		assert(message.type != FPLINUX_UI_SESSION_EXIT);
	}
}

static void save(struct fplinux_ui_frame_peer *peer, int observer,
		 const char *directory, const char *name)
{
	char path[512];

	wait_frame(peer, observer);
	assert(snprintf(path, sizeof(path), "%s/%s.rgb565", directory, name) <
	       (int)sizeof(path));
	assert(screen_frame_write(path, peer->pixels, peer->width, peer->height,
				  peer->width * sizeof(*peer->pixels)));
	printf("%s: %dx%d\n", name, peer->width, peer->height);
}

static void capture_symbols(struct fplinux_ui_frame_peer *peer, int session,
			    int observer, int control, const char *directory)
{
	press(peer, observer, KEY_NUMERIC_STAR);
	wait_legends(peer, session, "", "Insert", "Cancel");
	save(peer, observer, directory, "terminal-symbols-star");
	press(peer, observer, KEY_F14);
	wait_legends(peer, session, "Options", "Enter", "Clear");
	press(peer, observer, KEY_F13);
	wait_legends(peer, session, "", "Select", "Back");
	press(peer, observer, KEY_DOWN);
	press(peer, observer, KEY_OK);
	wait_legends(peer, session, "", "Insert", "Cancel");
	save(peer, observer, directory, "terminal-symbols-menu");
	press(peer, observer, KEY_OK);
	wait_shell(peer, control, '.');
	wait_legends(peer, session, "Options", "Enter", "Clear");
}

static void check_child_navigation(struct fplinux_ui_frame_peer *peer,
				   int session, int observer, int control)
{
	press(peer, observer, KEY_F13);
	wait_legends(peer, session, "", "Select", "Back");
	/* Modifiers, Symbols, then Special keys. */
	press(peer, observer, KEY_DOWN);
	press(peer, observer, KEY_DOWN);
	press(peer, observer, KEY_OK);
	wait_legends(peer, session, "", "Send", "Back");
	press(peer, observer, KEY_F14);
	wait_legends(peer, session, "", "Select", "Back");
	press(peer, observer, KEY_OK);
	wait_legends(peer, session, "", "Send", "Back");
	press(peer, observer, KEY_OK);
	wait_legends(peer, session, "Options", "Enter", "Clear");
	/* Back and reopening sent nothing; only Send writes the chosen Esc. */
	wait_shell(peer, control, '\033');
	press(peer, observer, KEY_F13);
	wait_legends(peer, session, "", "Select", "Back");
	for (unsigned int index = 0; index < 8; ++index)
		press(peer, observer, KEY_DOWN);
	press(peer, observer, KEY_OK);
	wait_legends(peer, session, "", "", "Back");
	press(peer, observer, KEY_F14);
	wait_legends(peer, session, "", "Select", "Back");
	press(peer, observer, KEY_OK);
	wait_legends(peer, session, "", "", "Back");
	press(peer, observer, KEY_F14);
	wait_legends(peer, session, "", "Select", "Back");
	press(peer, observer, KEY_F14);
	wait_legends(peer, session, "Options", "Enter", "Clear");
}

static int dimension(const char *text)
{
	char *end;
	long value = strtol(text, &end, 10);

	assert(*text && !*end && value > 0 && value <= 1024);
	return (int)value;
}

static void save_dimensions(const struct fplinux_ui_frame_peer *peer,
			    const char *directory)
{
	char path[512];
	FILE *file;

	assert(snprintf(path, sizeof(path), "%s/frame-size", directory) <
	       (int)sizeof(path));
	file = fopen(path, "w");
	assert(file);
	assert(fprintf(file, "%d %d\n", peer->width, peer->height) > 0);
	assert(!fclose(file));
}

static void finish(struct fplinux_ui_frame_peer *peer, int session,
		   pid_t application)
{
	struct fplinux_ui_session_message close = {
		.type = FPLINUX_UI_SESSION_CLOSE,
	};
	uint64_t deadline = fplinux_ui_frame_now_ms() + 3000;
	pid_t finished;
	int status;

	strcpy(close.reason, "capture-complete");
	assert(fplinux_ui_session_send(session, &close));
	do {
		fplinux_ui_frame_pump(peer);
		finished = waitpid(application, &status, WNOHANG);
		assert(finished >= 0);
	} while (!finished && fplinux_ui_frame_now_ms() < deadline);
	assert(finished == application);
	assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

int main(int argc, char **argv)
{
	struct fplinux_ui_frame_peer peer;
	struct fplinux_ui_session_message config = {
		.type = FPLINUX_UI_SESSION_CONFIG,
		.mode = FPLINUX_UI_SESSION_INTEGRATED,
		.theme = FPLINUX_UI_SESSION_THEME_DARK,
	};
	const struct fplinux_ui_profile_spec *profile;
	struct fplinux_ui_session_message hello;
	int wayland[2], control[2], observer[2];
	int listener, session;
	char descriptor[32];
	char state_directory[512];
	unsigned int columns, rows, other_columns, other_rows;
	pid_t application;

	if (argc != 11) {
		fprintf(stderr,
			"usage: %s WIDTH HEIGHT TERMINAL KEYMAP SESSION OUTPUT ISSUE HOSTNAME LANGUAGES THEME\n",
			argv[0]);
		return 2;
	}
	config.screen_width = dimension(argv[1]);
	config.screen_height = dimension(argv[2]);
	if (!strcmp(argv[10], "light"))
		config.theme = FPLINUX_UI_SESSION_THEME_LIGHT;
	else
		assert(!strcmp(argv[10], "dark"));
	profile = fplinux_ui_profile_spec(fplinux_ui_profile_for_screen(
		config.screen_width, config.screen_height));
	assert(profile);
	lv_init();
	measure_status(profile, config.screen_width);
	if (config.screen_width == 128) {
		columns = 21;
		rows = 9;
		other_columns = 16;
		other_rows = 6;
	} else if (config.screen_width == 240) {
		columns = 30;
		rows = 15;
		other_columns = 40;
		other_rows = 20;
	} else {
		assert(config.screen_width == 320);
		columns = 40;
		rows = 10;
		other_columns = 53;
		other_rows = 14;
	}
	listener = fplinux_ui_frame_listen_session(argv[5]);
	fplinux_ui_frame_socketpair(wayland);
	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, control) == 0);
	assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, observer) == 0);
	application = fork();
	assert(application >= 0);
	if (!application) {
		close(listener);
		close(wayland[0]);
		close(control[0]);
		close(observer[0]);
		fplinux_ui_frame_client_environment(wayland[1], argv[5]);
		snprintf(descriptor, sizeof(descriptor), "%d", control[1]);
		assert(setenv("FPLINUX_SCREEN_CONTROL_FD", descriptor, 1) == 0);
		snprintf(descriptor, sizeof(descriptor), "%d", observer[1]);
		assert(setenv("FPLINUX_TERMINAL_OBSERVER_FD", descriptor, 1) ==
		       0);
		assert(setenv("FPLINUX_SCREEN_ISSUE", argv[7], 1) == 0);
		assert(setenv("FPLINUX_SCREEN_HOSTNAME", argv[8], 1) == 0);
		assert(setenv("FPLINUX_SCREEN_LANGUAGES", argv[9], 1) == 0);
		assert(snprintf(state_directory, sizeof(state_directory),
				"%s/state",
				argv[6]) < (int)sizeof(state_directory));
		assert(setenv("XDG_STATE_HOME", state_directory, 1) == 0);
		execl(argv[3], argv[3], (char *)NULL);
		_exit(127);
	}
	close(wayland[1]);
	close(control[1]);
	close(observer[1]);
	fplinux_ui_frame_init(&peer, wayland[0], config.screen_width,
			      config.screen_height - profile->status_height -
				      profile->softkey_height,
			      argv[4]);
	save_dimensions(&peer, argv[6]);
	session = fplinux_ui_frame_accept_session(&peer, listener);
	hello = fplinux_ui_frame_wait_message(&peer, session);
	assert(hello.type == FPLINUX_UI_SESSION_HELLO);
	assert(!strcmp(hello.app_id, "org.fplinux.terminal"));
	strcpy(config.locale, "en");
	strcpy(config.input_locale, "en");
	assert(fplinux_ui_session_send(session, &config));
	read_shell_size(&peer, control[0], "default", columns, rows);
	wait_shell(&peer, control[0], 'R');
	wait_legends(&peer, session, "Options", "Enter", "Clear");
	assert(peer.keyboards[1].keyboard);
	fplinux_ui_frame_focus_keyboard(&peer.keyboards[1]);
	if (config.theme == FPLINUX_UI_SESSION_THEME_LIGHT) {
		press(&peer, observer[0], KEY_NUMERIC_STAR);
		wait_legends(&peer, session, "", "Insert", "Cancel");
		save(&peer, observer[0], argv[6], "terminal-symbols-light");
	} else {
		capture_symbols(&peer, session, observer[0], control[0],
				argv[6]);
		check_child_navigation(&peer, session, observer[0], control[0]);
		/* Production multi-tap sends "ec" through the PTY; "h" remains preedit. */
		press(&peer, observer[0], KEY_NUMERIC_3);
		press(&peer, observer[0], KEY_NUMERIC_3);
		press(&peer, observer[0], KEY_NUMERIC_2);
		wait_shell(&peer, control[0], 'e');
		press(&peer, observer[0], KEY_NUMERIC_2);
		press(&peer, observer[0], KEY_NUMERIC_2);
		press(&peer, observer[0], KEY_NUMERIC_4);
		wait_shell(&peer, control[0], 'c');
		press(&peer, observer[0], KEY_NUMERIC_4);
		save(&peer, observer[0], argv[6], "terminal-keypad");
		{
			struct pollfd input = { .fd = control[0],
						.events = POLLIN };

			/* The displayed h is still composition, not another PTY byte. */
			assert(poll(&input, 1, 0) == 0);
		}
		press(&peer, observer[0], KEY_F13);
		wait_legends(&peer, session, "", "Select", "Back");
		save(&peer, observer[0], argv[6], "terminal-menu");
		press(&peer, observer[0], KEY_OK);
		wait_legends(&peer, session, "Done", "Mark", "Back");
		press(&peer, observer[0], KEY_OK);
		wait_legends(&peer, session, "Done", "Unmark", "Back");
		save(&peer, observer[0], argv[6], "terminal-modifiers");
		/* Keep the pending h and discard the unconfirmed modifier selection. */
		press(&peer, observer[0], KEY_F14);
		wait_legends(&peer, session, "", "Select", "Back");
		/* Back restores the command; OK opens that same child again. */
		press(&peer, observer[0], KEY_OK);
		wait_legends(&peer, session, "Done", "Mark", "Back");
		press(&peer, observer[0], KEY_F14);
		wait_legends(&peer, session, "", "Select", "Back");
		for (unsigned int index = 0; index < 7; ++index)
			press(&peer, observer[0], KEY_DOWN);
		press(&peer, observer[0], KEY_OK);
		wait_legends(&peer, session, "", "Select", "Back");
		press(&peer, observer[0],
		      config.screen_width == 128 ? KEY_DOWN : KEY_UP);
		press(&peer, observer[0], KEY_F14);
		wait_legends(&peer, session, "", "Select", "Back");
		press(&peer, observer[0], KEY_OK);
		wait_legends(&peer, session, "", "Select", "Back");
		press(&peer, observer[0],
		      config.screen_width == 128 ? KEY_DOWN : KEY_UP);
		press(&peer, observer[0], KEY_OK);
		wait_legends(&peer, session, "Options", "Enter", "Clear");
		read_shell_size(&peer, control[0], "other", other_columns,
				other_rows);
		save(&peer, observer[0], argv[6], "terminal-font-other");
		press(&peer, observer[0], KEY_F14);
		press(&peer, observer[0], KEY_NUMERIC_STAR);
		wait_legends(&peer, session, "", "Insert", "Cancel");
		save(&peer, observer[0], argv[6],
		     "terminal-symbols-font-other");
		press(&peer, observer[0], KEY_NUMERIC_STAR);
		wait_legends(&peer, session, "Options", "Enter", "Clear");
		/* en lower -> en upper -> ru lower -> ru upper. */
		for (unsigned int index = 0; index < 3; ++index)
			press(&peer, observer[0], KEY_NUMERIC_POUND);
		assert(write(control[0], "H", 1) == 1);
		wait_shell(&peer, control[0], 'H');
		press(&peer, observer[0], KEY_F13);
		wait_legends(&peer, session, "", "Select", "Back");
		press(&peer, observer[0], KEY_OK);
		wait_legends(&peer, session, "Done", "Mark", "Back");
		for (unsigned int index = 0; index < 3; ++index) {
			press(&peer, observer[0], KEY_OK);
			if (index != 2)
				press(&peer, observer[0], KEY_DOWN);
		}
		press(&peer, observer[0], KEY_F13);
		wait_legends(&peer, session, "Options", "Enter", "Clear");
		press(&peer, observer[0], KEY_F13);
		wait_legends(&peer, session, "", "Select", "Back");
		for (unsigned int index = 0; index < 3; ++index)
			press(&peer, observer[0], KEY_DOWN);
		press(&peer, observer[0], KEY_OK);
		wait_legends(&peer, session, "Options", "Enter", "Back");
		for (unsigned int index = 0; index < 96; ++index)
			press(&peer, observer[0], KEY_LEFT);
		save(&peer, observer[0], argv[6], "terminal-status");
	}
	finish(&peer, session, application);
	close(control[0]);
	close(observer[0]);
	close(session);
	close(listener);
	fplinux_ui_frame_cleanup(&peer);
	lv_deinit();
	assert(unlink(argv[5]) == 0);
	if (config.theme == FPLINUX_UI_SESSION_THEME_DARK)
		puts("input=ec; Options; Modifiers; Font size; history with all modifiers; close=ok");
	return 0;
}
