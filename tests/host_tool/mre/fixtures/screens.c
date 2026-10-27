/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "fplinux-ui-profile.h"
#include "ui-frame-peer.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

struct capture {
	struct fplinux_ui_frame_peer peer;
	int session, listener, width, height;
	pid_t application;
	struct fplinux_ui_session_message result;
	unsigned int results, close_waits, close_cancels, close_ready, homes,
		exits;
	unsigned int accepted;
	char centre[128];
	char right[128];
};

static void pump(struct capture *capture)
{
	struct fplinux_ui_session_message message;
	int received;

	fplinux_ui_frame_pump(&capture->peer);
	while ((received = fplinux_ui_session_receive(capture->session,
						      &message)) == 1) {
		if (message.type == FPLINUX_UI_SESSION_MODE) {
			bool fullscreen = message.mode ==
					  FPLINUX_UI_SESSION_FULLSCREEN;
			const struct fplinux_ui_profile_spec *profile =
				fplinux_ui_profile_spec(
					fplinux_ui_profile_for_screen(
						capture->width,
						capture->height));
			int height = fullscreen ?
					     capture->height :
					     capture->height -
						     profile->status_height -
						     profile->softkey_height;
			fplinux_ui_frame_configure(&capture->peer,
						   capture->width, height,
						   fullscreen);
		} else if (message.type == FPLINUX_UI_SESSION_DOCUMENT_RESULT) {
			capture->results++;
			capture->accepted += message.document_status ==
					     FPLINUX_UI_DOCUMENT_ACCEPTED;
			capture->result = message;
		} else if (message.type == FPLINUX_UI_SESSION_SOFTKEYS) {
			strcpy(capture->centre, message.centre);
			strcpy(capture->right, message.right);
		} else if (message.type == FPLINUX_UI_SESSION_HOME) {
			capture->homes++;
		} else if (message.type == FPLINUX_UI_SESSION_EXIT) {
			capture->exits++;
		} else if (message.type == FPLINUX_UI_SESSION_CLOSE_REPLY) {
			capture->close_waits += !strcmp(message.value, "wait");
			capture->close_cancels +=
				!strcmp(message.value, "cancel");
			capture->close_ready += !strcmp(message.value, "ready");
		}
	}
	assert(received == 0);
}

static void pump_for(struct capture *capture, unsigned int milliseconds)
{
	uint64_t until = fplinux_ui_frame_now_ms() + milliseconds;
	while (fplinux_ui_frame_now_ms() < until)
		pump(capture);
}

static void wait_result(struct capture *capture, uint32_t serial,
			enum fplinux_ui_document_status status)
{
	uint64_t until = fplinux_ui_frame_now_ms() + 5000;

	do {
		pump(capture);
		if (capture->result.serial == serial &&
		    capture->result.document_status == status)
			return;
	} while (fplinux_ui_frame_now_ms() < until);
	fprintf(stderr, "expected document %u status %d; got %u status %d\n",
		serial, status, capture->result.serial,
		capture->result.document_status);
	assert(false);
}

static void press(struct capture *capture, unsigned int code)
{
	fplinux_ui_frame_send_key(&capture->peer.keyboards[0], code, true);
	fplinux_ui_frame_send_key(&capture->peer.keyboards[0], code, false);
	pump_for(capture, 40);
}

static void document(struct capture *capture, const char *directory,
		     const char *name, uint32_t serial)
{
	struct fplinux_ui_session_message message = {
		.type = FPLINUX_UI_SESSION_DOCUMENT,
		.serial = serial,
	};

	assert(snprintf(message.path, sizeof(message.path), "%s/%s", directory,
			name) < (int)sizeof(message.path));
	assert(fplinux_ui_session_send(capture->session, &message));
}

