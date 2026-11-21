/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "fplinux-ui-profile.h"
#include "screen-frame.h"
#include "ui-frame-peer.h"

#include <assert.h>
#include <errno.h>
#include <json-c/json.h>
#include <limits.h>
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

struct capture {
	struct fplinux_ui_frame_peer peer;
	struct fplinux_ui_session_message result;
	int session, listener, width, height;
	pid_t application;
	char left[128];
	struct json_object *layout;
	unsigned int layout_commit;
	const char *commit_directory;
	const char *directory;
	bool russian;
	bool mre;
	size_t setting_row;
	const char *app_id;
};

static const char *translated(const struct capture *capture,
			      const char *english, const char *russian)
{
	return capture->russian ? russian : english;
}

static const char *label_text(struct json_object *layout, size_t index)
{
	struct json_object *label = json_object_array_get_idx(layout, index);
	struct json_object *text = json_object_object_get(label, "text");

	assert(json_object_is_type(text, json_type_string));
	return json_object_get_string(text);
}

static bool layout_has_label(struct json_object *layout, const char *text)
{
	for (size_t i = 0; layout && i < json_object_array_length(layout); i++)
		if (!strcmp(label_text(layout, i), text))
			return true;
	return false;
}

static bool has_label(const struct capture *capture, const char *text)
{
	return layout_has_label(capture->layout, text);
}

static void read_layout(struct capture *capture)
{
	char path[PATH_MAX];
	struct json_object *layout;
	int length;

	if (capture->layout_commit == capture->peer.commits)
		return;
	length = snprintf(path, sizeof(path), "%s/%u.json",
			  capture->commit_directory, capture->peer.commits);
	assert(length >= 0 && length < (int)sizeof(path));
	layout = json_object_from_file(path);
	if (!json_object_is_type(layout, json_type_array))
		fprintf(stderr, "missing committed layout %s: %s\n", path,
			json_util_get_last_err());
	assert(json_object_is_type(layout, json_type_array));
	json_object_put(capture->layout);
	capture->layout = layout;
	capture->layout_commit = capture->peer.commits;
}

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
		} else if (message.type == FPLINUX_UI_SESSION_STORAGE_REQUEST) {
			struct fplinux_ui_session_message reply = {
				.type = FPLINUX_UI_SESSION_STORAGE_RESULT,
				.serial = message.serial,
				.volume_result = {
					.request_id = message.serial,
					.snapshot = { .home_persistent = true },
				},
			};

			assert(fplinux_ui_session_send(capture->session,
						       &reply));
		} else if (message.type == FPLINUX_UI_SESSION_DOCUMENT_RESULT) {
			capture->result = message;
		} else if (message.type == FPLINUX_UI_SESSION_SOFTKEYS) {
			strcpy(capture->left, message.left);
		}
	}
	assert(received == 0);
	read_layout(capture);
}

static void settle(struct capture *capture)
{
	uint64_t until = fplinux_ui_frame_now_ms() + 100;

	while (fplinux_ui_frame_now_ms() < until)
		pump(capture);
}

static void wait_label(struct capture *capture, const char *text)
{
	uint64_t until = fplinux_ui_frame_now_ms() + 5000;

	do {
		pump(capture);
		if (has_label(capture, text) && capture->peer.commits &&
		    !capture->peer.configure_pending) {
			settle(capture);
			if (has_label(capture, text) &&
			    !capture->peer.configure_pending)
				return;
		}
	} while (fplinux_ui_frame_now_ms() < until);
	fprintf(stderr, "expected %s, actual %s\n", text,
		json_object_to_json_string_ext(capture->layout,
					       JSON_C_TO_STRING_PLAIN));
	assert(false);
}

static void press(struct capture *capture, unsigned int key)
{
	fplinux_ui_frame_send_key(&capture->peer.keyboards[0], key, true);
	fplinux_ui_frame_send_key(&capture->peer.keyboards[0], key, false);
	settle(capture);
}

