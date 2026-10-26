/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "screen-frame.h"
#include "ui-frame-peer.h"

#include <assert.h>
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

struct capture {
	struct fplinux_ui_frame_peer peer;
	struct fplinux_ui_session_message request;
	pid_t application;
	int listener;
	int session;
	uint32_t serial;
	uint32_t redraw_ack;
	uint32_t active_ack;
	bool ready;
	bool tasks_open;
	bool task_closed;
	bool has_request;
	const char *directory;
	const char *session_path;
	const char *theme;
	char selected[FPLINUX_UI_SESSION_ID_BYTES];
	char view[FPLINUX_UI_SESSION_NAME_BYTES];
	char presentation[FPLINUX_UI_SESSION_NAME_BYTES];
	char left[FPLINUX_UI_SESSION_TEXT_BYTES];
	char centre[FPLINUX_UI_SESSION_TEXT_BYTES];
	char right[FPLINUX_UI_SESSION_TEXT_BYTES];
};

static int number(const char *text)
{
	char *end;
	long value = strtol(text, &end, 10);

	assert(*text && !*end && value > 0 && value <= 1024);
	return (int)value;
}

static void answer_tasks(struct capture *capture)
{
	struct fplinux_ui_session_message reply = {
		.type = FPLINUX_UI_SESSION_TASKS,
		.has_tasks = true,
		.task_count = capture->task_closed ? 1 : 2,
		.tasks = {
			{ .app_id = "org.test.app01", .title = "App 01",
			  .token = 101, .lifecycle = FPLINUX_UI_SESSION_BACKGROUND },
			{ .app_id = "org.test.app02", .title = "App 02",
			  .token = 102, .lifecycle = FPLINUX_UI_SESSION_BACKGROUND },
		},
	};

	if (capture->task_closed)
		reply.tasks[0] = reply.tasks[1];
	assert(fplinux_ui_session_send(capture->session, &reply));
}

static void read_messages(struct capture *capture)
{
	struct fplinux_ui_session_message message;
	int received;

	while ((received = fplinux_ui_session_receive(capture->session,
						      &message)) == 1) {
		switch (message.type) {
		case FPLINUX_UI_SESSION_READY:
			capture->ready = true;
			break;
		case FPLINUX_UI_SESSION_ACTIVE_ACK:
			capture->active_ack = message.serial;
			break;
		case FPLINUX_UI_SESSION_REDRAW_ACK:
			capture->redraw_ack = message.serial;
			break;
		case FPLINUX_UI_SESSION_SELECT:
			if (!strcmp(message.value, "tasks")) {
				capture->tasks_open = message.enabled;
			} else {
				strcpy(capture->selected, message.selected_id);
				strcpy(capture->view, message.value);
				strcpy(capture->presentation, message.name);
			}
			break;
		case FPLINUX_UI_SESSION_SOFTKEYS:
			strcpy(capture->left, message.left);
			strcpy(capture->centre, message.centre);
			strcpy(capture->right, message.right);
			break;
		case FPLINUX_UI_SESSION_TASKS:
			assert(!message.has_tasks);
			answer_tasks(capture);
			break;
		case FPLINUX_UI_SESSION_CLOSE_TASK:
			capture->task_closed = true;
			/* fall through */
		case FPLINUX_UI_SESSION_ACTIVATE:
		case FPLINUX_UI_SESSION_LAUNCH:
			assert(!capture->has_request);
			capture->request = message;
			capture->has_request = true;
			break;
		default:
			assert(!message.has_frame);
			assert(message.type != FPLINUX_UI_SESSION_EXIT);
			break;
		}
	}
	assert(received == 0);
}

static void pump(struct capture *capture)
{
	fplinux_ui_frame_pump(&capture->peer);
	read_messages(capture);
}

static void press(struct capture *capture, unsigned int key)
{
	struct fplinux_ui_frame_keyboard *keyboard =
		&capture->peer.keyboards[1];

	fplinux_ui_frame_send_key(keyboard, key, true);
	fplinux_ui_frame_send_key(keyboard, key, false);
}

static void press_frame(struct capture *capture, unsigned int key)
{
	unsigned int previous = capture->peer.commits;
	uint64_t deadline = fplinux_ui_frame_now_ms() + 3000;

	press(capture, key);
	while (capture->peer.commits <= previous &&
	       fplinux_ui_frame_now_ms() < deadline)
		pump(capture);
	assert(capture->peer.commits > previous);
}