static void wait_centre(struct capture *capture, const char *label)
{
	uint64_t until = fplinux_ui_frame_now_ms() + 5000;

	do {
		pump(capture);
		if (!strcmp(capture->centre, label))
			return;
	} while (fplinux_ui_frame_now_ms() < until);
	fprintf(stderr, "expected centre %s, actual centre %s\n", label,
		capture->centre);
	assert(false);
}

static void decision(struct capture *capture, const char *locale)
{
	wait_centre(capture, !strcmp(locale, "ru") ? "Закрыть" : "Close");
}

static void assert_frame(struct capture *capture, uint16_t expected)
{
	int left = 0;

	pump_for(capture, 60);
	assert(capture->peer.width == capture->width &&
	       capture->peer.height == capture->height);
	assert(capture->peer.pixels[(capture->height / 2) * capture->width +
				    capture->width / 2] == expected);
	if (capture->width == 128)
		left = capture->height == 128 ? 16 : 4;
	else if (capture->width == 176)
		left = 5;
	else if (capture->width == 320)
		left = 70;
	const uint16_t *row =
		capture->peer.pixels + (capture->height / 2) * capture->width;
	assert(row[left] == expected);
	if (left)
		assert(row[left - 1] == 0 && row[0] == 0);
}

static void first_guest_ui(struct capture *capture, const char *directory,
			   const char *locale, const char *scenario)
{
	const char *release = getenv("FPLINUX_MRE_ENGINE_RELEASE");
	const char *label =
		!strcmp(scenario, "selector") ?
			(!strcmp(locale, "ru") ? "Выбрать" : "Select") :
			(!strcmp(locale, "ru") ? "Удалить" : "Clear");
	const char *actual_label = !strcmp(scenario, "selector") ?
					   capture->centre :
					   capture->right;
	uint64_t until;
	unsigned int commits;
	int fd;

	/* Hold both SHM buffers before the peer publishes its first guest UI. */
	capture->peer.release_frames = false;
	document(capture, directory, "first.vxp", 1);
	wait_result(capture, 1, FPLINUX_UI_DOCUMENT_WAIT);
	until = fplinux_ui_frame_now_ms() + 5000;
	while ((!capture->peer.held[0] || !capture->peer.held[1]) &&
	       fplinux_ui_frame_now_ms() < until)
		pump(capture);
	assert(capture->peer.held[0] && capture->peer.held[1]);
	pump_for(capture, 100);
	assert(capture->result.serial == 1 &&
	       capture->result.document_status == FPLINUX_UI_DOCUMENT_WAIT &&
	       !capture->accepted);
	commits = capture->peer.commits;
	fd = open(release, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	assert(fd >= 0 && close(fd) == 0);
	if (!strcmp(scenario, "malformed")) {
		wait_result(capture, 1, FPLINUX_UI_DOCUMENT_ERROR);
		assert(capture->result.document_error == EIO &&
		       !capture->accepted);
	} else {
		until = fplinux_ui_frame_now_ms() + 5000;
		while (strcmp(actual_label, label) &&
		       fplinux_ui_frame_now_ms() < until)
			pump(capture);
		assert(!strcmp(actual_label, label));
		pump_for(capture, 100);
		assert(capture->peer.commits == commits &&
		       capture->result.document_status ==
			       FPLINUX_UI_DOCUMENT_WAIT &&
		       !capture->accepted);
	}
	capture->peer.release_frames = true;
	fplinux_ui_frame_release_buffers(&capture->peer);
	if (!strcmp(scenario, "malformed"))
		return;
	/* A queued commit cannot acknowledge the view before its Wayland sync. */
	struct pollfd descriptor = { .fd = capture->session, .events = POLLIN };
	assert(poll(&descriptor, 1, 100) == 0);
	wait_result(capture, 1, FPLINUX_UI_DOCUMENT_ACCEPTED);
	assert(capture->peer.commits > commits && capture->accepted == 1);
	assert(capture->peer.width == capture->width &&
	       capture->peer.height < capture->height);
	if (!strncmp(scenario, "editor-", 7)) {
		if (strstr(scenario, "options")) {
			press(capture, KEY_COMPOSE);
			wait_centre(capture, !strcmp(locale, "ru") ? "Выбрать" :
								     "Select");
		}
		if (strstr(scenario, "end")) {
			const char *end = getenv("FPLINUX_MRE_ENGINE_END");
			int status;

			fd = open(end, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
				  0600);
			assert(fd >= 0 && close(fd) == 0);
			until = fplinux_ui_frame_now_ms() + 5000;
			while (!capture->exits &&
			       fplinux_ui_frame_now_ms() < until)
				pump(capture);
			assert(capture->exits == 1);
			/* The manager has not sent CLOSE yet; pending input must be harmless. */
			press(capture, KEY_ESC);
			press(capture, KEY_Z);
			press(capture, KEY_ENTER);
			assert(waitpid(capture->application, &status,
				       WNOHANG) == 0);
			assert(capture->accepted == 1 && capture->results == 2);
		}
		return;
	}
	press(capture, KEY_HOME);
	assert(capture->homes == 1 && capture->accepted == 1);

	/* A second open reaches validation and consent while the modal stays live. */
	document(capture, directory, "invalid.vxp", 2);
	wait_result(capture, 2, FPLINUX_UI_DOCUMENT_ERROR);
	assert(capture->result.document_error == ENOEXEC);
	document(capture, directory, "second.vxp", 3);
	decision(capture, locale);
	press(capture, KEY_ESC);
	wait_result(capture, 3, FPLINUX_UI_DOCUMENT_CANCELLED);
	assert(!strcmp(actual_label, label) && capture->accepted == 1);
	press(capture, !strcmp(scenario, "selector") ? KEY_ESC : KEY_ENTER);
	assert_frame(capture, 0x001f);
	assert(capture->accepted == 1);
}

static void close_application(struct capture *capture, const char *session_path)
{
	struct fplinux_ui_session_message message = {
		.type = FPLINUX_UI_SESSION_CLOSE,
	};
	uint64_t until = fplinux_ui_frame_now_ms() + 5000;
	int status;
	pid_t ended;

	assert(fplinux_ui_session_send(capture->session, &message));
	do {
		fplinux_ui_frame_pump(&capture->peer);
		ended = waitpid(capture->application, &status, WNOHANG);
	} while (!ended && fplinux_ui_frame_now_ms() < until);
	assert(ended == capture->application && WIFEXITED(status) &&
	       WEXITSTATUS(status) == 0);
	close(capture->session);
	close(capture->listener);
	fplinux_ui_frame_cleanup(&capture->peer);
	assert(unlink(session_path) == 0);
}

static unsigned int tick_count(char *last)
{
	const char *path = getenv("FPLINUX_MRE_TICK_RECORD");
	FILE *input = fopen(path, "r");
	unsigned int ticks = 0;
	int value;

	assert(input);
	*last = 0;
	while ((value = fgetc(input)) != EOF) {
		if (value == 'T')
			ticks++;
		if (value != '\n')
			*last = (char)value;
	}
	assert(!ferror(input) && fclose(input) == 0);
	return ticks;
}

static void assert_idle_tick(struct capture *capture)
{
	uint64_t until = fplinux_ui_frame_now_ms() + 5000;
	char last;
	unsigned int ticks;

	do {
		pump(capture);
		ticks = tick_count(&last);
	} while (last != 'P' && fplinux_ui_frame_now_ms() < until);
	assert(last == 'P');
	pump_for(capture, 150);
	assert(tick_count(&last) == ticks && last == 'P');
}

static void idle_and_background_completion(struct capture *capture,
					   const char *directory)
{
	struct fplinux_ui_session_message message = {
		.type = FPLINUX_UI_SESSION_LIFECYCLE,
		.lifecycle = FPLINUX_UI_SESSION_BACKGROUND,
	};

	assert_idle_tick(capture);
	document(capture, directory, "invalid.vxp", 1);
	wait_result(capture, 1, FPLINUX_UI_DOCUMENT_ERROR);
	assert(capture->result.document_error == ENOEXEC);
	assert_idle_tick(capture);
	document(capture, directory, "first.vxp", 2);
	wait_result(capture, 2, FPLINUX_UI_DOCUMENT_ACCEPTED);
	assert_frame(capture, 0x001f);
	press(capture, KEY_HOME);
	assert(capture->homes == 1);
	assert(fplinux_ui_session_send(capture->session, &message));
	message = (struct fplinux_ui_session_message){
		.type = FPLINUX_UI_SESSION_VISIBILITY,
		.visible = false,
	};
	assert(fplinux_ui_session_send(capture->session, &message));
	/* The execution peer exits on hide. The real frontend must reap it and
	 * retain the requested picture while its surface is hidden. */
	assert_idle_tick(capture);
	message = (struct fplinux_ui_session_message){
		.type = FPLINUX_UI_SESSION_LIFECYCLE,
		.lifecycle = FPLINUX_UI_SESSION_FOREGROUND,
	};
	assert(fplinux_ui_session_send(capture->session, &message));
	message = (struct fplinux_ui_session_message){
		.type = FPLINUX_UI_SESSION_VISIBILITY,
		.visible = true,
	};
	assert(fplinux_ui_session_send(capture->session, &message));
	message = (struct fplinux_ui_session_message){
		.type = FPLINUX_UI_SESSION_ACTIVATION,
		.activation = FPLINUX_UI_SESSION_ACTIVATION_RESUME,
		.serial = 5,
	};
	assert(fplinux_ui_session_send(capture->session, &message));
	assert_frame(capture, 0x001f);
	assert_idle_tick(capture);
	message = (struct fplinux_ui_session_message){
		.type = FPLINUX_UI_SESSION_PREPARE_CLOSE,
		.serial = 90,
	};
	strcpy(message.reason, "user");
	assert(fplinux_ui_session_send(capture->session, &message));
	pump_for(capture, 60);
	assert(capture->close_ready == 1 && !capture->close_waits);
}

int main(int argc, char **argv)
{
	struct capture capture = { 0 };
	struct fplinux_ui_session_message config = {
		.type = FPLINUX_UI_SESSION_CONFIG,
		.mode = FPLINUX_UI_SESSION_INTEGRATED,
	};
	int wayland[2], status;

	assert(argc == 8 || argc == 9);
	capture.width = config.screen_width = atoi(argv[5]);
	capture.height = config.screen_height = atoi(argv[6]);
	snprintf(config.locale, sizeof(config.locale), "%s", argv[7]);
	strcpy(config.input_locale, "en");
	const struct fplinux_ui_profile_spec *profile = fplinux_ui_profile_spec(
		fplinux_ui_profile_for_screen(capture.width, capture.height));
	capture.listener = fplinux_ui_frame_listen_session(argv[3]);
	fplinux_ui_frame_socketpair(wayland);
	capture.application = fork();
	assert(capture.application >= 0);
	if (!capture.application) {
		close(capture.listener);
		close(wayland[0]);
		fplinux_ui_frame_client_environment(wayland[1], argv[3]);
		assert(setenv("HOME", argv[4], 1) == 0);
		assert(setenv("LANG", "en_US.UTF-8", 1) == 0);
		assert(unsetenv("LANGUAGE") == 0);
		execl(argv[1], argv[1], (char *)NULL);
		_exit(127);
	}
	close(wayland[1]);
	fplinux_ui_frame_init(&capture.peer, wayland[0], capture.width,
			      capture.height - profile->status_height -
				      profile->softkey_height,
			      argv[2]);
	capture.session = fplinux_ui_frame_accept_session(&capture.peer,
							  capture.listener);
	struct fplinux_ui_session_message hello =
		fplinux_ui_frame_wait_message(&capture.peer, capture.session);
	assert(hello.type == FPLINUX_UI_SESSION_HELLO &&
	       !strcmp(hello.app_id, "org.fplinux.mre"));
	assert(fplinux_ui_session_send(capture.session, &config));
	pump_for(&capture, 100);
	fplinux_ui_frame_focus_keyboard(&capture.peer.keyboards[0]);
	if (argc == 9) {
		if (!strcmp(argv[8], "idle-background"))
			idle_and_background_completion(&capture, argv[4]);
		else
			first_guest_ui(&capture, argv[4], argv[7], argv[8]);
		close_application(&capture, argv[3]);
		puts("mre-ui: first modal document lifecycle checked");
		return 0;
	}
	document(&capture, argv[4], "first.vxp", 1);
	wait_result(&capture, 1, FPLINUX_UI_DOCUMENT_ACCEPTED);
	assert_frame(&capture, 0xf800);
	press(&capture, KEY_ENTER);
	press(&capture, KEY_HOME);
	assert(capture.homes == 1);
	struct fplinux_ui_session_message message = {
		.type = FPLINUX_UI_SESSION_LIFECYCLE,
		.lifecycle = FPLINUX_UI_SESSION_BACKGROUND,
	};
	assert(fplinux_ui_session_send(capture.session, &message));
	message.type = FPLINUX_UI_SESSION_VISIBILITY;
	message.visible = false;
	assert(fplinux_ui_session_send(capture.session, &message));
	pump_for(&capture, 60);
	assert(waitpid(capture.application, &status, WNOHANG) == 0);
	message.type = FPLINUX_UI_SESSION_LIFECYCLE;
	message.lifecycle = FPLINUX_UI_SESSION_FOREGROUND;
	assert(fplinux_ui_session_send(capture.session, &message));
	message.type = FPLINUX_UI_SESSION_VISIBILITY;
	message.visible = true;
	assert(fplinux_ui_session_send(capture.session, &message));
	message.type = FPLINUX_UI_SESSION_ACTIVATION;
	message.activation = FPLINUX_UI_SESSION_ACTIVATION_RESUME;
	message.serial = 5;
	assert(fplinux_ui_session_send(capture.session, &message));
	assert_frame(&capture, 0xf800);
	document(&capture, argv[4], "invalid.vxp", 2);
	wait_result(&capture, 2, FPLINUX_UI_DOCUMENT_ERROR);
	assert_frame(&capture, 0xf800);
	document(&capture, argv[4], "second.vxp", 3);
	decision(&capture, argv[7]);
	assert(capture.result.serial != 3 ||
	       capture.result.document_status == FPLINUX_UI_DOCUMENT_WAIT);
	press(&capture, KEY_ESC);
	wait_result(&capture, 3, FPLINUX_UI_DOCUMENT_CANCELLED);
	assert_frame(&capture, 0xf800);
	press(&capture, KEY_ENTER);
	document(&capture, argv[4], "second.vxp", 4);
	decision(&capture, argv[7]);
	press(&capture, KEY_ENTER);
	wait_result(&capture, 4, FPLINUX_UI_DOCUMENT_ACCEPTED);
	assert_frame(&capture, 0x001f);
	message = (struct fplinux_ui_session_message){
		.type = FPLINUX_UI_SESSION_PREPARE_CLOSE,
		.serial = 90,
	};
	strcpy(message.reason, "user");
	assert(fplinux_ui_session_send(capture.session, &message));
	decision(&capture, argv[7]);
	press(&capture, KEY_ESC);
	assert(capture.close_waits == 1 && capture.close_cancels == 1 &&
	       !capture.close_ready);
	assert_frame(&capture, 0x001f);
	close_application(&capture, argv[3]);
	puts("mre-ui: first frame; retained Home/Resume; invalid preflight; cancel preserves guest; consent replaces; close cancels");
	return 0;
}
