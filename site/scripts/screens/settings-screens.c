/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "settings-observer.h"
#include "fplinux-ui-profile.h"
#include "screen-frame.h"
#include "ui-frame-peer.h"

#include <assert.h>
#include <errno.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
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
	char close_value[16];
	bool has_request;
	char left[128];
	char centre[128];
	char right[128];
	const char *directory;
	const char *session_path;
};

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

static void answer_query(struct capture *capture,
			 const struct fplinux_ui_session_message *query)
{
	struct fplinux_ui_session_message reply = {
		.type = FPLINUX_UI_SESSION_SETTING,
		.applied = true,
		.storage_state = FPLINUX_STORAGE_DURABLE,
	};
	const char *value;

	if (!strcmp(query->name, "brightness"))
		value = "7";
	else if (!strcmp(query->name, "screen_off_seconds"))
		value = "30";
	else if (!strcmp(query->name, "auto_lock_seconds"))
		value = "0";
	else {
		assert(!strcmp(query->name, "input_locale"));
		value = "en";
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
		} else if (message.type == FPLINUX_UI_SESSION_REDRAW_ACK) {
			capture->redraw_ack = message.serial;
		} else if (message.type == FPLINUX_UI_SESSION_CLOSE_REPLY) {
			strcpy(capture->close_value, message.value);
		}
	}
	assert(received == 0);
}

static void pump(struct capture *capture)
{
	fplinux_ui_frame_pump(&capture->peer);
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
		fprintf(stderr, "Settings: missing [%s], actual:%s\n", text,
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
			"Settings legends: [%s | %s | %s], expected [%s | %s | %s]\n",
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
	struct fplinux_ui_session_message config = {
		.type = FPLINUX_UI_SESSION_CONFIG,
		.mode = FPLINUX_UI_SESSION_INTEGRATED,
		.theme = FPLINUX_UI_SESSION_THEME_DARK,
		.screen_width = 240,
		.screen_height = 320,
	};
	const struct fplinux_ui_profile_spec *profile = fplinux_ui_profile_spec(
		fplinux_ui_profile_for_screen(240, 320));
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
		char descriptor[32], catalogues[512];
		const char *slash = strrchr(arguments[1], '/');

		assert(slash);
		close(capture->listener);
		close(wayland[0]);
		close(observer[0]);
		fplinux_ui_frame_client_environment(wayland[1], arguments[3]);
		snprintf(descriptor, sizeof(descriptor), "%d", observer[1]);
		assert(setenv("FPLINUX_SETTINGS_OBSERVER_FD", descriptor, 1) ==
		       0);
		assert(snprintf(catalogues, sizeof(catalogues), "%.*s/locale",
				(int)(slash - arguments[1]),
				arguments[1]) < (int)sizeof(catalogues));
		assert(setenv("FPLINUX_SETTINGS_LOCALE_DIR", catalogues, 1) ==
		       0);
		assert(setenv("FPLINUX_SETTINGS_LANGUAGE_DIR", arguments[5],
			      1) == 0);
		assert(setenv("LANG", "en_US.UTF-8", 1) == 0);
		assert(setenv("LC_ALL", "en_US.UTF-8", 1) == 0);
		assert(unsetenv("LANGUAGE") == 0);
		execl(arguments[1], arguments[1], (char *)NULL);
		_exit(127);
	}
	close(wayland[1]);
	close(observer[1]);
	capture->observer = observer[0];
	fplinux_ui_frame_init(&capture->peer, wayland[0], 240,
			      320 - profile->status_height -
				      profile->softkey_height,
			      arguments[2]);
	capture->session = fplinux_ui_frame_accept_session(&capture->peer,
							   capture->listener);
	hello = fplinux_ui_frame_wait_message(&capture->peer, capture->session);
	assert(hello.type == FPLINUX_UI_SESSION_HELLO);
	assert(!strcmp(hello.app_id, "org.fplinux.settings"));
	strcpy(config.locale, "en");
	strcpy(config.input_locale, "en");
	assert(fplinux_ui_session_send(capture->session, &config));
	wait_label(capture, "Settings");
	wait_legends(capture, "", "Open", "Back");
	assert(capture->peer.keyboards[1].keyboard);
	fplinux_ui_frame_focus_keyboard(&capture->peer.keyboards[1]);
	pump_for(capture, 100);
}

static void close_and_reap(struct capture *capture)
{
	struct fplinux_ui_session_message message = {
		.type = FPLINUX_UI_SESSION_CLOSE,
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
	wait_legends(capture, "", "Applying…", "");
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
	wait_legends(capture, "", "Applying…", "");
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
	press(capture, KEY_F13);
	wait_time_request(capture);
	request = capture->request;
	assert(!strcmp(request.value, "2024-02-29 08:45:00"));
	changes = capture->changes;
	wait_legends(capture, "Saving…", "Edit", "");
	press(capture, KEY_F13);
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
	wait_legends(capture, "Save", "Edit", "Back");
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
	wait_legends(capture, "Save", "Edit", "Back");
	pump_for(capture, 100);
	assert(!strcmp(capture->close_value, "cancel"));
	assert(capture->observed.date_value == date &&
	       capture->observed.time_value == time);
	press(capture, KEY_F13);
	expect_request(capture, "time", request.value);
	message = request;
	message.type = FPLINUX_UI_SESSION_SETTING;
	message.applied = true;
	/* The manager formats the live clock after setting it, rather than echoing input. */
	assert(strlen(message.value) == 19);
	message.value[18] = '1';
	capture->has_request = false;
	assert(fplinux_ui_session_send(capture->session, &message));
	wait_label(capture, "Time & language");
	wait_legends(capture, "", "Open", "Back");
	puts("settings-date: real editors retain date, time and focus through pending Home, refusal and cancelled close; live-clock reply completes Save");
}

int main(int argc, char **argv)
{
	struct capture capture = { 0 };

	assert(argc == 6);
	start(&capture, argv);
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