static void wait_tasks(struct capture *capture, bool open)
{
	uint64_t deadline = fplinux_ui_frame_now_ms() + 3000;

	while (capture->tasks_open != open &&
	       fplinux_ui_frame_now_ms() < deadline)
		pump(capture);
	assert(capture->tasks_open == open);
}

static void wait_legends(struct capture *capture, const char *left,
			 const char *centre, const char *right)
{
	uint64_t deadline = fplinux_ui_frame_now_ms() + 3000;

	while ((strcmp(capture->left, left) ||
		strcmp(capture->centre, centre) ||
		strcmp(capture->right, right)) &&
	       fplinux_ui_frame_now_ms() < deadline)
		pump(capture);
	if (strcmp(capture->left, left) || strcmp(capture->centre, centre) ||
	    strcmp(capture->right, right))
		fprintf(stderr, "Launcher legends: [%s] [%s] [%s]\n",
			capture->left, capture->centre, capture->right);
	assert(!strcmp(capture->left, left));
	assert(!strcmp(capture->centre, centre));
	assert(!strcmp(capture->right, right));
}

static void wait_selection(struct capture *capture, const char *view,
			   const char *presentation, int index)
{
	char id[FPLINUX_UI_SESSION_ID_BYTES];
	uint64_t deadline = fplinux_ui_frame_now_ms() + 3000;

	snprintf(id, sizeof(id), "org.test.app%02d", index);
	while ((strcmp(capture->selected, id) || strcmp(capture->view, view) ||
		strcmp(capture->presentation, presentation)) &&
	       fplinux_ui_frame_now_ms() < deadline)
		pump(capture);
	if (strcmp(capture->selected, id) || strcmp(capture->view, view) ||
	    strcmp(capture->presentation, presentation))
		fprintf(stderr,
			"Launcher selection: %s %s %s; expected %s %s %s\n",
			capture->view, capture->presentation, capture->selected,
			view, presentation, id);
	assert(!strcmp(capture->selected, id));
	assert(!strcmp(capture->view, view));
	assert(!strcmp(capture->presentation, presentation));
}

/* A session acknowledgement fences the selected view's rendered pixels. */
static void redraw(struct capture *capture)
{
	struct fplinux_ui_session_message message = {
		.type = FPLINUX_UI_SESSION_SETTING,
		.serial = ++capture->serial,
	};
	uint64_t deadline = fplinux_ui_frame_now_ms() + 3000;

	strcpy(message.name, "theme");
	strcpy(message.value, capture->theme);
	assert(fplinux_ui_session_send(capture->session, &message));
	while (capture->redraw_ack != message.serial &&
	       fplinux_ui_frame_now_ms() < deadline)
		pump(capture);
	assert(capture->redraw_ack == message.serial);
}

static void save_frame(struct capture *capture, const char *name)
{
	char path[512];

	redraw(capture);
	assert(snprintf(path, sizeof(path), "%s/%s.rgb565", capture->directory,
			name) < (int)sizeof(path));
	assert(screen_frame_write(path, capture->peer.pixels,
				  capture->peer.width, capture->peer.height,
				  capture->peer.width *
					  sizeof(*capture->peer.pixels)));
}

static void select_key(struct capture *capture, unsigned int key, int index)
{
	press(capture, key);
	wait_selection(capture, "catalogue", "list", index);
}

static void wait_request(struct capture *capture,
			 enum fplinux_ui_session_message_type type,
			 const char *id, uint32_t token)
{
	uint64_t deadline = fplinux_ui_frame_now_ms() + 3000;

	while (!capture->has_request && fplinux_ui_frame_now_ms() < deadline)
		pump(capture);
	assert(capture->has_request);
	assert(capture->request.type == type);
	assert(!strcmp(capture->request.app_id, id));
	assert(capture->request.token == token);
	capture->has_request = false;
}

static void save_launch_frame(struct capture *capture)
{
	struct stat status;
	uint16_t *pixels;
	size_t bytes = (size_t)capture->peer.width * capture->peer.height * 2;
	char path[512];
	int fd = capture->request.frame_fd;

	assert(capture->request.has_frame);
	assert(capture->request.width == capture->peer.width);
	assert(capture->request.height == capture->peer.height);
	assert(fstat(fd, &status) == 0 && status.st_size == (off_t)bytes);
	pixels = malloc(bytes);
	assert(pixels);
	assert(pread(fd, pixels, bytes, 0) == (ssize_t)bytes);
	assert(snprintf(path, sizeof(path), "%s/launcher-launch.rgb565",
			capture->directory) < (int)sizeof(path));
	assert(screen_frame_write(path, pixels, capture->peer.width,
				  capture->peer.height,
				  capture->peer.width * 2));
	free(pixels);
	assert(close(fd) == 0);
}