static void save_frame(struct capture *capture, const char *name)
{
	char path[1024];
	FILE *labels;

	settle(capture);
	assert(!capture->peer.configure_pending && capture->peer.pixels &&
	       capture->layout);
	assert(snprintf(path, sizeof(path), "%s/%s.rgb565", capture->directory,
			name) < (int)sizeof(path));
	assert(screen_frame_write(path, capture->peer.pixels,
				  capture->peer.width, capture->peer.height,
				  capture->peer.width *
					  sizeof(*capture->peer.pixels)));
	assert(snprintf(path, sizeof(path), "%s/%s.labels", capture->directory,
			name) < (int)sizeof(path));
	labels = fopen(path, "w");
	assert(labels && fputc('|', labels) != EOF);
	for (size_t i = 0; i < json_object_array_length(capture->layout); i++) {
		for (const char *text = label_text(capture->layout, i); *text;
		     text++)
			assert(fputc(*text == '\n' ? ' ' : *text, labels) !=
			       EOF);
		assert(fputc('|', labels) != EOF);
	}
	assert(fputc('\n', labels) != EOF);
	assert(fclose(labels) == 0);
	assert(snprintf(path, sizeof(path), "%s/%s.layout.json",
			capture->directory, name) < (int)sizeof(path));
	assert(json_object_to_file(path, capture->layout) == 0);
}

static void capture_held_buffers(struct capture *capture, const char *redraw)
{
	unsigned int held_commit;
	uint64_t until;
	bool redrawn;

	wait_label(capture, "Open VXP");
	capture->peer.release_frames = false;
	for (unsigned int i = 0; i < 2; ++i) {
		press(capture, KEY_DOWN);
		until = fplinux_ui_frame_now_ms() + 5000;
		while (!capture->peer.held[i] &&
		       fplinux_ui_frame_now_ms() < until)
			pump(capture);
		assert(capture->peer.held[i]);
	}
	save_frame(capture, "before-settings");
	held_commit = capture->peer.commits;
	press(capture, KEY_ENTER);

	until = fplinux_ui_frame_now_ms() + 5000;
	do {
		struct json_object *latest = json_object_from_file(redraw);

		assert(json_object_is_type(latest, json_type_array));
		redrawn = layout_has_label(latest, "Scale");
		json_object_put(latest);
		if (redrawn)
			break;
		pump(capture);
	} while (fplinux_ui_frame_now_ms() < until);
	assert(redrawn);
	save_frame(capture, "settings-with-buffers-held");
	assert(capture->peer.commits == held_commit);

	capture->peer.release_frames = true;
	fplinux_ui_frame_release_buffers(&capture->peer);
	wait_label(capture, "Scale");
	assert(capture->peer.commits > held_commit);
	save_frame(capture, "settings-after-release");
}

static void settings_frames(struct capture *capture, const char *name)
{
	char frame[128];

	wait_label(capture, translated(capture, "Scale", "Масштаб"));
	save_frame(capture, name);
	press(capture, KEY_ENTER);
	assert(snprintf(frame, sizeof(frame), "%s-choice-scale", name) <
	       (int)sizeof(frame));
	save_frame(capture, frame);
	press(capture, KEY_ESC);
	if (!capture->mre && capture->width == 128 && capture->height == 128) {
		for (unsigned int i = 0; i < 3; ++i)
			press(capture, KEY_DOWN);
		wait_label(capture, "JIT");
		assert(snprintf(frame, sizeof(frame), "%s-jit", name) <
		       (int)sizeof(frame));
		save_frame(capture, frame);
	}
	press(capture, KEY_ESC);
}

static void running_home(struct capture *capture, const char *document)
{
	struct fplinux_ui_session_message message = {
		.type = FPLINUX_UI_SESSION_DOCUMENT,
		.serial = 1,
	};
	uint64_t until = fplinux_ui_frame_now_ms() + 5000;

	assert(strlen(document) < sizeof(message.path));
	strcpy(message.path, document);
	assert(fplinux_ui_session_send(capture->session, &message));
	do {
		pump(capture);
		if (capture->result.serial == 1 &&
		    capture->result.document_status ==
			    FPLINUX_UI_DOCUMENT_ACCEPTED)
			break;
	} while (fplinux_ui_frame_now_ms() < until);
	assert(capture->result.serial == 1 &&
	       capture->result.document_status == FPLINUX_UI_DOCUMENT_ACCEPTED);
	message = (struct fplinux_ui_session_message){
		.type = FPLINUX_UI_SESSION_ACTIVATION,
		.activation = FPLINUX_UI_SESSION_ACTIVATION_OPEN,
		.serial = 2,
	};
	assert(fplinux_ui_session_send(capture->session, &message));
	wait_label(capture, translated(capture, "Continue", "Продолжить"));
}

