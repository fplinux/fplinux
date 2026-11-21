/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "bluez-fake.h"
#include "settings-observer.h"
#include "fplinux-ui-profile.h"
#include "screen-frame.h"
#include "ui-frame-peer.h"

#include <assert.h>
#include <errno.h>
#include <linux/input-event-codes.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

struct capture {
	struct fplinux_ui_frame_peer peer;
	struct settings_frame_observation observed;
	struct fplinux_ui_session_message request;
	int listener;
	int session;
	int observer;
	pid_t application;
	uint32_t serial;
	uint32_t redraw_ack;
	unsigned int changes;
	bool initial_media_unknown;
	struct fplinux_system_levels system_levels;
	char close_value[16];
	uint32_t close_serial;
	unsigned int close_replies;
	bool keep_screen_on;
	bool has_request;
	uint32_t wifi_serial;
	struct fplinux_ui_wifi_request wifi_request;
	char left[128];
	char centre[128];
	char right[128];
	char input_locale[FPLINUX_UI_SESSION_LOCALE_BYTES];
	const char *directory;
	const char *session_path;
	/* The Bluetooth scenario's BlueZ; other scenarios have no bus. */
	bool bluetooth;
	struct bluez_fake *bluez;
	unsigned int document_requests;
	char document_path[FPLINUX_UI_SESSION_PATH_BYTES];
	const char *stage;
};

#define HCI0 "/org/bluez/hci0"
#define PHONE HCI0 "/dev_11_22_33_44_55_66"
#define VISITOR HCI0 "/dev_22_33_44_55_66_77"
#define SPEAKER HCI0 "/dev_33_44_55_66_77_88"
#define TRANSFER_PEER HCI0 "/dev_01_23_45_67_89_AB"

static const char warning[] =
	"Changing this setting closes other apps. Unsaved changes may be lost.";
static const char theme_error[] =
	"The setting is applied for this session. It was not saved.\n"
	"Not enough storage space";
static const char locale_error[] =
	"Настройка применена. Сохранение не подтверждено.\n"
	"Ошибка ввода-вывода хранилища";

static void read_observations(struct capture *capture)
{
	struct settings_frame_observation observation;
	ssize_t bytes;

	while ((bytes = recv(capture->observer, &observation,
			     sizeof(observation), MSG_DONTWAIT)) > 0) {
		assert(bytes == (ssize_t)sizeof(observation));
		capture->observed = observation;
	}
	assert(bytes == -1 && (errno == EAGAIN || errno == EWOULDBLOCK));
}

static void send_level(struct capture *capture,
		       enum fplinux_system_level_kind kind,
		       enum fplinux_system_level_availability availability,
		       unsigned int value)
{
	struct fplinux_ui_session_message message = {
		.type = FPLINUX_UI_SESSION_SYSTEM_LEVELS,
	};

	capture->system_levels.level[kind].availability = availability;
	capture->system_levels.level[kind].value = value;
	message.system_levels = capture->system_levels;
	assert(fplinux_ui_session_send(capture->session, &message));
}

static void answer_query(struct capture *capture,
			 const struct fplinux_ui_session_message *query)
{
	struct fplinux_ui_session_message reply = {
		.type = FPLINUX_UI_SESSION_SETTING,
		.applied = true,
		.storage_state = FPLINUX_STORAGE_DURABLE,
	};
	const char *value;

	assert(strcmp(query->name, "brightness") &&
	       strcmp(query->name, "volume") &&
	       strcmp(query->name, "alarm_volume"));
	if (!strcmp(query->name, "screen_off_seconds"))
		value = "30";
	else if (!strcmp(query->name, "auto_lock_seconds"))
		value = "0";
	else if (!strcmp(query->name, "audio_output"))
		value = "AUTO";
	else if (!strcmp(query->name, "audio_outputs"))
		value = "AUTO,HEADPHONES";
	else {
		assert(!strcmp(query->name, "input_locale"));
		value = capture->input_locale;
	}
	strcpy(reply.name, query->name);
	strcpy(reply.value, value);
	assert(fplinux_ui_session_send(capture->session, &reply));
}

/* Only the session owner is controlled; Settings and its UI remain real. */
static void read_messages(struct capture *capture)
{
	struct fplinux_ui_session_message message;
	int received;

	while ((received = fplinux_ui_session_receive(capture->session,
						      &message)) == 1) {
		assert(message.type != FPLINUX_UI_SESSION_EXIT);
		if (message.type == FPLINUX_UI_SESSION_SETTING) {
			if (!message.value[0]) {
				answer_query(capture, &message);
			} else {
				assert(!capture->has_request);
				capture->request = message;
				capture->has_request = true;
				++capture->changes;
			}
		} else if (message.type == FPLINUX_UI_SESSION_SOFTKEYS) {
			strcpy(capture->left, message.left);
			strcpy(capture->centre, message.centre);
			strcpy(capture->right, message.right);
		} else if (message.type == FPLINUX_UI_SESSION_WIFI_REQUEST) {
			assert(!capture->wifi_serial);
			capture->wifi_serial = message.serial;
			capture->wifi_request = message.wifi_request;
		} else if (message.type == FPLINUX_UI_SESSION_REDRAW_ACK) {
			capture->redraw_ack = message.serial;
		} else if (message.type == FPLINUX_UI_SESSION_CLOSE_REPLY) {
			strcpy(capture->close_value, message.value);
			capture->close_serial = message.serial;
			++capture->close_replies;
		} else if (message.type == FPLINUX_UI_SESSION_KEEP_SCREEN_ON) {
			capture->keep_screen_on = message.enabled;
		} else if (message.type == FPLINUX_UI_SESSION_STORAGE_REQUEST) {
			struct fplinux_ui_session_message reply = {
				.type = FPLINUX_UI_SESSION_STORAGE_RESULT,
				.serial = message.serial,
				.volume_result = {
					.request_id = message.serial,
					.snapshot.home_persistent = true,
				},
			};

			assert(message.volume_operation ==
			       FPLINUX_VOLUME_REFRESH);
			assert(fplinux_ui_session_send(capture->session,
						       &reply));
		} else if (message.type == FPLINUX_UI_SESSION_OPEN_FILE) {
			struct fplinux_ui_session_message reply = {
				.type = FPLINUX_UI_SESSION_DOCUMENT_RESULT,
				.serial = message.serial,
				.document_status =
					FPLINUX_UI_DOCUMENT_CANCELLED,
			};

			++capture->document_requests;
			snprintf(capture->document_path,
				 sizeof(capture->document_path), "%s",
				 message.path);
			assert(fplinux_ui_session_send(capture->session,
						       &reply));
		}
	}
	assert(received == 0);
}

static void pump(struct capture *capture)
{
	fplinux_ui_frame_pump(&capture->peer);
	if (capture->bluez)
		bluez_fake_pump(capture->bluez);
	read_observations(capture);
	read_messages(capture);
}

static void pump_for(struct capture *capture, unsigned int milliseconds)
{
	uint64_t deadline = fplinux_ui_frame_now_ms() + milliseconds;

	while (fplinux_ui_frame_now_ms() < deadline)
		pump(capture);
}

static bool has_label(const struct capture *capture, const char *text)
{
	char line[1024];

	assert(snprintf(line, sizeof(line), "\n%s\n", text) <
	       (int)sizeof(line));
	return strstr(capture->observed.labels, line) != NULL;
}

static void wait_label(struct capture *capture, const char *text)
{
	uint64_t deadline = fplinux_ui_frame_now_ms() + 3000;

	while (!has_label(capture, text) &&
	       fplinux_ui_frame_now_ms() < deadline)
		pump(capture);
	if (!has_label(capture, text))
		fprintf(stderr, "Settings (%s): missing [%s], actual:%s\n",
			capture->stage ? capture->stage : "navigation", text,
			capture->observed.labels);
	assert(has_label(capture, text));
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
			"Settings legends (%s): [%s | %s | %s], expected [%s | %s | %s]\n",
			capture->stage ? capture->stage : "navigation",
			capture->left, capture->centre, capture->right, left,
			centre, right);
	assert(!strcmp(capture->left, left) &&
	       !strcmp(capture->centre, centre) &&
	       !strcmp(capture->right, right));
}

static void press(struct capture *capture, unsigned int key)
{
	struct fplinux_ui_frame_keyboard *keyboard =
		&capture->peer.keyboards[1];

	fplinux_ui_frame_send_key(keyboard, key, true);
	fplinux_ui_frame_send_key(keyboard, key, false);
	pump(capture);
}

static void hold(struct capture *capture, unsigned int key)
{
	struct fplinux_ui_frame_keyboard *keyboard =
		&capture->peer.keyboards[1];

	fplinux_ui_frame_send_key(keyboard, key, true);
	pump_for(capture, 600);
	fplinux_ui_frame_send_key(keyboard, key, false);
	pump(capture);
}

static void wait_form_text(struct capture *capture, const char *text)
{
	uint64_t deadline = fplinux_ui_frame_now_ms() + 3000;

	while (strcmp(capture->observed.form_text, text) &&
	       fplinux_ui_frame_now_ms() < deadline)
		pump(capture);
	if (strcmp(capture->observed.form_text, text))
		fprintf(stderr, "Settings form text: [%s], expected [%s]\n",
			capture->observed.form_text, text);
	assert(!strcmp(capture->observed.form_text, text));
}

static void expect_request(struct capture *capture, const char *name,
			   const char *value)
{
	uint64_t deadline = fplinux_ui_frame_now_ms() + 3000;

	while (!capture->has_request && fplinux_ui_frame_now_ms() < deadline)
		pump(capture);
	assert(capture->has_request);
	assert(!strcmp(capture->request.name, name));
	assert(!strcmp(capture->request.value, value));
}

static size_t count_color(const struct capture *capture, uint16_t color)
{
	size_t pixels = (size_t)capture->peer.width * capture->peer.height;
	size_t count = 0;

	for (size_t index = 0; index < pixels; ++index)
		count += capture->peer.pixels[index] == color;
	return count;
}

static void expect_palette(struct capture *capture, bool dark, bool notice)
{
	size_t pixels = (size_t)capture->peer.width * capture->peer.height;
	uint16_t expected = notice ? (dark ? 0x2945 : 0xe73c) :
				     (dark ? 0x1082 : 0xf7be);
	uint16_t previous = notice ? (dark ? 0xe73c : 0x2945) :
				     (dark ? 0xf7be : 0x1082);

	assert(count_color(capture, expected) > pixels / 8);
	assert(count_color(capture, previous) < pixels / 20);
}

/* A new painted view cannot be acknowledged while its compositor is paused. */
static void wait_painted_without_commit(struct capture *capture,
					unsigned int previous_frame,
					unsigned int previous_commit)
{
	struct pollfd descriptors[] = {
		{ .fd = capture->observer, .events = POLLIN },
		{ .fd = capture->session, .events = POLLIN },
	};
	uint64_t deadline = fplinux_ui_frame_now_ms() + 3000;
	uint64_t hold_until = 0;

	while (fplinux_ui_frame_now_ms() < deadline) {
		assert(poll(descriptors, 2, 5) >= 0);
		read_observations(capture);
		read_messages(capture);
		assert(capture->redraw_ack != capture->serial);
		assert(capture->peer.commits == previous_commit);
		if (capture->observed.frame > previous_frame && !hold_until)
			hold_until = fplinux_ui_frame_now_ms() + 100;
		if (hold_until && fplinux_ui_frame_now_ms() >= hold_until)
			return;
	}
	assert(!"Settings did not paint while its compositor was paused");
}