static void start(struct capture *capture, char **arguments)
{
	struct fplinux_ui_session_message config = {
		.type = FPLINUX_UI_SESSION_CONFIG,
		.mode = FPLINUX_UI_SESSION_LAUNCHER,
		.screen_width = number(arguments[1]),
		.screen_height = number(arguments[2]),
	};
	struct fplinux_ui_session_message active = {
		.type = FPLINUX_UI_SESSION_ACTIVE,
		.serial = ++capture->serial,
		.battery = -1,
		.visible = true,
		.value = "catalogue",
		.name = "list",
		.selected_id = "org.test.app01",
	};
	struct fplinux_ui_session_message hello;
	uint64_t deadline;
	int wayland[2];

	capture->session_path = arguments[5];
	capture->directory = arguments[6];
	capture->theme = arguments[9];
	config.theme = !strcmp(capture->theme, "dark") ?
			       FPLINUX_UI_SESSION_THEME_DARK :
			       FPLINUX_UI_SESSION_THEME_LIGHT;
	active.theme = config.theme;
	strcpy(config.locale, "en");
	strcpy(config.input_locale, "en");
	strcpy(active.locale, "en");
	strcpy(active.input_locale, "en");
	capture->listener =
		fplinux_ui_frame_listen_session(capture->session_path);
	fplinux_ui_frame_socketpair(wayland);
	capture->application = fork();
	assert(capture->application >= 0);
	if (!capture->application) {
		close(capture->listener);
		close(wayland[0]);
		fplinux_ui_frame_client_environment(wayland[1],
						    capture->session_path);
		assert(setenv("FPLINUX_UI_DESKTOP_DIR", arguments[7], 1) == 0);
		assert(setenv("FPLINUX_LAUNCHER_CAPTURE_ICONS", arguments[8],
			      1) == 0);
		assert(setenv("XDG_STATE_HOME", arguments[6], 1) == 0);
		assert(setenv("LANG", "en_US.UTF-8", 1) == 0);
		assert(setenv("LC_ALL", "en_US.UTF-8", 1) == 0);
		assert(setenv("TZ", "UTC0", 1) == 0);
		assert(unsetenv("LANGUAGE") == 0);
		execl(arguments[3], arguments[3], (char *)NULL);
		_exit(127);
	}
	close(wayland[1]);
	fplinux_ui_frame_init(&capture->peer, wayland[0], config.screen_width,
			      config.screen_height, arguments[4]);
	capture->session = fplinux_ui_frame_accept_session(&capture->peer,
							   capture->listener);
	hello = fplinux_ui_frame_wait_message(&capture->peer, capture->session);
	assert(hello.type == FPLINUX_UI_SESSION_HELLO);
	assert(!strcmp(hello.app_id, "org.fplinux.launcher"));
	assert(fplinux_ui_session_send(capture->session, &config));
	deadline = fplinux_ui_frame_now_ms() + 3000;
	while (!capture->ready && fplinux_ui_frame_now_ms() < deadline)
		pump(capture);
	assert(capture->ready);
	assert(fplinux_ui_session_send(capture->session, &active));
	deadline = fplinux_ui_frame_now_ms() + 3000;
	while (capture->active_ack != active.serial &&
	       fplinux_ui_frame_now_ms() < deadline)
		pump(capture);
	assert(capture->active_ack == active.serial);
	assert(capture->peer.keyboards[1].keyboard);
	fplinux_ui_frame_focus_keyboard(&capture->peer.keyboards[1]);
	wait_legends(capture, "Icons", "Open", "Back");
}

