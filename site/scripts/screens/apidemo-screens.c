/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "apidemo-observer.h"
#include "fplinux-ui-profile.h"
#include "screen-frame.h"
#include "ui-frame-peer.h"

#include <assert.h>
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

struct capture {
	struct fplinux_ui_frame_peer peer;
	int listener;
	int session;
	int observer;
	pid_t application;
	uint32_t serial;
	char **arguments;
	const char *directory;
	const char *locale;
	char centre[FPLINUX_UI_SESSION_TEXT_BYTES];
	char right[FPLINUX_UI_SESSION_TEXT_BYTES];
	bool legends_seen;
	unsigned int alerts;
};

static int dimension(const char *text)
{
	char *end;
	long value = strtol(text, &end, 10);

	assert(*text && !*end && value > 0 && value <= 1024);
	return (int)value;
}

static void wait_frame(struct capture *capture, unsigned int previous)
{
	uint64_t deadline = fplinux_ui_frame_now_ms() + 3000;

	while (capture->peer.commits <= previous &&
	       fplinux_ui_frame_now_ms() < deadline)
		fplinux_ui_frame_pump(&capture->peer);
	assert(capture->peer.commits > previous);
}

static struct apidemo_observation
wait_observation(struct capture *capture, enum apidemo_observation_kind kind,
		 const char *expected)
{
	for (;;) {
		struct apidemo_observation observation;
		const char *text;

		fplinux_ui_frame_wait_readable(&capture->peer,
					       capture->observer);
		assert(recv(capture->observer, &observation,
			    sizeof(observation),
			    0) == (ssize_t)sizeof(observation));
		if (observation.kind == APIDEMO_ALERT)
			++capture->alerts;
		text = kind == APIDEMO_LABEL || kind == APIDEMO_TEXTAREA ?
			       observation.text :
			       observation.title;
		if (observation.kind == kind &&
		    (kind == APIDEMO_TEXTAREA ?
			     !strcmp(text, expected) :
		     kind == APIDEMO_LABEL && !expected[0] ?
			     !strcmp(observation.title, "Raw text") &&
				     !text[0] :
			     strstr(text, expected) != NULL)) {
			printf("apidemo-observed %d: %s\n", kind, expected);
			return observation;
		}
	}
}

static void send_press(struct capture *capture, unsigned int seat,
		       unsigned int code)
{
	fplinux_ui_frame_send_key(&capture->peer.keyboards[seat], code, true);
	fplinux_ui_frame_send_key(&capture->peer.keyboards[seat], code, false);
}

static void press(struct capture *capture, unsigned int seat, unsigned int code)
{
	unsigned int previous = capture->peer.commits;

	send_press(capture, seat, code);
	wait_frame(capture, previous);
}

static void type_query(struct capture *capture, const char *query)
{
	static const unsigned int letters[] = {
		KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I,
		KEY_J, KEY_K, KEY_L, KEY_M, KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R,
		KEY_S, KEY_T, KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z,
	};

	for (; *query; ++query) {
		if (*query == ' ')
			press(capture, 0, KEY_SPACE);
		else if (*query == '_')
			press(capture, 0, KEY_MINUS);
		else {
			assert(*query >= 'A' && *query <= 'Z');
			press(capture, 0, letters[*query - 'A']);
		}
	}
}

static void redraw(struct capture *capture)
{
	struct fplinux_ui_session_message setting = {
		.type = FPLINUX_UI_SESSION_SETTING,
		.serial = ++capture->serial,
	};

	strcpy(setting.name, "theme");
	strcpy(setting.value, "dark");
	assert(fplinux_ui_session_send(capture->session, &setting));
	for (;;) {
		struct fplinux_ui_session_message message =
			fplinux_ui_frame_wait_message(&capture->peer,
						      capture->session);

		assert(message.type != FPLINUX_UI_SESSION_EXIT);
		if (message.type == FPLINUX_UI_SESSION_SOFTKEYS) {
			strcpy(capture->centre, message.centre);
			strcpy(capture->right, message.right);
			capture->legends_seen = true;
		}
		if (message.type == FPLINUX_UI_SESSION_REDRAW_ACK) {
			assert(message.serial == setting.serial);
			return;
		}
	}
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
	printf("%s: %dx%d\n", name, capture->peer.width, capture->peer.height);
}