static void reply_and_check_frame(struct capture *capture, const char *name,
				  const char *value,
				  enum fplinux_storage_state state, int error,
				  const char *locale, const char *title,
				  const char *body, bool dark)
{
	struct fplinux_ui_session_message reply = {
		.type = error ? FPLINUX_UI_SESSION_ERROR :
				FPLINUX_UI_SESSION_SETTING,
		.serial = ++capture->serial,
		.applied = true,
		.storage_state = state,
		.storage_error = error,
	};
	unsigned int previous_commit = capture->peer.commits;
	unsigned int previous_frame = capture->observed.frame;
	uint64_t deadline;

	strcpy(reply.name, name);
	strcpy(reply.value, value);
	if (error)
		strcpy(reply.reason, "save-failed");
	capture->has_request = false;
	assert(fplinux_ui_session_send(capture->session, &reply));
	wait_painted_without_commit(capture, previous_frame, previous_commit);
	deadline = fplinux_ui_frame_now_ms() + 3000;
	while (capture->redraw_ack != reply.serial &&
	       fplinux_ui_frame_now_ms() < deadline)
		pump(capture);
	assert(capture->redraw_ack == reply.serial);
	read_observations(capture);
	assert(capture->peer.commits > previous_commit);
	assert(capture->observed.frame > previous_frame);
	assert(capture->observed.theme == (dark ? 1 : 0));
	assert(!strcmp(capture->observed.locale, locale));
	assert(has_label(capture, title));
	if (body)
		assert(has_label(capture, body));
	expect_palette(capture, dark, body != NULL);
}

static void save_frame(struct capture *capture, const char *name)
{
	char path[512];

	assert(snprintf(path, sizeof(path), "%s/%s.rgb565", capture->directory,
			name) < (int)sizeof(path));
	assert(screen_frame_write(path, capture->peer.pixels,
				  capture->peer.width, capture->peer.height,
				  capture->peer.width *
					  sizeof(*capture->peer.pixels)));
}

static void start(struct capture *capture, char **arguments)
{
	const char *width = getenv("FPLINUX_SETTINGS_TEST_WIDTH");
	const char *height = getenv("FPLINUX_SETTINGS_TEST_HEIGHT");
	const char *locale = getenv("FPLINUX_SETTINGS_TEST_LOCALE");
	const char *theme = getenv("FPLINUX_SETTINGS_TEST_THEME");
	struct fplinux_ui_session_message config = {
		.type = FPLINUX_UI_SESSION_CONFIG,
		.mode = FPLINUX_UI_SESSION_INTEGRATED,
		.theme = theme && !strcmp(theme, "light") ?
				 FPLINUX_UI_SESSION_THEME_LIGHT :
				 FPLINUX_UI_SESSION_THEME_DARK,
		.screen_width = width ? (unsigned int)atoi(width) : 240,
		.screen_height = height ? (unsigned int)atoi(height) : 320,
	};
	const struct fplinux_ui_profile_spec *profile = fplinux_ui_profile_spec(
		fplinux_ui_profile_for_screen(config.screen_width,
					      config.screen_height));
	struct fplinux_ui_session_message hello;
	int wayland[2], observer[2];

	assert(profile);
	capture->directory = arguments[4];
	capture->session_path = arguments[3];
	capture->listener = fplinux_ui_frame_listen_session(arguments[3]);
	fplinux_ui_frame_socketpair(wayland);
	assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, observer) == 0);
	capture->application = fork();
	assert(capture->application >= 0);
	if (!capture->application) {
		char descriptor[32], catalogues[512], bus[512];
		const char *slash = strrchr(arguments[1], '/');

		assert(slash);
		close(capture->listener);
		close(wayland[0]);
		close(observer[0]);
		fplinux_ui_frame_client_environment(wayland[1], arguments[3]);
		snprintf(descriptor, sizeof(descriptor), "%d", observer[1]);
		assert(setenv("FPLINUX_SETTINGS_OBSERVER_FD", descriptor, 1) ==
		       0);
		if (arguments[6] && !strcmp(arguments[6], "wifi"))
			assert(setenv("FPLINUX_SETTINGS_TEST_WIFI", "1", 1) ==
			       0);
		assert(snprintf(catalogues, sizeof(catalogues), "%.*s/locale",
				(int)(slash - arguments[1]),
				arguments[1]) < (int)sizeof(catalogues));
		assert(setenv("FPLINUX_SETTINGS_LOCALE_DIR", catalogues, 1) ==
		       0);
		assert(snprintf(catalogues, sizeof(catalogues),
				"%.*s/icons/usr/share/fplinux/glyphs",
				(int)(slash - arguments[1]),
				arguments[1]) < (int)sizeof(catalogues));
		if (access(catalogues, R_OK) == 0)
			assert(setenv("FPLINUX_NATIVE_GLYPHS", catalogues, 1) ==
			       0);
		assert(setenv("FPLINUX_SETTINGS_LANGUAGE_DIR", arguments[5],
			      1) == 0);
		/* Without the scenario's bus, no system bus is reachable. */
		if (!capture->bluetooth) {
			assert(snprintf(bus, sizeof(bus),
					"unix:path=%s/absent-bus",
					arguments[4]) < (int)sizeof(bus));
			assert(setenv("DBUS_SYSTEM_BUS_ADDRESS", bus, 1) == 0);
		}
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
	assert(!strcmp(hello.app_id, "org.fplinux.settings"));
	strcpy(config.locale, locale ? locale : "en");
	strcpy(config.input_locale, config.locale);
	strcpy(capture->input_locale, config.input_locale);
	assert(fplinux_ui_session_send(capture->session, &config));
	capture->system_levels = (struct fplinux_system_levels){
		.level = {
			[FPLINUX_SYSTEM_LEVEL_MEDIA] = {
				.maximum = 10,
			},
			[FPLINUX_SYSTEM_LEVEL_ALARM] = {
				.availability = FPLINUX_SYSTEM_LEVEL_READY,
				.maximum = 10,
				.value = 8,
			},
			[FPLINUX_SYSTEM_LEVEL_BRIGHTNESS] = {
				.availability = FPLINUX_SYSTEM_LEVEL_READY,
				.minimum = 1,
				.maximum = 10,
				.value = 7,
			},
		},
	};
	send_level(capture, FPLINUX_SYSTEM_LEVEL_MEDIA,
		   capture->initial_media_unknown ?
			   FPLINUX_SYSTEM_LEVEL_UNKNOWN :
			   FPLINUX_SYSTEM_LEVEL_READY,
		   0);
	wait_label(capture,
		   !strcmp(config.locale, "ru") ? "Настройки" : "Settings");
	wait_legends(capture, "",
		     !strcmp(config.locale, "ru") ? "Открыть" : "Open",
		     !strcmp(config.locale, "ru") ? "Назад" : "Back");
	assert(capture->peer.keyboards[1].keyboard);
	fplinux_ui_frame_focus_keyboard(&capture->peer.keyboards[1]);
	pump_for(capture, 100);
}

