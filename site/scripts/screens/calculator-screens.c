/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "calculator-observer.h"
#include "calculator.h"
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
	int listener;
	int session;
	int observer;
	pid_t application;
	uint32_t serial;
	const char *directory;
	const char *session_path;
	unsigned int alerts;
	char close_left[FPLINUX_UI_SESSION_TEXT_BYTES];
	char close_centre[FPLINUX_UI_SESSION_TEXT_BYTES];
	char close_right[FPLINUX_UI_SESSION_TEXT_BYTES];
};

static int dimension(const char *text)
{
	char *end;
	long value = strtol(text, &end, 10);

	assert(*text && !*end && value > 0 && value <= 1024);
	return (int)value;
}

static void pump_for(struct capture *capture, unsigned int milliseconds)
{
	uint64_t deadline = fplinux_ui_frame_now_ms() + milliseconds;

	while (fplinux_ui_frame_now_ms() < deadline)
		fplinux_ui_frame_pump(&capture->peer);
}

static void wait_legends(struct capture *capture, const char *left,
			 const char *centre, const char *right)
{
	for (;;) {
		struct fplinux_ui_session_message message =
			fplinux_ui_frame_wait_message(&capture->peer,
						      capture->session);

		assert(message.type != FPLINUX_UI_SESSION_EXIT);
		if (message.type == FPLINUX_UI_SESSION_SOFTKEYS &&
		    !strcmp(message.left, left) &&
		    !strcmp(message.centre, centre) &&
		    !strcmp(message.right, right))
			return;
	}
}

static void wait_number(struct capture *capture, const char *expected)
{
	for (;;) {
		struct calculator_number_observation observation;

		fplinux_ui_frame_wait_readable(&capture->peer,
					       capture->observer);
		assert(recv(capture->observer, &observation,
			    sizeof(observation),
			    0) == (ssize_t)sizeof(observation));
		if (observation.kind == CALCULATOR_ALERT) {
			++capture->alerts;
			continue;
		}
		assert(observation.text_width > 0 &&
		       observation.text_width <= observation.width);
		assert(observation.text_height > 0 &&
		       observation.text_height <= observation.height);
		assert(observation.left >= 0 && observation.top >= 0);
		assert(observation.left + observation.width <=
		       observation.parent_width);
		assert(observation.top + observation.height <=
		       observation.parent_height);
		if (strcmp(observation.text, expected))
			continue;
		printf("calculator-number %dx%d %s: %dx%d within %dx%d\n",
		       capture->peer.width, capture->peer.height, expected,
		       observation.text_width, observation.text_height,
		       observation.width, observation.height);
		return;
	}
}

static void wait_save_alert(struct capture *capture)
{
	struct calculator_number_observation observation;

	for (;;) {
		fplinux_ui_frame_wait_readable(&capture->peer,
					       capture->observer);
		assert(recv(capture->observer, &observation,
			    sizeof(observation),
			    0) == (ssize_t)sizeof(observation));
		if (observation.kind != CALCULATOR_ALERT)
			continue;
		++capture->alerts;
		assert(!strcmp(observation.title, "Calculation not saved"));
		assert(!strcmp(observation.text, "Not enough storage space"));
		return;
	}
}

