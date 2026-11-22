/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "bluez-fake.h"
#include "fplinux-clock-wire.h"
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
	struct fplinux_ui_session_message clock_request;
	pid_t application;
	int listener;
	int session;
	int observer;
	struct launcher_frame_observation observed;
	uint32_t serial;
	uint32_t redraw_ack;
	uint32_t active_ack;
	unsigned int softkeys;
	/* Rendered frames that show the ring's busy alert. */
	unsigned int waiting_frames;
	/* What successive distinct frames showed since reset_frames(). */
	char frames[256];
	char frame[32];
	bool ready;
	bool tasks_open;
	bool quick_open;
	bool task_closed;
	enum fplinux_ui_session_playback task_playback;
	bool has_request;
	bool has_clock_request;
	char dismissed_notice[FPLINUX_UI_SESSION_NAME_BYTES];
	const char *directory;
	const char *session_path;
	const char *theme;
	const char *locale;
	const char *scenario;
	char selected[FPLINUX_UI_SESSION_ID_BYTES];
	enum fplinux_ui_launcher_view view;
	enum fplinux_ui_launcher_catalogue presentation;
	char left[FPLINUX_UI_SESSION_TEXT_BYTES];
	char centre[FPLINUX_UI_SESSION_TEXT_BYTES];
	char right[FPLINUX_UI_SESSION_TEXT_BYTES];
	/* Quick settings: the controlled BlueZ and the manager's brightness. */
	struct bluez_fake *bluez;
	unsigned int brightness;
	unsigned int volume;
	int volume_draft;
	bool volume_preview_active;
	bool wifi_supported;
	bool wifi_on;
	bool brightness_unavailable;
	bool hold_volume_save;
	struct fplinux_ui_session_message held_volume_save;
	bool hold_volume_preview;
	struct fplinux_ui_session_message volume_preview;
	struct fplinux_ui_setting level_request;
	unsigned int brightness_requests;
};

#define HCI0 "/org/bluez/hci0"

static void wait_request(struct capture *capture,
			 enum fplinux_ui_session_message_type type,
			 const char *id, uint32_t token);
static void wait_no_label(struct capture *capture, const char *text);

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
	if (!strcmp(capture->scenario, "playback")) {
		reply.task_count = 1;
		strcpy(reply.tasks[0].title,
		       !strcmp(capture->locale, "ru") ? "Музыка" : "Music");
		reply.tasks[0].playback = capture->task_playback;
	}
	assert(fplinux_ui_session_send(capture->session, &reply));
}

static void send_system_levels(struct capture *capture)
{
	struct fplinux_ui_session_message message = {
		.type = FPLINUX_UI_SESSION_SYSTEM_LEVELS,
		.system_levels.level = {
			[FPLINUX_SYSTEM_LEVEL_MEDIA] = {
				.availability = FPLINUX_SYSTEM_LEVEL_READY,
				.maximum = 10,
			},
			[FPLINUX_SYSTEM_LEVEL_ALARM] = {
				.availability = FPLINUX_SYSTEM_LEVEL_READY,
				.maximum = 10,
				.value = 8,
			},
			[FPLINUX_SYSTEM_LEVEL_BRIGHTNESS] = {
				.minimum = 1,
				.maximum = 10,
			},
		},
	};
	struct fplinux_system_level_state *brightness =
		&message.system_levels.level[FPLINUX_SYSTEM_LEVEL_BRIGHTNESS];

	message.system_levels.level[FPLINUX_SYSTEM_LEVEL_MEDIA].value =
		capture->volume_preview_active ?
			(unsigned int)capture->volume_draft :
			capture->volume;
	brightness->availability = capture->brightness_unavailable ?
					   FPLINUX_SYSTEM_LEVEL_UNAVAILABLE :
					   FPLINUX_SYSTEM_LEVEL_READY;
	brightness->value =
		capture->brightness_unavailable ? 0 : capture->brightness;
	assert(fplinux_ui_session_send(capture->session, &message));
}

static void publish_wifi(struct capture *capture,
			 enum fplinux_ui_wifi_link link, const char *network)
{
	struct fplinux_ui_session_message message = {
		.type = FPLINUX_UI_SESSION_WIFI_STATUS,
		.wifi_status = {
			.power = !capture->wifi_supported ? FPLINUX_UI_WIFI_UNAVAILABLE :
				 capture->wifi_on ? FPLINUX_UI_WIFI_ON : FPLINUX_UI_WIFI_OFF,
			.link = link,
		},
	};

	snprintf(message.wifi_status.name, sizeof(message.wifi_status.name),
		 "%s", network);
	assert(fplinux_ui_session_send(capture->session, &message));
}

/* Preview shows a level, Save stores it and Cancel restores the saved level. */
static void answer_setting(struct capture *capture,
			   const struct fplinux_ui_session_message *request)
{
	const struct fplinux_ui_setting *setting = &request->setting;
	enum fplinux_ui_setting_kind kind = setting->kind;
	enum fplinux_ui_setting_operation operation = setting->operation;
	struct fplinux_ui_session_message reply = {
		.type = FPLINUX_UI_SESSION_SETTING,
		.serial = request->serial,
		.applied = true,
		.storage_state = FPLINUX_STORAGE_DURABLE,
		.setting = { .kind = kind,
			     .operation = FPLINUX_UI_SETTING_RESULT },
	};

	assert((kind != FPLINUX_UI_SETTING_BRIGHTNESS &&
		kind != FPLINUX_UI_SETTING_VOLUME) ||
	       operation == FPLINUX_UI_SETTING_SET);
	if (kind == FPLINUX_UI_SETTING_WIFI) {
		if (!capture->wifi_supported) {
			reply.type = FPLINUX_UI_SESSION_ERROR;
			reply.applied = false;
			reply.storage_state = FPLINUX_STORAGE_UNPUBLISHED;
			reply.setting = *setting;
			strcpy(reply.reason, "unsupported");
		} else {
			if (operation == FPLINUX_UI_SETTING_SET)
				capture->wifi_on = setting->value.enabled;
			reply.setting.value.enabled = capture->wifi_on;
		}
		publish_wifi(capture, FPLINUX_UI_WIFI_DISCONNECTED, "");
		assert(fplinux_ui_session_send(capture->session, &reply));
		return;
	}
	capture->level_request = *setting;
	++capture->brightness_requests;
	if (kind == FPLINUX_UI_SETTING_VOLUME_STEP) {
		assert(operation == FPLINUX_UI_SETTING_STEP);
		assert(setting->value.step == -1 || setting->value.step == 1);
		if (!capture->volume_preview_active) {
			capture->volume_draft = (int)capture->volume;
			capture->volume_preview_active = true;
		}
		capture->volume_draft += setting->value.step;
		if (capture->volume_draft < 0)
			capture->volume_draft = 0;
		if (capture->volume_draft > 10)
			capture->volume_draft = 10;
		reply.setting.kind = FPLINUX_UI_SETTING_VOLUME_PREVIEW;
		reply.setting.value.level = capture->volume_draft;
		capture->volume_preview = reply;
		if (capture->hold_volume_preview) {
			send_system_levels(capture);
			return;
		}
	} else if (kind == FPLINUX_UI_SETTING_VOLUME_COMMIT) {
		assert(operation == FPLINUX_UI_SETTING_COMMIT);
		capture->volume = (unsigned int)capture->volume_draft;
		capture->volume_preview_active = false;
		reply.setting.kind = FPLINUX_UI_SETTING_VOLUME;
		reply.setting.value.level = (int)capture->volume;
		if (capture->hold_volume_save) {
			capture->held_volume_save = reply;
			return;
		}
	} else if (kind == FPLINUX_UI_SETTING_BRIGHTNESS) {
		capture->brightness = (unsigned int)setting->value.level;
		reply.setting.value.level = (int)capture->brightness;
	} else if (kind == FPLINUX_UI_SETTING_VOLUME) {
		capture->volume = (unsigned int)setting->value.level;
		reply.setting.value.level = (int)capture->volume;
		if (capture->hold_volume_save) {
			capture->held_volume_save = reply;
			return;
		}
	} else if (operation == FPLINUX_UI_SETTING_PREVIEW) {
		reply.setting.value.level = setting->value.level;
		if (kind == FPLINUX_UI_SETTING_VOLUME_PREVIEW) {
			capture->volume_preview = reply;
			if (capture->hold_volume_preview)
				return;
		}
	} else {
		bool volume = kind == FPLINUX_UI_SETTING_VOLUME_PREVIEW;

		assert(operation == FPLINUX_UI_SETTING_CANCEL);
		if (volume)
			capture->volume_preview_active = false;
		reply.setting.kind = volume ? FPLINUX_UI_SETTING_VOLUME :
					      FPLINUX_UI_SETTING_BRIGHTNESS;
		reply.setting.value.level =
			(int)(volume ? capture->volume : capture->brightness);
	}
	assert(fplinux_ui_session_send(capture->session, &reply));
	send_system_levels(capture);
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
			if (message.launcher_view ==
			    FPLINUX_UI_LAUNCHER_VIEW_TASKS) {
				capture->tasks_open = message.enabled;
			} else if (message.launcher_view ==
				   FPLINUX_UI_LAUNCHER_VIEW_QUICK_SETTINGS) {
				capture->quick_open = message.enabled;
			} else {
				strcpy(capture->selected, message.selected_id);
				capture->view = message.launcher_view;
				capture->presentation =
					message.launcher_catalogue;
			}
			break;
		case FPLINUX_UI_SESSION_SOFTKEYS:
			++capture->softkeys;
			strcpy(capture->left, message.left);
			strcpy(capture->centre, message.centre);
			strcpy(capture->right, message.right);
			break;
		case FPLINUX_UI_SESSION_TASKS:
			assert(!message.has_tasks);
			answer_tasks(capture);
			break;
		case FPLINUX_UI_SESSION_CLOSE_TASK:
			/* fall through */
		case FPLINUX_UI_SESSION_ACTIVATE:
		case FPLINUX_UI_SESSION_LAUNCH:
			assert(!capture->has_request);
			capture->request = message;
			capture->has_request = true;
			break;
		case FPLINUX_UI_SESSION_SETTING:
			/* Quick settings asks only while it reports itself. */
			assert(capture->quick_open);
			answer_setting(capture, &message);
			break;
		case FPLINUX_UI_SESSION_CLOCK:
			assert(!capture->has_clock_request);
			capture->clock_request = message;
			capture->has_clock_request = true;
			break;
		case FPLINUX_UI_SESSION_DISMISS:
			snprintf(capture->dismissed_notice,
				 sizeof(capture->dismissed_notice), "%s",
				 message.notice);
			break;
		default:
			assert(!message.has_frame);
			assert(message.type != FPLINUX_UI_SESSION_EXIT);
			break;
		}
	}
	assert(received == 0);
}