static const char *third_label(const struct capture *capture)
{
	return capture->mre ? "Pointer" : "Size";
}

static void assert_settings(struct capture *capture, const char *scale,
			    const char *rotation, const char *third)
{
	wait_label(capture, third_label(capture));
	assert(has_label(capture, scale) && has_label(capture, rotation) &&
	       has_label(capture, third));
}

static void choose_value(struct capture *capture, size_t row, size_t current,
			 size_t selected)
{
	while (capture->setting_row < row) {
		press(capture, KEY_DOWN);
		++capture->setting_row;
	}
	while (capture->setting_row > row) {
		press(capture, KEY_UP);
		--capture->setting_row;
	}
	press(capture, KEY_ENTER);
	while (current < selected) {
		press(capture, KEY_DOWN);
		++current;
	}
	while (current > selected) {
		press(capture, KEY_UP);
		--current;
	}
	press(capture, KEY_ENTER);
}

static void open_global_settings(struct capture *capture)
{
	wait_label(capture, capture->mre ? "Open VXP" : "Open");
	press(capture, KEY_DOWN);
	press(capture, KEY_DOWN);
	press(capture, KEY_ENTER);
	wait_label(capture, third_label(capture));
	capture->setting_row = 0;
}

static void open_running_settings(struct capture *capture, const char *document,
				  bool program)
{
	running_home(capture, document);
	for (unsigned int i = 0; i < 3; ++i)
		press(capture, KEY_DOWN);
	press(capture, KEY_ENTER);
	wait_label(capture, "This program");
	if (!program)
		press(capture, KEY_DOWN);
	press(capture, KEY_ENTER);
	wait_label(capture, third_label(capture));
	capture->setting_row = 0;
}

static void set_fault(char operation)
{
	FILE *fault = fopen(getenv("FPLINUX_APP_SAVE_FAULT"), "w");

	assert(fault && fputc(operation, fault) == operation &&
	       fclose(fault) == 0);
}

static void dismiss_save_error(struct capture *capture, char operation)
{
	wait_label(capture,
		   operation == 'f' ? "Cannot save" : "Saving not confirmed");
	press(capture, KEY_ESC);
	wait_label(capture, third_label(capture));
	assert(!has_label(capture, "Cannot save") &&
	       !has_label(capture, "Saving not confirmed"));
	assert(has_label(capture, "Unsaved"));
}

static void snapshot_preferences(struct capture *capture)
{
	char path[4096], buffer[1024];
	FILE *input, *output;
	size_t count;

	assert(snprintf(path, sizeof(path), "%s/fplinux/%s/preferences",
			getenv("XDG_STATE_HOME"),
			capture->app_id) < (int)sizeof(path));
	input = fopen(path, "r");
	assert(input || errno == ENOENT);
	assert(snprintf(path, sizeof(path), "%s/failed.preferences",
			capture->directory) < (int)sizeof(path));
	output = fopen(path, "w");
	assert(output);
	if (input) {
		while ((count = fread(buffer, 1, sizeof(buffer), input)) != 0)
			assert(fwrite(buffer, 1, count, output) == count);
		assert(!ferror(input) && fclose(input) == 0);
	} else
		assert(fputs("missing\n", output) >= 0);
	assert(fclose(output) == 0);
}