static void start(struct capture *capture)
{
	char **arguments = capture->arguments;
	struct fplinux_ui_session_message config = {
		.type = FPLINUX_UI_SESSION_CONFIG,
		.mode = FPLINUX_UI_SESSION_INTEGRATED,
		.theme = FPLINUX_UI_SESSION_THEME_DARK,
		.screen_width = dimension(arguments[1]),
		.screen_height = dimension(arguments[2]),
	};
	const struct fplinux_ui_profile_spec *profile = fplinux_ui_profile_spec(
		fplinux_ui_profile_for_screen(config.screen_width,
					      config.screen_height));
	struct fplinux_ui_session_message hello;
	int wayland[2], observer[2];
	int buffer_bytes = 1024 * 1024;

	assert(profile);
	capture->alerts = 0;
	capture->centre[0] = '\0';
	capture->right[0] = '\0';
	capture->legends_seen = false;
	capture->listener = fplinux_ui_frame_listen_session(arguments[5]);
	fplinux_ui_frame_socketpair(wayland);
	assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, observer) == 0);
	assert(setsockopt(observer[1], SOL_SOCKET, SO_SNDBUF, &buffer_bytes,
			  sizeof(buffer_bytes)) == 0);
	capture->application = fork();
	assert(capture->application >= 0);
	if (!capture->application) {
		char descriptor[32], state[512], catalogues[512],
			failure_path[512];
		const char *slash = strrchr(arguments[3], '/');
		const char *language = !strcmp(capture->locale, "ru") ?
					       "ru_RU.UTF-8" :
					       "en_US.UTF-8";

		assert(slash);
		close(capture->listener);
		close(wayland[0]);
		close(observer[0]);
		fplinux_ui_frame_client_environment(wayland[1], arguments[5]);
		snprintf(descriptor, sizeof(descriptor), "%d", observer[1]);
		assert(setenv("FPLINUX_APIDEMO_OBSERVER_FD", descriptor, 1) ==
		       0);
		assert(snprintf(state, sizeof(state), "%s/state",
				capture->directory) < (int)sizeof(state));
		assert(snprintf(catalogues, sizeof(catalogues), "%.*s/locale",
				(int)(slash - arguments[3]),
				arguments[3]) < (int)sizeof(catalogues));
		assert(setenv("FPLINUX_APIDEMO_LOCALE_DIR", catalogues, 1) ==
		       0);
		assert(setenv("XDG_STATE_HOME", state, 1) == 0);
		assert(snprintf(failure_path, sizeof(failure_path),
				"%s/save-failure", capture->directory) <
		       (int)sizeof(failure_path));
		assert(setenv("FPLINUX_APIDEMO_SAVE_FAILURE", failure_path,
			      1) == 0);
		assert(setenv("LANG", language, 1) == 0);
		assert(setenv("LC_ALL", language, 1) == 0);
		assert(unsetenv("LANGUAGE") == 0);
		execl(arguments[3], arguments[3], (char *)NULL);
		_exit(127);
	}
	close(wayland[1]);
	close(observer[1]);
	capture->observer = observer[0];
	fplinux_ui_frame_init(&capture->peer, wayland[0], config.screen_width,
			      config.screen_height - profile->status_height -
				      profile->softkey_height,
			      arguments[4]);
	capture->session = fplinux_ui_frame_accept_session(&capture->peer,
							   capture->listener);
	hello = fplinux_ui_frame_wait_message(&capture->peer, capture->session);
	assert(hello.type == FPLINUX_UI_SESSION_HELLO);
	assert(!strcmp(hello.app_id, "org.fplinux.example.apidemo"));
	strcpy(config.locale, capture->locale);
	strcpy(config.input_locale, capture->locale);
	assert(fplinux_ui_session_send(capture->session, &config));
	wait_observation(capture, APIDEMO_LABEL,
			 !strcmp(capture->locale, "ru") ? "Демо API" :
							  "API Demos");
	wait_frame(capture, 0);
	assert(capture->peer.keyboards[0].keyboard &&
	       capture->peer.keyboards[1].keyboard);
	fplinux_ui_frame_focus_keyboard(&capture->peer.keyboards[0]);
	fplinux_ui_frame_focus_keyboard(&capture->peer.keyboards[1]);
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
	assert(unlink(capture->arguments[5]) == 0);
}