static void collect_observations(struct capture *capture)
{
	struct calculator_number_observation observation;
	ssize_t count;

	while ((count = recv(capture->observer, &observation,
			     sizeof(observation), MSG_DONTWAIT)) > 0) {
		assert(count == (ssize_t)sizeof(observation));
		if (observation.kind == CALCULATOR_ALERT)
			++capture->alerts;
	}
	assert(count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
}

static void wait_frame(struct capture *capture, unsigned int previous,
		       const char *operation)
{
	uint64_t deadline = fplinux_ui_frame_now_ms() + 3000;

	while (capture->peer.commits <= previous &&
	       fplinux_ui_frame_now_ms() < deadline)
		fplinux_ui_frame_pump(&capture->peer);
	if (capture->peer.commits <= previous)
		fprintf(stderr, "calculator: no frame after %s\n", operation);
	assert(capture->peer.commits > previous);
}

static void press(struct capture *capture, unsigned int seat, unsigned int code,
		  const char *number)
{
	unsigned int previous = capture->peer.commits;
	struct fplinux_ui_frame_keyboard *keyboard =
		&capture->peer.keyboards[seat];
	char operation[64];

	fplinux_ui_frame_send_key(keyboard, code, true);
	fplinux_ui_frame_send_key(keyboard, code, false);
	if (number)
		wait_number(capture, number);
	snprintf(operation, sizeof(operation), "seat=%u code=%u", seat, code);
	wait_frame(capture, previous, operation);
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

static void visibility(struct capture *capture, bool visible)
{
	unsigned int previous = capture->peer.commits;
	struct fplinux_ui_session_message message = {
		.type = FPLINUX_UI_SESSION_VISIBILITY,
		.visible = visible,
	};

	assert(fplinux_ui_session_send(capture->session, &message));
	/* The resumed frame confirms visibility before input on another socket. */
	if (visible)
		wait_frame(capture, previous, "visibility(true)");
}

static void send_lifecycle(struct capture *capture,
			   enum fplinux_ui_session_lifecycle state)
{
	struct fplinux_ui_session_message message = {
		.type = FPLINUX_UI_SESSION_LIFECYCLE,
		.lifecycle = state,
	};

	assert(fplinux_ui_session_send(capture->session, &message));
}

static void lifecycle(struct capture *capture,
		      enum fplinux_ui_session_lifecycle state)
{
	send_lifecycle(capture, state);
	/* The redraw acknowledgement follows the lifecycle on this connection. */
	redraw(capture);
}

static void expect_saved(struct capture *capture, const char *keys,
			 const char *expected)
{
	struct fplinux_calculator calculator;
	char path[512], saved[128], displayed[32];
	FILE *file;
	size_t length;

	assert(snprintf(path, sizeof(path),
			"%s/state/fplinux/org.fplinux.calculator/preferences",
			capture->directory) < (int)sizeof(path));
	file = fopen(path, "r");
	assert(file && fgets(saved, sizeof(saved), file));
	assert(!fclose(file));
	assert(!strncmp(saved, "state=", 6));
	length = strlen(saved);
	assert(length && saved[length - 1] == '\n');
	saved[length - 1] = '\0';
	assert(fplinux_calculator_restore(&calculator, saved + 6));
	for (; *keys; keys++)
		fplinux_calculator_input(&calculator, *keys);
	fplinux_calculator_display(&calculator, '.', displayed,
				   sizeof(displayed));
	assert(!strcmp(displayed, expected));
}

static void reset_from_options(struct capture *capture)
{
	press(capture, 1, KEY_F13, NULL);
	wait_legends(capture, "", "Select", "Back");
	press(capture, 1, KEY_OK, "0");
	wait_legends(capture, "", "", "Back");
}

static void cancel_clear_holds(struct capture *capture)
{
	const char *scenarios[] = { "leave", "hidden", "menu", "background",
				    "another-key" };

	for (size_t index = 0; index < sizeof(scenarios) / sizeof(scenarios[0]);
	     index++) {
		struct fplinux_ui_frame_keyboard *phone =
			&capture->peer.keyboards[1];

		press(capture, 1, KEY_NUMERIC_1, "1");
		press(capture, 1, KEY_NUMERIC_2, "12");
		press(capture, 1, KEY_NUMERIC_3, "123");
		fplinux_ui_frame_send_key(phone, KEY_F14, true);
		wait_number(capture, "12");
		if (index == 0) {
			wl_keyboard_send_leave(
				phone->keyboard,
				wl_display_next_serial(capture->peer.display),
				capture->peer.surface);
			wl_display_flush_clients(capture->peer.display);
		} else if (index == 1) {
			visibility(capture, false);
		} else if (index == 2) {
			press(capture, 1, KEY_F13, NULL);
			wait_legends(capture, "", "Select", "Back");
		} else if (index == 3) {
			lifecycle(capture, FPLINUX_UI_SESSION_BACKGROUND);
		} else {
			press(capture, 1, KEY_NUMERIC_4, "124");
		}
		pump_for(capture, 650);
		if (index == 0)
			fplinux_ui_frame_focus_keyboard(phone);
		else if (index == 1) {
			expect_saved(capture, "4", "124");
			visibility(capture, true);
		} else if (index == 3) {
			expect_saved(capture, "4", "124");
			lifecycle(capture, FPLINUX_UI_SESSION_FOREGROUND);
		}
		fplinux_ui_frame_send_key(phone, KEY_F14, false);
		if (index == 2) {
			press(capture, 1, KEY_F14, NULL);
			wait_legends(capture, "Options", "", "Clear");
		}
		if (index == 4)
			press(capture, 1, KEY_NUMERIC_5, "1245");
		else
			press(capture, 1, KEY_NUMERIC_4, "124");
		reset_from_options(capture);
		printf("calculator-clear-cancel %s: preserved\n",
		       scenarios[index]);
	}
}

static void start(struct capture *capture, char **argv, const char *initial,
		  const char *left, const char *centre, const char *right)
{
	struct fplinux_ui_session_message config = {
		.type = FPLINUX_UI_SESSION_CONFIG,
		.mode = FPLINUX_UI_SESSION_INTEGRATED,
		.theme = FPLINUX_UI_SESSION_THEME_DARK,
		.screen_width = dimension(argv[1]),
		.screen_height = dimension(argv[2]),
	};
	const struct fplinux_ui_profile_spec *profile = fplinux_ui_profile_spec(
		fplinux_ui_profile_for_screen(config.screen_width,
					      config.screen_height));
	struct fplinux_ui_session_message hello;
	int wayland[2], observer[2];

	assert(profile);
	capture->alerts = 0;
	capture->listener = fplinux_ui_frame_listen_session(argv[5]);
	capture->directory = argv[6];
	capture->session_path = argv[5];
	fplinux_ui_frame_socketpair(wayland);
	assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, observer) == 0);
	capture->application = fork();
	assert(capture->application >= 0);
	if (!capture->application) {
		char descriptor[32], state_directory[512], failure_path[512];

		close(capture->listener);
		close(wayland[0]);
		close(observer[0]);
		fplinux_ui_frame_client_environment(wayland[1], argv[5]);
		snprintf(descriptor, sizeof(descriptor), "%d", observer[1]);
		assert(setenv("FPLINUX_CALCULATOR_OBSERVER_FD", descriptor,
			      1) == 0);
		assert(snprintf(state_directory, sizeof(state_directory),
				"%s/state",
				argv[6]) < (int)sizeof(state_directory));
		assert(setenv("XDG_STATE_HOME", state_directory, 1) == 0);
		assert(snprintf(failure_path, sizeof(failure_path),
				"%s/save-failure",
				argv[6]) < (int)sizeof(failure_path));
		assert(setenv("FPLINUX_CALCULATOR_SAVE_FAILURE", failure_path,
			      1) == 0);
		assert(setenv("LC_ALL", "C", 1) == 0);
		execl(argv[3], argv[3], (char *)NULL);
		_exit(127);
	}
	close(wayland[1]);
	close(observer[1]);
	capture->observer = observer[0];
	fplinux_ui_frame_init(&capture->peer, wayland[0], config.screen_width,
			      config.screen_height - profile->status_height -
				      profile->softkey_height,
			      argv[4]);
	capture->session = fplinux_ui_frame_accept_session(&capture->peer,
							   capture->listener);
	hello = fplinux_ui_frame_wait_message(&capture->peer, capture->session);
	assert(hello.type == FPLINUX_UI_SESSION_HELLO);
	assert(!strcmp(hello.app_id, "org.fplinux.calculator"));
	strcpy(config.locale, "en");
	strcpy(config.input_locale, "en");
	assert(fplinux_ui_session_send(capture->session, &config));
	wait_number(capture, initial);
	wait_legends(capture, left, centre, right);
	assert(capture->peer.keyboards[0].keyboard &&
	       capture->peer.keyboards[1].keyboard);
	fplinux_ui_frame_focus_keyboard(&capture->peer.keyboards[0]);
	fplinux_ui_frame_focus_keyboard(&capture->peer.keyboards[1]);
}