static void settings_scenario(struct capture *capture, const char *document,
			      const char *scenario)
{
	if (!strcmp(scenario, "roundtrip-write")) {
		open_global_settings(capture);
		choose_value(capture, 0, 0, 2);
		choose_value(capture, 1, 0, 1);
		choose_value(capture, 2, 0, capture->mre ? 1 : 3);
		assert_settings(capture, "Integer", "90°",
				capture->mre ? "On" : "176x208");
		press(capture, KEY_ESC);
		open_running_settings(capture, document, true);
		choose_value(capture, 2, capture->mre ? 1 : 3, 0);
		choose_value(capture, 0, 2, 1);
		choose_value(capture, 1, 1, 2);
		assert_settings(capture, "1:1", "270°",
				capture->mre ? "Off" : "Auto");
	} else if (!strcmp(scenario, "roundtrip-reset")) {
		open_global_settings(capture);
		assert_settings(capture, "Integer", "90°",
				capture->mre ? "On" : "176x208");
		press(capture, KEY_ESC);
		open_running_settings(capture, document, true);
		assert_settings(capture, "1:1", "270°",
				capture->mre ? "Off" : "Auto");
		assert(!strcmp(capture->left, "Options"));
		press(capture, KEY_COMPOSE);
		wait_label(capture, "Reset");
		press(capture, KEY_ENTER);
		assert_settings(capture, "Integer", "90°",
				capture->mre ? "On" : "176x208");
	} else if (!strcmp(scenario, "roundtrip-inherit")) {
		open_running_settings(capture, document, true);
		assert_settings(capture, "Integer", "90°",
				capture->mre ? "On" : "176x208");
	} else if (!strcmp(scenario, "rotation-only")) {
		open_running_settings(capture, document, true);
		choose_value(capture, 1, 0, 2);
		assert_settings(capture, "Fit", "270°",
				capture->mre ? "Off" : "Auto");
		press(capture, KEY_ESC);
		press(capture, KEY_DOWN);
		press(capture, KEY_ENTER);
		wait_label(capture, third_label(capture));
		capture->setting_row = 0;
		choose_value(capture, 0, 0, 2);
		choose_value(capture, 2, 0, capture->mre ? 1 : 3);
		assert_settings(capture, "Integer", "0°",
				capture->mre ? "On" : "176x208");
	} else if (!strcmp(scenario, "rotation-reopen")) {
		open_running_settings(capture, document, true);
		if (!has_label(capture, "Integer"))
			fprintf(stderr,
				"expected inherited Integer scale, actual %s\n",
				json_object_to_json_string_ext(
					capture->layout,
					JSON_C_TO_STRING_PLAIN));
		assert_settings(capture, "Integer", "270°",
				capture->mre ? "On" : "176x208");
	} else if (!strncmp(scenario, "retry-", 6)) {
		char operation = scenario[6];

		open_global_settings(capture);
		set_fault(operation);
		choose_value(capture, 0, 1, 2);
		dismiss_save_error(capture, operation);
		assert_settings(capture, "Integer", "270°",
				capture->mre ? "Off" : "Auto");
		snapshot_preferences(capture);
		assert(unlink(getenv("FPLINUX_APP_SAVE_FAULT")) == 0);
		choose_value(capture, 0, 2, 2);
		assert_settings(capture, "Integer", "270°",
				capture->mre ? "Off" : "Auto");
		assert(!has_label(capture, "Unsaved"));
	} else {
		bool program = !strcmp(scenario, "cross-program");

		assert(program || !strcmp(scenario, "cross-global"));
		open_running_settings(capture, document, program);
		set_fault('f');
		choose_value(capture, 0, 0, 2);
		dismiss_save_error(capture, 'f');
		assert(unlink(getenv("FPLINUX_APP_SAVE_FAULT")) == 0);
		press(capture, KEY_ESC);
		press(capture, program ? KEY_DOWN : KEY_UP);
		press(capture, KEY_ENTER);
		capture->setting_row = 0;
		choose_value(capture, 1, 0, 1);
		wait_label(capture, third_label(capture));
		assert(!has_label(capture, "Unsaved"));
		press(capture, KEY_ESC);
		press(capture, program ? KEY_UP : KEY_DOWN);
		press(capture, KEY_ENTER);
		wait_label(capture, third_label(capture));
		assert(has_label(capture, "Unsaved") &&
		       has_label(capture, "Integer"));
		press(capture, KEY_ESC);
		press(capture, KEY_ESC);
		wait_label(capture, "Continue");
		assert(has_label(capture, "Unsaved"));
	}
}