static void open_search(struct capture *capture, const char *query,
			const char *title, enum apidemo_observation_kind kind)
{
	char results[128];

	start(capture);
	type_query(capture, query);
	press(capture, 1, KEY_OK);
	snprintf(results, sizeof(results), "Search: %s", query);
	wait_observation(capture, APIDEMO_LIST, results);
	press(capture, 1, KEY_OK);
	wait_observation(capture, kind, title);
}

static void catalogue_and_virtual(struct capture *capture)
{
	struct apidemo_observation selected;

	start(capture);
	type_query(capture, "ZZZ");
	press(capture, 1, KEY_OK);
	wait_observation(capture, APIDEMO_LABEL, "No demo matches the search.");
	save_frame(capture, "apidemo-no-results");
	press(capture, 1, KEY_F13);
	press(capture, 1, KEY_DOWN);
	press(capture, 1, KEY_OK);
	wait_observation(capture, APIDEMO_LABEL, "");
	redraw(capture);
	assert(capture->legends_seen && !strcmp(capture->centre, "Open") &&
	       !strcmp(capture->right, "Back"));
	press(capture, 1, KEY_OK);
	wait_observation(capture, APIDEMO_LIST, "API Demos");
	save_frame(capture, "apidemo-catalogue");
	press(capture, 1, KEY_OK);
	wait_observation(capture, APIDEMO_LIST, "Lists");
	press(capture, 1, KEY_OK);
	wait_observation(capture, APIDEMO_LIST, "Implicit list");
	press(capture, 1, KEY_F14);
	press(capture, 1, KEY_F14);
	close_and_reap(capture);

	open_search(capture, "VIRTUAL", "Virtual list", APIDEMO_LIST);
	press(capture, 1, KEY_DOWN);
	press(capture, 1, KEY_OK);
	selected = wait_observation(capture, APIDEMO_ALERT, "Selected");
	assert(strstr(selected.text, "Event 2:"));
	save_frame(capture, "apidemo-virtual-selection");
	press(capture, 1, KEY_F14);
	press(capture, 1, KEY_F14);
	close_and_reap(capture);
	puts("apidemo: Options Clear empties search with Open/Back; catalogue sections; virtual row 2 selected");
}

static void form_choice(struct capture *capture)
{
	open_search(capture, "FORM_APPEND_CHOICE", "Choices", APIDEMO_FORM);
	press(capture, 1, KEY_DOWN);
	press(capture, 1, KEY_DOWN);
	press(capture, 1, KEY_OK);
	wait_observation(capture, APIDEMO_LABEL, "Item 1: Large");
	save_frame(capture, "apidemo-choice");
	press(capture, 1, KEY_F14);
	close_and_reap(capture);
	puts("apidemo: API-name search; real form choice changed to Large");
}

