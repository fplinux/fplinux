/* SPDX-License-Identifier: GPL-2.0-only */
#define _POSIX_C_SOURCE 200809L
#include "fplinux-ui-prefs.h"
#include "fplinux-ui-profile.h"
#include "ui-frame-peer.h"

#include <assert.h>
#include <errno.h>
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static void press(struct fplinux_ui_frame_peer *compositor, unsigned int code)
{
	struct fplinux_ui_frame_keyboard *keypad = &compositor->keyboards[1];

	fplinux_ui_frame_send_key(keypad, code, true);
	fplinux_ui_frame_send_key(keypad, code, false);
}

static void wait_right_label(struct fplinux_ui_frame_peer *compositor,
			     int session, const char *label)
{
	struct fplinux_ui_session_message message;

	do {
		message = fplinux_ui_frame_wait_message(compositor, session);
	} while (message.type != FPLINUX_UI_SESSION_SOFTKEYS ||
		 strcmp(message.right, label));
}

static void wait_saved(struct fplinux_ui_frame_peer *compositor,
		       const char *key, const char *value)
{
	uint64_t deadline = fplinux_ui_frame_now_ms() + 3000;

	do {
		struct fplinux_ui_prefs *prefs =
			fplinux_ui_prefs_open("org.fplinux.music");
		assert(prefs);
		bool matched =
			!strcmp(fplinux_ui_prefs_get(prefs, key, ""), value);
		fplinux_ui_prefs_close(prefs);
		if (matched)
			return;
		fplinux_ui_frame_pump(compositor);
	} while (fplinux_ui_frame_now_ms() < deadline);
	fprintf(stderr, "Music did not save %s=%s\n", key, value);
	assert(false);
}

static void open_document(struct fplinux_ui_frame_peer *compositor, int session,
			  const char *path, uint32_t serial, int error)
{
	struct fplinux_ui_session_message message = {
		.type = FPLINUX_UI_SESSION_DOCUMENT,
		.activation = FPLINUX_UI_SESSION_ACTIVATION_OPEN,
		.serial = serial,
	};
	assert(strlen(path) < sizeof(message.path));
	strcpy(message.path, path);
	assert(fplinux_ui_session_send(session, &message));
	bool waiting = false;
	for (;;) {
		message = fplinux_ui_frame_wait_message(compositor, session);
		if (message.type != FPLINUX_UI_SESSION_DOCUMENT_RESULT)
			continue;
		assert(message.serial == serial);
		if (message.document_status == FPLINUX_UI_DOCUMENT_WAIT) {
			waiting = true;
			continue;
		}
		assert(waiting);
		assert(message.document_status ==
		       (error ? FPLINUX_UI_DOCUMENT_ERROR :
				FPLINUX_UI_DOCUMENT_ACCEPTED));
		assert(message.document_error == error);
		return;
	}
}

static void exercise_restoration(struct fplinux_ui_frame_peer *compositor,
				 int session, const char *replacement,
				 const char *track)
{
	/* Music adds the default repeat option when it finishes restoration. */
	wait_saved(compositor, "repeat", "0");
	fplinux_ui_frame_focus_keyboard(&compositor->keyboards[1]);
	press(compositor, KEY_RIGHT);
	wait_right_label(compositor, session, "Close");
	if (strcmp(replacement, "-")) {
		assert(rename(replacement, track) == 0);
		press(compositor, KEY_ESC);
		wait_right_label(compositor, session, "Back");
		press(compositor, KEY_RIGHT);
		wait_saved(compositor, "track", "b.wav");
	}
}

int main(int argc, char **argv)
{
	assert(argc == 6 || (argc == 7 && !strcmp(argv[6], "document")));
	alarm(10);
	const struct fplinux_ui_session_message config = {
		.type = FPLINUX_UI_SESSION_CONFIG,
		.mode = FPLINUX_UI_SESSION_INTEGRATED,
		.screen_width = 240,
		.screen_height = 320,
		.locale = "en",
		.input_locale = "en",
	};
	const struct fplinux_ui_profile_spec *profile = fplinux_ui_profile_spec(
		fplinux_ui_profile_for_screen(config.screen_width,
					      config.screen_height));
	assert(profile);
	struct fplinux_ui_frame_peer compositor = { 0 };
	int wayland[2];
	int listener = fplinux_ui_frame_listen_session(argv[2]);
	fplinux_ui_frame_socketpair(wayland);
	pid_t child = fork();
	assert(child >= 0);
	if (!child) {
		close(listener);
		close(wayland[0]);
		fplinux_ui_frame_client_environment(wayland[1], argv[2]);
		execl(argv[1], argv[1], (char *)NULL);
		_exit(127);
	}
	close(wayland[1]);
	int height = config.screen_height - profile->status_height -
		     profile->softkey_height;
	fplinux_ui_frame_init(&compositor, wayland[0], config.screen_width,
			      height, argv[3]);
	compositor.release_frames = true;
	int session = fplinux_ui_frame_accept_session(&compositor, listener);
	struct fplinux_ui_session_message message =
		fplinux_ui_frame_wait_message(&compositor, session);
	assert(message.type == FPLINUX_UI_SESSION_HELLO);
	assert(fplinux_ui_session_send(session, &config));
	do {
		message = fplinux_ui_frame_wait_message(&compositor, session);
	} while (message.type != FPLINUX_UI_SESSION_READY);
	if (argc == 7) {
		open_document(&compositor, session, argv[4], 1, 0);
		open_document(&compositor, session, argv[5], 2, EINVAL);
	} else {
		exercise_restoration(&compositor, session, argv[4], argv[5]);
	}

	message = (struct fplinux_ui_session_message){
		.type = FPLINUX_UI_SESSION_CLOSE,
	};
	assert(fplinux_ui_session_send(session, &message));
	int status;
	pid_t finished;
	while ((finished = waitpid(child, &status, WNOHANG)) == 0)
		fplinux_ui_frame_pump(&compositor);
	assert(finished == child && WIFEXITED(status) &&
	       WEXITSTATUS(status) == 0);
	close(session);
	close(listener);
	fplinux_ui_frame_cleanup(&compositor);
	assert(unlink(argv[2]) == 0);
	return 0;
}