static void capture_menus(struct capture *capture, const char *document)
{
	const char *options = translated(capture, "Options", "Опции");

	wait_label(capture,
		   strstr(document, ".vxp") ?
			   translated(capture, "Open VXP", "Открыть VXP") :
			   translated(capture, "Open", "Открыть"));
	save_frame(capture, "home");
	press(capture, KEY_DOWN);
	press(capture, KEY_DOWN);
	press(capture, KEY_ENTER);
	settings_frames(capture, "settings-global");
	running_home(capture, document);
	save_frame(capture, "home-running");
	press(capture, KEY_DOWN);
	press(capture, KEY_DOWN);
	press(capture, KEY_ENTER);
	save_frame(capture, "recent");
	assert(!strcmp(capture->left, options));
	press(capture, KEY_COMPOSE);
	wait_label(capture, translated(capture, "Remove", "Удалить"));
	press(capture, KEY_ESC);
	press(capture, KEY_ESC);
	press(capture, KEY_DOWN);
	press(capture, KEY_ENTER);
	settle(capture);
	wait_label(capture,
		   translated(capture, "This program", "Эта программа"));
	save_frame(capture, "scope");
	press(capture, KEY_ENTER);
	settings_frames(capture, "settings-program");
}

int main(int argc, char **argv)
{
	struct capture capture = { 0 };
	struct fplinux_ui_session_message config = {
		.type = FPLINUX_UI_SESSION_CONFIG,
		.mode = FPLINUX_UI_SESSION_INTEGRATED,
	};
	struct fplinux_ui_session_message message;
	const struct fplinux_ui_profile_spec *profile;
	const char *record = getenv("FPLINUX_APP_FRAME_RECORD");
	char redraw[PATH_MAX];
	int wayland[2], length, status;
	uint64_t until;
	pid_t ended;

	assert(argc == 11 || argc == 12);
	capture.commit_directory = getenv("FPLINUX_APP_COMMIT_DIRECTORY");
	assert(record && capture.commit_directory);
	length = snprintf(redraw, sizeof(redraw), "%s.layout.json", record);
	assert(length >= 0 && length < (int)sizeof(redraw));
	assert(unlink(redraw) == 0 || errno == ENOENT);
	config.theme = !strcmp(argv[10], "dark") ?
			       FPLINUX_UI_SESSION_THEME_DARK :
			       FPLINUX_UI_SESSION_THEME_LIGHT;
	capture.width = config.screen_width = atoi(argv[5]);
	capture.height = config.screen_height = atoi(argv[6]);
	capture.directory = argv[4];
	capture.russian = !strcmp(argv[7], "ru");
	capture.mre = !strcmp(argv[8], "org.fplinux.mre");
	capture.app_id = argv[8];
	snprintf(config.locale, sizeof(config.locale), "%s", argv[7]);
	strcpy(config.input_locale, "en");
	profile = fplinux_ui_profile_spec(
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
	message = fplinux_ui_frame_wait_message(&capture.peer, capture.session);
	assert(message.type == FPLINUX_UI_SESSION_HELLO &&
	       !strcmp(message.app_id, argv[8]));
	assert(fplinux_ui_session_send(capture.session, &config));
	settle(&capture);
	fplinux_ui_frame_focus_keyboard(&capture.peer.keyboards[0]);
	if (argc == 12 && !strcmp(argv[11], "held-buffers"))
		capture_held_buffers(&capture, redraw);
	else if (argc == 12)
		settings_scenario(&capture, argv[9], argv[11]);
	else
		capture_menus(&capture, argv[9]);
	message = (struct fplinux_ui_session_message){
		.type = FPLINUX_UI_SESSION_CLOSE
	};
	assert(fplinux_ui_session_send(capture.session, &message));
	until = fplinux_ui_frame_now_ms() + 5000;
	do {
		fplinux_ui_frame_pump(&capture.peer);
		ended = waitpid(capture.application, &status, WNOHANG);
	} while (!ended && fplinux_ui_frame_now_ms() < until);
	assert(ended == capture.application && WIFEXITED(status) &&
	       !WEXITSTATUS(status));
	close(capture.session);
	close(capture.listener);
	json_object_put(capture.layout);
	fplinux_ui_frame_cleanup(&capture.peer);
	assert(unlink(argv[3]) == 0);
	puts(argc == 12 ?
		     "application settings scenario checked" :
		     "application menus captured from real Wayland frames");
	return 0;
}