static bool shows_waiting(const struct launcher_frame_observation *observation)
{
	for (unsigned int i = 0; i < observation->label_count; ++i)
		if (!strcmp(observation->labels[i].text, "Waiting…"))
			return true;
	return false;
}

/* Waiting over a ring, a ring by its event and status, or the view below. */
static void frame_name(const struct launcher_frame_observation *observation,
		       char *name, size_t capacity)
{
	if (shows_waiting(observation)) {
		snprintf(name, capacity, "wait");
		return;
	}
	for (unsigned int i = 0; i < observation->label_count; ++i) {
		const char *text = observation->labels[i].text;
		const char *ring = strstr(text, "Ring ");

		if (ring) {
			snprintf(name, capacity, "ring%c%s", ring[5],
				 strstr(text, "Action failed") ? "-failed" :
								 "");
			return;
		}
	}
	snprintf(name, capacity, "view");
}

static void note_frame(struct capture *capture)
{
	char name[sizeof(capture->frame)];
	size_t length = strlen(capture->frames);

	frame_name(&capture->observed, name, sizeof(name));
	if (!strcmp(name, capture->frame))
		return;
	strcpy(capture->frame, name);
	assert(length + 1 + strlen(name) < sizeof(capture->frames));
	snprintf(capture->frames + length, sizeof(capture->frames) - length,
		 "%s%s", length ? " " : "", name);
}

/* Starts the record with what the latest frame shows. */
static void reset_frames(struct capture *capture)
{
	capture->frames[0] = '\0';
	capture->frame[0] = '\0';
	note_frame(capture);
}

static void assert_frames(const struct capture *capture, const char *expected)
{
	if (strcmp(capture->frames, expected))
		fprintf(stderr, "Launcher frames: [%s]; expected [%s]\n",
			capture->frames, expected);
	assert(!strcmp(capture->frames, expected));
}

static void pump(struct capture *capture)
{
	struct launcher_frame_observation observation;
	ssize_t bytes;

	fplinux_ui_frame_pump(&capture->peer);
	if (capture->bluez)
		bluez_fake_pump(capture->bluez);
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
			if (shows_waiting(&observation))
				++capture->waiting_frames;
			note_frame(capture);
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
		fprintf(stderr,
			"Launcher legends: [%s] [%s] [%s]; expected [%s] [%s] [%s]\n",
			capture->left, capture->centre, capture->right, left,
			centre, right);
	assert(!strcmp(capture->left, left));
	assert(!strcmp(capture->centre, centre));
	assert(!strcmp(capture->right, right));
}

static void wait_selection(struct capture *capture,
			   enum fplinux_ui_launcher_view view,
			   enum fplinux_ui_launcher_catalogue presentation,
			   int index)
{
	char id[FPLINUX_UI_SESSION_ID_BYTES];
	uint64_t deadline = fplinux_ui_frame_now_ms() + 3000;

	snprintf(id, sizeof(id), "org.test.app%02d", index);
	while ((strcmp(capture->selected, id) || capture->view != view ||
		capture->presentation != presentation) &&
	       fplinux_ui_frame_now_ms() < deadline)
		pump(capture);
	if (strcmp(capture->selected, id) || capture->view != view ||
	    capture->presentation != presentation)
		fprintf(stderr,
			"Launcher selection: %u %u %s; expected %u %u %s\n",
			capture->view, capture->presentation, capture->selected,
			view, presentation, id);
	assert(!strcmp(capture->selected, id));
	assert(capture->view == view);
	assert(capture->presentation == presentation);
}