static void preferences(struct capture *capture)
{
	for (int expected = 1; expected <= 2; ++expected) {
		char value[64];
		struct fplinux_ui_session_message visibility = {
			.type = FPLINUX_UI_SESSION_VISIBILITY,
		};

		open_search(capture, "PREFERENCES", "Preferences",
			    APIDEMO_FORM);
		press(capture, 1, KEY_DOWN);
		press(capture, 1, KEY_RIGHT);
		/* Stored may be below the viewport: its value is the save barrier. */
		send_press(capture, 1, KEY_OK);
		snprintf(value, sizeof(value), "locale.count = %d", expected);
		wait_observation(capture, APIDEMO_LABEL, value);
		assert(fplinux_ui_session_send(capture->session, &visibility));
		visibility.visible = true;
		assert(fplinux_ui_session_send(capture->session, &visibility));
		redraw(capture);
		if (expected == 2)
			save_frame(capture, "apidemo-preferences");
		close_and_reap(capture);
	}
	puts("apidemo: preferences reopened at 1; saved 2; hidden/show/close preserved state");
}

static void wait_preference(struct capture *capture, const char *label,
			    const char *expected)
{
	struct apidemo_observation observation =
		wait_observation(capture, APIDEMO_PREFERENCE, label);

	assert(strstr(observation.text, expected));
}

static void set_save_failure(const char *path, char mode)
{
	FILE *file = fopen(path, "w");

	assert(file && fputc(mode, file) == mode && !fclose(file));
}

static void preference_storage_failure(struct capture *capture)
{
	char failure_path[512];
	struct fplinux_ui_session_message visibility = {
		.type = FPLINUX_UI_SESSION_VISIBILITY,
	};

	assert(snprintf(failure_path, sizeof(failure_path), "%s/save-failure",
			capture->directory) < (int)sizeof(failure_path));
	open_search(capture, "PREFERENCES", "Preferences", APIDEMO_FORM);
	wait_preference(capture, "Stored", "locale.count = 0");
	press(capture, 1, KEY_DOWN);
	press(capture, 1, KEY_RIGHT);
	send_press(capture, 1, KEY_OK);
	wait_preference(capture, "In memory", "locale.count = 1");
	wait_preference(capture, "Stored", "locale.count = 1");
	wait_preference(capture, "Save result", "FPLINUX_STORAGE_DURABLE");
	set_save_failure(failure_path, 'f');
	press(capture, 1, KEY_RIGHT);
	send_press(capture, 1, KEY_OK);
	wait_preference(capture, "In memory", "locale.count = 2");
	wait_preference(capture, "Stored", "locale.count = 1");
	wait_preference(capture, "Save result",
			"FPLINUX_STORAGE_UNPUBLISHED, ENOSPC");
	assert(capture->alerts == 1);
	press(capture, 1, KEY_F14);
	for (int attempt = 0; attempt < 2; ++attempt) {
		visibility.visible = false;
		assert(fplinux_ui_session_send(capture->session, &visibility));
		visibility.visible = true;
		assert(fplinux_ui_session_send(capture->session, &visibility));
		redraw(capture);
		wait_preference(capture, "In memory", "locale.count = 2");
		wait_preference(capture, "Stored", "locale.count = 1");
		wait_preference(capture, "Save result",
				"FPLINUX_STORAGE_UNPUBLISHED, ENOSPC");
		assert(capture->alerts == 1);
	}
	set_save_failure(failure_path, 'u');
	send_press(capture, 1, KEY_OK);
	wait_preference(capture, "In memory", "locale.count = 2");
	wait_preference(capture, "Stored", "locale.count = 2");
	wait_preference(capture, "Save result",
			"FPLINUX_STORAGE_PUBLISHED_UNSYNCED, EIO");
	assert(capture->alerts == 2);
	press(capture, 1, KEY_F14);
	assert(unlink(failure_path) == 0);
	send_press(capture, 1, KEY_OK);
	wait_preference(capture, "In memory", "locale.count = 2");
	wait_preference(capture, "Stored", "locale.count = 2");
	wait_preference(capture, "Save result", "FPLINUX_STORAGE_DURABLE");
	assert(capture->alerts == 2);
	close_and_reap(capture);
	open_search(capture, "PREFERENCES", "Preferences", APIDEMO_FORM);
	wait_preference(capture, "Stored", "locale.count = 2");
	close_and_reap(capture);
	puts("apidemo-storage: memory differs from file after ENOSPC; autosave has no modal spam; published sync failure remains unconfirmed; retry reopens 2");
}

