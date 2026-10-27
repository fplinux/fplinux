/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "launcher-observer.h"
#include "screen-frame.h"
#include "ui-frame-peer.h"

#include <assert.h>
#include <json-c/json.h>
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

struct capture {
	struct fplinux_ui_frame_peer peer;
	struct fplinux_ui_session_message request;
	pid_t application;
	int listener;
	int session;
	int observer;
	struct launcher_frame_observation observed;
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
	const char *locale;
	const char *scenario;
	char selected[FPLINUX_UI_SESSION_ID_BYTES];
	char view[FPLINUX_UI_SESSION_NAME_BYTES];
	char presentation[FPLINUX_UI_SESSION_NAME_BYTES];
	char left[FPLINUX_UI_SESSION_TEXT_BYTES];
	char centre[FPLINUX_UI_SESSION_TEXT_BYTES];
	char right[FPLINUX_UI_SESSION_TEXT_BYTES];
};

static void wait_request(struct capture *capture,
			 enum fplinux_ui_session_message_type type,
			 const char *id, uint32_t token);

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
			assert(message.type != FPLINUX_UI_SESSION_CLOCK);
			break;
		}
	}
	assert(received == 0);
}

static void pump(struct capture *capture)
{
	struct launcher_frame_observation observation;
	ssize_t bytes;

	fplinux_ui_frame_pump(&capture->peer);
	read_messages(capture);
	while ((bytes = recv(capture->observer, &observation,
			     sizeof(observation), MSG_DONTWAIT)) > 0) {
		assert(bytes == (ssize_t)sizeof(observation));
		if (observation.timer_only) {
			capture->observed.clock_callbacks =
				observation.clock_callbacks;
			capture->observed.clock_period_ms =
				observation.clock_period_ms;
			capture->observed.clock_paused =
				observation.clock_paused;
			capture->observed.animations = observation.animations;
		} else {
			capture->observed = observation;
		}
	}
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

static void save_observation(struct capture *capture, const char *name)
{
	struct json_object *document = json_object_new_object();
	struct json_object *labels = json_object_new_array();
	struct json_object *lines = json_object_new_array();
	char path[512];

	save_frame(capture, name);
	json_object_object_add(
		document, "lines",
		json_object_new_int((int)capture->observed.lines));
	json_object_object_add(
		document, "scroll_extent",
		json_object_new_int(capture->observed.scroll_extent));
	json_object_object_add(
		document, "animations",
		json_object_new_int((int)capture->observed.animations));
	json_object_object_add(
		document, "active_serial",
		json_object_new_int64(capture->observed.active_serial));
	json_object_object_add(document, "line_objects", lines);
	for (unsigned int i = 0; i < capture->observed.line_objects_count;
	     ++i) {
		const struct launcher_line_observation *line =
			&capture->observed.line_objects[i];
		struct json_object *entry = json_object_new_object();
		struct json_object *bounds = json_object_new_array();
		struct json_object *parent_bounds = json_object_new_array();

		json_object_object_add(entry, "visible",
				       json_object_new_boolean(line->visible));
		json_object_object_add(entry, "points",
				       json_object_new_int((int)line->points));
		json_object_object_add(
			entry, "parent_radius",
			json_object_new_int(line->parent_radius));
		for (size_t j = 0; j < 4; ++j) {
			json_object_array_add(
				bounds, json_object_new_int(line->bounds[j]));
			json_object_array_add(
				parent_bounds,
				json_object_new_int(line->parent_bounds[j]));
		}
		json_object_object_add(entry, "bounds", bounds);
		json_object_object_add(entry, "parent_bounds", parent_bounds);
		json_object_array_add(lines, entry);
	}
	json_object_object_add(document, "labels", labels);
	for (unsigned int i = 0; i < capture->observed.label_count; ++i) {
		const struct launcher_label_observation *label =
			&capture->observed.labels[i];
		struct json_object *entry = json_object_new_object();
		struct json_object *bounds = json_object_new_array();
		struct json_object *text_size = json_object_new_array();
		int values[] = { label->x, label->y, label->width,
				 label->height };

		json_object_object_add(entry, "text",
				       json_object_new_string(label->text));
		for (size_t j = 0; j < sizeof(values) / sizeof(values[0]); ++j)
			json_object_array_add(bounds,
					      json_object_new_int(values[j]));
		json_object_object_add(entry, "bounds", bounds);
		json_object_array_add(text_size,
				      json_object_new_int(label->text_width));
		json_object_array_add(text_size,
				      json_object_new_int(label->text_height));
		json_object_object_add(entry, "text_size", text_size);
		json_object_array_add(labels, entry);
	}
	assert(snprintf(path, sizeof(path), "%s/%s.json", capture->directory,
			name) < (int)sizeof(path));
	assert(json_object_to_file_ext(path, document,
				       JSON_C_TO_STRING_PRETTY) == 0);
	json_object_put(document);
}

static const struct launcher_label_observation *
find_label(const struct capture *capture, const char *text)
{
	for (unsigned int i = 0; i < capture->observed.label_count; ++i)
		if (!strcmp(capture->observed.labels[i].text, text))
			return &capture->observed.labels[i];
	return NULL;
}

static void wait_label(struct capture *capture, const char *text)
{
	uint64_t deadline = fplinux_ui_frame_now_ms() + 3000;

	while (!find_label(capture, text) &&
	       fplinux_ui_frame_now_ms() < deadline)
		pump(capture);
	if (!find_label(capture, text)) {
		fprintf(stderr, "Launcher missing [%s]; actual:", text);
		for (unsigned int i = 0; i < capture->observed.label_count; ++i)
			fprintf(stderr, " [%s]",
				capture->observed.labels[i].text);
		fputc('\n', stderr);
	}
	assert(find_label(capture, text));
}

static bool has_label_prefix(const struct capture *capture, const char *prefix)
{
	for (unsigned int i = 0; i < capture->observed.label_count; ++i)
		if (!strncmp(capture->observed.labels[i].text, prefix,
			     strlen(prefix)))
			return true;
	return false;
}

static void wait_label_prefix(struct capture *capture, const char *prefix)
{
	uint64_t deadline = fplinux_ui_frame_now_ms() + 3000;

	while (!has_label_prefix(capture, prefix) &&
	       fplinux_ui_frame_now_ms() < deadline)
		pump(capture);
	assert(has_label_prefix(capture, prefix));
}

static void
home_active(struct capture *capture,
	    const struct fplinux_ui_session_clock_presentation *presentation)
{
	struct fplinux_ui_session_message active = {
		.type = FPLINUX_UI_SESSION_ACTIVE,
		.serial = ++capture->serial,
		.battery = 80,
		.visible = true,
		.value = "home",
		.name = "list",
		.clock_presentation = *presentation,
	};
	uint64_t deadline = fplinux_ui_frame_now_ms() + 3000;

	strcpy(active.locale, capture->locale);
	strcpy(active.input_locale, capture->locale);
	active.theme = !strcmp(capture->theme, "dark") ?
			       FPLINUX_UI_SESSION_THEME_DARK :
			       FPLINUX_UI_SESSION_THEME_LIGHT;
	assert(fplinux_ui_session_send(capture->session, &active));
	while ((capture->active_ack != active.serial ||
		capture->observed.active_serial != active.serial) &&
	       fplinux_ui_frame_now_ms() < deadline)
		pump(capture);
	assert(capture->active_ack == active.serial);
	assert(capture->observed.active_serial == active.serial);
}

static void home_standby(struct capture *capture, bool installed)
{
	struct fplinux_ui_session_clock_presentation presentation = {
		.alarm_utc = 1793086200,
		.alarm_label = "Wake",
		.timer_remaining_ms = 90000,
		.missed_total = 3,
	};
	bool russian = !strcmp(capture->locale, "ru");
	const char *timer = russian ? "Таймер: 00:08" : "Timer: 00:08";
	struct fplinux_ui_session_message visibility = {
		.type = FPLINUX_UI_SESSION_VISIBILITY,
	};
	uint64_t deadline;
	unsigned int callbacks;

	wait_label(capture, "07:00");
	save_observation(capture, "launcher-home-idle");
	if (installed)
		assert(find_label(capture, russian ? "Часы" : "Clock"));
	else
		assert(!find_label(capture, russian ? "Часы" : "Clock"));
	home_active(capture, &presentation);
	wait_label_prefix(capture, russian ? "Будильник 07:30" : "Alarm 07:30");
	wait_label(capture, russian ? "Пауза: 01:30" : "Paused: 01:30");
	wait_label(capture, russian ? "Пропущено: 3" : "Missed: 3");
	assert(capture->observed.clock_period_ms == 60000);
	save_observation(capture, "launcher-home-events");
	presentation.analog = true;
	home_active(capture, &presentation);
	save_observation(capture, "launcher-home-analog");
	if (capture->observed.lines != 3)
		fprintf(stderr,
			"Launcher analog ACTIVE %u: observed %u visible lines (%u total line objects)\n",
			capture->observed.active_serial,
			capture->observed.lines,
			capture->observed.line_objects_count);
	assert(capture->observed.lines == 3);
	assert(capture->observed.clock_period_ms == 1000);
	presentation.analog = false;
	presentation.alarm_utc = 0;
	presentation.snooze_deadline_ms = fplinux_ui_frame_now_ms() + 9000;
	presentation.timer_remaining_ms = 0;
	presentation.timer_deadline_ms = fplinux_ui_frame_now_ms() + 9000;
	home_active(capture, &presentation);
	wait_label_prefix(capture, russian ? "Отложен 00:09" : "Snooze 00:09");
	wait_label(capture, russian ? "Таймер: 00:09" : "Timer: 00:09");
	callbacks = capture->observed.clock_callbacks;
	wait_label(capture, timer);
	assert(capture->observed.clock_callbacks > callbacks);
	assert(capture->observed.clock_period_ms <= 1000);
	save_observation(capture, "launcher-home-countdown");
	assert(fplinux_ui_session_send(capture->session, &visibility));
	deadline = fplinux_ui_frame_now_ms() + 3000;
	while (!capture->observed.clock_paused &&
	       fplinux_ui_frame_now_ms() < deadline)
		pump(capture);
	assert(capture->observed.clock_paused);
	callbacks = capture->observed.clock_callbacks;
	deadline = fplinux_ui_frame_now_ms() + 1200;
	while (fplinux_ui_frame_now_ms() < deadline)
		pump(capture);
	assert(capture->observed.clock_callbacks == callbacks);
	assert(capture->observed.animations == 0);
	visibility.visible = true;
	assert(fplinux_ui_session_send(capture->session, &visibility));
	deadline = fplinux_ui_frame_now_ms() + 3000;
	while (capture->observed.clock_paused &&
	       fplinux_ui_frame_now_ms() < deadline)
		pump(capture);
	assert(!capture->observed.clock_paused);
	assert(capture->observed.clock_period_ms <= 1000);
	presentation = (struct fplinux_ui_session_clock_presentation){ 0 };
	home_active(capture, &presentation);
	assert(capture->observed.clock_period_ms == 60000);
	assert(!capture->has_request);
	if (installed) {
		press(capture, KEY_F14);
		wait_request(capture, FPLINUX_UI_SESSION_LAUNCH,
			     "org.fplinux.clock", 0);
		assert(capture->request.has_frame);
		assert(close(capture->request.frame_fd) == 0);
	} else {
		assert(!find_label(capture, russian ? "Часы" : "Clock"));
		press(capture, KEY_F14);
		redraw(capture);
		assert(!capture->has_request);
	}
	puts("launcher-home: simultaneous events; local countdown; hidden timer paused; installed Clock launch");
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
	int wayland[2], observer[2];

	capture->session_path = arguments[5];
	capture->directory = arguments[6];
	capture->theme = arguments[9];
	config.theme = !strcmp(capture->theme, "dark") ?
			       FPLINUX_UI_SESSION_THEME_DARK :
			       FPLINUX_UI_SESSION_THEME_LIGHT;
	active.theme = config.theme;
	strcpy(config.locale, capture->locale);
	strcpy(config.input_locale, capture->locale);
	strcpy(active.locale, capture->locale);
	strcpy(active.input_locale, capture->locale);
	if (!strcmp(capture->scenario, "home"))
		strcpy(active.value, "home");
	else if (!strcmp(capture->scenario, "grid") ||
		 !strcmp(capture->scenario, "marquee"))
		strcpy(active.name, "grid");
	capture->listener =
		fplinux_ui_frame_listen_session(capture->session_path);
	fplinux_ui_frame_socketpair(wayland);
	assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, observer) == 0);
	capture->application = fork();
	assert(capture->application >= 0);
	if (!capture->application) {
		char descriptor[32];

		close(capture->listener);
		close(wayland[0]);
		close(observer[0]);
		snprintf(descriptor, sizeof(descriptor), "%d", observer[1]);
		assert(setenv("FPLINUX_LAUNCHER_OBSERVER_FD", descriptor, 1) ==
		       0);
		assert(setenv("FPLINUX_LAUNCHER_TEST_CLOCK", "1", 1) == 0);
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
	close(observer[1]);
	capture->observer = observer[0];
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
	if (!strcmp(capture->scenario, "navigation"))
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
	close(capture->observer);
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
	const struct fplinux_ui_session_message late_error = {
		.type = FPLINUX_UI_SESSION_ERROR,
		.reason = "task-unavailable",
	};
	const struct fplinux_ui_session_message home = {
		.type = FPLINUX_UI_SESSION_HOME,
	};

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
	assert(fplinux_ui_session_send(capture->session, &late_error));
	wait_legends(capture, "", "", "Close");
	assert(fplinux_ui_session_send(capture->session, &home));
	wait_selection(capture, "home", "list", 2);
	press(capture, KEY_OK);
	wait_selection(capture, "catalogue", "list", 2);
	wait_legends(capture, "Icons", "Open", "Back");
	puts("launcher-tasks: Home opens Tasks; Cancel keeps app; Close and Open target exact instances");
}