/* A session acknowledgement fences the selected view's rendered pixels. */
static void redraw(struct capture *capture)
{
	struct fplinux_ui_session_message message = {
		.type = FPLINUX_UI_SESSION_SETTING,
		.applied = true,
		.serial = ++capture->serial,
		.setting = { .kind = FPLINUX_UI_SETTING_THEME,
			     .operation = FPLINUX_UI_SETTING_RESULT,
			     .value.theme =
				     !strcmp(capture->theme, "dark") ?
					     FPLINUX_UI_SESSION_THEME_DARK :
					     FPLINUX_UI_SESSION_THEME_LIGHT },
	};
	uint64_t deadline = fplinux_ui_frame_now_ms() + 3000;

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

static void active_frame(struct capture *capture,
			 struct fplinux_ui_session_message *active)
{
	uint64_t deadline = fplinux_ui_frame_now_ms() + 3000;

	active->serial = ++capture->serial;
	strcpy(active->locale, capture->locale);
	strcpy(active->input_locale, capture->locale);
	active->theme = !strcmp(capture->theme, "dark") ?
				FPLINUX_UI_SESSION_THEME_DARK :
				FPLINUX_UI_SESSION_THEME_LIGHT;
	assert(fplinux_ui_session_send(capture->session, active));
	while ((capture->active_ack != active->serial ||
		capture->observed.active_serial != active->serial) &&
	       fplinux_ui_frame_now_ms() < deadline)
		pump(capture);
	assert(capture->active_ack == active->serial);
	assert(capture->observed.active_serial == active->serial);
}

static void
home_active(struct capture *capture,
	    const struct fplinux_ui_session_clock_presentation *presentation)
{
	struct fplinux_ui_session_message active = {
		.type = FPLINUX_UI_SESSION_ACTIVE,
		.battery = 80,
		.visible = true,
		.launcher_view = FPLINUX_UI_LAUNCHER_VIEW_HOME,
		.launcher_catalogue = FPLINUX_UI_LAUNCHER_CATALOGUE_LIST,
		.clock_presentation = *presentation,
	};

	active_frame(capture, &active);
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
	wait_selection(capture, FPLINUX_UI_LAUNCHER_VIEW_CATALOGUE,
		       FPLINUX_UI_LAUNCHER_CATALOGUE_LIST, index);
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
	if (strcmp(capture->request.app_id, id))
		fprintf(stderr, "Launcher request: wanted %s, received %s\n",
			id, capture->request.app_id);
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
		.launcher_view = FPLINUX_UI_LAUNCHER_VIEW_CATALOGUE,
		.launcher_catalogue = FPLINUX_UI_LAUNCHER_CATALOGUE_LIST,
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
	if (!strcmp(capture->scenario, "home") ||
	    !strcmp(capture->scenario, "notice-home") ||
	    !strcmp(capture->scenario, "quick-settings"))
		active.launcher_view = FPLINUX_UI_LAUNCHER_VIEW_HOME;
	else if (!strcmp(capture->scenario, "grid") ||
		 !strcmp(capture->scenario, "notice-grid") ||
		 !strcmp(capture->scenario, "marquee"))
		active.launcher_catalogue = FPLINUX_UI_LAUNCHER_CATALOGUE_GRID;
	capture->listener =
		fplinux_ui_frame_listen_session(capture->session_path);
	fplinux_ui_frame_socketpair(wayland);
	assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, observer) == 0);
	capture->application = fork();
	assert(capture->application >= 0);
	if (!capture->application) {
		char descriptor[32], bus[512];

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
		/* Only Quick settings has a bus, the scenario's own. */
		if (strcmp(capture->scenario, "quick-settings")) {
			assert(snprintf(bus, sizeof(bus),
					"unix:path=%s/absent-bus",
					arguments[6]) < (int)sizeof(bus));
			assert(setenv("DBUS_SYSTEM_BUS_ADDRESS", bus, 1) == 0);
		}
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
	send_system_levels(capture);
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
	wait_selection(capture, FPLINUX_UI_LAUNCHER_VIEW_CATALOGUE,
		       FPLINUX_UI_LAUNCHER_CATALOGUE_GRID, 2);
	save_frame(capture, "launcher-grid");
	press(capture, KEY_TAB);
	wait_selection(capture, FPLINUX_UI_LAUNCHER_VIEW_CATALOGUE,
		       FPLINUX_UI_LAUNCHER_CATALOGUE_LIST, 2);
	wait_legends(capture, "Icons", "Open", "Back");
	puts("launcher-navigation: arrows, pages and digits 1-9 select; Tab preserves app 02");
}