static void profile_metrics(struct capture *capture)
{
	static const struct {
		int width;
		int height;
		const char *name;
	} profiles[] = {
		{ 128, 128, "FPLINUX_UI_PROFILE_128X128" },
		{ 128, 160, "FPLINUX_UI_PROFILE_128X160" },
		{ 176, 220, "FPLINUX_UI_PROFILE_176X220" },
		{ 240, 320, "FPLINUX_UI_PROFILE_240X320" },
		{ 320, 240, "FPLINUX_UI_PROFILE_320X240" },
	};
	int width = dimension(capture->arguments[1]);
	int height = dimension(capture->arguments[2]);
	const char *expected = NULL;

	for (size_t index = 0; index < sizeof(profiles) / sizeof(profiles[0]);
	     ++index)
		if (profiles[index].width == width &&
		    profiles[index].height == height)
			expected = profiles[index].name;
	assert(expected);
	open_search(capture, "METRICS", "Metrics", APIDEMO_FORM);
	wait_observation(capture, APIDEMO_LABEL, expected);
	save_frame(capture, "apidemo-metrics");
	close_and_reap(capture);
	printf("apidemo: Metrics reports %s\n", expected);
}

static void configure_mode(struct capture *capture, bool fullscreen, int width,
			   int height, int content_height)
{
	unsigned int previous;
	char expected[192];

	for (;;) {
		struct fplinux_ui_session_message message =
			fplinux_ui_frame_wait_message(&capture->peer,
						      capture->session);

		assert(message.type != FPLINUX_UI_SESSION_EXIT);
		if (message.type != FPLINUX_UI_SESSION_MODE)
			continue;
		assert(message.mode == (fullscreen ?
						FPLINUX_UI_SESSION_FULLSCREEN :
						FPLINUX_UI_SESSION_INTEGRATED));
		break;
	}
	/* Retire every old-size frame before the peer applies the requested size. */
	redraw(capture);
	previous = capture->peer.commits;
	fplinux_ui_frame_configure(&capture->peer, width, height, fullscreen);
	wait_frame(capture, previous);
	snprintf(expected, sizeof(expected),
		 "Surface: %d × %d\u00a0px\nFull screen: %s\n"
		 "Content area: %d × %d\u00a0px",
		 width, height, fullscreen ? "Yes" : "No", width,
		 content_height);
	wait_observation(capture, APIDEMO_LABEL, expected);
}

static void fullscreen_canvas(struct capture *capture)
{
	static const struct {
		int width;
		int height;
		int normal_height;
		int normal_content;
		int fullscreen_content;
	} geometry[] = {
		{ 128, 128, 96, 76, 108 },   { 128, 160, 128, 108, 140 },
		{ 176, 220, 178, 154, 196 }, { 240, 320, 268, 240, 292 },
		{ 320, 240, 196, 170, 214 },
	};
	int width = dimension(capture->arguments[1]);
	int height = dimension(capture->arguments[2]);
	size_t index;

	for (index = 0; index < sizeof(geometry) / sizeof(geometry[0]); ++index)
		if (geometry[index].width == width &&
		    geometry[index].height == height)
			break;
	assert(index < sizeof(geometry) / sizeof(geometry[0]));
	open_search(capture, "FULL SCREEN", "Full screen", APIDEMO_CANVAS);
	press(capture, 1, KEY_F13);
	press(capture, 1, KEY_OK);
	configure_mode(capture, true, width, height,
		       geometry[index].fullscreen_content);
	save_frame(capture, "apidemo-fullscreen");
	press(capture, 1, KEY_F13);
	press(capture, 1, KEY_OK);
	configure_mode(capture, false, width, geometry[index].normal_height,
		       geometry[index].normal_content);
	save_frame(capture, "apidemo-normal-view");
	press(capture, 1, KEY_F14);
	close_and_reap(capture);
	puts("apidemo: Canvas full screen and normal view keep content geometry and soft keys");
}