static void catalogue_marquee(struct capture *capture)
{
	struct fplinux_ui_session_message visibility = {
		.type = FPLINUX_UI_SESSION_VISIBILITY,
	};
	uint64_t deadline = fplinux_ui_frame_now_ms() + 12000;
	unsigned int commits;

	assert(capture->observed.animations > 0);
	while (capture->observed.animations &&
	       fplinux_ui_frame_now_ms() < deadline)
		pump(capture);
	redraw(capture);
	assert(capture->observed.animations == 0);
	commits = capture->peer.commits;
	deadline = fplinux_ui_frame_now_ms() + 1200;
	while (fplinux_ui_frame_now_ms() < deadline)
		pump(capture);
	assert(capture->peer.commits == commits);
	press(capture, KEY_RIGHT);
	wait_selection(capture, "catalogue", "grid", 2);
	press(capture, KEY_LEFT);
	wait_selection(capture, "catalogue", "grid", 1);
	redraw(capture);
	assert(capture->observed.animations > 0);
	assert(fplinux_ui_session_send(capture->session, &visibility));
	deadline = fplinux_ui_frame_now_ms() + 3000;
	while (!capture->observed.clock_paused &&
	       fplinux_ui_frame_now_ms() < deadline)
		pump(capture);
	assert(capture->observed.clock_paused);
	assert(capture->observed.animations == 0);
	visibility.visible = true;
	assert(fplinux_ui_session_send(capture->session, &visibility));
	redraw(capture);
	assert(capture->observed.animations == 0);
	puts("launcher-marquee: one complete pass; idle stays still; new focus restarts; hiding stops");
}

int main(int argc, char **argv)
{
	struct capture capture = { 0 };

	assert(argc == 11 || argc == 13 || argc == 14);
	capture.scenario = argc >= 13 ? argv[11] : "navigation";
	capture.locale = argc >= 13 ? argv[12] : "en";
	start(&capture, argv);
	if (!strcmp(capture.scenario, "home")) {
		assert(argc == 14);
		assert(!strcmp(argv[13], "installed") ||
		       !strcmp(argv[13], "absent"));
		home_standby(&capture, !strcmp(argv[13], "installed"));
	} else if (!strcmp(capture.scenario, "grid")) {
		save_observation(&capture, "launcher-grid-fit");
	} else if (!strcmp(capture.scenario, "marquee")) {
		catalogue_marquee(&capture);
	} else {
		save_frame(&capture, "launcher-list-first");
		catalogue_navigation(&capture, number(argv[10]));
		launch_and_notice(&capture);
		task_overlay(&capture);
	}
	close_and_reap(&capture);
	return 0;
}