/* EXIT asks the manager to close; CLOSE lets the client finish and save. */
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

static void capture_number_limit(struct capture *capture)
{
	const unsigned int digits[] = {
		KEY_NUMERIC_1, KEY_NUMERIC_2, KEY_NUMERIC_3, KEY_NUMERIC_4,
		KEY_NUMERIC_5, KEY_NUMERIC_6, KEY_NUMERIC_7, KEY_NUMERIC_8,
		KEY_NUMERIC_9, KEY_NUMERIC_0, KEY_NUMERIC_1,
	};

	for (size_t index = 0; index < sizeof(digits) / sizeof(digits[0]);
	     index++)
		press(capture, 1, digits[index], NULL);
	press(capture, 1, KEY_NUMERIC_STAR, "12345678901.");
	press(capture, 1, KEY_NUMERIC_2, "12345678901.2");
	press(capture, 1, KEY_NUMERIC_POUND, NULL);
	wait_number(capture, "-12345678901.2");
	save_frame(capture, "calculator-number");
	fplinux_ui_frame_send_key(&capture->peer.keyboards[1], KEY_F14, true);
	wait_number(capture, "-12345678901.");
	wait_number(capture, "0");
	fplinux_ui_frame_send_key(&capture->peer.keyboards[1], KEY_F14, false);
	wait_legends(capture, "", "", "Back");
}