static void frames_and_errors(struct capture *capture)
{
	struct fplinux_ui_session_message error = {
		.type = FPLINUX_UI_SESSION_ERROR,
	};

	open_search(capture, "INTERRUPTED", "Interrupted painting",
		    APIDEMO_CANVAS);
	save_frame(capture, "apidemo-frames");
	press(capture, 1, KEY_F14);
	close_and_reap(capture);

	open_search(capture, "TONE", "Tone", APIDEMO_FORM);
	press(capture, 1, KEY_F13);
	wait_observation(capture, APIDEMO_LABEL, "ENOTSUP");
	save_frame(capture, "apidemo-audio-unavailable");
	press(capture, 1, KEY_F14);
	close_and_reap(capture);

	open_search(capture, "PATTERNS", "Patterns", APIDEMO_FORM);
	strcpy(error.name, "vibration");
	strcpy(error.reason, "controlled device error");
	assert(fplinux_ui_session_send(capture->session, &error));
	wait_observation(capture, APIDEMO_LABEL,
			 "Vibration request failed: controlled device error");
	save_frame(capture, "apidemo-vibration-error");
	close_and_reap(capture);
	puts("apidemo: interrupted frames; unavailable audio; asynchronous vibration error; close cleanup");
}

static void expect_legends(struct capture *capture, const char *centre,
			   const char *right)
{
	redraw(capture);
	assert(capture->legends_seen && !strcmp(capture->centre, centre) &&
	       !strcmp(capture->right, right));
}

static void help_returns_to_options(struct capture *capture)
{
	open_search(capture, "HELP COMMAND", "Help command", APIDEMO_FORM);
	press(capture, 1, KEY_F13);
	press(capture, 1, KEY_OK);
	wait_observation(capture, APIDEMO_ALERT, "Help");
	press(capture, 1, KEY_F14);
	expect_legends(capture, "Select", "Back");
	press(capture, 1, KEY_OK);
	wait_observation(capture, APIDEMO_ALERT, "Help");
	press(capture, 1, KEY_F14);
	expect_legends(capture, "Select", "Back");
	press(capture, 1, KEY_F14);
	expect_legends(capture, "", "Back");
	close_and_reap(capture);

	open_search(capture, "OPTIONS MENU WITH ICONS",
		    "Options menu with icons", APIDEMO_FORM);
	press(capture, 1, KEY_F13);
	for (unsigned int index = 0; index < 7; ++index)
		press(capture, 1, KEY_DOWN);
	press(capture, 1, KEY_OK);
	wait_observation(capture, APIDEMO_ALERT, "Help");
	press(capture, 1, KEY_F14);
	expect_legends(capture, "Select", "Back");
	press(capture, 1, KEY_OK);
	wait_observation(capture, APIDEMO_ALERT, "Help");
	press(capture, 1, KEY_F14);
	for (unsigned int index = 0; index < 7; ++index)
		press(capture, 1, KEY_UP);
	press(capture, 1, KEY_OK);
	wait_observation(capture, APIDEMO_ALERT, "Save");
	press(capture, 1, KEY_F14);
	expect_legends(capture, "", "Back");
	close_and_reap(capture);
	puts("apidemo: Help Close restores selected Options; result Close returns to its screen");
}

