/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "observer.h"
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
	struct snake_observation observed;
	pid_t application;
	int observer;
	int session;
	int listener;
	unsigned int exits;
	unsigned int homes;
	bool kept_on;
	bool russian;
	char centre[128];
	char right[128];
	const char *directory;
	const char *session_path;
};

static void pump(struct capture *capture)
{
	struct snake_observation observation;
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
		if (message.type == FPLINUX_UI_SESSION_KEEP_SCREEN_ON)
			capture->kept_on = message.enabled;
		else if (message.type == FPLINUX_UI_SESSION_EXIT)
			++capture->exits;
		else if (message.type == FPLINUX_UI_SESSION_HOME)
			++capture->homes;
		else if (message.type == FPLINUX_UI_SESSION_SOFTKEYS) {
			assert(!message.left[0]);
			strcpy(capture->centre, message.centre);
			strcpy(capture->right, message.right);
		}
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

static void wait_state(struct capture *capture, bool canvas, const char *action,
		       const char *title)
{
	uint64_t deadline = fplinux_ui_frame_now_ms() + 3000;
	const char *back = word(capture, "Back", "Назад");

	do {
		pump(capture);
		if (capture->observed.frame &&
		    capture->observed.canvas == canvas &&
		    capture->observed.paused != canvas &&
		    capture->kept_on == canvas &&
		    !strcmp(capture->centre, action) &&
		    !strcmp(capture->right, back) &&
		    (!title || has_label(capture, title)))
			break;
	} while (fplinux_ui_frame_now_ms() < deadline);
	if (capture->observed.canvas != canvas ||
	    strcmp(capture->centre, action) ||
	    (title && !has_label(capture, title)))
		fprintf(stderr, "Snake expected %s [%s], got [%s], labels:%s\n",
			canvas ? "Canvas" : "Alert", action, capture->centre,
			capture->observed.labels);
	assert(capture->observed.frame && capture->observed.canvas == canvas);
	assert(capture->observed.paused != canvas &&
	       capture->kept_on == canvas);
	assert(!strcmp(capture->centre, action) &&
	       !strcmp(capture->right, back));
	assert(!title || has_label(capture, title));
	assert(capture->observed.canvases == 1 && capture->exits == 0);
}

static void press(struct capture *capture, unsigned int code)
{
	fplinux_ui_frame_send_key(&capture->peer.keyboards[0], code, true);
	fplinux_ui_frame_send_key(&capture->peer.keyboards[0], code, false);
	pump(capture);
}

static void space_to(struct capture *capture, bool canvas)
{
	const char *action = canvas ? word(capture, "Pause", "Пауза") :
				      word(capture, "Resume", "Продолжить");

	fplinux_ui_frame_send_key(&capture->peer.keyboards[0], KEY_SPACE, true);
	wait_state(capture, canvas, action,
		   canvas ? NULL : word(capture, "Paused", "Пауза"));
	/* Release after the Alert has gone: the old owner must consume it. */
	fplinux_ui_frame_send_key(&capture->peer.keyboards[0], KEY_SPACE,
				  false);
	pump_for(capture, 80);
	assert(capture->observed.canvas == canvas);
	assert(capture->observed.paused != canvas && capture->exits == 0);
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

static void start(struct capture *capture, char **arguments)
{
	struct fplinux_ui_session_message config = {
		.type = FPLINUX_UI_SESSION_CONFIG,
		.mode = FPLINUX_UI_SESSION_FULLSCREEN,
		.screen_width = atoi(arguments[5]),
		.screen_height = atoi(arguments[6]),
		.theme = !strcmp(arguments[7], "dark") ?
				 FPLINUX_UI_SESSION_THEME_DARK :
				 FPLINUX_UI_SESSION_THEME_LIGHT,
	};
	struct fplinux_ui_session_message hello;
	int wayland[2], observer[2];

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
		assert(setenv("FPLINUX_SNAKE_OBSERVER_FD", descriptor, 1) == 0);
		assert(snprintf(path, sizeof(path), "%.*s/locale",
				(int)(slash - arguments[1]),
				arguments[1]) < (int)sizeof(path));
		assert(setenv("FPLINUX_SNAKE_LOCALE_DIR", path, 1) == 0);
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
			      config.screen_height, arguments[2]);
	capture->session = fplinux_ui_frame_accept_session(&capture->peer,
							   capture->listener);
	hello = fplinux_ui_frame_wait_message(&capture->peer, capture->session);
	assert(hello.type == FPLINUX_UI_SESSION_HELLO);
	assert(!strcmp(hello.app_id, "org.fplinux.snake"));
	strcpy(config.locale, arguments[8]);
	strcpy(config.input_locale, "en");
	assert(fplinux_ui_session_send(capture->session, &config));
	wait_state(capture, false, word(capture, "Play", "Играть"),
		   word(capture, "Snake", "Змейка"));
	assert(capture->peer.keyboards[0].keyboard);
	fplinux_ui_frame_focus_keyboard(&capture->peer.keyboards[0]);
	pump_for(capture, 30);
}

static void hide_and_return(struct capture *capture)
{
	struct fplinux_ui_session_message visibility = {
		.type = FPLINUX_UI_SESSION_VISIBILITY,
	};
	unsigned int head;
	uint64_t deadline = fplinux_ui_frame_now_ms() + 3000;

	press(capture, KEY_HOME);
	while (!capture->homes && fplinux_ui_frame_now_ms() < deadline)
		pump(capture);
	assert(capture->homes == 1);
	assert(fplinux_ui_session_send(capture->session, &visibility));
	pump_for(capture, 220);
	assert(!capture->kept_on);
	visibility.visible = true;
	assert(fplinux_ui_session_send(capture->session, &visibility));
	wait_state(capture, false, word(capture, "Resume", "Продолжить"),
		   word(capture, "Paused", "Пауза"));
	head = capture->observed.head;
	pump_for(capture, 220);
	assert(capture->observed.head == head && capture->observed.paused);
}

static void close_and_reap(struct capture *capture)
{
	struct fplinux_ui_session_message close_message = {
		.type = FPLINUX_UI_SESSION_CLOSE,
	};
	uint64_t deadline = fplinux_ui_frame_now_ms() + 3000;
	pid_t finished;
	int status;

	strcpy(close_message.reason, "capture-complete");
	assert(fplinux_ui_session_send(capture->session, &close_message));
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

static void back_closes(struct capture *capture)
{
	uint64_t deadline = fplinux_ui_frame_now_ms() + 3000;

	press(capture, KEY_ESC);
	while (!capture->exits && fplinux_ui_frame_now_ms() < deadline)
		pump(capture);
	assert(capture->exits == 1);
	assert(capture->observed.canvases == 1 && capture->observed.paused);
}

int main(int argc, char **argv)
{
	struct capture capture = { 0 };
	unsigned int head, pending;

	assert(argc == 10);
	start(&capture, argv);
	save_frame(&capture, "snake-start");
	space_to(&capture, true);
	save_frame(&capture, "snake-playing");
	if (!strcmp(argv[9], "divider")) {
		close_and_reap(&capture);
		puts("snake-screens: playing Canvas panel boundary captured");
		return 0;
	}
	space_to(&capture, false);
	save_frame(&capture, "snake-paused");
	head = capture.observed.head;
	pending = capture.observed.pending;
	press(&capture, KEY_UP);
	pump_for(&capture, 180);
	assert(capture.observed.head == head &&
	       capture.observed.pending == pending);
	if (!strcmp(argv[9], "pause-back")) {
		back_closes(&capture);
	} else {
		space_to(&capture, true);
		hide_and_return(&capture);
		/* Ordinary OK has the same single-pop route as Space. */
		press(&capture, KEY_ENTER);
		wait_state(&capture, true, word(&capture, "Pause", "Пауза"),
			   NULL);
		wait_state(&capture, false, word(&capture, "Restart", "Заново"),
			   word(&capture, "Game over", "Игра окончена"));
		assert(capture.observed.dead && !capture.observed.won);
		save_frame(&capture, "snake-loss");
		space_to(&capture, true);
		assert(capture.observed.starts == 2 && !capture.observed.dead);
		assert(send(capture.observer, "w", 1, MSG_NOSIGNAL) == 1);
		wait_state(&capture, false, word(&capture, "Restart", "Заново"),
			   word(&capture, "You won!", "Победа!"));
		assert(capture.observed.won && !capture.observed.dead);
		save_frame(&capture, "snake-win");
		space_to(&capture, true);
		assert(capture.observed.starts == 3 && !capture.observed.won);
		assert(send(capture.observer, "w", 1, MSG_NOSIGNAL) == 1);
		wait_state(&capture, false, word(&capture, "Restart", "Заново"),
			   word(&capture, "You won!", "Победа!"));
		back_closes(&capture);
	}
	close_and_reap(&capture);
	puts("snake-screens: permanent Canvas; Alert commands; Space press/release; paused input; hidden return; Back closes");
	return 0;
}
