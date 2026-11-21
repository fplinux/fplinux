/* SPDX-License-Identifier: GPL-2.0-only */
#define _POSIX_C_SOURCE 200809L
#include "ui-frame-peer.h"
#include "screen-frame.h"
#include "fplinux-ui-profile.h"

#include <assert.h>
#include <linux/input-event-codes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static void send_media_level(int session, unsigned int update)
{
	struct fplinux_ui_session_message message = {
		.type = FPLINUX_UI_SESSION_SYSTEM_LEVELS,
		.system_levels.level = {
			[FPLINUX_SYSTEM_LEVEL_MEDIA] = {
				.availability = FPLINUX_SYSTEM_LEVEL_READY,
				.maximum = 10,
				.value = 7,
				.pending = true,
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
				.value = 4,
			},
		},
	};
	struct fplinux_system_level_state *media =
		&message.system_levels.level[FPLINUX_SYSTEM_LEVEL_MEDIA];

	if (update) {
		media->pending = false;
		media->value = update == 1 ? 3 : 0;
		if (update == 2 || update == 3)
			media->availability =
				update == 2 ? FPLINUX_SYSTEM_LEVEL_UNAVAILABLE :
					      FPLINUX_SYSTEM_LEVEL_UNKNOWN;
	}
	assert(fplinux_ui_session_send(session, &message));
}

static void wait_legends(struct fplinux_ui_frame_peer *compositor, int session,
			 const char *left, const char *centre,
			 const char *right)
{
	struct fplinux_ui_session_message message;

	do {
		message = fplinux_ui_frame_wait_message(compositor, session);
	} while (message.type != FPLINUX_UI_SESSION_SOFTKEYS ||
		 strcmp(message.left, left) || strcmp(message.centre, centre) ||
		 strcmp(message.right, right));
}

static void press(struct fplinux_ui_frame_peer *compositor, unsigned int code)
{
	fplinux_ui_frame_send_key(&compositor->keyboards[1], code, true);
	fplinux_ui_frame_send_key(&compositor->keyboards[1], code, false);
}

static void exercise_repeat(struct fplinux_ui_frame_peer *compositor,
			    int session, bool russian)
{
	const char *options = russian ? "Опции" : "Options";
	const char *open = russian ? "Открыть" : "Open";
	const char *select = russian ? "Выбрать" : "Select";
	const char *back = russian ? "Назад" : "Back";

	wait_legends(compositor, session, options, open, back);
	assert(compositor->keyboards[1].keyboard);
	fplinux_ui_frame_focus_keyboard(&compositor->keyboards[1]);
	press(compositor, KEY_F13);
	wait_legends(compositor, session, "", select, back);
	/* Pass Open folder, Queue and Stop to reach Repeat. */
	for (int item = 0; item < 3; ++item)
		press(compositor, KEY_DOWN);
	press(compositor, KEY_OK);
	/* The Off row is selected initially. Choose Queue. */
	press(compositor, KEY_DOWN);
	press(compositor, KEY_OK);
	wait_legends(compositor, session, options, open, back);
}

int main(int argc, char **argv)
{
	assert(argc == 9 || argc == 10);
	alarm(10);
	bool commands = argc == 10;
	int width = atoi(argv[4]), height = atoi(argv[5]);
	struct fplinux_ui_session_message config = {
		.type = FPLINUX_UI_SESSION_CONFIG,
		.mode = FPLINUX_UI_SESSION_INTEGRATED,
		.screen_width = width,
		.screen_height = height,
		.audio = true,
		.theme = !strcmp(argv[6], "dark") ?
				 FPLINUX_UI_SESSION_THEME_DARK :
				 FPLINUX_UI_SESSION_THEME_LIGHT,
	};
	strcpy(config.locale, argv[8]);
	strcpy(config.input_locale, "en");
	const struct fplinux_ui_profile_spec *profile = fplinux_ui_profile_spec(
		fplinux_ui_profile_for_screen(width, height));
	assert(profile);
	int surface_height =
		height - profile->status_height - profile->softkey_height;
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
		if (commands)
			execl(argv[1], argv[1], (char *)NULL);
		else
			execl(argv[1], argv[1], argv[7], (char *)NULL);
		_exit(127);
	}
	close(wayland[1]);
	fplinux_ui_frame_init(&compositor, wayland[0], width, surface_height,
			      commands ? argv[9] : NULL);
	compositor.release_frames = true;
	int session = fplinux_ui_frame_accept_session(&compositor, listener);
	struct fplinux_ui_session_message message =
		fplinux_ui_frame_wait_message(&compositor, session);
	assert(message.type == FPLINUX_UI_SESSION_HELLO);
	assert(fplinux_ui_session_send(session, &config));
	if (commands) {
		exercise_repeat(&compositor, session, !strcmp(argv[8], "ru"));
	} else {
		send_media_level(session, 0);
		do {
			message = fplinux_ui_frame_wait_message(&compositor,
								session);
		} while (message.type != FPLINUX_UI_SESSION_READY);
		assert(compositor.commits > 0 && compositor.width == width &&
		       compositor.height == surface_height);
		if (strcmp(argv[3], "-"))
			assert(screen_frame_write(argv[3], compositor.pixels,
						  width, compositor.height,
						  (size_t)width * 2));
		for (unsigned int update = 1; update < 5; ++update) {
			unsigned int previous = compositor.commits;
			char expected[32];

			send_media_level(session, update);
			message = fplinux_ui_frame_wait_message(&compositor,
								session);
			assert(message.type == FPLINUX_UI_SESSION_TITLE);
			snprintf(expected, sizeof(expected), "level-%u",
				 update);
			assert(!strcmp(message.title, expected));
			while (compositor.commits == previous)
				fplinux_ui_frame_pump(&compositor);
		}
	}
	message = (struct fplinux_ui_session_message){
		.type = FPLINUX_UI_SESSION_CLOSE
	};
	assert(fplinux_ui_session_send(session, &message));
	int status;
	while (waitpid(child, &status, WNOHANG) == 0)
		fplinux_ui_frame_pump(&compositor);
	assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	close(session);
	close(listener);
	fplinux_ui_frame_cleanup(&compositor);
	assert(unlink(argv[2]) == 0);
	printf("music-view: %dx%d %s %s %s\n", width, height, argv[6], argv[7],
	       argv[8]);
	return 0;
}