static void russian_search(struct capture *capture)
{
	const unsigned int keys[] = { KEY_F1, KEY_F2, KEY_F3,
				      KEY_F1, KEY_F4, KEY_F5 };

	start(capture);
	for (size_t index = 0; index < sizeof(keys) / sizeof(keys[0]); ++index)
		press(capture, 0, keys[index]);
	press(capture, 1, KEY_OK);
	wait_observation(capture, APIDEMO_LIST, "Поиск: СПИСОК");
	press(capture, 1, KEY_OK);
	wait_observation(capture, APIDEMO_LIST, "Простой список");
	save_frame(capture, "apidemo-russian-search");
	close_and_reap(capture);
	puts("apidemo: Cyrillic uppercase search found translated list title");
}

static void dates_error(struct capture *capture)
{
	open_search(capture, "FORMAT_DATE", "Dates", APIDEMO_FORM);
	wait_observation(capture, APIDEMO_LABEL, "false, ERANGE");
	close_and_reap(capture);
	puts("apidemo: Dates small-buffer row reports false, ERANGE");
}

static void cyrillic_widget(struct capture *capture, bool keyboard)
{
	open_search(capture, keyboard ? "LV_KEYBOARD" : "TEXT AREA",
		    keyboard ? "Keyboard" : "Text area", APIDEMO_CANVAS);
	press(capture, 0, KEY_F1);
	press(capture, 0, KEY_F2);
	wait_observation(capture, APIDEMO_TEXTAREA, "СП");
	press(capture, 0, KEY_BACKSPACE);
	wait_observation(capture, APIDEMO_TEXTAREA, "С");
	press(capture, 1, KEY_NUMERIC_2);
	wait_observation(capture, APIDEMO_TEXTAREA, "С2");
	save_frame(capture, keyboard ? "apidemo-keyboard-cyrillic" :
				       "apidemo-textarea-cyrillic");
	close_and_reap(capture);
	puts(keyboard ?
		     "apidemo: Keyboard types СП; Backspace leaves С; phone digit gives С2" :
		     "apidemo: Text area types СП; Backspace leaves С; phone digit gives С2");
}

static void matrix_typed(struct capture *capture, const char *expected)
{
	struct apidemo_observation status =
		wait_observation(capture, APIDEMO_LABEL, "Typed: ");

	printf("apidemo matrix status=\"%s\"\n", status.text);
	assert(!strcmp(status.text, expected));
}

static void buttonmatrix_keys(struct capture *capture)
{
	open_search(capture, "BUTTON MATRIX", "Button matrix", APIDEMO_CANVAS);
	matrix_typed(capture, "Typed: ");
	press(capture, 1, KEY_NUMERIC_2);
	matrix_typed(capture, "Typed: 2");
	/* U+0431 must leave both the typed text and selected key alone. */
	send_press(capture, 0, KEY_F6);
	press(capture, 1, KEY_OK);
	matrix_typed(capture, "Typed: 22");
	press(capture, 1, KEY_NUMERIC_1);
	matrix_typed(capture, "Typed: 221");
	press(capture, 1, KEY_NUMERIC_STAR);
	matrix_typed(capture, "Typed: 221*");
	press(capture, 1, KEY_NUMERIC_POUND);
	matrix_typed(capture, "Typed: 221*#");
	save_frame(capture, "apidemo-buttonmatrix-keys");
	close_and_reap(capture);
	puts("apidemo: Button matrix ignores б, preserves selected 2; ASCII 1/*/# type their keys");
}

static void numeric_field_legend(struct capture *capture)
{
	open_search(capture, "CANVAS FIELDS", "Canvas fields", APIDEMO_CANVAS);
	press(capture, 1, KEY_DOWN);
	press(capture, 1, KEY_F13);
	press(capture, 1, KEY_DOWN);
	press(capture, 1, KEY_OK);
	wait_observation(capture, APIDEMO_LABEL,
			 "Several lines, no limit: NUMERIC");
	redraw(capture);
	printf("apidemo numeric field centre=\"%s\"\n", capture->centre);
	assert(capture->legends_seen && !strcmp(capture->centre, ""));
	save_frame(capture, "apidemo-numeric-field");
	close_and_reap(capture);
	puts("apidemo: numeric Canvas field offers no New line command");
}

