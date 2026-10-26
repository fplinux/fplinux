/* SPDX-License-Identifier: 0BSD */
#define _GNU_SOURCE
#include "observer.h"
#include "fplinux-ui-profile.h"
#include "screen-frame.h"
#include "ui-frame-peer.h"

#include <assert.h>
#include <errno.h>
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

struct capture {
	struct fplinux_ui_frame_peer peer;
	struct notes_observation observed;
	pid_t application;
	int observer;
	int session;
	int listener;
	unsigned int exits;
	bool russian;
	char left[128];
	const char *directory;
	const char *session_path;
};

static void pump(struct capture *capture)
{
	struct notes_observation observation;
	struct fplinux_ui_session_message message;
	ssize_t bytes;
	int received;

	fplinux_ui_frame_pump(&capture->peer);
	while ((bytes = recv(capture->observer, &observation,
			     sizeof(observation), MSG_DONTWAIT)) > 0) {
		assert(bytes == (ssize_t)sizeof(observation));
		capture->observed = observation;
	}
	assert(bytes == -1 && (errno == EAGAIN || errno == EWOULDBLOCK));
	while ((received = fplinux_ui_session_receive(capture->session,
						      &message)) == 1) {
		if (message.type == FPLINUX_UI_SESSION_EXIT)
			++capture->exits;
		else if (message.type == FPLINUX_UI_SESSION_SOFTKEYS)
			strcpy(capture->left, message.left);
	}
	assert(received == 0);
}

static void pump_for(struct capture *capture, unsigned int milliseconds)
{
	uint64_t deadline = fplinux_ui_frame_now_ms() + milliseconds;

	while (fplinux_ui_frame_now_ms() < deadline)
		pump(capture);
}

static const char *word(const struct capture *capture, const char *english,
			const char *russian)
{
	return capture->russian ? russian : english;
}

static bool has_label(const struct capture *capture, const char *text)
{
	char line[256];

	assert(snprintf(line, sizeof(line), "\n%s\n", text) <
	       (int)sizeof(line));
	return strstr(capture->observed.labels, line) != NULL;
}

static void wait_view(struct capture *capture, const char *text, bool dirty,
		      bool menu)
{
	const char *title = dirty ? word(capture, "Notes (unsaved)",
					 "Заметки (не сохранено)") :
				    word(capture, "Notes", "Заметки");
	uint64_t deadline = fplinux_ui_frame_now_ms() + 3000;

	do {
		pump(capture);
		if (capture->observed.frame && capture->observed.editor &&
		    capture->observed.menu == menu &&
		    !strcmp(capture->observed.text, text) &&
		    has_label(capture, title))
			break;
	} while (fplinux_ui_frame_now_ms() < deadline);
	if (strcmp(capture->observed.text, text) || !has_label(capture, title))
		fprintf(stderr,
			"Notes expected [%s] [%s], actual [%s], labels:%s\n",
			title, text, capture->observed.text,
			capture->observed.labels);
	assert(capture->observed.frame && capture->observed.editor);
	assert(capture->observed.menu == menu && !capture->exits);
	assert(!strcmp(capture->observed.text, text) &&
	       has_label(capture, title));
	assert(capture->observed.editors == 1);
}

static void press(struct capture *capture, unsigned int code)
{
	fplinux_ui_frame_send_key(&capture->peer.keyboards[0], code, true);
	fplinux_ui_frame_send_key(&capture->peer.keyboards[0], code, false);
	pump(capture);
}

static void save_frame(struct capture *capture, const char *name)
{
	char path[1024];

	pump_for(capture, 40);
	assert(snprintf(path, sizeof(path), "%s/%s.rgb565", capture->directory,
			name) < (int)sizeof(path));
	assert(screen_frame_write(
		path, capture->peer.pixels, (unsigned int)capture->peer.width,
		(unsigned int)capture->peer.height,
		(size_t)capture->peer.width * sizeof(uint16_t)));
}

static void check_file(const struct capture *capture, const char *expected)
{
	char path[1024], contents[256] = { 0 };
	FILE *file;

	assert(snprintf(path, sizeof(path),
			"%s/state/fplinux/org.fplinux.notes/preferences",
			capture->directory) < (int)sizeof(path));
	file = fopen(path, "r");
	if (!expected) {
		assert(!file && errno == ENOENT);
		return;
	}
	assert(file);
	assert(fread(contents, 1, sizeof(contents) - 1, file) ==
	       strlen(expected));
	assert(!strcmp(contents, expected));
	assert(fclose(file) == 0);
}