static void wait_close_reply(struct capture *capture, const char *expected)
{
	for (;;) {
		struct fplinux_ui_session_message reply =
			fplinux_ui_frame_wait_message(&capture->peer,
						      capture->session);

		if (reply.type == FPLINUX_UI_SESSION_SOFTKEYS) {
			strcpy(capture->close_left, reply.left);
			strcpy(capture->close_centre, reply.centre);
			strcpy(capture->close_right, reply.right);
		}
		if (reply.type != FPLINUX_UI_SESSION_CLOSE_REPLY)
			continue;
		assert(reply.serial == capture->serial);
		assert(!strcmp(reply.value, expected));
		return;
	}
}

static void prepare_close(struct capture *capture, const char *expected)
{
	struct fplinux_ui_session_message request = {
		.type = FPLINUX_UI_SESSION_PREPARE_CLOSE,
		.serial = ++capture->serial,
	};

	strcpy(request.reason, "user-close");
	assert(fplinux_ui_session_send(capture->session, &request));
	wait_close_reply(capture, expected);
}

static void storage_failure(struct capture *capture, char **argv)
{
	char failure_path[512];
	FILE *gate;

	assert(snprintf(failure_path, sizeof(failure_path), "%s/save-failure",
			argv[6]) < (int)sizeof(failure_path));
	start(capture, argv, "0", "", "", "Back");
	press(capture, 1, KEY_NUMERIC_9, "9");
	press(capture, 1, KEY_UP, NULL);
	press(capture, 1, KEY_NUMERIC_0, "0");
	press(capture, 1, KEY_OK, "9");
	close_and_reap(capture);
	expect_saved(capture, "", "9");
	gate = fopen(failure_path, "wx");
	assert(gate && !fclose(gate));
	start(capture, argv, "9", "Back", "", "Clear");
	press(capture, 1, KEY_NUMERIC_4, "4");
	send_lifecycle(capture, FPLINUX_UI_SESSION_BACKGROUND);
	visibility(capture, false);
	send_lifecycle(capture, FPLINUX_UI_SESSION_FOREGROUND);
	visibility(capture, true);
	/* Frame acknowledgements resume only once the application is visible. */
	redraw(capture);
	collect_observations(capture);
	assert(capture->alerts == 0);
	puts("calculator-storage: Home resumed without an alert");
	press(capture, 1, KEY_NUMERIC_2, "42");
	expect_saved(capture, "", "9");
	prepare_close(capture, "wait");
	wait_save_alert(capture);
	assert(!strcmp(capture->close_left, "Discard"));
	assert(!strcmp(capture->close_centre, "Retry"));
	assert(!strcmp(capture->close_right, "Stay"));
	press(capture, 1, KEY_F14, NULL);
	wait_close_reply(capture, "cancel");
	/* The alert's listener sends CANCEL before the base legends refresh. */
	press(capture, 1, KEY_NUMERIC_3, "423");
	prepare_close(capture, "wait");
	wait_save_alert(capture);
	press(capture, 1, KEY_OK, NULL);
	wait_save_alert(capture);
	expect_saved(capture, "", "9");
	assert(unlink(failure_path) == 0);
	fplinux_ui_frame_send_key(&capture->peer.keyboards[1], KEY_OK, true);
	fplinux_ui_frame_send_key(&capture->peer.keyboards[1], KEY_OK, false);
	wait_close_reply(capture, "ready");
	expect_saved(capture, "", "423");
	close_and_reap(capture);

	gate = fopen(failure_path, "wx");
	assert(gate && !fclose(gate));
	start(capture, argv, "423", "Options", "", "Clear");
	reset_from_options(capture);
	press(capture, 1, KEY_NUMERIC_7, "7");
	prepare_close(capture, "wait");
	wait_save_alert(capture);
	fplinux_ui_frame_send_key(&capture->peer.keyboards[1], KEY_F13, true);
	fplinux_ui_frame_send_key(&capture->peer.keyboards[1], KEY_F13, false);
	wait_close_reply(capture, "ready");
	/* Even a recovered writer must not save after the user's Discard. */
	assert(unlink(failure_path) == 0);
	visibility(capture, false);
	send_lifecycle(capture, FPLINUX_UI_SESSION_BACKGROUND);
	close_and_reap(capture);
	expect_saved(capture, "", "423");
	start(capture, argv, "423", "Options", "", "Clear");
	close_and_reap(capture);
	puts("calculator-storage: Home retained draft; Stay continued input; Retry saved; Discard reopened old state");
}