static void whole_surface_back(struct capture *capture, bool keyboard)
{
	open_search(capture, "WHOLE SURFACE", "", APIDEMO_CANVAS);
	press(capture, keyboard ? 0 : 1, keyboard ? KEY_ESC : KEY_F14);
	press(capture, 1, KEY_OK);
	wait_observation(capture, APIDEMO_CANVAS, "");
	close_and_reap(capture);
	puts(keyboard ? "apidemo: external Escape leaves Whole surface" :
			"apidemo: phone Back leaves Whole surface");
}

static void canvas_field_clear_hold(struct capture *capture)
{
	uint64_t deadline;

	open_search(capture, "CANVAS FIELDS", "Canvas fields", APIDEMO_CANVAS);
	type_query(capture, "ABC");
	fplinux_ui_frame_send_key(&capture->peer.keyboards[1], KEY_F14, true);
	deadline = fplinux_ui_frame_now_ms() + 1500;
	while (fplinux_ui_frame_now_ms() < deadline)
		fplinux_ui_frame_pump(&capture->peer);
	fplinux_ui_frame_send_key(&capture->peer.keyboards[1], KEY_F14, false);
	/* A held Clear and its release must leave the empty field open. */
	press(capture, 1, KEY_F13);
	for (int index = 0; index < 3; ++index)
		press(capture, 1, KEY_DOWN);
	press(capture, 1, KEY_OK);
	wait_observation(capture, APIDEMO_LABEL, "Text: “”\nEmpty: true");
	press(capture, 1, KEY_F14);
	/* A new, separate Back press now leaves the empty field. */
	press(capture, 1, KEY_F14);
	press(capture, 1, KEY_OK);
	wait_observation(capture, APIDEMO_CANVAS, "Canvas fields");
	close_and_reap(capture);
	puts("apidemo: held Clear empties Canvas field; release stays; next Back leaves");
}

int main(int argc, char **argv)
{
	struct capture capture = { .arguments = argv };

	assert(setvbuf(stdout, NULL, _IONBF, 0) == 0);
	if (argc != 9) {
		fprintf(stderr,
			"usage: %s WIDTH HEIGHT APP KEYMAP SESSION OUTPUT LOCALE SCENARIO\n",
			argv[0]);
		return 2;
	}
	capture.directory = argv[6];
	capture.locale = argv[7];
	if (!strcmp(argv[8], "storage-failure")) {
		preference_storage_failure(&capture);
		return 0;
	}
	if (!strcmp(argv[8], "dates")) {
		dates_error(&capture);
		return 0;
	}
	if (!strcmp(argv[8], "keyboard") || !strcmp(argv[8], "textarea")) {
		cyrillic_widget(&capture, !strcmp(argv[8], "keyboard"));
		return 0;
	}
	if (!strcmp(argv[8], "buttonmatrix")) {
		buttonmatrix_keys(&capture);
		return 0;
	}
	if (!strcmp(argv[8], "numeric")) {
		numeric_field_legend(&capture);
		return 0;
	}
	if (!strcmp(argv[8], "whole-escape") ||
	    !strcmp(argv[8], "whole-phone-back")) {
		whole_surface_back(&capture, !strcmp(argv[8], "whole-escape"));
		return 0;
	}
	if (!strcmp(argv[8], "canvas-clear-hold")) {
		canvas_field_clear_hold(&capture);
		return 0;
	}
	assert(!strcmp(argv[8], "all"));
	if (!strcmp(capture.locale, "ru")) {
		russian_search(&capture);
		return 0;
	}
	assert(!strcmp(capture.locale, "en"));
	catalogue_and_virtual(&capture);
	form_choice(&capture);
	preferences(&capture);
	profile_metrics(&capture);
	fullscreen_canvas(&capture);
	frames_and_errors(&capture);
	help_returns_to_options(&capture);
	return 0;
}