/* Each focused app of List, down to the last of twelve and back up. */
static void catalogue_rows(struct capture *capture)
{
	char name[64];

	for (int index = 1; index <= 12; ++index) {
		if (index > 1)
			select_key(capture, KEY_DOWN, index);
		snprintf(name, sizeof(name), "launcher-rows-down-%02d", index);
		save_frame(capture, name);
	}
	for (int index = 11; index >= 1; --index) {
		select_key(capture, KEY_UP, index);
		snprintf(name, sizeof(name), "launcher-rows-up-%02d", index);
		save_frame(capture, name);
	}
	puts("launcher-rows: List focus moved down to app 12 and back to app 01");
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

static void playback_tasks(struct capture *capture)
{
	static const struct {
		enum fplinux_ui_session_playback playback;
		const char *english;
		const char *russian;
	} states[] = {
		{ FPLINUX_UI_SESSION_PLAYBACK_PLAYING, "Playing", "Играет" },
		{ FPLINUX_UI_SESSION_PLAYBACK_PAUSED, "Paused", "Пауза" },
		{ FPLINUX_UI_SESSION_PLAYBACK_HELD, "Audio interrupted",
		  "Звук прерван" },
		{ FPLINUX_UI_SESSION_PLAYBACK_UNAVAILABLE, "Audio unavailable",
		  "Звук недоступен" },
		{ FPLINUX_UI_SESSION_PLAYBACK_NONE, "Background", "В фоне" },
	};
	bool russian = !strcmp(capture->locale, "ru");
	char name[64];

	press(capture, KEY_F14);
	wait_selection(capture, FPLINUX_UI_LAUNCHER_VIEW_HOME,
		       FPLINUX_UI_LAUNCHER_CATALOGUE_LIST, 1);
	press(capture, KEY_TAB);
	wait_tasks(capture, true);
	for (size_t i = 0; i < sizeof(states) / sizeof(states[0]); ++i) {
		capture->task_playback = states[i].playback;
		answer_tasks(capture);
		wait_label_prefix(capture, russian ? states[i].russian :
						     states[i].english);
		assert(has_label_prefix(capture, russian ? "Музыка" : "Music"));
		assert(snprintf(name, sizeof(name), "playback-tasks-%zu", i) <
		       (int)sizeof(name));
		save_observation(capture, name);
	}
	puts("launcher-playback: current locale, application name and audio states drawn");
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
	wait_selection(capture, FPLINUX_UI_LAUNCHER_VIEW_HOME,
		       FPLINUX_UI_LAUNCHER_CATALOGUE_LIST, 2);
	save_frame(capture, "launcher-home");
	press(capture, KEY_TAB);
	wait_legends(capture, "Quit", "Open", "Back");
	wait_tasks(capture, true);
	save_frame(capture, "launcher-tasks-first");
	press_frame(capture, KEY_DOWN);
	save_frame(capture, "launcher-tasks-second");
	press_frame(capture, KEY_UP);
	press(capture, KEY_F13);
	wait_legends(capture, "", "Close", "Cancel");
	press(capture, KEY_F14);
	wait_legends(capture, "Quit", "Open", "Back");
	assert(!capture->task_closed && !capture->has_request);
	press(capture, KEY_F13);
	wait_legends(capture, "", "Close", "Cancel");
	press(capture, KEY_OK);
	wait_request(capture, FPLINUX_UI_SESSION_CLOSE_TASK, "org.test.app01",
		     101);
	/* A waiting close brings its decision to the foreground. */
	struct fplinux_ui_session_message closing = {
		.type = FPLINUX_UI_SESSION_ACTIVE,
		.active_id = "org.test.app01",
		.selected_id = "org.test.app02",
		.title = "App 01",
		.launcher_view = FPLINUX_UI_LAUNCHER_VIEW_HOME,
		.launcher_catalogue = FPLINUX_UI_LAUNCHER_CATALOGUE_LIST,
		.battery = 80,
	};

	active_frame(capture, &closing);
	wait_tasks(capture, false);
	capture->task_closed = true;
	closing.active_id[0] = '\0';
	closing.tasks_view = true;
	active_frame(capture, &closing);
	/* ACTIVE_ACK must cover Tasks, not an intermediate catalogue frame. */
	assert(capture->tasks_open);
	assert(find_label(capture, "Open apps"));
	wait_tasks(capture, true);
	wait_legends(capture, "Quit", "Open", "Back");
	press(capture, KEY_OK);
	wait_request(capture, FPLINUX_UI_SESSION_ACTIVATE, "org.test.app02",
		     102);
	wait_tasks(capture, false);
	press(capture, KEY_OK);
	wait_selection(capture, FPLINUX_UI_LAUNCHER_VIEW_CATALOGUE,
		       FPLINUX_UI_LAUNCHER_CATALOGUE_LIST, 2);
	wait_legends(capture, "Icons", "Open", "Back");
	save_frame(capture, "launcher-returned-list");
	assert(fplinux_ui_session_send(capture->session, &late_error));
	wait_legends(capture, "", "", "Close");
	assert(fplinux_ui_session_send(capture->session, &home));
	wait_selection(capture, FPLINUX_UI_LAUNCHER_VIEW_HOME,
		       FPLINUX_UI_LAUNCHER_CATALOGUE_LIST, 2);
	press(capture, KEY_OK);
	wait_selection(capture, FPLINUX_UI_LAUNCHER_VIEW_CATALOGUE,
		       FPLINUX_UI_LAUNCHER_CATALOGUE_LIST, 2);
	wait_legends(capture, "Icons", "Open", "Back");
	puts("launcher-tasks: Home opens Tasks; Cancel keeps app; Close and Open target exact instances");
}

static void ring_data(char *data, size_t capacity, uint64_t event_id)
{
	struct clock_ring ring = { .snooze_minutes = 5 };

	assert(event_id < 10);
	if (event_id) {
		ring.event = (struct clock_event){
			.id = event_id,
			.kind = CLOCK_EVENT_ALARM,
			.source_id = 42,
			.generation = 1,
			.due_wall_utc = 1793086200,
		};
		/* Each event names itself in the drawn ring. */
		snprintf(ring.event.label, sizeof(ring.event.label), "Ring %u",
			 (unsigned int)event_id);
	}
	assert(clock_wire_ring_encode(&ring, data, capacity) == CLOCK_OK);
}

/* The manager names the ringing event in ACTIVE; its ACK means drawn. */
static void ring_active(struct capture *capture, uint64_t event_id)
{
	struct fplinux_ui_session_message active = {
		.type = FPLINUX_UI_SESSION_ACTIVE,
		.serial = ++capture->serial,
		.battery = -1,
		.visible = true,
		.launcher_view = FPLINUX_UI_LAUNCHER_VIEW_CATALOGUE,
		.launcher_catalogue = FPLINUX_UI_LAUNCHER_CATALOGUE_LIST,
	};
	uint64_t deadline = fplinux_ui_frame_now_ms() + 3000;

	strcpy(active.locale, capture->locale);
	strcpy(active.input_locale, capture->locale);
	active.theme = !strcmp(capture->theme, "dark") ?
			       FPLINUX_UI_SESSION_THEME_DARK :
			       FPLINUX_UI_SESSION_THEME_LIGHT;
	ring_data(active.clock_data, sizeof(active.clock_data), event_id);
	assert(fplinux_ui_session_send(capture->session, &active));
	while (capture->active_ack != active.serial &&
	       fplinux_ui_frame_now_ms() < deadline)
		pump(capture);
	assert(capture->active_ack == active.serial);
	wait_legends(capture, "Snooze", "", "Stop");
}

static uint32_t stop_ring(struct capture *capture, uint64_t event_id)
{
	struct clock_request request;
	const char *json = capture->clock_request.clock_data;
	uint64_t deadline = fplinux_ui_frame_now_ms() + 3000;

	press(capture, KEY_F14);
	while (!capture->has_clock_request &&
	       fplinux_ui_frame_now_ms() < deadline)
		pump(capture);
	assert(capture->has_clock_request);
	capture->has_clock_request = false;
	assert(!capture->clock_request.name[0]);
	assert(clock_wire_request_decode(json, strlen(json), &request) ==
	       CLOCK_OK);
	assert(request.op == CLOCK_REQUEST_DISMISS);
	assert(request.event_id == event_id);
	return capture->clock_request.serial;
}

static void clock_reply(struct capture *capture, uint32_t serial,
			uint64_t event_id, const char *reason)
{
	struct fplinux_ui_session_message reply = {
		.type = FPLINUX_UI_SESSION_CLOCK,
		.serial = serial,
		.name = "dismiss",
	};

	snprintf(reply.reason, sizeof(reply.reason), "%s", reason);
	ring_data(reply.clock_data, sizeof(reply.clock_data), event_id);
	assert(fplinux_ui_session_send(capture->session, &reply));
}

static void settle(struct capture *capture, unsigned int duration_ms)
{
	uint64_t deadline = fplinux_ui_frame_now_ms() + duration_ms;

	while (fplinux_ui_frame_now_ms() < deadline)
		pump(capture);
}

static void wait_busy(struct capture *capture, bool shown)
{
	uint64_t deadline = fplinux_ui_frame_now_ms() + 3000;

	while (shows_waiting(&capture->observed) != shown &&
	       fplinux_ui_frame_now_ms() < deadline)
		pump(capture);
	assert(shows_waiting(&capture->observed) == shown);
}

static void wait_frame(struct capture *capture, const char *name)
{
	uint64_t deadline = fplinux_ui_frame_now_ms() + 3000;

	while (strcmp(capture->frame, name) &&
	       fplinux_ui_frame_now_ms() < deadline)
		pump(capture);
	if (strcmp(capture->frame, name))
		fprintf(stderr, "Launcher frames: [%s]; waited for [%s]\n",
			capture->frames, name);
	assert(!strcmp(capture->frame, name));
}

/*
 * The answered ring stays, without input, until the owner replies; its
 * result is the next step. The pause before an immediate reply lets a
 * frame of the view below show if the ring left early.
 */
static void ring_answers(struct capture *capture)
{
	const struct fplinux_ui_session_message home = {
		.type = FPLINUX_UI_SESSION_HOME,
	};
	unsigned int softkeys;
	uint32_t first;

	wait_legends(capture, "Icons", "Open", "Back");
	/* An immediate success: the list returns in one step, never busy. */
	ring_active(capture, 1);
	reset_frames(capture);
	softkeys = capture->softkeys;
	first = stop_ring(capture, 1);
	settle(capture, 150);
	assert_frames(capture, "ring1");
	assert(capture->softkeys == softkeys);
	clock_reply(capture, first, 0, "");
	wait_legends(capture, "Icons", "Open", "Back");
	settle(capture, 1200);
	assert_frames(capture, "ring1 view");
	assert(capture->softkeys == softkeys + 1 && !capture->waiting_frames);
	puts("launcher-ring-fast: the ring stays until the reply, then the list returns without a busy alert");

	/*
	 * A slow success shows the busy alert over the ring. HOME during the
	 * wait leaves the list, the screen below, in place until the ring goes.
	 */
	ring_active(capture, 2);
	reset_frames(capture);
	first = stop_ring(capture, 2);
	assert(!capture->waiting_frames);
	assert(fplinux_ui_session_send(capture->session, &home));
	wait_selection(capture, FPLINUX_UI_LAUNCHER_VIEW_HOME,
		       FPLINUX_UI_LAUNCHER_CATALOGUE_LIST, 1);
	wait_busy(capture, true);
	wait_legends(capture, "", "", "");
	clock_reply(capture, first, 0, "");
	wait_busy(capture, false);
	settle(capture, 300);
	assert_frames(capture, "ring2 wait view");
	/* Home took the list's place: OK opens the list again. */
	press(capture, KEY_OK);
	wait_selection(capture, FPLINUX_UI_LAUNCHER_VIEW_CATALOGUE,
		       FPLINUX_UI_LAUNCHER_CATALOGUE_LIST, 1);
	wait_legends(capture, "Icons", "Open", "Back");
	puts("launcher-ring-slow: the busy alert covers the ring; HOME applies once the ring goes");

	/* An immediate failure changes the ring's text in place. */
	capture->waiting_frames = 0;
	ring_active(capture, 3);
	reset_frames(capture);
	softkeys = capture->softkeys;
	first = stop_ring(capture, 3);
	settle(capture, 150);
	clock_reply(capture, first, 3, "clock-failed");
	wait_frame(capture, "ring3-failed");
	settle(capture, 300);
	assert_frames(capture, "ring3 ring3-failed");
	assert(capture->softkeys == softkeys && !capture->waiting_frames);
	puts("launcher-ring-failed: the ring stays with Stop and Snooze and shows the error");

	/* A slow failure: the busy alert leaves the failed ring again. */
	reset_frames(capture);
	first = stop_ring(capture, 3);
	wait_busy(capture, true);
	clock_reply(capture, first, 3, "clock-failed");
	wait_busy(capture, false);
	wait_legends(capture, "Snooze", "", "Stop");
	settle(capture, 300);
	assert_frames(capture, "ring3-failed wait ring3-failed");
	puts("launcher-ring-slow-failed: the busy alert returns to the failed ring");

	/*
	 * A new ring before the busy alert replaces the answered one at once
	 * and is acknowledged; the old reply changes nothing.
	 */
	reset_frames(capture);
	first = stop_ring(capture, 3);
	settle(capture, 150);
	ring_active(capture, 4);
	clock_reply(capture, first, 0, "");
	settle(capture, 1200);
	assert_frames(capture, "ring3-failed ring4");
	clock_reply(capture, stop_ring(capture, 4), 0, "");
	wait_legends(capture, "Icons", "Open", "Back");
	puts("launcher-ring-superseded-early: a new ring replaces the answered one at once");

	/* A new ring after the busy alert replaces both at once. */
	capture->waiting_frames = 0;
	ring_active(capture, 5);
	reset_frames(capture);
	first = stop_ring(capture, 5);
	wait_busy(capture, true);
	ring_active(capture, 6);
	clock_reply(capture, first, 0, "");
	softkeys = capture->softkeys;
	settle(capture, 1200);
	assert_frames(capture, "ring5 wait ring6");
	assert(capture->softkeys == softkeys);
	clock_reply(capture, stop_ring(capture, 6), 0, "");
	wait_legends(capture, "Icons", "Open", "Back");
	settle(capture, 300);
	assert_frames(capture, "ring5 wait ring6 view");
	/* The catalogue follows its view again: Tab changes its presentation. */
	press(capture, KEY_TAB);
	wait_selection(capture, FPLINUX_UI_LAUNCHER_VIEW_CATALOGUE,
		       FPLINUX_UI_LAUNCHER_CATALOGUE_GRID, 1);
	press(capture, KEY_TAB);
	wait_selection(capture, FPLINUX_UI_LAUNCHER_VIEW_CATALOGUE,
		       FPLINUX_UI_LAUNCHER_CATALOGUE_LIST, 1);
	wait_legends(capture, "Icons", "Open", "Back");
	select_key(capture, KEY_DOWN, 2);
	puts("launcher-ring-superseded-late: a new ring replaces the busy alert and the answered ring; the catalogue follows its view again");
}

static void notice_battery_active(struct capture *capture, const char *notice)
{
	struct fplinux_ui_session_message active = {
		.type = FPLINUX_UI_SESSION_ACTIVE,
		.serial = ++capture->serial,
		.battery = 5,
		.visible = true,
		.launcher_view = FPLINUX_UI_LAUNCHER_VIEW_HOME,
		.launcher_catalogue = FPLINUX_UI_LAUNCHER_CATALOGUE_LIST,
	};
	uint64_t deadline = fplinux_ui_frame_now_ms() + 3000;

	snprintf(active.notice, sizeof(active.notice), "%s", notice);
	strcpy(active.locale, capture->locale);
	strcpy(active.input_locale, capture->locale);
	active.theme = !strcmp(capture->theme, "dark") ?
			       FPLINUX_UI_SESSION_THEME_DARK :
			       FPLINUX_UI_SESSION_THEME_LIGHT;
	assert(fplinux_ui_session_send(capture->session, &active));
	while (capture->active_ack != active.serial &&
	       fplinux_ui_frame_now_ms() < deadline)
		pump(capture);
	assert(capture->active_ack == active.serial);
}

static void system_notice(struct capture *capture)
{
	const struct fplinux_ui_session_message error = {
		.type = FPLINUX_UI_SESSION_ERROR,
		.reason = "tasks-full",
	};
	const struct fplinux_ui_session_message home = {
		.type = FPLINUX_UI_SESSION_HOME,
	};
	bool on_home = !strcmp(capture->scenario, "notice-home");
	bool on_grid = !strcmp(capture->scenario, "notice-grid");
	uint64_t deadline;
	uint32_t serial;

	save_frame(capture, "notice-before");
	assert(fplinux_ui_session_send(capture->session, &error));
	wait_label(capture, "Too many open apps");
	wait_label(
		capture,
		"Close an application in Tasks, then try again. Your open applications were kept.");
	wait_legends(capture, "", "", "Close");
	save_observation(capture, "notice-shown");
	press(capture, KEY_DOWN);
	press(capture, KEY_DOWN);
	press(capture, KEY_DOWN);
	press(capture, KEY_TAB);
	press(capture, KEY_PICKUP_PHONE);
	redraw(capture);
	assert(!capture->tasks_open && !capture->quick_open);
	press(capture, KEY_F14);
	wait_no_label(capture, "Too many open apps");
	if (on_home) {
		wait_label(capture, "Tasks");
		wait_label(capture, "Menu");
	} else if (on_grid) {
		wait_label(capture, "List");
		wait_label(capture, "Open");
		wait_label(capture, "Back");
	} else {
		wait_legends(capture, "Icons", "Open", "Back");
	}
	save_frame(capture, "notice-closed");
	if (on_home) {
		notice_battery_active(capture, "battery-low");
		wait_label(capture, "Battery low");
		press(capture, KEY_F14);
		deadline = fplinux_ui_frame_now_ms() + 3000;
		while (!capture->dismissed_notice[0] &&
		       fplinux_ui_frame_now_ms() < deadline)
			pump(capture);
		assert(!strcmp(capture->dismissed_notice, "battery-low"));
		notice_battery_active(capture, "battery-low");
		redraw(capture);
		assert(!find_label(capture, "Battery low"));
		notice_battery_active(capture, "");
		notice_battery_active(capture, "battery-low");
		wait_label(capture, "Battery low");
		press(capture, KEY_F14);
		wait_no_label(capture, "Battery low");
		wait_label(capture, "Tasks");
		wait_label(capture, "Menu");
	} else if (!on_grid) {
		assert(fplinux_ui_session_send(capture->session, &error));
		wait_label(capture, "Too many open apps");
		ring_active(capture, 1);
		serial = stop_ring(capture, 1);
		wait_busy(capture, true);
		assert(fplinux_ui_session_send(capture->session, &home));
		redraw(capture);
		assert(shows_waiting(&capture->observed));
		clock_reply(capture, serial, 0, "");
		wait_busy(capture, false);
		wait_label(capture, "Too many open apps");
		wait_legends(capture, "", "", "Close");
		press(capture, KEY_F14);
		wait_no_label(capture, "Too many open apps");
		wait_label(capture, "Tasks");
		wait_label(capture, "Menu");
	}
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
	wait_selection(capture, FPLINUX_UI_LAUNCHER_VIEW_CATALOGUE,
		       FPLINUX_UI_LAUNCHER_CATALOGUE_GRID, 2);
	press(capture, KEY_LEFT);
	wait_selection(capture, FPLINUX_UI_LAUNCHER_VIEW_CATALOGUE,
		       FPLINUX_UI_LAUNCHER_CATALOGUE_GRID, 1);
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

/* The manager's battery reading in ACTIVE; its ACK means drawn. */
static void battery_active(struct capture *capture, int percent, bool charging,
			   bool locked)
{
	struct fplinux_ui_session_message active = {
		.type = FPLINUX_UI_SESSION_ACTIVE,
		.serial = ++capture->serial,
		.battery = percent,
		.charging = charging,
		.locked = locked,
		.visible = true,
		.launcher_view = FPLINUX_UI_LAUNCHER_VIEW_CATALOGUE,
		.launcher_catalogue = FPLINUX_UI_LAUNCHER_CATALOGUE_LIST,
		.selected_id = "org.test.app01",
	};
	uint64_t deadline = fplinux_ui_frame_now_ms() + 3000;

	strcpy(active.locale, capture->locale);
	strcpy(active.input_locale, capture->locale);
	active.theme = !strcmp(capture->theme, "dark") ?
			       FPLINUX_UI_SESSION_THEME_DARK :
			       FPLINUX_UI_SESSION_THEME_LIGHT;
	assert(fplinux_ui_session_send(capture->session, &active));
	while (capture->active_ack != active.serial &&
	       fplinux_ui_frame_now_ms() < deadline)
		pump(capture);
	assert(capture->active_ack == active.serial);
}

/*
 * Every battery icon state, one frame each: launcher-battery-<state>, in the
 * catalogue with the status clock; launcher-status-locked adds the lock glyph.
 */
static void battery_states(struct capture *capture)
{
	static const struct {
		const char *name;
		int percent;
		bool charging;
	} states[] = {
		{ "absent", -1, false },
		{ "low", 5, false },
		{ "steps-1", 20, false },
		{ "steps-2", 40, false },
		{ "steps-3", 60, false },
		{ "steps-4", 80, false },
		{ "steps-5", 100, false },
		{ "charging-low", 5, true },
		{ "charging-steps-3", 60, true },
		{ "charging-steps-5", 100, true },
	};
	char name[64];

	for (size_t i = 0; i < sizeof(states) / sizeof(*states); ++i) {
		battery_active(capture, states[i].percent, states[i].charging,
			       false);
		assert(snprintf(name, sizeof(name), "launcher-battery-%s",
				states[i].name) < (int)sizeof(name));
		save_frame(capture, name);
	}
	battery_active(capture, 60, false, true);
	save_frame(capture, "launcher-status-locked");
	struct fplinux_ui_session_message active = {
		.type = FPLINUX_UI_SESSION_ACTIVE,
		.battery = 60,
		.visible = true,
		.launcher_view = FPLINUX_UI_LAUNCHER_VIEW_CATALOGUE,
		.launcher_catalogue = FPLINUX_UI_LAUNCHER_CATALOGUE_LIST,
		.selected_id = "org.test.app01",
	};

	active_frame(capture, &active);
	save_frame(capture, "launcher-playback-idle");
	active.background_playing = true;
	active_frame(capture, &active);
	save_frame(capture, "launcher-playback-playing");
	active.locked = true;
	active_frame(capture, &active);
	save_frame(capture, "launcher-playback-locked");
	puts("launcher-battery: every level, low and charging state drawn");
}

static void wait_quick_settings(struct capture *capture, bool open)
{
	uint64_t deadline = fplinux_ui_frame_now_ms() + 3000;

	while (capture->quick_open != open &&
	       fplinux_ui_frame_now_ms() < deadline)
		pump(capture);
	assert(capture->quick_open == open);
}

static void wait_level_request(struct capture *capture,
			       enum fplinux_ui_setting_kind kind,
			       enum fplinux_ui_setting_operation operation,
			       int value)
{
	uint64_t deadline = fplinux_ui_frame_now_ms() + 3000;
	const struct fplinux_ui_setting *request = &capture->level_request;

	while ((request->kind != kind || request->operation != operation) &&
	       fplinux_ui_frame_now_ms() < deadline)
		pump(capture);
	assert(request->kind == kind && request->operation == operation);
	if (operation == FPLINUX_UI_SETTING_STEP)
		assert(request->value.step == value);
	else if (operation == FPLINUX_UI_SETTING_PREVIEW ||
		 operation == FPLINUX_UI_SETTING_SET)
		assert(request->value.level == value);
	capture->level_request.kind = FPLINUX_UI_SETTING_NONE;
}

static void wait_no_label(struct capture *capture, const char *text)
{
	uint64_t deadline = fplinux_ui_frame_now_ms() + 3000;

	while (find_label(capture, text) &&
	       fplinux_ui_frame_now_ms() < deadline)
		pump(capture);
	assert(!find_label(capture, text));
}

/* The real Quick settings with a controlled manager and BlueZ. */
static void quick_settings(struct capture *capture)
{
	const struct fplinux_ui_session_message home = {
		.type = FPLINUX_UI_SESSION_HOME,
	};

	wait_label(capture, "07:00");
	press(capture, KEY_PICKUP_PHONE);
	wait_quick_settings(capture, true);
	wait_label(capture, "Quick settings");
	wait_label(capture, "← 7 →");
	wait_legends(capture, "", "", "Back");
	save_frame(capture, "quick-list");
	press_frame(capture, KEY_DOWN);
	press_frame(capture, KEY_DOWN);
	wait_label(capture, "Off");
	wait_legends(capture, "", "Turn on", "Back");
	save_frame(capture, "quick-bluetooth-off");

	/* Another client switches it on; the row follows. */
	bluez_fake_set_powered(capture->bluez, HCI0, true);
	wait_label(capture, "On");
	wait_legends(capture, "", "Turn off", "Back");
	save_frame(capture, "quick-bluetooth-on");

	/* A slow change keeps the panel usable and reports progress in the row. */
	bluez_fake_answer(capture->bluez, BLUEZ_FAKE_APPLY, 1500);
	press(capture, KEY_OK);
	wait_label(capture, "Turning off");
	wait_legends(capture, "", "", "Back");
	settle(capture, 100);
	save_frame(capture, "quick-bluetooth-busy");
	wait_legends(capture, "", "Turn on", "Back");
	wait_label(capture, "Off");
	wait_no_label(capture, "Turning off");
	assert(!bluez_fake_powered(capture->bluez, HCI0));

	bluez_fake_answer(capture->bluez, BLUEZ_FAKE_FAIL, 0);
	press(capture, KEY_OK);
	wait_label(capture, "Cannot turn Bluetooth on. Try again.");
	wait_legends(capture, "", "", "Close");
	settle(capture, 100);
	save_frame(capture, "quick-bluetooth-error");
	press(capture, KEY_F14);
	wait_legends(capture, "", "Turn on", "Back");
	assert(bluez_fake_requests(capture->bluez) == 2);

	/* Brightness stays in the list while previewing, cancelling and saving. */
	press_frame(capture, KEY_UP);
	press_frame(capture, KEY_UP);
	press(capture, KEY_OK);
	wait_label(capture, "Quick settings");
	save_frame(capture, "quick-brightness");
	press(capture, KEY_RIGHT);
	wait_level_request(capture, FPLINUX_UI_SETTING_BRIGHTNESS_PREVIEW,
			   FPLINUX_UI_SETTING_PREVIEW, 8);
	wait_legends(capture, "Save", "", "Cancel");
	settle(capture, 100);
	save_frame(capture, "quick-brightness-preview");
	press(capture, KEY_F14);
	wait_level_request(capture, FPLINUX_UI_SETTING_BRIGHTNESS_PREVIEW,
			   FPLINUX_UI_SETTING_CANCEL, 0);
	wait_legends(capture, "", "", "Back");
	wait_label(capture, "← 7 →");
	press(capture, KEY_RIGHT);
	wait_level_request(capture, FPLINUX_UI_SETTING_BRIGHTNESS_PREVIEW,
			   FPLINUX_UI_SETTING_PREVIEW, 8);
	wait_legends(capture, "Save", "", "Cancel");
	press(capture, KEY_F13);
	wait_level_request(capture, FPLINUX_UI_SETTING_BRIGHTNESS,
			   FPLINUX_UI_SETTING_SET, 8);
	wait_label(capture, "← 8 →");
	/* The preview already had 8; Back confirms the save reply was applied. */
	wait_legends(capture, "", "", "Back");
	assert(capture->brightness == 8);

	press_frame(capture, KEY_DOWN);
	wait_label(capture, "← 1 →");
	/* Every GUI step reaches the manager while the previous ACK is held. */
	capture->hold_volume_preview = true;
	press(capture, KEY_RIGHT);
	wait_level_request(capture, FPLINUX_UI_SETTING_VOLUME_STEP,
			   FPLINUX_UI_SETTING_STEP, 1);
	struct fplinux_ui_session_message old_preview = capture->volume_preview;
	unsigned int requests = capture->brightness_requests;
	uint64_t steps_deadline = fplinux_ui_frame_now_ms() + 3000;

	assert(old_preview.serial);
	press(capture, KEY_RIGHT);
	press(capture, KEY_RIGHT);
	while (capture->brightness_requests < requests + 2 &&
	       fplinux_ui_frame_now_ms() < steps_deadline)
		pump(capture);
	assert(capture->brightness_requests == requests + 2);
	wait_level_request(capture, FPLINUX_UI_SETTING_VOLUME_STEP,
			   FPLINUX_UI_SETTING_STEP, 1);
	wait_label(capture, "← 4 →");
	assert(capture->volume_preview.serial != old_preview.serial);
	assert(capture->volume_preview.setting.value.level == 4);
	capture->hold_volume_preview = false;
	assert(fplinux_ui_session_send(capture->session,
				       &capture->volume_preview));
	assert(fplinux_ui_session_send(capture->session, &old_preview));
	settle(capture, 100);
	assert(capture->level_request.kind == FPLINUX_UI_SETTING_NONE);
	wait_label(capture, "← 4 →");
	/* A relative intent still leaves the client at its displayed limit. */
	capture->hold_volume_preview = true;
	requests = capture->brightness_requests;
	steps_deadline = fplinux_ui_frame_now_ms() + 3000;
	for (unsigned int i = 0; i < 7; ++i)
		press(capture, KEY_RIGHT);
	while (capture->brightness_requests < requests + 7 &&
	       fplinux_ui_frame_now_ms() < steps_deadline)
		pump(capture);
	assert(capture->brightness_requests == requests + 7);
	wait_level_request(capture, FPLINUX_UI_SETTING_VOLUME_STEP,
			   FPLINUX_UI_SETTING_STEP, 1);
	wait_label(capture, "← 10 →");
	assert(capture->volume_preview.setting.value.level == 10);
	capture->hold_volume_preview = false;
	assert(fplinux_ui_session_send(capture->session,
				       &capture->volume_preview));
	press(capture, KEY_F14);
	wait_level_request(capture, FPLINUX_UI_SETTING_VOLUME_PREVIEW,
			   FPLINUX_UI_SETTING_CANCEL, 0);
	wait_legends(capture, "", "", "Back");
	wait_label(capture, "← 1 →");
	/* An earlier editor's error cannot cancel the new pending preview. */
	capture->hold_volume_preview = true;
	press(capture, KEY_RIGHT);
	wait_level_request(capture, FPLINUX_UI_SETTING_VOLUME_STEP,
			   FPLINUX_UI_SETTING_STEP, 1);
	old_preview.type = FPLINUX_UI_SESSION_ERROR;
	old_preview.applied = false;
	old_preview.storage_state = FPLINUX_STORAGE_UNPUBLISHED;
	strcpy(old_preview.reason, "unavailable");
	assert(old_preview.serial != capture->volume_preview.serial);
	assert(fplinux_ui_session_send(capture->session, &old_preview));
	settle(capture, 100);
	assert(capture->level_request.kind == FPLINUX_UI_SETTING_NONE);
	wait_legends(capture, "Save", "", "Cancel");
	wait_label(capture, "← 2 →");
	struct fplinux_ui_session_message error = capture->volume_preview;

	error.type = FPLINUX_UI_SESSION_ERROR;
	error.applied = false;
	error.storage_state = FPLINUX_STORAGE_UNPUBLISHED;
	error.setting = (struct fplinux_ui_setting){
		.kind = FPLINUX_UI_SETTING_VOLUME_STEP,
		.operation = FPLINUX_UI_SETTING_STEP,
		.value.step = 1,
	};
	strcpy(error.reason, "unavailable");
	assert(fplinux_ui_session_send(capture->session, &error));
	wait_level_request(capture, FPLINUX_UI_SETTING_VOLUME_PREVIEW,
			   FPLINUX_UI_SETTING_CANCEL, 0);
	wait_label(capture,
		   "This setting is unavailable now. Try again later.");
	wait_legends(capture, "", "", "Close");
	press(capture, KEY_F14);
	wait_legends(capture, "", "", "Back");
	wait_label(capture, "← 1 →");
	capture->hold_volume_preview = false;

	/* Commit saves the current zero preview without a client-side target. */
	press(capture, KEY_LEFT);
	wait_level_request(capture, FPLINUX_UI_SETTING_VOLUME_STEP,
			   FPLINUX_UI_SETTING_STEP, -1);
	wait_label(capture, "← 0 →");
	capture->hold_volume_save = true;
	press(capture, KEY_F13);
	wait_level_request(capture, FPLINUX_UI_SETTING_VOLUME_COMMIT,
			   FPLINUX_UI_SETTING_COMMIT, 0);
	wait_legends(capture, "", "", "");
	press(capture, KEY_F14);
	settle(capture, 100);
	assert(capture->level_request.kind == FPLINUX_UI_SETTING_NONE);
	capture->hold_volume_save = false;
	error = capture->held_volume_save;
	error.type = FPLINUX_UI_SESSION_ERROR;
	error.applied = false;
	error.setting = (struct fplinux_ui_setting){
		.kind = FPLINUX_UI_SETTING_VOLUME_COMMIT,
		.operation = FPLINUX_UI_SETTING_COMMIT,
	};
	error.storage_state = FPLINUX_STORAGE_UNPUBLISHED;
	strcpy(error.reason, "unavailable");
	capture->volume = 1;
	assert(fplinux_ui_session_send(capture->session, &error));
	wait_label(capture,
		   "This setting is unavailable now. Try again later.");
	wait_legends(capture, "", "", "Close");
	press(capture, KEY_F14);
	wait_legends(capture, "", "", "Back");
	wait_label(capture, "← 1 →");
	capture->level_request.kind = FPLINUX_UI_SETTING_NONE;
	press(capture, KEY_LEFT);
	wait_level_request(capture, FPLINUX_UI_SETTING_VOLUME_STEP,
			   FPLINUX_UI_SETTING_STEP, -1);
	wait_label(capture, "← 0 →");
	press(capture, KEY_F13);
	wait_level_request(capture, FPLINUX_UI_SETTING_VOLUME_COMMIT,
			   FPLINUX_UI_SETTING_COMMIT, 0);
	wait_legends(capture, "", "", "Back");
	assert(capture->volume == 0);

	/* bluetoothd stops and starts again: the row goes and returns. */
	bluez_fake_stop(capture->bluez);
	capture->bluez = NULL;
	wait_no_label(capture, "Bluetooth");
	wait_legends(capture, "", "", "Back");
	save_frame(capture, "quick-no-bluetooth");
	capture->bluez = bluez_fake_start();
	bluez_fake_add_adapter(capture->bluez, HCI0, true);
	/* The adapter appears asynchronously and its row can be off-screen. */
	uint64_t deadline = fplinux_ui_frame_now_ms() + 3000;
	while (!find_label(capture, "On") &&
	       fplinux_ui_frame_now_ms() < deadline) {
		press(capture, KEY_DOWN);
		settle(capture, 100);
	}
	wait_label(capture, "On");

	/* Dial closes it; HOME from the manager does as well. */
	press(capture, KEY_PICKUP_PHONE);
	wait_quick_settings(capture, false);
	wait_no_label(capture, "Quick settings");
	capture->wifi_supported = true;
	publish_wifi(capture, FPLINUX_UI_WIFI_DISCONNECTED, "");
	press(capture, KEY_PICKUP_PHONE);
	wait_quick_settings(capture, true);
	deadline = fplinux_ui_frame_now_ms() + 3000;
	while (!find_label(capture, "Wi-Fi") &&
	       fplinux_ui_frame_now_ms() < deadline) {
		press(capture, KEY_DOWN);
		settle(capture, 100);
	}
	wait_label(capture, "Wi-Fi");
	deadline = fplinux_ui_frame_now_ms() + 3000;
	while (!find_label(capture, "← 8 →") &&
	       fplinux_ui_frame_now_ms() < deadline) {
		press(capture, KEY_UP);
		settle(capture, 100);
	}
	wait_label(capture, "← 8 →");
	press(capture, KEY_DOWN);
	wait_label(capture, "← 0 →");
	press(capture, KEY_DOWN);
	wait_legends(capture, "", "Turn on", "Back");
	press(capture, KEY_OK);
	wait_legends(capture, "", "Turn off", "Back");
	assert(capture->wifi_on);
	wait_label(capture, "No network");
	publish_wifi(capture, FPLINUX_UI_WIFI_CONNECTING, "Test station");
	wait_label(capture, "Connecting…");
	publish_wifi(capture, FPLINUX_UI_WIFI_ADDRESSING, "Test station");
	wait_label(capture, "Getting IP address…");
	publish_wifi(capture, FPLINUX_UI_WIFI_NO_ADDRESS, "Test station");
	wait_label(capture, "No IP address");
	save_frame(capture, "quick-wifi-no-address");
	publish_wifi(capture, FPLINUX_UI_WIFI_CONNECTED, "Test station");
	wait_label(capture, "Test station");
	wait_legends(capture, "", "Turn off", "Back");
	save_frame(capture, "quick-wifi-connected");
	publish_wifi(capture, FPLINUX_UI_WIFI_DISCONNECTED, "");
	wait_label(capture, "No network");
	save_frame(capture, "quick-wifi-disconnected");
	press(capture, KEY_PICKUP_PHONE);
	wait_quick_settings(capture, false);
	capture->wifi_supported = false;
	publish_wifi(capture, FPLINUX_UI_WIFI_DISCONNECTED, "");
	capture->brightness_unavailable = true;
	send_system_levels(capture);
	press(capture, KEY_PICKUP_PHONE);
	wait_quick_settings(capture, true);
	wait_label(capture, "Unavailable");
	wait_legends(capture, "", "", "Back");
	save_frame(capture, "quick-brightness-unavailable");
	assert(fplinux_ui_session_send(capture->session, &home));
	wait_quick_settings(capture, false);
	wait_no_label(capture, "Quick settings");
	puts("launcher-quick-settings: inline levels and Bluetooth follow the manager and BlueZ");
}

/*
 * Over an application the launcher draws the system bars with the legends
 * the manager forwards, around the application's surface. Each argument is
 * "left|centre|right".
 */
static void application_bars(struct capture *capture, int count, char **legends)
{
	for (int i = 0; i < count; ++i) {
		struct fplinux_ui_session_message active = {
			.type = FPLINUX_UI_SESSION_ACTIVE,
			.serial = ++capture->serial,
			.battery = -1,
			.visible = true,
			.launcher_view = FPLINUX_UI_LAUNCHER_VIEW_CATALOGUE,
			.launcher_catalogue =
				FPLINUX_UI_LAUNCHER_CATALOGUE_LIST,
			.active_id = "org.test.app01",
			.title = "App 01",
		};
		const char *centre = strchr(legends[i], '|');
		const char *right = centre ? strchr(centre + 1, '|') : NULL;
		uint64_t deadline = fplinux_ui_frame_now_ms() + 3000;
		char name[32];

		assert(right);
		snprintf(active.left, sizeof(active.left), "%.*s",
			 (int)(centre - legends[i]), legends[i]);
		snprintf(active.centre, sizeof(active.centre), "%.*s",
			 (int)(right - centre - 1), centre + 1);
		snprintf(active.right, sizeof(active.right), "%s", right + 1);
		strcpy(active.locale, capture->locale);
		strcpy(active.input_locale, capture->locale);
		active.theme = !strcmp(capture->theme, "dark") ?
				       FPLINUX_UI_SESSION_THEME_DARK :
				       FPLINUX_UI_SESSION_THEME_LIGHT;
		assert(fplinux_ui_session_send(capture->session, &active));
		while (capture->active_ack != active.serial &&
		       fplinux_ui_frame_now_ms() < deadline)
			pump(capture);
		assert(capture->active_ack == active.serial);
		/* The launcher draws these legends; they are not its own. */
		if (active.left[0])
			wait_label(capture, active.left);
		if (active.centre[0])
			wait_label(capture, active.centre);
		if (active.right[0])
			wait_label(capture, active.right);
		snprintf(name, sizeof(name), "application-%d", i + 1);
		save_frame(capture, name);
	}
	puts("launcher-application: the bars carry each application's legends");
}

int main(int argc, char **argv)
{
	struct capture capture = { .brightness = 7 };

	assert(argc == 11 || argc >= 13);
	capture.scenario = argc >= 13 ? argv[11] : "navigation";
	capture.locale = argc >= 13 ? argv[12] : "en";
	if (!strcmp(capture.scenario, "quick-settings")) {
		capture.volume = 1;
		capture.bluez = bluez_fake_start();
		bluez_fake_add_adapter(capture.bluez, HCI0, false);
	}
	start(&capture, argv);
	if (!strcmp(capture.scenario, "quick-settings")) {
		quick_settings(&capture);
	} else if (!strcmp(capture.scenario, "application")) {
		application_bars(&capture, argc - 13, argv + 13);
	} else if (!strcmp(capture.scenario, "home")) {
		assert(argc == 14);
		assert(!strcmp(argv[13], "installed") ||
		       !strcmp(argv[13], "absent"));
		home_standby(&capture, !strcmp(argv[13], "installed"));
	} else if (!strcmp(capture.scenario, "grid")) {
		save_observation(&capture, "launcher-grid-fit");
	} else if (!strcmp(capture.scenario, "marquee")) {
		catalogue_marquee(&capture);
	} else if (!strcmp(capture.scenario, "battery")) {
		battery_states(&capture);
	} else if (!strcmp(capture.scenario, "playback")) {
		playback_tasks(&capture);
	} else if (!strcmp(capture.scenario, "ring")) {
		ring_answers(&capture);
	} else if (!strncmp(capture.scenario, "notice-", 7)) {
		system_notice(&capture);
	} else if (!strcmp(capture.scenario, "rows")) {
		catalogue_rows(&capture);
	} else {
		save_frame(&capture, "launcher-list-first");
		catalogue_navigation(&capture, number(argv[10]));
		launch_and_notice(&capture);
		task_overlay(&capture);
	}
	close_and_reap(&capture);
	if (capture.bluez)
		bluez_fake_stop(capture.bluez);
	return 0;
}