static void start(struct capture *capture, char **arguments, const char *text)
{
	struct fplinux_ui_session_message config = {
		.type = FPLINUX_UI_SESSION_CONFIG,
		.mode = FPLINUX_UI_SESSION_INTEGRATED,
		.screen_width = atoi(arguments[5]),
		.screen_height = atoi(arguments[6]),
		.theme = !strcmp(arguments[7], "dark") ?
				 FPLINUX_UI_SESSION_THEME_DARK :
				 FPLINUX_UI_SESSION_THEME_LIGHT,
	};
	const struct fplinux_ui_profile_spec *profile = fplinux_ui_profile_spec(
		fplinux_ui_profile_for_screen(config.screen_width,
					      config.screen_height));
	struct fplinux_ui_session_message hello;
	int wayland[2], observer[2];

	assert(profile);
	capture->directory = arguments[4];
	capture->session_path = arguments[3];
	capture->russian = !strcmp(arguments[8], "ru");
	capture->listener = fplinux_ui_frame_listen_session(arguments[3]);
	fplinux_ui_frame_socketpair(wayland);
	assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, observer) == 0);
	capture->application = fork();
	assert(capture->application >= 0);
	if (!capture->application) {
		char descriptor[32], path[1024];
		const char *slash = strrchr(arguments[1], '/');

		assert(slash);
		close(capture->listener);
		close(wayland[0]);
		close(observer[0]);
		fplinux_ui_frame_client_environment(wayland[1], arguments[3]);
		snprintf(descriptor, sizeof(descriptor), "%d", observer[1]);
		assert(setenv("FPLINUX_NOTES_OBSERVER_FD", descriptor, 1) == 0);
		assert(snprintf(path, sizeof(path), "%.*s/locale",
				(int)(slash - arguments[1]),
				arguments[1]) < (int)sizeof(path));
		assert(setenv("FPLINUX_NOTES_LOCALE_DIR", path, 1) == 0);
		assert(snprintf(path, sizeof(path), "%s/state", arguments[4]) <
		       (int)sizeof(path));
		assert(setenv("XDG_STATE_HOME", path, 1) == 0);
		assert(setenv("LANG", "en_US.UTF-8", 1) == 0);
		assert(setenv("LC_ALL", "en_US.UTF-8", 1) == 0);
		assert(unsetenv("LANGUAGE") == 0);
		execl(arguments[1], arguments[1], (char *)NULL);
		_exit(127);
	}
	close(wayland[1]);
	close(observer[1]);
	capture->observer = observer[0];
	fplinux_ui_frame_init(&capture->peer, wayland[0], config.screen_width,
			      config.screen_height - profile->status_height -
				      profile->softkey_height,
			      arguments[2]);
	capture->session = fplinux_ui_frame_accept_session(&capture->peer,
							   capture->listener);
	hello = fplinux_ui_frame_wait_message(&capture->peer, capture->session);
	assert(hello.type == FPLINUX_UI_SESSION_HELLO);
	assert(!strcmp(hello.app_id, "org.fplinux.notes"));
	strcpy(config.locale, arguments[8]);
	strcpy(config.input_locale, "en");
	assert(fplinux_ui_session_send(capture->session, &config));
	wait_view(capture, text, false, false);
	assert(capture->peer.keyboards[0].keyboard);
	fplinux_ui_frame_focus_keyboard(&capture->peer.keyboards[0]);
	pump_for(capture, 30);
}

static void close_and_reap(struct capture *capture)
{
	struct fplinux_ui_session_message message = {
		.type = FPLINUX_UI_SESSION_CLOSE
	};
	uint64_t deadline = fplinux_ui_frame_now_ms() + 3000;
	pid_t finished;
	int status;

	strcpy(message.reason, "capture-complete");
	assert(fplinux_ui_session_send(capture->session, &message));
	do {
		fplinux_ui_frame_pump(&capture->peer);
		finished = waitpid(capture->application, &status, WNOHANG);
		assert(finished >= 0);
	} while (!finished && fplinux_ui_frame_now_ms() < deadline);
	assert(finished == capture->application);
	assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	close(capture->observer);
	close(capture->session);
	close(capture->listener);
	fplinux_ui_frame_cleanup(&capture->peer);
	assert(unlink(capture->session_path) == 0);
}

int main(int argc, char **argv)
{
	struct fplinux_ui_session_message visibility = {
		.type = FPLINUX_UI_SESSION_VISIBILITY,
	};
	struct capture capture = { 0 };
	uint64_t deadline;

	assert(argc == 9);
	start(&capture, argv, "");
	assert(!capture.left[0]);
	check_file(&capture, NULL);
	save_frame(&capture, "notes-clean");
	press(&capture, KEY_A);
	press(&capture, KEY_B);
	press(&capture, KEY_C);
	wait_view(&capture, "abc", true, false);
	assert(capture.observed.cursor == 3);
	save_frame(&capture, "notes-dirty");
	press(&capture, KEY_LEFT);
	deadline = fplinux_ui_frame_now_ms() + 3000;
	while (capture.observed.cursor != 2 &&
	       fplinux_ui_frame_now_ms() < deadline)
		pump(&capture);
	assert(capture.observed.cursor == 2);
	press(&capture, KEY_COMPOSE);
	wait_view(&capture, "abc", true, true);
	assert(has_label(&capture, word(&capture, "Save", "Сохранить")));
	save_frame(&capture, "notes-menu");
	press(&capture, KEY_ENTER);
	wait_view(&capture, "abc", false, false);
	assert(capture.observed.cursor == 2);
	check_file(&capture, "note=abc\n");
	save_frame(&capture, "notes-saved");
	press(&capture, KEY_COMPOSE);
	wait_view(&capture, "abc", false, true);
	assert(!has_label(&capture, word(&capture, "Save", "Сохранить")));
	press(&capture, KEY_ESC);
	wait_view(&capture, "abc", false, false);
	press(&capture, KEY_D);
	wait_view(&capture, "abdc", true, false);
	assert(capture.observed.cursor == 3);
	assert(fplinux_ui_session_send(capture.session, &visibility));
	pump_for(&capture, 100);
	check_file(&capture, "note=abdc\n");
	visibility.visible = true;
	assert(fplinux_ui_session_send(capture.session, &visibility));
	wait_view(&capture, "abdc", false, false);
	assert(capture.observed.cursor == 3);
	save_frame(&capture, "notes-returned");
	close_and_reap(&capture);
	capture = (struct capture){ 0 };
	start(&capture, argv, "abdc");
	press(&capture, KEY_COMPOSE);
	wait_view(&capture, "abdc", false, true);
	assert(!has_label(&capture, word(&capture, "Save", "Сохранить")));
	press(&capture, KEY_ESC);
	wait_view(&capture, "abdc", false, false);
	save_frame(&capture, "notes-reopened");
	close_and_reap(&capture);
	puts("notes-screens: clean has no Save; real typing and menu Save retain editor/cursor; Home saves; reopen reads saved text");
	return 0;
}
