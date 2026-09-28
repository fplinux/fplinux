/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "brightness-ui.h"
#include "fplinux-brightness-client.h"
#include "fplinux-cli.h"
#include "fplinux-drm-session.h"
#include "fplinux-input-session.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

struct brightness_app {
	struct fplinux_brightness_client brightness;
	struct fplinux_drm_session display;
	struct fplinux_input_session input;
	unsigned int level;
	bool display_open;
	bool input_open;
	bool input_enabled;
	bool dirty;
	bool refresh;
	bool set_failed;
};

static volatile sig_atomic_t stop_signal;

static void request_stop(int number)
{
	stop_signal = number;
}

static bool change_focus(struct fplinux_drm_session *display, bool active,
			 void *data)
{
	struct brightness_app *app = data;
	struct fplinux_input_event event;
	char error[160];

	(void)display;
	if (!active && app->input_enabled) {
		fplinux_input_session_suspend(&app->input);
		while (fplinux_input_session_next(&app->input, &event))
			;
		app->input_enabled = false;
	} else if (active && !app->input_enabled) {
		if (!fplinux_input_session_resume(&app->input, error,
						  sizeof(error))) {
			fprintf(stderr, "fplinux-brightness-ui: %s\n", error);
			errno = EIO;
			return false;
		}
		app->input_enabled = true;
		app->refresh = true;
		app->dirty = true;
	}
	return true;
}

static bool present(struct brightness_app *app)
{
	unsigned int page =
		app->display.pages > 1U ? 1U - app->display.shown_page : 0U;
	struct brightness_ui_surface surface = {
		.pixels = (uint16_t *)(app->display.mapping +
				       page * app->display.page_bytes),
		.width = app->display.width,
		.height = app->display.height,
		.stride_bytes = app->display.stride,
	};

	if (!brightness_ui_render(&surface, app->level, app->set_failed)) {
		errno = EINVAL;
		return false;
	}
	__sync_synchronize();
	return fplinux_drm_session_present(&app->display, page);
}

static int run_app(struct brightness_app *app)
{
	while (!stop_signal) {
		struct pollfd descriptors[2] = {
			{ .fd = fplinux_drm_session_fd(&app->display),
			  .events = POLLIN },
			{ .fd = app->input_enabled ?
					fplinux_input_session_get_fd(
						&app->input) :
					-1,
			  .events = POLLIN },
		};
		struct fplinux_input_event event;

		if (app->refresh && app->display.active) {
			if (fplinux_brightness_get(&app->brightness,
						   &app->level) < 0) {
				fprintf(stderr,
					"fplinux-brightness-ui: cannot read brightness: %s\n",
					strerror(errno));
				return 1;
			}
			app->set_failed = false;
			app->refresh = false;
		}
		if (app->dirty && app->display.active) {
			if (!present(app)) {
				fprintf(stderr,
					"fplinux-brightness-ui: cannot present display: %s\n",
					strerror(errno));
				return 1;
			}
			app->dirty = false;
		}
		if (poll(descriptors, 2, -1) < 0) {
			if (errno == EINTR)
				continue;
			fprintf(stderr,
				"fplinux-brightness-ui: poll failed: %s\n",
				strerror(errno));
			return 1;
		}
		if (descriptors[0].revents &&
		    !fplinux_drm_session_dispatch(&app->display)) {
			fprintf(stderr,
				"fplinux-brightness-ui: VT transition failed: %s\n",
				strerror(errno));
			return 1;
		}
		if (!app->input_enabled || !app->display.active)
			continue;
		while (fplinux_input_session_next(&app->input, &event)) {
			unsigned int requested;
			enum brightness_ui_action action;

			if (event.type != FPLINUX_INPUT_EVENT_KEY ||
			    event.source != FPLINUX_INPUT_SOURCE_KEYPAD ||
			    !event.pressed)
				continue;
			action = brightness_ui_key(app->level, event.code,
						   &requested);
			if (action == BRIGHTNESS_UI_EXIT)
				return 0;
			if (action != BRIGHTNESS_UI_SET)
				continue;
			if (fplinux_brightness_set(&app->brightness,
						   requested) < 0) {
				fprintf(stderr,
					"fplinux-brightness-ui: cannot set brightness: %s\n",
					strerror(errno));
				app->set_failed = true;
			} else {
				app->level = requested;
				app->set_failed = false;
			}
			app->dirty = true;
		}
	}
	return 128 + stop_signal;
}

static bool close_app(struct brightness_app *app)
{
	bool restored = true;

	if (app->input_open)
		fplinux_input_session_close(&app->input);
	if (app->display_open) {
		fplinux_drm_session_set_active_handler(&app->display, NULL,
						       NULL);
		if (!fplinux_drm_session_close(&app->display)) {
			fprintf(stderr,
				"fplinux-brightness-ui: display restore was incomplete\n");
			restored = false;
		}
	}
	fplinux_brightness_close(&app->brightness);
	return restored;
}

int main(int argc, char **argv)
{
	struct fplinux_cli cli = {
		.program = "fplinux-brightness-ui",
		.description = "Adjust the phone LCD level with Up and Down",
	};
	struct brightness_app app = {
		.brightness = { .fd = -1 },
	};
	struct sigaction action = { .sa_handler = request_stop };
	char error[160];
	int result = fplinux_cli_parse(&cli, argc, argv);

	if (result != FPLINUX_CLI_READY)
		return result;
	if (sigaction(SIGINT, &action, NULL) < 0 ||
	    sigaction(SIGTERM, &action, NULL) < 0) {
		fprintf(stderr,
			"fplinux-brightness-ui: cannot install signal handlers: %s\n",
			strerror(errno));
		return 1;
	}
	if (fplinux_brightness_connect(&app.brightness, NULL) < 0) {
		fprintf(stderr,
			"fplinux-brightness-ui: cannot connect to brightness service: %s\n",
			strerror(errno));
		return 1;
	}
	if (fplinux_brightness_get(&app.brightness, &app.level) < 0) {
		fprintf(stderr,
			"fplinux-brightness-ui: cannot read brightness: %s\n",
			strerror(errno));
		fplinux_brightness_close(&app.brightness);
		return 1;
	}
	if (!fplinux_drm_session_open(&app.display, "/dev/dri/card0", NULL,
				      DRM_FORMAT_RGB565, error,
				      sizeof(error))) {
		fprintf(stderr, "fplinux-brightness-ui: %s\n", error);
		close_app(&app);
		return 1;
	}
	app.display_open = true;
	if (!fplinux_input_session_open(
		    &app.input,
		    FPLINUX_INPUT_SOURCE_MASK(FPLINUX_INPUT_SOURCE_KEYPAD),
		    error, sizeof(error))) {
		fprintf(stderr, "fplinux-brightness-ui: %s\n", error);
		close_app(&app);
		return 1;
	}
	app.input_open = true;
	app.input_enabled = true;
	app.dirty = true;
	if (!fplinux_drm_session_set_active_handler(&app.display, change_focus,
						    &app)) {
		fprintf(stderr,
			"fplinux-brightness-ui: cannot follow VT focus: %s\n",
			strerror(errno));
		close_app(&app);
		return 1;
	}
	result = run_app(&app);
	if (!close_app(&app) && result == 0)
		result = 1;
	return result;
}