int main(int argc, char **argv)
{
	struct capture capture = { 0 };
	FILE *dimensions;
	char path[512];

	assert(setvbuf(stdout, NULL, _IONBF, 0) == 0);
	if (argc != 7 && argc != 8) {
		fprintf(stderr,
			"usage: %s WIDTH HEIGHT CALCULATOR KEYMAP SESSION OUTPUT\n",
			argv[0]);
		return 2;
	}
	if (argc == 8) {
		assert(!strcmp(argv[7], "storage-failure"));
		storage_failure(&capture, argv);
		return 0;
	}
	start(&capture, argv, "0", "", "", "Back");
	assert(snprintf(path, sizeof(path), "%s/frame-size", argv[6]) <
	       (int)sizeof(path));
	dimensions = fopen(path, "w");
	assert(dimensions);
	assert(fprintf(dimensions, "%d %d\n", capture.peer.width,
		       capture.peer.height) > 0);
	assert(!fclose(dimensions));
	press(&capture, 1, KEY_NUMERIC_2, "2");
	wait_legends(&capture, "Options", "", "Clear");
	press(&capture, 1, KEY_UP, NULL);
	press(&capture, 1, KEY_NUMERIC_3, "3");
	wait_legends(&capture, "Options", "Equals", "Clear");
	save_frame(&capture, "calculator-pending");
	press(&capture, 1, KEY_OK, "5");
	wait_legends(&capture, "Back", "", "Clear");
	save_frame(&capture, "calculator-result");
	press(&capture, 1, KEY_F14, "0");
	wait_legends(&capture, "", "", "Back");
	capture_number_limit(&capture);
	press(&capture, 0, KEY_1, "1");
	press(&capture, 0, KEY_COMMA, "1.");
	press(&capture, 0, KEY_5, "1.5");
	press(&capture, 0, KEY_X, NULL);
	press(&capture, 0, KEY_2, "2");
	press(&capture, 0, KEY_ENTER, "3");
	wait_legends(&capture, "Back", "", "Clear");
	press(&capture, 1, KEY_F14, "0");
	press(&capture, 1, KEY_NUMERIC_8, "8");
	press(&capture, 1, KEY_RIGHT, NULL);
	press(&capture, 1, KEY_NUMERIC_0, "0");
	press(&capture, 1, KEY_OK, "Error");
	wait_legends(&capture, "Back", "", "Reset");
	save_frame(&capture, "calculator-error");
	press(&capture, 1, KEY_NUMERIC_4, "4");
	reset_from_options(&capture);
	press(&capture, 1, KEY_NUMERIC_1, "1");
	press(&capture, 1, KEY_NUMERIC_2, "12");
	press(&capture, 0, KEY_COMPOSE, NULL);
	wait_legends(&capture, "", "Select", "Back");
	save_frame(&capture, "calculator-menu");
	press(&capture, 1, KEY_OK, "0");
	wait_legends(&capture, "", "", "Back");
	cancel_clear_holds(&capture);
	press(&capture, 1, KEY_NUMERIC_9, "9");
	press(&capture, 1, KEY_UP, NULL);
	press(&capture, 1, KEY_NUMERIC_4, "4");
	lifecycle(&capture, FPLINUX_UI_SESSION_BACKGROUND);
	expect_saved(&capture, "=", "13");
	lifecycle(&capture, FPLINUX_UI_SESSION_FOREGROUND);
	press(&capture, 1, KEY_NUMERIC_5, "45");
	close_and_reap(&capture);
	start(&capture, argv, "45", "Options", "Equals", "Clear");
	press(&capture, 1, KEY_OK, "54");
	wait_legends(&capture, "Back", "", "Clear");
	fplinux_ui_frame_send_key(&capture.peer.keyboards[1], KEY_F13, true);
	for (;;) {
		struct fplinux_ui_session_message message =
			fplinux_ui_frame_wait_message(&capture.peer,
						      capture.session);

		if (message.type == FPLINUX_UI_SESSION_EXIT)
			break;
	}
	close_and_reap(&capture);
	expect_saved(&capture, "*2=", "108");
	puts("calculator: phone and keyboard; menu; Clear holds; hidden/background/close persistence; reopen=54; Back preserves result");
	return 0;
}