static void reap_closed(struct capture *capture)
{
	uint64_t deadline = fplinux_ui_frame_now_ms() + 3000;
	pid_t finished;
	int status;

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

static void close_and_reap(struct capture *capture)
{
	struct fplinux_ui_session_message message = {
		.type = FPLINUX_UI_SESSION_CLOSE,
	};

	strcpy(message.reason, "capture-complete");
	assert(fplinux_ui_session_send(capture->session, &message));
	reap_closed(capture);
}

static void open_theme(struct capture *capture, bool from_root)
{
	if (from_root) {
		press(capture, KEY_OK);
		wait_label(capture, "Display");
		for (unsigned int index = 0; index < 3; ++index)
			press(capture, KEY_DOWN);
	}
	press(capture, KEY_OK);
	wait_label(capture, "Light");
	wait_label(capture, "Dark");
	wait_legends(capture, "", "Select", "Back");
}

static void pending_choice_retains_applied_marker(struct capture *capture,
						  uint32_t selected)
{
	unsigned int changes = capture->changes;

	pump_for(capture, 100);
	assert(capture->observed.selection_mask == selected);
	wait_label(capture, "Applying…");
	wait_legends(capture, "", "", "");
	press(capture, KEY_UP);
	press(capture, KEY_OK);
	pump_for(capture, 100);
	assert(capture->observed.selection_mask == selected);
	press(capture, KEY_DOWN);
	press(capture, KEY_OK);
	press(capture, KEY_F14);
	pump_for(capture, 100);
	assert(capture->observed.selection_mask == selected);
	assert(capture->changes == changes);
	wait_label(capture, "Applying…");
	wait_legends(capture, "", "", "");
}

static void wait_time_request(struct capture *capture)
{
	uint64_t deadline = fplinux_ui_frame_now_ms() + 3000;

	while (!capture->has_request && fplinux_ui_frame_now_ms() < deadline)
		pump(capture);
	assert(capture->has_request && !strcmp(capture->request.name, "time"));
}

static void home_and_resume_inline_edit(struct capture *capture, uint64_t item)
{
	struct fplinux_ui_session_message message = {
		.type = FPLINUX_UI_SESSION_LIFECYCLE,
		.lifecycle = FPLINUX_UI_SESSION_BACKGROUND,
	};
	unsigned int frame;

	pump_for(capture, 100);
	assert(capture->observed.editing &&
	       capture->observed.focused_item == item);
	frame = capture->observed.frame;
	assert(fplinux_ui_session_send(capture->session, &message));
	message.type = FPLINUX_UI_SESSION_VISIBILITY;
	message.visible = false;
	assert(fplinux_ui_session_send(capture->session, &message));
	pump_for(capture, 100);
	message.type = FPLINUX_UI_SESSION_LIFECYCLE;
	message.lifecycle = FPLINUX_UI_SESSION_FOREGROUND;
	assert(fplinux_ui_session_send(capture->session, &message));
	message.type = FPLINUX_UI_SESSION_VISIBILITY;
	message.visible = true;
	assert(fplinux_ui_session_send(capture->session, &message));
	pump_for(capture, 100);
	assert(capture->observed.frame > frame);
	assert(capture->observed.editing &&
	       capture->observed.focused_item == item);
}

static void save_date_form(struct capture *capture)
{
	wait_legends(capture, "", "Edit", "Back");
	press(capture, KEY_DOWN);
	wait_label(capture, "Save");
	wait_legends(capture, "", "Save", "Back");
	press(capture, KEY_OK);
}

static void
date_draft_survives_pending_home_and_refusal(struct capture *capture)
{
	struct fplinux_ui_session_message message = { 0 };
	struct fplinux_ui_session_message request;
	const unsigned int date_digits[] = { KEY_NUMERIC_0, KEY_NUMERIC_2,
					     KEY_NUMERIC_2, KEY_NUMERIC_9,
					     KEY_NUMERIC_2, KEY_NUMERIC_0,
					     KEY_NUMERIC_2, KEY_NUMERIC_4 };
	const unsigned int time_digits[] = { KEY_NUMERIC_0, KEY_NUMERIC_8,
					     KEY_NUMERIC_4, KEY_NUMERIC_5 };
	int64_t date, time;
	unsigned int changes;

	press(capture, KEY_OK);
	wait_label(capture, "Time (UTC)");
	press(capture, KEY_OK);
	for (size_t index = 0;
	     index < sizeof(date_digits) / sizeof(date_digits[0]); ++index)
		press(capture, date_digits[index]);
	home_and_resume_inline_edit(capture, 0);
	press(capture, KEY_OK);
	press(capture, KEY_DOWN);
	press(capture, KEY_OK);
	for (size_t index = 0;
	     index < sizeof(time_digits) / sizeof(time_digits[0]); ++index)
		press(capture, time_digits[index]);
	home_and_resume_inline_edit(capture, 1);
	press(capture, KEY_OK);
	pump_for(capture, 100);
	assert(!capture->observed.editing &&
	       capture->observed.focused_item == 1);
	date = capture->observed.date_value;
	time = capture->observed.time_value;
	save_date_form(capture);
	wait_time_request(capture);
	request = capture->request;
	assert(!strcmp(request.value, "2024-02-29 08:45:00"));
	changes = capture->changes;
	wait_label(capture, "Saving…");
	wait_legends(capture, "", "Edit", "");
	assert(!has_label(capture, "Save"));
	press(capture, KEY_DOWN);
	pump_for(capture, 100);
	assert(capture->changes == changes);
	message.type = FPLINUX_UI_SESSION_LIFECYCLE;
	message.lifecycle = FPLINUX_UI_SESSION_BACKGROUND;
	assert(fplinux_ui_session_send(capture->session, &message));
	message.type = FPLINUX_UI_SESSION_VISIBILITY;
	message.visible = false;
	assert(fplinux_ui_session_send(capture->session, &message));
	message = request;
	message.type = FPLINUX_UI_SESSION_ERROR;
	strcpy(message.reason, "not-foreground");
	capture->has_request = false;
	assert(fplinux_ui_session_send(capture->session, &message));
	pump_for(capture, 100);
	message = (struct fplinux_ui_session_message){
		.type = FPLINUX_UI_SESSION_LIFECYCLE,
		.lifecycle = FPLINUX_UI_SESSION_FOREGROUND,
	};
	assert(fplinux_ui_session_send(capture->session, &message));
	message.type = FPLINUX_UI_SESSION_VISIBILITY;
	message.visible = true;
	assert(fplinux_ui_session_send(capture->session, &message));
	wait_label(capture, "The setting was not changed. Try again.");
	press(capture, KEY_F14);
	wait_legends(capture, "", "Edit", "Back");
	pump_for(capture, 100);
	assert(capture->observed.date_value == date);
	assert(capture->observed.time_value == time);
	assert(capture->observed.focused_item == 1);
	message = (struct fplinux_ui_session_message){
		.type = FPLINUX_UI_SESSION_PREPARE_CLOSE,
		.serial = 800,
	};
	strcpy(message.reason, "user-close");
	assert(fplinux_ui_session_send(capture->session, &message));
	wait_label(capture, "Leave without saving?");
	assert(!strcmp(capture->close_value, "wait"));
	press(capture, KEY_F14);
	wait_legends(capture, "", "Edit", "Back");
	pump_for(capture, 100);
	assert(!strcmp(capture->close_value, "cancel"));
	assert(capture->observed.date_value == date &&
	       capture->observed.time_value == time);
	save_date_form(capture);
	expect_request(capture, "time", request.value);
	message = request;
	message.type = FPLINUX_UI_SESSION_SETTING;
	message.applied = true;
	/* The manager formats the live clock after setting it, rather than echoing input. */
	message.setting.operation = FPLINUX_UI_SETTING_RESULT;
	message.setting.value.time.second = 1;
	capture->has_request = false;
	assert(fplinux_ui_session_send(capture->session, &message));
	wait_label(capture, "Time & language");
	wait_legends(capture, "", "Open", "Back");
	puts("settings-date: real editors retain date, time and focus through pending Home, refusal and cancelled close; live-clock reply completes Save");
}

static void root_values_fit_and_follow_current_settings(struct capture *capture)
{
	bool russian = !strcmp(capture->observed.locale, "ru");
	bool landscape = atoi(getenv("FPLINUX_SETTINGS_TEST_WIDTH")) >
			 atoi(getenv("FPLINUX_SETTINGS_TEST_HEIGHT"));
	const char *expected[3] = {
		capture->observed.theme ? (russian ? "Тёмная" : "Dark") :
					  (russian ? "Светлая" : "Light"),
		russian ? "Русский" : "English",
		russian ? "Русский" : "English",
	};
	int column_right = 0;
	uint64_t deadline;
	struct fplinux_ui_session_message reply = {
		.type = FPLINUX_UI_SESSION_SETTING,
		.applied = true,
		.storage_state = FPLINUX_STORAGE_DURABLE,
	};

	for (size_t index = 0; index < 4; ++index) {
		const struct list_value_observation *row;

		if (index)
			press(capture, KEY_DOWN);
		if (index == 1)
			press(capture, KEY_DOWN);
		pump_for(capture, 80);
		row = &capture->observed.root_values[index];
		screen_check_list_value(row, landscape);
		if (landscape && index)
			assert(row->value_right == column_right);
		column_right = row->value_right;
		if (index < 3) {
			if (strcmp(row->value, expected[index]))
				fprintf(stderr,
					"Settings root row %zu: actual [%s], expected [%s]\n",
					index, row->value, expected[index]);
			assert(!strcmp(row->value, expected[index]));
			assert(row->value_text_width <=
			       row->value_right - row->value_left + 1);
		}
		save_frame(capture, index == 0 ? "settings-root-display" :
				    index == 1 ? "settings-root-language" :
				    index == 2 ? "settings-root-input" :
						 "settings-root-about");
	}
	deadline = fplinux_ui_frame_now_ms() + 5000;
	while (!capture->observed.root_values[3].value_scrolling &&
	       fplinux_ui_frame_now_ms() < deadline)
		pump(capture);
	assert(capture->observed.root_values[3].value_scrolling);
	assert(!strcmp(capture->observed.root_values[3].value,
		       getenv("FPLINUX_SETTINGS_TEST_SYSTEM_NAME")));
	assert(capture->observed.root_values[3].value_text_width >
	       capture->observed.root_values[3].value_right -
		       capture->observed.root_values[3].value_left + 1);
	for (size_t index = 3; index-- > 0;) {
		press(capture, KEY_UP);
		if (index == 0)
			press(capture, KEY_UP);
		pump_for(capture, 80);
		save_frame(capture, index == 0 ? "settings-root-up-display" :
				    index == 1 ? "settings-root-up-language" :
						 "settings-root-up-input");
	}
	strcpy(reply.name, "theme");
	strcpy(reply.value, capture->observed.theme ? "light" : "dark");
	assert(fplinux_ui_session_send(capture->session, &reply));
	wait_label(capture, !strcmp(reply.value, "light") ?
				    (russian ? "Светлая" : "Light") :
				    (russian ? "Тёмная" : "Dark"));
	press(capture, KEY_DOWN);
	press(capture, KEY_DOWN);
	press(capture, KEY_DOWN);
	strcpy(reply.name, "input_locale");
	strcpy(reply.value, russian ? "en" : "ru");
	strcpy(capture->input_locale, reply.value);
	assert(fplinux_ui_session_send(capture->session, &reply));
	deadline = fplinux_ui_frame_now_ms() + 3000;
	while (strcmp(capture->observed.root_values[2].value,
		      russian ? "English" : "Русский") &&
	       fplinux_ui_frame_now_ms() < deadline)
		pump(capture);
	assert(!strcmp(capture->observed.root_values[2].value,
		       russian ? "English" : "Русский"));
	puts("settings-root-values: real glyphs and current values fit; landscape shares a right column; long system name scrolls; replies refresh retained values");
}

static void wait_root_sound(struct capture *capture, const char *value)
{
	uint64_t deadline = fplinux_ui_frame_now_ms() + 3000;

	while (strcmp(capture->observed.root_sound.value, value) &&
	       fplinux_ui_frame_now_ms() < deadline)
		pump(capture);
	assert(!strcmp(capture->observed.root_sound.value, value));
}

static void root_sound_summary(struct capture *capture)
{
	bool russian = !strcmp(capture->observed.locale, "ru");
	bool landscape = capture->peer.width >
			 atoi(getenv("FPLINUX_SETTINGS_TEST_HEIGHT"));
	struct fplinux_ui_session_message lifecycle = {
		.type = FPLINUX_UI_SESSION_LIFECYCLE,
		.lifecycle = FPLINUX_UI_SESSION_BACKGROUND,
	};

	press(capture, KEY_DOWN);
	wait_root_sound(capture, russian ? "Недоступно" : "Unavailable");
	send_level(capture, FPLINUX_SYSTEM_LEVEL_MEDIA,
		   FPLINUX_SYSTEM_LEVEL_READY, 10);
	wait_root_sound(capture, russian ? "Медиа: 10/10" : "Media: 10/10");
	screen_check_list_value(&capture->observed.root_sound, landscape);
	save_frame(capture, "settings-root-sound-full");
	send_level(capture, FPLINUX_SYSTEM_LEVEL_MEDIA,
		   FPLINUX_SYSTEM_LEVEL_READY, 0);
	wait_root_sound(capture, russian ? "Медиа: 0/10" : "Media: 0/10");
	send_level(capture, FPLINUX_SYSTEM_LEVEL_ALARM,
		   FPLINUX_SYSTEM_LEVEL_READY, 8);
	pump_for(capture, 80);
	wait_root_sound(capture, russian ? "Медиа: 0/10" : "Media: 0/10");
	save_frame(capture, "settings-root-sound-zero");

	press(capture, KEY_OK);
	wait_label(capture, russian ? "Громкость медиа" : "Media volume");
	send_level(capture, FPLINUX_SYSTEM_LEVEL_MEDIA,
		   FPLINUX_SYSTEM_LEVEL_READY, 4);
	wait_label(capture, "4");
	press(capture, KEY_F14);
	wait_root_sound(capture, russian ? "Медиа: 4/10" : "Media: 4/10");
	assert(fplinux_ui_session_send(capture->session, &lifecycle));
	send_level(capture, FPLINUX_SYSTEM_LEVEL_MEDIA,
		   FPLINUX_SYSTEM_LEVEL_READY, 7);
	lifecycle.lifecycle = FPLINUX_UI_SESSION_FOREGROUND;
	assert(fplinux_ui_session_send(capture->session, &lifecycle));
	wait_root_sound(capture, russian ? "Медиа: 7/10" : "Media: 7/10");
	send_level(capture, FPLINUX_SYSTEM_LEVEL_MEDIA,
		   FPLINUX_SYSTEM_LEVEL_UNAVAILABLE, 0);
	wait_root_sound(capture, russian ? "Недоступно" : "Unavailable");
	puts("settings-root-sound: applied snapshots update Media without queries and refresh before resume");
}

static void section_value_frames(struct capture *capture)
{
	static const char *const names[] = { "display", "time", "input" };
	static const unsigned int first[] = { 0, 4, 6 };
	static const unsigned int counts[] = { 4, 2, 1 };
	char path[1024], name[64];
	FILE *geometry;

	assert(snprintf(path, sizeof(path), "%s/section-values.txt",
			capture->directory) < (int)sizeof(path));
	geometry = fopen(path, "w");
	assert(geometry);
	for (size_t section = 0; section < 3; ++section) {
		if (section)
			press(capture, KEY_DOWN);
		if (section == 1)
			press(capture, KEY_DOWN);
		press(capture, KEY_OK);
		for (unsigned int index = 0; index < counts[section]; ++index) {
			const struct list_value_observation *row;

			if (index)
				press(capture, KEY_DOWN);
			pump_for(capture, 100);
			row = &capture->observed
				       .section_values[first[section] + index];
			assert(row->found && row->images == 1);
			assert(fprintf(geometry,
				       "%s %u %d %d %d %d %d %d %d %d %d %d\n",
				       names[section], index, row->row_top,
				       row->row_bottom, row->text_top,
				       row->text_bottom, row->value_top,
				       row->value_bottom, row->text_left,
				       row->text_right, row->value_left,
				       row->value_right) > 0);
			assert(snprintf(name, sizeof(name),
					"settings-section-%s-%u",
					names[section],
					index) < (int)sizeof(name));
			save_frame(capture, name);
		}
		press(capture, KEY_F14);
		pump_for(capture, 100);
	}
	assert(fclose(geometry) == 0);
	puts("settings-sections: captured actual value rows and geometry");
}

static void about_reading_edges(struct capture *capture)
{
	int previous = -1;

	for (int index = 0; index < 4; ++index)
		press(capture, KEY_DOWN);
	press(capture, KEY_OK);
	pump_for(capture, 200);
	for (int index = 0; index < 100; ++index) {
		const struct settings_frame_observation *view =
			&capture->observed;
		int clipped;

		press(capture, KEY_DOWN);
		pump_for(capture, 80);
		assert(view->about_memory_found && view->about_line_height > 0);
		clipped = view->about_memory_bottom - view->about_view_bottom;
		printf("about down %d: y=%d remaining=%d clipped=%d line=%d\n",
		       index + 1, view->about_scroll_y,
		       view->about_scroll_bottom, clipped,
		       view->about_line_height);
		if (index == 4)
			save_frame(capture, "settings-about-five-down");
		assert(clipped <= 0 || clipped >= view->about_line_height);
		if (view->about_scroll_y == previous)
			break;
		previous = view->about_scroll_y;
	}
	assert(capture->observed.about_scroll_bottom == 0);
	puts("settings-about: last text line is whole on every downward step");
}

static void sound_values_outputs_and_editors(struct capture *capture)
{
	bool russian = !strcmp(capture->observed.locale, "ru");
	const char *height = getenv("FPLINUX_SETTINGS_TEST_HEIGHT");
	bool landscape = height && capture->peer.width > atoi(height);
	const char *media = russian ? "Громкость медиа" : "Media volume";
	const char *alarm = russian ? "Громкость будильника" : "Alarm volume";
	char name[64];

	press(capture, KEY_DOWN);
	press(capture, KEY_OK);
	wait_label(capture, media);
	pump_for(capture, 150);
	for (size_t row = 0; row < 3; ++row) {
		const struct list_value_observation *value;

		if (row)
			press(capture, KEY_DOWN);
		pump_for(capture, 100);
		value = &capture->observed.sound_values[row];
		screen_check_list_value(value, landscape);
		assert(!strcmp(value->value,
			       row == 0 ? "0" :
			       row == 1 ? "8" :
					  (russian ? "Автоматически" :
						     "Automatic")));
		snprintf(name, sizeof(name), "settings-sound-row-%zu", row);
		save_frame(capture, name);
	}
	press(capture, KEY_OK);
	wait_label(capture, russian ? "Наушники" : "Headphones");
	assert(!has_label(capture, russian ? "Динамик" : "Speaker"));
	save_frame(capture, "settings-sound-output");
	press(capture, KEY_F14);
	press(capture, KEY_UP);
	press(capture, KEY_UP);
	press(capture, KEY_OK);
	wait_legends(capture, "", russian ? "Сохранить" : "Save",
		     russian ? "Отмена" : "Cancel");
	wait_label(capture, media);
	wait_label(capture,
		   russian ? "Влево/вправо: громкость. Сохранить — оставить." :
			     "Left/Right: volume. Save keeps the level.");
	save_frame(capture, "settings-sound-media");
	press(capture, KEY_F14);
	press(capture, KEY_DOWN);
	press(capture, KEY_OK);
	wait_legends(capture, "", russian ? "Сохранить" : "Save",
		     russian ? "Отмена" : "Cancel");
	wait_label(capture, alarm);
	save_frame(capture, "settings-sound-alarm");
	puts("settings-sound: Media and Alarm values, supported outputs and both volume editors are visible");
}

static bool shows_bluetooth(const struct capture *capture, const char *value)
{
	return capture->observed.bluetooth.found &&
	       !strcmp(capture->observed.bluetooth.value, value);
}

static void wait_bluetooth(struct capture *capture, const char *value)
{
	uint64_t deadline = fplinux_ui_frame_now_ms() + 3000;

	while (!shows_bluetooth(capture, value) &&
	       fplinux_ui_frame_now_ms() < deadline)
		pump(capture);
	if (!shows_bluetooth(capture, value))
		fprintf(stderr, "Settings Bluetooth row: [%s], expected [%s]\n",
			capture->observed.bluetooth.found ?
				capture->observed.bluetooth.value :
				"(none)",
			value);
	assert(shows_bluetooth(capture, value));
}

/* The real Settings against a BlueZ fake on the scenario's private bus. */
static void bluetooth_switch(struct capture *capture)
{
	bool landscape = atoi(getenv("FPLINUX_SETTINGS_TEST_WIDTH")) >
			 atoi(getenv("FPLINUX_SETTINGS_TEST_HEIGHT"));
	static const char *const root_frames[] = {
		"display", "bluetooth", "language", "input", "about",
	};
	char name[64];

	/* Reveal the adapter row on profiles that show only two root rows. */
	press(capture, KEY_DOWN);
	press(capture, KEY_DOWN);
	wait_bluetooth(capture, "Off");
	press(capture, KEY_UP);
	press(capture, KEY_UP);
	for (size_t index = 0; index < 5; ++index) {
		if (index)
			press(capture, KEY_DOWN);
		if (index == 1)
			press(capture, KEY_DOWN);
		pump_for(capture, 80);
		snprintf(name, sizeof(name), "settings-bluetooth-root-%s",
			 root_frames[index]);
		save_frame(capture, name);
	}
	for (size_t index = 4; index-- > 1;) {
		press(capture, KEY_UP);
		pump_for(capture, 80);
		snprintf(name, sizeof(name), "settings-bluetooth-root-up-%s",
			 root_frames[index]);
		save_frame(capture, name);
	}
	screen_check_list_value(&capture->observed.bluetooth, landscape);
	press(capture, KEY_OK);
	wait_legends(capture, "", "Turn on", "Back");
	wait_label(capture, "Off");
	wait_label(capture, "Power");
	save_frame(capture, "settings-bluetooth-off");

	/* Another client switches it on; the screen follows. */
	bluez_fake_set_powered(capture->bluez, HCI0, true);
	wait_label(capture, "On");
	wait_legends(capture, "", "Turn off", "Back");
	save_frame(capture, "settings-bluetooth-on");

	/* A slow change covers the screen with a busy alert. */
	bluez_fake_answer(capture->bluez, BLUEZ_FAKE_APPLY, 1500);
	press(capture, KEY_OK);
	wait_label(capture, "Turning Bluetooth off…");
	wait_legends(capture, "", "", "");
	pump_for(capture, 100);
	save_frame(capture, "settings-bluetooth-busy");
	wait_legends(capture, "", "Turn on", "Back");
	wait_label(capture, "Off");
	assert(!has_label(capture, "Turning Bluetooth off…"));
	assert(bluez_fake_requests(capture->bluez) == 1);

	bluez_fake_answer(capture->bluez, BLUEZ_FAKE_FAIL, 0);
	press(capture, KEY_OK);
	wait_label(capture, "Cannot turn Bluetooth on. Try again.");
	wait_legends(capture, "", "", "Close");
	pump_for(capture, 100);
	save_frame(capture, "settings-bluetooth-error");
	press(capture, KEY_F14);
	wait_legends(capture, "", "Turn on", "Back");
	wait_label(capture, "Off");
	assert(!bluez_fake_powered(capture->bluez, HCI0));

	/* bluetoothd stops: the switch is unavailable and the root row goes. */
	bluez_fake_stop(capture->bluez);
	capture->bluez = NULL;
	wait_label(capture, "Unavailable");
	wait_legends(capture, "", "", "Back");
	save_frame(capture, "settings-bluetooth-unavailable");
	press(capture, KEY_F14);
	wait_legends(capture, "", "Open", "Back");
	pump_for(capture, 100);
	assert(!capture->observed.bluetooth.found);
	assert(!has_label(capture, "Bluetooth"));

	/* It starts again with the adapter on: the row returns above the focus. */
	capture->bluez = bluez_fake_start();
	bluez_fake_add_adapter(capture->bluez, HCI0, true);
	pump_for(capture, 200);
	press(capture, KEY_UP);
	wait_bluetooth(capture, "On");
	puts("settings-bluetooth: the switch follows BlueZ, waits for slow changes, reports refusals and goes with bluetoothd");
}

static void wait_pairing(struct capture *capture, const char *path)
{
	uint64_t deadline = fplinux_ui_frame_now_ms() + 3000;

	while (!bluez_fake_device_paired(capture->bluez, path) &&
	       fplinux_ui_frame_now_ms() < deadline)
		pump(capture);
	assert(bluez_fake_device_paired(capture->bluez, path));
	wait_legends(capture, "", "Select", "Back");
}

static void wait_screen_hold(struct capture *capture, bool enabled)
{
	uint64_t deadline = fplinux_ui_frame_now_ms() + 3000;

	while (capture->keep_screen_on != enabled &&
	       fplinux_ui_frame_now_ms() < deadline)
		pump(capture);
	assert(capture->keep_screen_on == enabled);
}

static void wait_scan_controls(struct capture *capture)
{
	wait_legends(capture, "Options", "Select", "Stop");
}

static void bluetooth_name_wait_retains_editor(struct capture *capture)
{
	press(capture, KEY_DOWN);
	press(capture, KEY_DOWN);
	press(capture, KEY_OK);
	wait_label(capture, "Name");
	hold(capture, KEY_NUMERIC_POUND);
	wait_label(capture, "123");
	press(capture, KEY_NUMERIC_1);
	press(capture, KEY_NUMERIC_2);
	press(capture, KEY_NUMERIC_3);
	wait_form_text(capture, "Test adapter123");
	wait_legends(capture, "Options", "", "Clear");
	bluez_fake_answer(capture->bluez, BLUEZ_FAKE_FAIL, 1000);
	press(capture, KEY_DOWN);
	wait_legends(capture, "", "Save", "Back");
	press(capture, KEY_OK);
	pump_for(capture, 150);
	assert(has_label(capture, "Name"));
	assert(!strcmp(capture->observed.form_text, "Test adapter123"));
	assert(!has_label(capture, "Power"));
	wait_label(capture, "Saving name…");
	wait_legends(capture, "", "", "Close");
	press(capture, KEY_F14);
	wait_label(capture, "Name");
	wait_form_text(capture, "Test adapter123");
	wait_legends(capture, "", "Save", "Back");
	bluez_fake_answer(capture->bluez, BLUEZ_FAKE_APPLY, 1000);
	press(capture, KEY_OK);
	pump_for(capture, 150);
	assert(has_label(capture, "Name"));
	assert(!strcmp(capture->observed.form_text, "Test adapter123"));
	assert(!has_label(capture, "Power"));
	wait_label(capture, "Saving name…");
	wait_legends(capture, "", "Open", "Back");
	wait_label(capture, "Test adapter123");
	bluez_fake_answer(capture->bluez, BLUEZ_FAKE_APPLY, 0);
	press(capture, KEY_UP);
	press(capture, KEY_UP);
}

static void bluetooth_pair_with_passkey(struct capture *capture)
{
	uint64_t deadline;

	bluez_fake_request_agent(capture->bluez, BLUEZ_FAKE_AGENT_PASSKEY,
				 VISITOR, 0, 0, NULL);
	wait_label(capture, "Passkey");
	wait_legends(capture, "", "", "Reject");
	for (unsigned int key = KEY_NUMERIC_1; key <= KEY_NUMERIC_6; ++key)
		press(capture, key);
	wait_legends(capture, "Options", "", "Clear");
	press(capture, KEY_DOWN);
	wait_label(capture, "Pair");
	wait_legends(capture, "", "Pair", "Reject");
	press(capture, KEY_OK);
	deadline = fplinux_ui_frame_now_ms() + 3000;
	while (!bluez_fake_agent_answered(capture->bluez) &&
	       fplinux_ui_frame_now_ms() < deadline)
		pump(capture);
	assert(bluez_fake_agent_answered(capture->bluez));
	assert(!bluez_fake_agent_rejected(capture->bluez));
	assert(!strcmp(bluez_fake_agent_code(capture->bluez), "123456"));
	wait_legends(capture, "", "Open", "Back");
}

/* Navigate the real device inventory; only the radio daemon is replaced. */
static void bluetooth_devices(struct capture *capture)
{
	struct fplinux_ui_session_message lifecycle = {
		.type = FPLINUX_UI_SESSION_LIFECYCLE,
		.lifecycle = FPLINUX_UI_SESSION_BACKGROUND,
	};
	const char *const services[] = {
		"00001105-0000-1000-8000-00805f9b34fb",
		"0000110b-0000-1000-8000-00805f9b34fb",
		"0000180f-0000-1000-8000-00805f9b34fb",
		"dc02a3f0-6d83-49ef-8c6c-7523a533beba",
	};
	uint64_t deadline;

	bluez_fake_add_device(capture->bluez, PHONE, "11:22:33:44:55:66",
			      "Office phone", false);
	bluez_fake_add_device(capture->bluez, VISITOR, "22:33:44:55:66:77",
			      "Visitor", false);
	bluez_fake_set_device_rssi(capture->bluez, VISITOR, -52, true);
	bluez_fake_set_device_battery(capture->bluez, VISITOR, 73, true);
	bluez_fake_set_device_uuids(capture->bluez, VISITOR, services, 4);
	bluez_fake_add_device(capture->bluez, SPEAKER, "33:44:55:66:77:88",
			      "Desk speaker", true);
	bluez_fake_set_external_discovery(capture->bluez, HCI0, true);
	pump_for(capture, 150);
	press(capture, KEY_DOWN);
	press(capture, KEY_DOWN);
	wait_bluetooth(capture, "On");
	press(capture, KEY_OK);
	wait_label(capture, "Power");
	bluetooth_name_wait_retains_editor(capture);
	save_frame(capture, "settings-bluetooth-root");
	for (size_t i = 0; i < 3; ++i)
		press(capture, KEY_DOWN);
	bluez_fake_answer(capture->bluez, BLUEZ_FAKE_APPLY, 700);
	press(capture, KEY_OK);
	wait_label(capture, "Starting search…");
	wait_label(capture, "Office phone");
	wait_scan_controls(capture);
	assert(bluez_fake_discovery_requests(capture->bluez) == 1);
	save_frame(capture, "settings-bluetooth-nearby");
	press(capture, KEY_DOWN);
	bluez_fake_remove_device(capture->bluez, PHONE);
	pump_for(capture, 150);
	press(capture, KEY_OK);
	wait_label(capture, "Address");
	wait_label(capture, "22:33:44:55:66:77");

	/* The focus remains on Visitor when another inventory row disappears. */
	for (size_t i = 0; i < 5; ++i)
		press(capture, KEY_DOWN);
	wait_label(capture, "Pair");
	bluez_fake_set_pairing_request(capture->bluez,
				       BLUEZ_FAKE_AGENT_CONFIRMATION, 123456);
	press(capture, KEY_OK);
	wait_label(capture, "Pairing…");
	wait_label(capture, "Does this code match on Visitor?\n123456");
	wait_screen_hold(capture, true);
	wait_legends(capture, "", "Accept", "Reject");
	assert(!bluez_fake_device_paired(capture->bluez, VISITOR));
	save_frame(capture, "settings-bluetooth-pairing");
	press(capture, KEY_OK);
	wait_pairing(capture, VISITOR);
	wait_screen_hold(capture, false);
	assert(bluez_fake_agent_answered(capture->bluez));
	assert(!bluez_fake_agent_rejected(capture->bluez));
	assert(!bluez_fake_device_connected(capture->bluez, VISITOR));
	save_frame(capture, "settings-bluetooth-device");

	/* A pair is separate from a connection and from trust. */
	for (size_t i = 0; i < 5; ++i)
		press(capture, KEY_DOWN);
	press(capture, KEY_OK);
	wait_label(capture, "Connecting…");
	deadline = fplinux_ui_frame_now_ms() + 3000;
	while (!bluez_fake_device_connected(capture->bluez, VISITOR) &&
	       fplinux_ui_frame_now_ms() < deadline)
		pump(capture);
	assert(bluez_fake_device_connected(capture->bluez, VISITOR));
	wait_legends(capture, "", "Select", "Back");
	press(capture, KEY_DOWN);
	press(capture, KEY_OK);
	wait_label(capture, "Changing trust…");
	deadline = fplinux_ui_frame_now_ms() + 3000;
	while (!bluez_fake_device_trusted(capture->bluez, VISITOR) &&
	       fplinux_ui_frame_now_ms() < deadline)
		pump(capture);
	assert(bluez_fake_device_trusted(capture->bluez, VISITOR));
	wait_legends(capture, "", "Select", "Back");
	bluez_fake_answer(capture->bluez, BLUEZ_FAKE_APPLY, 0);
	press(capture, KEY_UP);
	press(capture, KEY_UP);
	press(capture, KEY_OK);
	wait_label(capture, "Object Push");
	wait_legends(capture, "", "", "Back");
	save_frame(capture, "settings-bluetooth-services");
	press(capture, KEY_DOWN);
	wait_legends(capture, "", "Connect", "Back");
	press(capture, KEY_DOWN);
	wait_legends(capture, "", "", "Back");
	unsigned int requests = bluez_fake_requests(capture->bluez);
	press(capture, KEY_OK);
	pump_for(capture, 80);
	assert(bluez_fake_requests(capture->bluez) == requests);
	press(capture, KEY_DOWN);
	wait_legends(capture, "", "", "Back");
	press(capture, KEY_OK);
	pump_for(capture, 80);
	assert(bluez_fake_requests(capture->bluez) == requests);
	press(capture, KEY_F14);
	press(capture, KEY_F14);
	wait_label(capture,
		   capture->peer.width <= 128 ? "Search" : "Searching…");
	assert(bluez_fake_discovery_owned(capture->bluez, HCI0));
	assert(bluez_fake_agent_registered(capture->bluez));

	/* Hide releases the local scan and agent while the external scan stays active. */
	assert(fplinux_ui_session_send(capture->session, &lifecycle));
	deadline = fplinux_ui_frame_now_ms() + 3000;
	while ((bluez_fake_discovery_owned(capture->bluez, HCI0) ||
		bluez_fake_agent_registered(capture->bluez)) &&
	       fplinux_ui_frame_now_ms() < deadline)
		pump(capture);
	assert(!bluez_fake_discovery_owned(capture->bluez, HCI0));
	assert(!bluez_fake_agent_registered(capture->bluez));
	assert(bluez_fake_discovering(capture->bluez, HCI0));
	assert(bluez_fake_powered(capture->bluez, HCI0));
	bluez_fake_set_external_discovery(capture->bluez, HCI0, false);
	lifecycle.lifecycle = FPLINUX_UI_SESSION_FOREGROUND;
	assert(fplinux_ui_session_send(capture->session, &lifecycle));
	pump_for(capture, 150);
	assert(bluez_fake_agent_registered(capture->bluez));
	wait_legends(capture, "Options", "Select", "Back");
	press(capture, KEY_F14);
	wait_label(capture, "Bluetooth");
	wait_legends(capture, "", "Open", "Back");
	bluetooth_pair_with_passkey(capture);
	bluez_fake_request_agent(capture->bluez, BLUEZ_FAKE_AGENT_AUTHORIZATION,
				 VISITOR, 0, 0, NULL);
	wait_label(capture, "Allow pairing with Visitor?");
	press(capture, KEY_F14);
	pump_for(capture, 150);
	assert(bluez_fake_agent_rejected(capture->bluez));
	press(capture, KEY_F14);
	wait_label(capture, "Settings");
	pump_for(capture, 150);
	assert(!bluez_fake_agent_registered(capture->bluez));
	assert(bluez_fake_powered(capture->bluez, HCI0));
	puts("settings-bluetooth-devices: real paths retain focus; pairing, connection and trust are distinct; hiding releases only owned discovery and the agent");
}

static pid_t start_obex(struct capture *capture, const char *mode,
			const char *name)
{
	const char *executable = getenv("FPLINUX_SETTINGS_TEST_OBEX_SERVICE");
	char directory[PATH_MAX];
	char ready[PATH_MAX];
	uint64_t deadline = fplinux_ui_frame_now_ms() + 3000;
	pid_t child;
	int status;

	assert(executable);
	assert(snprintf(directory, sizeof(directory), "%s/obex-%s",
			capture->directory, name) < (int)sizeof(directory));
	assert(mkdir(directory, 0700) == 0);
	assert(snprintf(ready, sizeof(ready), "%s/ready", directory) <
	       (int)sizeof(ready));
	child = fork();
	assert(child >= 0);
	if (!child) {
		assert(chdir(directory) == 0);
		execl(executable, executable, mode, ready, "--obex-only",
		      (char *)NULL);
		perror("exec OBEX fixture");
		_exit(127);
	}
	while (access(ready, R_OK) < 0 &&
	       fplinux_ui_frame_now_ms() < deadline) {
		assert(waitpid(child, &status, WNOHANG) == 0);
		pump(capture);
	}
	assert(access(ready, R_OK) == 0);
	return child;
}

static void reap_obex(struct capture *capture, pid_t child)
{
	uint64_t deadline = fplinux_ui_frame_now_ms() + 3000;
	pid_t finished;
	int status;

	do {
		finished = waitpid(child, &status, WNOHANG);
		assert(finished >= 0);
		if (!finished)
			pump(capture);
	} while (!finished && fplinux_ui_frame_now_ms() < deadline);
	assert(finished == child);
	assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void choose_transfer_folder(struct capture *capture)
{
	wait_label(capture, "My files");
	press(capture, KEY_OK);
	wait_legends(capture, "Here", "", "Back");
	press(capture, KEY_F13);
}

static void send_prepare_close(struct capture *capture, uint32_t serial)
{
	struct fplinux_ui_session_message close = {
		.type = FPLINUX_UI_SESSION_PREPARE_CLOSE,
		.serial = serial,
	};
	uint64_t deadline = fplinux_ui_frame_now_ms() + 3000;
	unsigned int previous_replies = capture->close_replies;

	strcpy(close.reason, "user-close");
	assert(fplinux_ui_session_send(capture->session, &close));
	while (capture->close_replies == previous_replies &&
	       fplinux_ui_frame_now_ms() < deadline)
		pump(capture);
	assert(capture->close_replies > previous_replies);
	assert(capture->close_serial == serial);
}

static void request_user_close(struct capture *capture)
{
	send_prepare_close(capture, ++capture->serial);
	assert(!strcmp(capture->close_value, "wait"));
	wait_label(capture,
		   "Stop the current file transfer and close Settings?");
}

static void wait_for_cancelled_receive(struct capture *capture)
{
	char cancel_requested[PATH_MAX];
	char release_cancel[PATH_MAX];
	char cancelled[PATH_MAX];
	FILE *release;
	uint64_t deadline;

	assert(snprintf(cancel_requested, sizeof(cancel_requested),
			"%s/obex-cancel/cancel-requested",
			capture->directory) < (int)sizeof(cancel_requested));
	assert(snprintf(release_cancel, sizeof(release_cancel),
			"%s/obex-cancel/release-cancel",
			capture->directory) < (int)sizeof(release_cancel));
	assert(snprintf(cancelled, sizeof(cancelled),
			"%s/obex-cancel/cancelled",
			capture->directory) < (int)sizeof(cancelled));
	deadline = fplinux_ui_frame_now_ms() + 3000;
	while (access(cancel_requested, F_OK) < 0 &&
	       fplinux_ui_frame_now_ms() < deadline)
		pump(capture);
	assert(access(cancel_requested, F_OK) == 0);
	assert(access(cancelled, F_OK) < 0 && errno == ENOENT);

	/* Replaying the same serial observes the close decision with Cancel held. */
	send_prepare_close(capture, capture->serial);
	assert(!strcmp(capture->close_value, "wait"));
	release = fopen(release_cancel, "wx");
	assert(release && fclose(release) == 0);
	deadline = fplinux_ui_frame_now_ms() + 3000;
	while (strcmp(capture->close_value, "ready") &&
	       fplinux_ui_frame_now_ms() < deadline)
		pump(capture);
	assert(!strcmp(capture->close_value, "ready"));
	assert(capture->close_serial == capture->serial);
	assert(access(cancelled, F_OK) == 0);
}

static void bluetooth_transfers(struct capture *capture)
{
	const char *home = getenv("HOME");
	char incoming[PATH_MAX];
	uint64_t deadline;
	pid_t obex;

	assert(home);
	assert(snprintf(incoming, sizeof(incoming), "%s/Files/incoming.txt",
			home) < (int)sizeof(incoming));
	bluez_fake_add_device(capture->bluez, TRANSFER_PEER,
			      "01:23:45:67:89:AB", "Test peer", true);
	pump_for(capture, 100);
	press(capture, KEY_DOWN);
	press(capture, KEY_DOWN);
	press(capture, KEY_OK);
	wait_label(capture, "Power");
	for (size_t i = 0; i < 5; ++i)
		press(capture, KEY_DOWN);
	capture->stage = "send: choose peer";
	obex = start_obex(capture, "send-fast", "send");
	press(capture, KEY_OK);
	wait_label(capture, "Test peer");
	wait_scan_controls(capture);
	press(capture, KEY_OK);
	capture->stage = "send: choose file";
	wait_label(capture, "My files");
	press(capture, KEY_OK);
	wait_label(capture, "payload.txt");
	press(capture, KEY_OK);
	capture->stage = "send: await completion";
	wait_label(capture, "Complete");
	/* A long displayed path can push the count below the first page. */
	if (!has_label(capture, "17 / 17 bytes"))
		press(capture, KEY_RIGHT);
	wait_label(capture, "17 / 17 bytes");
	reap_obex(capture, obex);
	press(capture, KEY_F14);
	press(capture, KEY_F14);
	wait_label(capture, "Send file");
	press(capture, KEY_DOWN);
	capture->stage = "receive: choose peer";
	obex = start_obex(capture, "receive", "receive");
	press(capture, KEY_OK);
	wait_label(capture, "Test peer");
	wait_scan_controls(capture);
	press(capture, KEY_OK);
	capture->stage = "receive: choose destination";
	choose_transfer_folder(capture);
	capture->stage = "receive: consent";
	wait_label(capture, "Receive incoming.txt from Test peer?");
	wait_screen_hold(capture, true);
	assert(access(incoming, F_OK) < 0 && errno == ENOENT);
	press(capture, KEY_OK);
	capture->stage = "receive: await publication";
	wait_label(capture, "Complete");
	wait_legends(capture, "", "Open file", "Back");
	assert(access(incoming, R_OK) == 0);
	wait_screen_hold(capture, false);
	press(capture, KEY_OK);
	capture->stage = "receive: open document";
	deadline = fplinux_ui_frame_now_ms() + 3000;
	while (!capture->document_requests &&
	       fplinux_ui_frame_now_ms() < deadline)
		pump(capture);
	assert(capture->document_requests == 1);
	assert(!strcmp(capture->document_path, incoming));
	reap_obex(capture, obex);
	press(capture, KEY_F14);
	press(capture, KEY_F14);
	/* Cancelling a new send picker preserves the last received result. */
	press(capture, KEY_UP);
	capture->stage = "cancel new send: choose peer";
	press(capture, KEY_OK);
	wait_label(capture, "Test peer");
	wait_scan_controls(capture);
	press(capture, KEY_OK);
	capture->stage = "cancel new send: cancel picker";
	wait_label(capture, "My files");
	press(capture, KEY_F14);
	wait_label(capture, "Test peer");
	wait_scan_controls(capture);
	press(capture, KEY_F14);
	wait_legends(capture, "Options", "Select", "Back");
	assert(!bluez_fake_discovering(capture->bluez, HCI0));
	press(capture, KEY_F14);
	wait_label(capture, "Bluetooth");
	wait_legends(capture, "", "Open", "Back");
	press(capture, KEY_DOWN);
	press(capture, KEY_DOWN);
	press(capture, KEY_OK);
	capture->stage = "cancel new send: reopen received result";
	wait_legends(capture, "", "Open file", "Back");
	press(capture, KEY_F14);
	press(capture, KEY_UP);

	capture->stage = "cancel receive: choose peer";
	obex = start_obex(capture, "receive-cancel-held", "cancel");
	press(capture, KEY_OK);
	wait_label(capture, "Test peer");
	wait_scan_controls(capture);
	press(capture, KEY_OK);
	capture->stage = "cancel receive: choose destination";
	choose_transfer_folder(capture);
	capture->stage = "cancel receive: consent";
	wait_label(capture, "Receive incoming.txt from Test peer?");
	press(capture, KEY_OK);
	capture->stage = "cancel receive: await partial bytes";
	wait_label(capture, "3 / 7 bytes");
	wait_legends(capture, "Options", "", "Stop");
	save_frame(capture, "settings-bluetooth-transfer");
	capture->stage = "cancel receive: stay after close";
	request_user_close(capture);
	press(capture, KEY_F14);
	deadline = fplinux_ui_frame_now_ms() + 3000;
	while (strcmp(capture->close_value, "cancel") &&
	       fplinux_ui_frame_now_ms() < deadline)
		pump(capture);
	assert(!strcmp(capture->close_value, "cancel"));
	wait_label(capture, "3 / 7 bytes");
	capture->stage = "cancel receive: cancel and close";
	request_user_close(capture);
	press(capture, KEY_OK);
	wait_for_cancelled_receive(capture);
	reap_obex(capture, obex);
	close_and_reap(capture);
	puts("settings-bluetooth-transfers: shared pickers send bytes, consent publishes received files, document results return, and close waits for cancellation");
}

static void bluetooth_conflict(struct capture *capture, const char *choice)
{
	bool cancel = !strcmp(choice, "cancel");
	const char *result = cancel ? "Cancelled" : "Complete";
	pid_t obex;

	bluez_fake_add_device(capture->bluez, TRANSFER_PEER,
			      "01:23:45:67:89:AB", "Test peer", true);
	pump_for(capture, 100);
	press(capture, KEY_DOWN);
	press(capture, KEY_DOWN);
	press(capture, KEY_OK);
	wait_label(capture, "Power");
	for (size_t i = 0; i < 6; ++i)
		press(capture, KEY_DOWN);
	capture->stage = "conflict: choose peer and destination";
	obex = start_obex(capture, "receive", "conflict");
	press(capture, KEY_OK);
	wait_label(capture, "Test peer");
	wait_scan_controls(capture);
	press(capture, KEY_OK);
	choose_transfer_folder(capture);
	capture->stage = "conflict: consent and choose how to publish";
	wait_label(capture, "Receive incoming.txt from Test peer?");
	press(capture, KEY_OK);
	wait_label(capture, "incoming.txt already exists.");
	wait_label(capture, "Keep both");
	wait_label(capture, "Replace");
	wait_legends(capture, "", "Keep both", "Cancel");
	if (!strcmp(choice, "replace")) {
		press(capture, KEY_DOWN);
		wait_legends(capture, "", "Replace", "Cancel");
	}
	press(capture, cancel ? KEY_F14 : KEY_OK);
	wait_legends(capture, "", cancel ? "" : "Open file", "Back");
	for (size_t i = 0; i < 3 && !has_label(capture, result); ++i)
		press(capture, KEY_DOWN);
	wait_label(capture, result);
	reap_obex(capture, obex);
}

static void wifi_publish(struct capture *capture,
			 const struct fplinux_ui_wifi_state *state)
{
	assert(fplinux_ui_session_send(
		capture->session, &(const struct fplinux_ui_session_message){
					  .type = FPLINUX_UI_SESSION_WIFI_STATE,
					  .wifi_state = *state,
				  }));
}

static uint32_t wifi_request(struct capture *capture,
			     enum fplinux_ui_wifi_action action,
			     uint32_t target, const char *key)
{
	uint64_t deadline = fplinux_ui_frame_now_ms() + 3000;
	uint32_t serial;

	while (!capture->wifi_serial && fplinux_ui_frame_now_ms() < deadline)
		pump(capture);
	serial = capture->wifi_serial;
	if (!serial)
		fprintf(stderr,
			"Settings (%s): missing Wi-Fi action %u target %u; legends [%s | %s | %s]\n",
			capture->stage, (unsigned int)action, target,
			capture->left, capture->centre, capture->right);
	assert(serial);
	assert(capture->wifi_request.action == action);
	assert(capture->wifi_request.target == target);
	assert(!strcmp(capture->wifi_request.key, key));
	explicit_bzero(capture->wifi_request.key,
		       sizeof(capture->wifi_request.key));
	capture->wifi_serial = 0;
	return serial;
}

static void wifi_result(struct capture *capture, uint32_t serial,
			enum fplinux_ui_wifi_result result)
{
	assert(fplinux_ui_session_send(
		capture->session,
		&(const struct fplinux_ui_session_message){
			.type = FPLINUX_UI_SESSION_WIFI_RESULT,
			.serial = serial,
			.wifi_result = result,
		}));
}

static void wifi_numeric_key(struct capture *capture)
{
	hold(capture, KEY_NUMERIC_POUND);
	wait_label(capture, "123");
	for (unsigned int i = 0; i < 8; ++i)
		press(capture, KEY_NUMERIC_1 + i);
}

static void wifi_choose_option(struct capture *capture, const char *label,
			       unsigned int menu_row)
{
	press(capture, KEY_F13);
	wait_label(capture, label);
	for (unsigned int i = 0; i < menu_row; ++i)
		press(capture, KEY_DOWN);
	press(capture, KEY_OK);
}

static void wifi_submit_editor(struct capture *capture, unsigned int rows)
{
	bool ru = !strcmp(capture->observed.locale, "ru");

	for (unsigned int i = 0; i < rows; ++i)
		press(capture, KEY_DOWN);
	wait_legends(capture, "", ru ? "Подключить" : "Connect",
		     ru ? "Отмена" : "Cancel");
	press(capture, KEY_OK);
}

static void wifi_screens(struct capture *capture)
{
	bool ru = !strcmp(capture->observed.locale, "ru");
	struct fplinux_ui_wifi_state state = { 0 };
	struct fplinux_ui_wifi_network open = {
		.id = 411,
		.security = FPLINUX_UI_WIFI_OPEN,
		.name = "Twin",
	};
	struct fplinux_ui_wifi_network wpa2 = {
		.id = 912,
		.security = FPLINUX_UI_WIFI_WPA2,
		.name = "Twin",
	};
	uint32_t serial, original;

	capture->stage = "wifi qualification and root focus";
	press(capture, KEY_DOWN);
	press(capture, KEY_DOWN);
	wait_label(capture, ru ? "Время и язык" : "Time & language");
	assert(!has_label(capture, "Wi-Fi") && !capture->wifi_serial);
	state.power = FPLINUX_UI_WIFI_OFF;
	wifi_publish(capture, &state);
	pump_for(capture, 100);
	press(capture, KEY_OK);
	wait_label(capture, ru ? "Дата и время" : "Date and time");
	press(capture, KEY_F14);
	press(capture, KEY_UP);
	press(capture, KEY_OK);
	wait_label(capture, ru ? "Выкл." : "Off");
	assert(!has_label(capture,
			  ru ? "Доступные сети" : "Available networks"));
	assert(!capture->wifi_serial);
	press(capture, KEY_OK);
	serial = wifi_request(capture, FPLINUX_UI_WIFI_SET_POWER, 0, "");
	assert(capture->wifi_request.enabled);
	assert(has_label(capture, ru ? "Выкл." : "Off"));
	state.power = FPLINUX_UI_WIFI_ON;
	state.open_supported = true;
	wifi_publish(capture, &state);
	wifi_result(capture, serial, FPLINUX_UI_WIFI_APPLIED);
	wait_label(capture, ru ? "Вкл." : "On");
	save_frame(capture, "wifi-on");
	assert(!capture->wifi_serial);

	capture->stage = "wifi scan refusal without a snapshot";
	press(capture, KEY_DOWN);
	press(capture, KEY_DOWN);
	press(capture, KEY_OK);
	serial = wifi_request(capture, FPLINUX_UI_WIFI_SCAN, 0, "");
	wifi_result(capture, serial, FPLINUX_UI_WIFI_BUSY);
	wait_label(capture, ru ? "Wi-Fi занят" : "Wi-Fi busy");
	assert(!has_label(capture,
			  ru ? "Ожидание поиска…" : "Waiting to scan…"));
	wifi_choose_option(capture, ru ? "Обновить" : "Refresh", 0);
	serial = wifi_request(capture, FPLINUX_UI_WIFI_SCAN, 0, "");
	wifi_result(capture, serial, FPLINUX_UI_WIFI_FAILED);
	wait_label(capture, ru ? "Ошибка поиска" : "Scan failed");
	assert(!has_label(capture,
			  ru ? "Ожидание поиска…" : "Waiting to scan…"));
	wifi_choose_option(capture, ru ? "Обновить" : "Refresh", 0);
	serial = wifi_request(capture, FPLINUX_UI_WIFI_SCAN, 0, "");

	capture->stage = "wifi scan and stable network identity";
	state.scan = FPLINUX_UI_WIFI_SCANNING;
	wifi_publish(capture, &state);
	wait_label(capture, ru ? "Поиск…" : "Scanning…");
	state.available[0] = open;
	state.available[1] = (struct fplinux_ui_wifi_network){
		.id = 919,
		.security = FPLINUX_UI_WIFI_WPA2,
		.name = "Long name of 32 bytes for a WLAN",
	};
	state.available[2] = wpa2;
	state.available[3] = (struct fplinux_ui_wifi_network){
		.id = 513,
		.security = FPLINUX_UI_WIFI_WPA3,
		.name = "WPA3-only",
	};
	state.available_count = 4;
	state.scan = FPLINUX_UI_WIFI_SCAN_READY;
	wifi_publish(capture, &state);
	wifi_result(capture, serial, FPLINUX_UI_WIFI_APPLIED);
	wait_label(capture, "Twin");
	wait_legends(capture, ru ? "Опции" : "Options",
		     ru ? "Выбрать" : "Select", ru ? "Назад" : "Back");
	press(capture, KEY_DOWN);
	wait_label(capture, state.available[1].name);
	save_frame(capture, "wifi-available-long");
	press(capture, KEY_DOWN);
	wifi_choose_option(capture, ru ? "Обновить" : "Refresh", 0);
	serial = wifi_request(capture, FPLINUX_UI_WIFI_SCAN, 0, "");
	state.scan = FPLINUX_UI_WIFI_SCANNING;
	wifi_publish(capture, &state);
	wait_label(capture, ru ? "Поиск…" : "Scanning…");
	assert(has_label(capture, "Twin"));
	state.available[0] = wpa2;
	state.available[2] = open;
	state.scan = FPLINUX_UI_WIFI_SCAN_READY;
	wifi_publish(capture, &state);
	wifi_result(capture, serial, FPLINUX_UI_WIFI_APPLIED);
	pump_for(capture, 100);
	press(capture, KEY_OK);
	wait_label(capture, ru ? "Пароль" : "Password");

	capture->stage = "wifi masked password and cancellation";
	wait_legends(capture, "", "", ru ? "Отмена" : "Cancel");
	wifi_submit_editor(capture, 2);
	wait_label(
		capture,
		ru ? "Введите 8–63 символа ASCII или 64 шестнадцатеричные цифры" :
		     "Enter 8–63 ASCII characters or 64 hex digits");
	assert(!capture->wifi_serial);
	press(capture, KEY_F14);
	press(capture, KEY_UP);
	press(capture, KEY_UP);
	wifi_numeric_key(capture);
	wait_label(capture, "••••••••");
	wait_legends(capture, ru ? "Опции" : "Options", "",
		     ru ? "Стереть" : "Clear");
	assert(!has_label(capture, "12345678"));
	save_frame(capture, "wifi-masked");
	wifi_choose_option(capture, ru ? "Отмена" : "Cancel", 0);
	wait_label(capture, ru ? "Доступные сети" : "Available networks");
	assert(!capture->wifi_serial);
	press(capture, KEY_OK);
	wait_label(capture, ru ? "Пароль" : "Password");
	assert(!has_label(capture, "••••••••"));
	wifi_numeric_key(capture);
	wifi_submit_editor(capture, 2);
	original =
		wifi_request(capture, FPLINUX_UI_WIFI_CONNECT, 912, "12345678");
	assert(!capture->wifi_request.save);
	wait_label(capture, ru ? "Подключение…" : "Connecting…");
	state.current = wpa2;
	state.link = FPLINUX_UI_WIFI_ADDRESSING;
	wifi_publish(capture, &state);
	wait_label(capture, ru ? "Получение IP…" : "Getting IP address…");
	press(capture, KEY_F14);
	serial = wifi_request(capture, FPLINUX_UI_WIFI_CANCEL, original, "");
	state.link = FPLINUX_UI_WIFI_DISCONNECTED;
	wifi_publish(capture, &state);
	wifi_result(capture, original, FPLINUX_UI_WIFI_BAD_PASSWORD);
	wifi_result(capture, serial, FPLINUX_UI_WIFI_APPLIED);
	wait_label(capture, ru ? "Доступные сети" : "Available networks");
	pump_for(capture, 80);
	assert(!has_label(capture,
			  ru ? "Неверный пароль" : "Incorrect password"));

	capture->stage = "wifi available open network and scan failure";
	press(capture, KEY_DOWN);
	press(capture, KEY_DOWN);
	press(capture, KEY_OK);
	serial = wifi_request(capture, FPLINUX_UI_WIFI_CONNECT, 411, "");
	wifi_result(capture, serial, FPLINUX_UI_WIFI_NETWORK_UNAVAILABLE);
	wait_label(capture, ru ? "Сеть недоступна" : "Network unavailable");
	press(capture, KEY_F14);
	press(capture, KEY_DOWN);
	wait_legends(capture, ru ? "Опции" : "Options",
		     ru ? "Свойства" : "Properties", ru ? "Назад" : "Back");
	press(capture, KEY_OK);
	wait_label(capture, "WPA3");
	assert(!capture->wifi_serial);
	press(capture, KEY_F14);
	wifi_choose_option(capture, ru ? "Обновить" : "Refresh", 0);
	serial = wifi_request(capture, FPLINUX_UI_WIFI_SCAN, 0, "");
	state.scan = FPLINUX_UI_WIFI_SCAN_FAILED;
	wifi_publish(capture, &state);
	wifi_result(capture, serial, FPLINUX_UI_WIFI_FAILED);
	wait_label(capture, ru ? "Ошибка поиска" : "Scan failed");
	assert(has_label(capture, "Twin"));
	wifi_choose_option(capture, ru ? "Обновить" : "Refresh", 0);
	serial = wifi_request(capture, FPLINUX_UI_WIFI_SCAN, 0, "");
	state.available_count = 0;
	state.scan = FPLINUX_UI_WIFI_SCAN_READY;
	wifi_publish(capture, &state);
	wifi_result(capture, serial, FPLINUX_UI_WIFI_APPLIED);
	wait_label(capture, ru ? "Сетей не найдено" : "No networks found");
	wifi_choose_option(capture, ru ? "Добавить сеть" : "Add network", 1);
	wait_label(capture, ru ? "Имя сети" : "Network name");
	wifi_submit_editor(capture, 4);
	wait_label(capture, ru ? "Введите имя сети длиной до 32 байт" :
				 "Enter a network name of up to 32 bytes");
	assert(!capture->wifi_serial);
	press(capture, KEY_F14);
	for (unsigned int i = 0; i < 4; ++i)
		press(capture, KEY_UP);
	wifi_numeric_key(capture);
	press(capture, KEY_DOWN);
	press(capture, KEY_OK);
	wait_label(capture, ru ? "Открытая сеть" : "Open network");
	press(capture, KEY_DOWN);
	press(capture, KEY_OK);
	wifi_submit_editor(capture, 2);
	serial = wifi_request(capture, FPLINUX_UI_WIFI_CONNECT_HIDDEN, 0, "");
	assert(capture->wifi_request.security == FPLINUX_UI_WIFI_OPEN);
	assert(!strcmp(capture->wifi_request.ssid, "12345678"));
	wifi_result(capture, serial, FPLINUX_UI_WIFI_BUSY);
	wait_label(capture, ru ? "Wi-Fi занят" : "Wi-Fi busy");
	press(capture, KEY_F14);
	wait_label(capture, ru ? "Сетей не найдено" : "No networks found");

	capture->stage = "wifi saved network, replacement password and IP";
	press(capture, KEY_F14);
	press(capture, KEY_DOWN);
	press(capture, KEY_OK);
	wait_label(capture, ru ? "Нет сохранённых сетей" : "No saved networks");
	state.saved[0] = wpa2;
	state.saved[0].id = 731;
	state.saved[0].profile_id = 731;
	state.saved_count = 1;
	wifi_publish(capture, &state);
	wait_label(capture, "Twin");
	press(capture, KEY_OK);
	serial = wifi_request(capture, FPLINUX_UI_WIFI_CONNECT_SAVED, 731, "");
	wifi_result(capture, serial, FPLINUX_UI_WIFI_AUTH_FAILED);
	wait_label(capture, ru ? "Не удалось войти в сеть" :
				 "Could not join the network");
	wifi_choose_option(
		capture, ru ? "Ввести другой пароль" : "Enter another password",
		0);
	wait_label(capture, ru ? "Пароль" : "Password");
	wifi_numeric_key(capture);
	wifi_submit_editor(capture, 1);
	serial = wifi_request(capture, FPLINUX_UI_WIFI_CONNECT_SAVED, 731,
			      "12345678");
	assert(capture->wifi_request.save);
	state.current = wpa2;
	state.current.profile_id = 731;
	state.link = FPLINUX_UI_WIFI_NO_ADDRESS;
	wifi_publish(capture, &state);
	wifi_result(capture, serial, FPLINUX_UI_WIFI_ADDRESS_FAILED);
	wait_label(capture, ru ? "Не получен IP-адрес" : "No IP address");
	wait_legends(capture, ru ? "Опции" : "Options", "",
		     ru ? "Закрыть" : "Close");
	press(capture, KEY_F13);
	wait_label(capture, ru ? "Повторить" : "Retry");
	wait_label(capture, ru ? "Отключить" : "Disconnect");
	press(capture, KEY_F14);
	press(capture, KEY_F14);
	wait_label(capture, ru ? "Сохранённые сети" : "Saved networks");
	press(capture, KEY_F14);
	press(capture, KEY_UP);
	press(capture, KEY_UP);
	press(capture, KEY_OK);
	wait_label(capture, ru ? "Свойства" : "Properties");
	wait_legends(capture, ru ? "Опции" : "Options", "",
		     ru ? "Назад" : "Back");
	wifi_choose_option(capture, ru ? "Повторить" : "Retry", 0);
	serial = wifi_request(capture, FPLINUX_UI_WIFI_RETRY_ADDRESS, 0, "");
	state.link = FPLINUX_UI_WIFI_CONNECTED;
	strcpy(state.address, "192.0.2.33");
	wifi_publish(capture, &state);
	wifi_result(capture, serial, FPLINUX_UI_WIFI_APPLIED);
	wait_label(capture, "Twin");
	press(capture, KEY_OK);
	wait_label(capture, ru ? "Свойства" : "Properties");
	for (unsigned int i = 0; i < 4; ++i)
		press(capture, KEY_DOWN);
	wait_label(capture, "192.0.2.33");

	capture->stage = "wifi disconnect and deferred scan";
	wifi_choose_option(capture, ru ? "Отключить" : "Disconnect", 0);
	serial = wifi_request(capture, FPLINUX_UI_WIFI_DISCONNECT, 0, "");
	press(capture, KEY_DOWN);
	press(capture, KEY_OK);
	wait_label(capture, ru ? "Доступные сети" : "Available networks");
	wait_label(capture, ru ? "Ожидание поиска…" : "Waiting to scan…");
	wait_legends(capture, "", "", ru ? "Назад" : "Back");
	assert(!has_label(capture,
			  ru ? "Сетей не найдено" : "No networks found"));
	assert(!capture->wifi_serial);
	state.available[0] = wpa2;
	state.available_count = 1;
	wifi_publish(capture, &state);
	wait_label(capture, "Twin");
	wait_label(capture, ru ? "Ожидание поиска…" : "Waiting to scan…");
	wait_legends(capture, "", "", ru ? "Назад" : "Back");
	press(capture, KEY_OK);
	pump_for(capture, 80);
	assert(has_label(capture, "Twin"));
	assert(!has_label(capture, ru ? "Пароль" : "Password"));
	assert(!capture->wifi_serial);
	state.link = FPLINUX_UI_WIFI_DISCONNECTED;
	state.address[0] = '\0';
	wifi_publish(capture, &state);
	wifi_result(capture, serial, FPLINUX_UI_WIFI_APPLIED);
	serial = wifi_request(capture, FPLINUX_UI_WIFI_SCAN, 0, "");
	wifi_result(capture, serial, FPLINUX_UI_WIFI_APPLIED);
	press(capture, KEY_F14);
	press(capture, KEY_DOWN);
	press(capture, KEY_OK);
	wait_label(capture, "Twin");

	capture->stage = "wifi connected Forget and adapter loss";
	state.link = FPLINUX_UI_WIFI_CONNECTED;
	strcpy(state.address, "192.0.2.33");
	wifi_publish(capture, &state);
	pump_for(capture, 80);
	wifi_choose_option(capture, ru ? "Забыть сеть" : "Forget network", 0);
	wait_label(capture, ru ? "Забыть сеть и разорвать соединение?" :
				 "Forget this network and disconnect?");
	press(capture, KEY_F14);
	pump_for(capture, 80);
	assert(!capture->wifi_serial);
	wifi_choose_option(capture, ru ? "Забыть сеть" : "Forget network", 0);
	wait_label(capture, ru ? "Забыть сеть и разорвать соединение?" :
				 "Forget this network and disconnect?");
	wait_legends(capture, "", ru ? "Забыть" : "Forget",
		     ru ? "Отмена" : "Cancel");
	press(capture, KEY_OK);
	serial = wifi_request(capture, FPLINUX_UI_WIFI_FORGET, 731, "");
	state.link = FPLINUX_UI_WIFI_DISCONNECTED;
	state.saved_count = 0;
	state.address[0] = '\0';
	wifi_publish(capture, &state);
	wifi_result(capture, serial, FPLINUX_UI_WIFI_APPLIED);
	wait_label(capture, ru ? "Нет сохранённых сетей" : "No saved networks");
	state.power = FPLINUX_UI_WIFI_UNAVAILABLE;
	wifi_publish(capture, &state);
	wait_label(capture, ru ? "Wi-Fi недоступен" : "Wi-Fi unavailable");
	press(capture, KEY_F14);
	wait_label(capture, ru ? "Настройки" : "Settings");
	assert(!has_label(capture, "Wi-Fi"));
	assert(!capture->wifi_serial);
	puts("settings-wifi: keyboard station routes followed confirmed state and exact request identities");
}

int main(int argc, char **argv)
{
	struct capture capture = { 0 };
	const char *conflict_choice = NULL;

	assert(setvbuf(stdout, NULL, _IONBF, 0) == 0);
	if (argc == 7 && !strncmp(argv[6], "bluetooth-conflict-", 19))
		conflict_choice = argv[6] + 19;
	assert(argc == 6 ||
	       (argc == 7 &&
		(!strcmp(argv[6], "root-values") ||
		 !strcmp(argv[6], "bluetooth") ||
		 !strcmp(argv[6], "bluetooth-devices") ||
		 !strcmp(argv[6], "bluetooth-transfers") ||
		 !strcmp(argv[6], "section-values") ||
		 !strcmp(argv[6], "sound") || !strcmp(argv[6], "root-sound") ||
		 !strcmp(argv[6], "about-edges") || !strcmp(argv[6], "wifi") ||
		 conflict_choice)));
	capture.bluetooth = argc == 7 &&
			    (!strcmp(argv[6], "bluetooth") ||
			     !strcmp(argv[6], "bluetooth-devices") ||
			     !strcmp(argv[6], "bluetooth-transfers") ||
			     conflict_choice);
	capture.initial_media_unknown = argc == 7 &&
					!strcmp(argv[6], "root-sound");
	if (capture.bluetooth) {
		capture.bluez = bluez_fake_start();
		bluez_fake_add_adapter(capture.bluez, HCI0,
				       strcmp(argv[6], "bluetooth") != 0);
	}
	start(&capture, argv);
	if (capture.bluetooth) {
		if (!strcmp(argv[6], "bluetooth-transfers")) {
			bluetooth_transfers(&capture);
			bluez_fake_stop(capture.bluez);
			return 0;
		} else if (conflict_choice)
			bluetooth_conflict(&capture, conflict_choice);
		else if (!strcmp(argv[6], "bluetooth-devices"))
			bluetooth_devices(&capture);
		else
			bluetooth_switch(&capture);
		close_and_reap(&capture);
		bluez_fake_stop(capture.bluez);
		return 0;
	}
	if (argc == 7) {
		if (!strcmp(argv[6], "section-values"))
			section_value_frames(&capture);
		else if (!strcmp(argv[6], "about-edges"))
			about_reading_edges(&capture);
		else if (!strcmp(argv[6], "sound"))
			sound_values_outputs_and_editors(&capture);
		else if (!strcmp(argv[6], "root-sound"))
			root_sound_summary(&capture);
		else if (!strcmp(argv[6], "wifi"))
			wifi_screens(&capture);
		else
			root_values_fit_and_follow_current_settings(&capture);
		close_and_reap(&capture);
		return 0;
	}
	open_theme(&capture, true);
	press(&capture, KEY_UP);
	press(&capture, KEY_OK);
	wait_label(&capture, warning);
	wait_legends(&capture, "Change", "", "Cancel");
	press(&capture, KEY_F14);
	wait_legends(&capture, "", "Select", "Back");
	pump_for(&capture, 100);
	assert(capture.changes == 0);
	assert(capture.observed.theme == 1);
	assert(!has_label(&capture, warning));
	puts("settings-warning: Cancel leaves the live theme and sends no change");

	press(&capture, KEY_UP);
	press(&capture, KEY_OK);
	wait_label(&capture, warning);
	press(&capture, KEY_F13);
	expect_request(&capture, "theme", "light");
	pending_choice_retains_applied_marker(&capture, 2);
	reply_and_check_frame(&capture, "theme", "light",
			      FPLINUX_STORAGE_DURABLE, 0, "en", "Display", NULL,
			      false);
	assert(!has_label(&capture, warning));
	assert(!has_label(&capture, theme_error));
	save_frame(&capture, "settings-theme-saved");
	puts("settings-success: live Light widgets and pixels precede REDRAW_ACK");

	open_theme(&capture, false);
	press(&capture, KEY_DOWN);
	press(&capture, KEY_OK);
	wait_label(&capture, warning);
	press(&capture, KEY_F13);
	expect_request(&capture, "theme", "dark");
	pending_choice_retains_applied_marker(&capture, 1);
	reply_and_check_frame(&capture, "theme", "dark",
			      FPLINUX_STORAGE_UNPUBLISHED, ENOSPC, "en",
			      "Theme", theme_error, true);
	wait_legends(&capture, "", "", "Close");
	pump_for(&capture, 150);
	assert(has_label(&capture, theme_error));
	save_frame(&capture, "settings-theme-error");
	press(&capture, KEY_F14);
	wait_legends(&capture, "", "Open", "Back");
	wait_label(&capture, "Display");
	assert(!has_label(&capture, theme_error));
	puts("settings-theme-error: applied Dark frame acknowledges ENOSPC; notice waits for Close");

	press(&capture, KEY_F14);
	wait_label(&capture, "Settings");
	press(&capture, KEY_DOWN);
	press(&capture, KEY_DOWN);
	press(&capture, KEY_OK);
	wait_label(&capture, "Time & language");
	date_draft_survives_pending_home_and_refusal(&capture);
	press(&capture, KEY_DOWN);
	press(&capture, KEY_OK);
	wait_label(&capture, "English");
	wait_label(&capture, "Русский");
	press(&capture, KEY_DOWN);
	press(&capture, KEY_OK);
	wait_label(&capture, warning);
	press(&capture, KEY_F13);
	expect_request(&capture, "locale", "ru");
	pending_choice_retains_applied_marker(&capture, 1);
	reply_and_check_frame(&capture, "locale", "ru",
			      FPLINUX_STORAGE_PUBLISHED_UNSYNCED, EIO, "ru",
			      "Язык", locale_error, true);
	wait_legends(&capture, "", "", "Закрыть");
	save_frame(&capture, "settings-locale-error");
	/* A later current-value reply rebuilds the view without closing its notice. */
	reply_and_check_frame(&capture, "locale", "ru", FPLINUX_STORAGE_DURABLE,
			      0, "ru", "Язык", locale_error, true);
	pump_for(&capture, 150);
	assert(has_label(&capture, locale_error));
	wait_legends(&capture, "", "", "Закрыть");
	press(&capture, KEY_F14);
	wait_legends(&capture, "", "Открыть", "Назад");
	wait_label(&capture, "Время и язык");
	wait_label(&capture, "Русский");
	assert(!has_label(&capture, locale_error));
	expect_palette(&capture, true, false);
	save_frame(&capture, "settings-locale-closed");
	assert(capture.changes == 5);
	close_and_reap(&capture);
	puts("settings-locale-error: applied Русский frame acknowledges EIO; rebuilt notice waits for Закрыть");
	return 0;
}