static void close_and_reap(struct capture *capture)
{
	struct fplinux_ui_session_message message = {
		.type = FPLINUX_UI_SESSION_CLOSE,
		.reason = "capture-complete",
	};
	uint64_t deadline = fplinux_ui_frame_now_ms() + 3000;
	pid_t finished;
	int status;

	assert(fplinux_ui_session_send(capture->session, &message));
	do {
		fplinux_ui_frame_pump(&capture->peer);
		finished = waitpid(capture->application, &status, WNOHANG);
		assert(finished >= 0);
	} while (!finished && fplinux_ui_frame_now_ms() < deadline);
	assert(finished == capture->application);
	assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	close(capture->session);
	close(capture->listener);
	fplinux_ui_frame_cleanup(&capture->peer);
	assert(unlink(capture->session_path) == 0);
}

static void catalogue_navigation(struct capture *capture, int page_rows)
{
	select_key(capture, KEY_DOWN, 2);
	save_frame(capture, "launcher-list-second");
	select_key(capture, KEY_UP, 1);
	select_key(capture, KEY_RIGHT, page_rows + 1);
	select_key(capture, KEY_LEFT, 1);
	for (int digit = 1; digit <= 9; ++digit) {
		int selected = digit;

		if (digit == 1)
			select_key(capture, KEY_DOWN, 2);
		select_key(capture, KEY_NUMERIC_1 + (unsigned int)digit - 1,
			   digit);
		while (selected > 1) {
			selected = selected > page_rows ? selected - page_rows :
							  1;
			select_key(capture, KEY_LEFT, selected);
		}
		assert(!capture->has_request);
	}
	select_key(capture, KEY_DOWN, 2);
	press(capture, KEY_TAB);
	wait_selection(capture, "catalogue", "grid", 2);
	save_frame(capture, "launcher-grid");
	press(capture, KEY_TAB);
	wait_selection(capture, "catalogue", "list", 2);
	wait_legends(capture, "Icons", "Open", "Back");
	puts("launcher-navigation: arrows, pages and digits 1-9 select; Tab preserves app 02");
}

static void launch_and_notice(struct capture *capture)
{
	struct fplinux_ui_session_message error = {
		.type = FPLINUX_UI_SESSION_ERROR,
		.reason = "app-start-failed",
	};

	press(capture, KEY_OK);
	wait_request(capture, FPLINUX_UI_SESSION_LAUNCH, "org.test.app02", 0);
	save_launch_frame(capture);
	assert(fplinux_ui_session_send(capture->session, &error));
	wait_legends(capture, "", "", "Close");
	save_frame(capture, "launcher-notice");
	press(capture, KEY_F14);
	wait_legends(capture, "Icons", "Open", "Back");
	save_frame(capture, "launcher-notice-closed");
	puts("launcher-launch: app 02 snapshot is full size; failed launch closes back to List");
}

static void task_overlay(struct capture *capture)
{
	press(capture, KEY_F14);
	wait_selection(capture, "home", "list", 2);
	save_frame(capture, "launcher-home");
	press(capture, KEY_TAB);
	wait_legends(capture, "Close app", "Open", "Back");
	wait_tasks(capture, true);
	save_frame(capture, "launcher-tasks-first");
	press_frame(capture, KEY_DOWN);
	save_frame(capture, "launcher-tasks-second");
	press_frame(capture, KEY_UP);
	press(capture, KEY_F13);
	wait_legends(capture, "", "Close", "Cancel");
	press(capture, KEY_F14);
	wait_legends(capture, "Close app", "Open", "Back");
	assert(!capture->task_closed && !capture->has_request);
	press(capture, KEY_F13);
	wait_legends(capture, "", "Close", "Cancel");
	press(capture, KEY_OK);
	wait_request(capture, FPLINUX_UI_SESSION_CLOSE_TASK, "org.test.app01",
		     101);
	wait_legends(capture, "Close app", "Open", "Back");
	press(capture, KEY_OK);
	wait_request(capture, FPLINUX_UI_SESSION_ACTIVATE, "org.test.app02",
		     102);
	wait_tasks(capture, false);
	press(capture, KEY_OK);
	wait_selection(capture, "catalogue", "list", 2);
	wait_legends(capture, "Icons", "Open", "Back");
	save_frame(capture, "launcher-returned-list");
	puts("launcher-tasks: Home opens Tasks; Cancel keeps app; Close and Open target exact instances");
}

int main(int argc, char **argv)
{
	struct capture capture = { 0 };

	assert(argc == 11);
	start(&capture, argv);
	save_frame(&capture, "launcher-list-first");
	catalogue_navigation(&capture, number(argv[10]));
	launch_and_notice(&capture);
	task_overlay(&capture);
	close_and_reap(&capture);
	return 0;
}
