/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "apidemo-observer.h"
#include "fplinux-ui-profile.h"
#include "screen-frame.h"
#include "ui-frame-peer.h"

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <json-c/json.h>
#include <linux/input-event-codes.h>
#include <lvgl/lvgl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

struct capture {
	struct fplinux_ui_frame_peer peer;
	struct apidemo_frame_observation frame;
	int listener;
	int session;
	int observer;
	int frame_observer;
	pid_t application;
	uint32_t serial;
	char **arguments;
	const char *directory;
	const char *locale;
	const char *theme;
	char left[FPLINUX_UI_SESSION_TEXT_BYTES];
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
		text = kind == APIDEMO_LABEL ? observation.text :
					       observation.title;
		if (observation.kind == kind &&
		    (kind == APIDEMO_LABEL && !expected[0] ?
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
	strcpy(setting.value, capture->theme);
	assert(fplinux_ui_session_send(capture->session, &setting));
	for (;;) {
		struct fplinux_ui_session_message message =
			fplinux_ui_frame_wait_message(&capture->peer,
						      capture->session);

		assert(message.type != FPLINUX_UI_SESSION_EXIT);
		if (message.type == FPLINUX_UI_SESSION_SOFTKEYS) {
			strcpy(capture->left, message.left);
			strcpy(capture->centre, message.centre);
			strcpy(capture->right, message.right);
			capture->legends_seen = true;
		}
		if (message.type == FPLINUX_UI_SESSION_REDRAW_ACK) {
			assert(message.serial == setting.serial);
			fplinux_ui_frame_wait_readable(&capture->peer,
						       capture->frame_observer);
			assert(recv(capture->frame_observer, &capture->frame,
				    sizeof(capture->frame),
				    0) == (ssize_t)sizeof(capture->frame));
			assert(capture->frame.serial == setting.serial);
			return;
		}
	}
}

static void activate(struct capture *capture,
		     enum fplinux_ui_session_activation activation)
{
	const struct fplinux_ui_session_message request = {
		.type = FPLINUX_UI_SESSION_ACTIVATION,
		.serial = ++capture->serial,
		.activation = activation,
	};
	unsigned int previous = capture->peer.commits;

	assert(fplinux_ui_session_send(capture->session, &request));
	for (;;) {
		struct fplinux_ui_session_message reply =
			fplinux_ui_frame_wait_message(&capture->peer,
						      capture->session);

		assert(reply.type != FPLINUX_UI_SESSION_EXIT);
		if (reply.type == FPLINUX_UI_SESSION_SOFTKEYS) {
			strcpy(capture->left, reply.left);
			strcpy(capture->centre, reply.centre);
			strcpy(capture->right, reply.right);
			capture->legends_seen = true;
		}
		if (reply.type == FPLINUX_UI_SESSION_ACTIVATION_ACK) {
			assert(reply.serial == request.serial);
			assert(capture->peer.commits > previous);
			return;
		}
	}
}

static struct json_object *rectangle_json(struct apidemo_rectangle rectangle)
{
	struct json_object *array = json_object_new_array();

	json_object_array_add(array, json_object_new_int(rectangle.left));
	json_object_array_add(array, json_object_new_int(rectangle.top));
	json_object_array_add(array, json_object_new_int(rectangle.width));
	json_object_array_add(array, json_object_new_int(rectangle.height));
	return array;
}

static void save_geometry(struct capture *capture, const char *name)
{
	struct json_object *root = json_object_new_object();
	struct json_object *images = json_object_new_array();
	struct json_object *menu = json_object_new_array();
	struct json_object *canvas_objects = json_object_new_array();
	char path[512];

	json_object_object_add(root, "picker",
			       rectangle_json(capture->frame.picker));
	for (size_t index = 0; index < capture->frame.image_count; ++index) {
		const struct apidemo_image_observation *image =
			&capture->frame.images[index];
		struct json_object *entry = json_object_new_object();
		struct json_object *source = json_object_new_array();

		json_object_object_add(entry, "bounds",
				       rectangle_json(image->bounds));
		json_object_object_add(entry, "frame",
				       rectangle_json(image->frame));
		json_object_object_add(entry, "viewport",
				       rectangle_json(image->viewport));
		json_object_array_add(source,
				      json_object_new_int(image->source_width));
		json_object_array_add(
			source, json_object_new_int(image->source_height));
		json_object_object_add(entry, "source", source);
		json_object_array_add(images, entry);
	}
	for (size_t index = 0; index < capture->frame.menu_count; ++index) {
		const struct apidemo_menu_observation *row =
			&capture->frame.menu[index];
		struct json_object *entry = json_object_new_object();
		char alpha[sizeof(row->alpha) * 2 + 1];
		size_t pixels =
			(size_t)row->icon_width * (size_t)row->icon_height;

		for (size_t pixel = 0; pixel < pixels; ++pixel)
			snprintf(alpha + 2 * pixel, 3, "%02x",
				 row->alpha[pixel]);
		alpha[2 * pixels] = '\0';

		json_object_object_add(entry, "text",
				       json_object_new_string(row->text));
		json_object_object_add(entry, "bounds",
				       rectangle_json(row->bounds));
		json_object_object_add(entry, "icon",
				       json_object_new_boolean(row->icon));
		json_object_object_add(entry, "alpha",
				       json_object_new_string(alpha));
		json_object_array_add(menu, entry);
	}
	json_object_object_add(root, "images", images);
	json_object_object_add(root, "menu", menu);
	json_object_object_add(root, "canvas",
			       rectangle_json(capture->frame.canvas));
	for (size_t index = 0; index < capture->frame.canvas_object_count;
	     ++index) {
		const struct apidemo_canvas_object_observation *item =
			&capture->frame.canvas_objects[index];
		struct json_object *entry = json_object_new_object();
		struct json_object *padding = json_object_new_array();

		json_object_object_add(entry, "parent",
				       json_object_new_int(item->parent));
		json_object_object_add(entry, "bounds",
				       rectangle_json(item->bounds));
		json_object_object_add(entry, "content",
				       rectangle_json(item->content));
		json_object_object_add(entry, "border",
				       json_object_new_int(item->border));
		for (size_t side = 0; side < 4; ++side)
			json_object_array_add(
				padding,
				json_object_new_int(item->padding[side]));
		json_object_object_add(entry, "padding", padding);
		json_object_object_add(entry, "scroll_y",
				       json_object_new_int(item->scroll_y));
		json_object_object_add(entry, "line_height",
				       json_object_new_int(item->line_height));
		json_object_object_add(entry, "max_height",
				       json_object_new_int(item->max_height));
		json_object_object_add(entry, "text",
				       json_object_new_string(item->text));
		json_object_array_add(canvas_objects, entry);
	}
	json_object_object_add(root, "canvas_objects", canvas_objects);
	assert(snprintf(path, sizeof(path), "%s/%s.json", capture->directory,
			name) < (int)sizeof(path));
	assert(json_object_to_file_ext(path, root, JSON_C_TO_STRING_PRETTY) ==
	       0);
	json_object_put(root);
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
	save_geometry(capture, name);
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
	int wayland[2], observer[2], frame_observer[2];
	int buffer_bytes = 1024 * 1024;

	assert(profile);
	config.theme = !strcmp(capture->theme, "light") ?
			       FPLINUX_UI_SESSION_THEME_LIGHT :
			       FPLINUX_UI_SESSION_THEME_DARK;
	capture->alerts = 0;
	capture->centre[0] = '\0';
	capture->right[0] = '\0';
	capture->legends_seen = false;
	capture->frame = (struct apidemo_frame_observation){ 0 };
	capture->listener = fplinux_ui_frame_listen_session(arguments[5]);
	fplinux_ui_frame_socketpair(wayland);
	assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, observer) == 0);
	assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, frame_observer) == 0);
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
		close(frame_observer[0]);
		fplinux_ui_frame_client_environment(wayland[1], arguments[5]);
		snprintf(descriptor, sizeof(descriptor), "%d", observer[1]);
		assert(setenv("FPLINUX_APIDEMO_OBSERVER_FD", descriptor, 1) ==
		       0);
		snprintf(descriptor, sizeof(descriptor), "%d",
			 frame_observer[1]);
		assert(setenv("FPLINUX_APIDEMO_FRAME_FD", descriptor, 1) == 0);
		assert(snprintf(state, sizeof(state), "%s/state",
				capture->directory) < (int)sizeof(state));
		assert(snprintf(catalogues, sizeof(catalogues), "%.*s/locale",
				(int)(slash - arguments[3]),
				arguments[3]) < (int)sizeof(catalogues));
		assert(setenv("FPLINUX_APIDEMO_LOCALE_DIR", catalogues, 1) ==
		       0);
		assert(snprintf(catalogues, sizeof(catalogues),
				"%.*s/icons/usr/share/fplinux/glyphs",
				(int)(slash - arguments[3]),
				arguments[3]) < (int)sizeof(catalogues));
		if (access(catalogues, R_OK) == 0)
			assert(setenv("FPLINUX_NATIVE_GLYPHS", catalogues, 1) ==
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
	close(frame_observer[1]);
	capture->observer = observer[0];
	capture->frame_observer = frame_observer[0];
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
	wait_observation(capture, APIDEMO_LIST,
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
	close(capture->frame_observer);
	close(capture->session);
	close(capture->listener);
	fplinux_ui_frame_cleanup(&capture->peer);
	assert(unlink(capture->arguments[5]) == 0);
}

static void search_query(struct capture *capture, const char *query,
			 const char *title, enum apidemo_observation_kind kind)
{
	char results[128];

	type_query(capture, query);
	press(capture, 1, KEY_OK);
	snprintf(results, sizeof(results),
		 !strcmp(capture->locale, "ru") ? "Поиск: %s" : "Search: %s",
		 query);
	wait_observation(capture, APIDEMO_LIST, results);
	press(capture, 1, KEY_OK);
	wait_observation(capture, kind, title);
}

static void open_search(struct capture *capture, const char *query,
			const char *title, enum apidemo_observation_kind kind)
{
	start(capture);
	press(capture, 1, KEY_F13);
	wait_observation(capture, APIDEMO_FORM,
			 !strcmp(capture->locale, "ru") ? "Поиск" : "Search");
	search_query(capture, query, title, kind);
}

/* The preceding demo has returned to its results; reuse the search editor. */
static void next_demo(struct capture *capture, const char *previous_query,
		      const char *query, const char *title,
		      enum apidemo_observation_kind kind)
{
	press(capture, 1, KEY_F14);
	for (size_t index = 0; index < strlen(previous_query); ++index)
		press(capture, 0, KEY_BACKSPACE);
	search_query(capture, query, title, kind);
}

static void catalogue_and_virtual(struct capture *capture)
{
	struct apidemo_observation selected;

	start(capture);
	save_frame(capture, "apidemo-catalogue");
	press(capture, 1, KEY_OK);
	wait_observation(capture, APIDEMO_LIST, "Lists");
	press(capture, 1, KEY_DOWN);
	press(capture, 1, KEY_OK);
	wait_observation(capture, APIDEMO_LIST, "Exclusive list");
	press(capture, 1, KEY_F14);
	/* Returning preserves the category's focused example. */
	press(capture, 1, KEY_OK);
	wait_observation(capture, APIDEMO_LIST, "Exclusive list");
	press(capture, 1, KEY_F14);
	press(capture, 1, KEY_F14);
	/* Search remains available after browsing the catalogue. */
	press(capture, 1, KEY_F13);
	wait_observation(capture, APIDEMO_FORM, "Search");
	type_query(capture, "ZZZ");
	save_frame(capture, "apidemo-search");
	press(capture, 1, KEY_OK);
	wait_observation(capture, APIDEMO_LABEL, "No demo matches the search.");
	save_frame(capture, "apidemo-no-results");
	press(capture, 1, KEY_F14);
	/* A nonempty field keeps Clear; Back is the sole Options action. */
	press(capture, 1, KEY_F13);
	press(capture, 1, KEY_OK);
	press(capture, 1, KEY_F13);
	wait_observation(capture, APIDEMO_FORM, "Search");
	/* The query survived leaving and reopening the search screen. */
	press(capture, 1, KEY_OK);
	wait_observation(capture, APIDEMO_LABEL, "No demo matches the search.");
	close_and_reap(capture);

	open_search(capture, "LIST", "List actions", APIDEMO_LIST);
	press(capture, 1, KEY_F14);
	save_frame(capture, "apidemo-search-results");
	press(capture, 1, KEY_DOWN);
	press(capture, 1, KEY_OK);
	wait_observation(capture, APIDEMO_LIST, "Exclusive list");
	press(capture, 1, KEY_F14);
	press(capture, 1, KEY_OK);
	wait_observation(capture, APIDEMO_LIST, "Exclusive list");
	press(capture, 1, KEY_F14);
	press(capture, 1, KEY_F14);
	press(capture, 1, KEY_OK);
	wait_observation(capture, APIDEMO_LIST, "Search: LIST");
	close_and_reap(capture);

	open_search(capture, "VIRTUAL", "Virtual list", APIDEMO_LIST);
	press(capture, 1, KEY_DOWN);
	press(capture, 1, KEY_OK);
	selected = wait_observation(capture, APIDEMO_ALERT, "Selected");
	assert(strstr(selected.text, "Index 1: Message 999."));
	save_frame(capture, "apidemo-virtual-selection");
	close_and_reap(capture);
	puts("apidemo: root and search remain available; category and result focus preserved; query reused; virtual row 2 selected");
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
	activate(capture, FPLINUX_UI_SESSION_ACTIVATION_OPEN);
	activate(capture, FPLINUX_UI_SESSION_ACTIVATION_RESUME);
	assert(!strcmp(capture->right, "Close"));
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

	open_search(capture, "FRAME_PRESENT", "Direct drawing", APIDEMO_CANVAS);
	press(capture, 1, KEY_F14);
	press(capture, 1, KEY_DOWN);
	press(capture, 1, KEY_OK);
	wait_observation(capture, APIDEMO_CANVAS, "Interrupted painting");
	save_frame(capture, "apidemo-frames");
	press(capture, 1, KEY_F14);
	close_and_reap(capture);

	open_search(capture, "TONE", "Tone", APIDEMO_FORM);
	send_press(capture, 1, KEY_F13);
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

static const char *word(const struct capture *capture, const char *english,
			const char *russian)
{
	return !strcmp(capture->locale, "ru") ? russian : english;
}

static void picker_capture(struct capture *capture, const char *name)
{
	press(capture, 1, KEY_NUMERIC_STAR);
	save_frame(capture, name);
	assert(capture->frame.picker.width > 0);
	expect_legends(capture, word(capture, "Insert", "Вставить"),
		       word(capture, "Cancel", "Отмена"));
	press(capture, 1, KEY_F14);
	redraw(capture);
	assert(capture->frame.picker.width == 0);
}

/* Real app routes share one process, including the two raw field owners. */
static void popup_routes(struct capture *capture)
{
	struct apidemo_observation result;

	open_search(capture, "TEXT EDITOR",
		    word(capture, "Text editor", "Редактор текста"),
		    APIDEMO_TEXT_BOX);
	picker_capture(capture, "apidemo-picker-textbox");
	type_query(capture, "A");
	press(capture, 1, KEY_F13);
	save_frame(capture, "apidemo-text-options");
	press(capture, 1, KEY_OK);
	result = wait_observation(capture, APIDEMO_ALERT,
				  word(capture, "Text", "Текст"));
	assert(!strcmp(result.text,
		       word(capture, "1 character:\nA", "1 символ:\nA")));
	press(capture, 1, KEY_F14);
	press(capture, 1, KEY_F13);
	press(capture, 1, KEY_DOWN);
	press(capture, 1, KEY_DOWN);
	/* The last Options row invokes the original screen's Back. */
	press(capture, 1, KEY_OK);
	next_demo(capture, "TEXT EDITOR", "TEXT FIELDS",
		  word(capture, "Text fields", "Поля ввода"), APIDEMO_FORM);
	picker_capture(capture, "apidemo-picker-form-top");
	for (unsigned int index = 0; index < 4; ++index)
		press(capture, 1, KEY_DOWN);
	picker_capture(capture, "apidemo-picker-form-scrolled");
	press(capture, 1, KEY_F14);
	next_demo(capture, "TEXT FIELDS", "CANVAS FIELDS",
		  word(capture, "Canvas fields", "Поля на холсте"),
		  APIDEMO_CANVAS);
	picker_capture(capture, "apidemo-picker-canvas-first");
	press(capture, 1, KEY_DOWN);
	picker_capture(capture, "apidemo-picker-canvas-second");
	press(capture, 1, KEY_F14);
	next_demo(capture, "CANVAS FIELDS", "LINKS AND BUTTONS",
		  word(capture, "Links and buttons", "Ссылки и кнопки"),
		  APIDEMO_FORM);
	press(capture, 1, KEY_DOWN);
	press(capture, 1, KEY_DOWN);
	save_frame(capture, "apidemo-image-link");
	press(capture, 1, KEY_DOWN);
	save_frame(capture, "apidemo-image-button");
	press(capture, 1, KEY_F13);
	save_frame(capture, "apidemo-links-options");
	press(capture, 1, KEY_F14);
	press(capture, 1, KEY_OK);
	result = wait_observation(capture, APIDEMO_ALERT,
				  word(capture, "View", "Посмотреть"));
	assert(!strcmp(
		result.text,
		word(capture, "Item 3 ran “View”.\nget_focus() = 3",
		     "Элемент 3 выполнил «Посмотреть».\nget_focus() = 3")));
	press(capture, 1, KEY_F14);
	press(capture, 1, KEY_F14);
	next_demo(capture, "LINKS AND BUTTONS", "OPTIONS MENU WITH ICONS",
		  word(capture, "Options menu with icons", "Меню со значками"),
		  APIDEMO_FORM);
	press(capture, 1, KEY_F13);
	save_frame(capture, "apidemo-long-options");
	for (unsigned int index = 0; index < 6; ++index)
		press(capture, 1, KEY_DOWN);
	save_frame(capture, "apidemo-long-options-end");
	press(capture, 1, KEY_OK);
	result = wait_observation(capture, APIDEMO_ALERT,
				  word(capture, "Rename", "Переименовать"));
	assert(!strcmp(result.text, word(capture, "The listener received id 7.",
					 "Обработчик получил id 7.")));
	close_and_reap(capture);
	puts("apidemo: real text, Form and both Canvas pickers; framed source images; Options icons and original callbacks");
}

static void compact_menu_layout(struct capture *capture)
{
	bool russian = !strcmp(capture->locale, "ru");

	open_search(capture, "MENU ON A CANVAS",
		    russian ? "Меню на холсте" : "Menu on a canvas",
		    APIDEMO_CANVAS);
	save_frame(capture, "apidemo-menu-before");
	press(capture, 1, KEY_F13);
	expect_legends(capture, russian ? "Выбрать" : "Select",
		       russian ? "Назад" : "Back");
	save_frame(capture, "apidemo-menu-open");
	press(capture, 1, KEY_F14);
	expect_legends(capture, "", russian ? "Назад" : "Back");
	save_frame(capture, "apidemo-menu-after");
	close_and_reap(capture);
	puts("apidemo: compact menu keeps background placement and restores Canvas legends");
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
	save_frame(capture, "apidemo-catalogue");
	press(capture, 1, KEY_F13);
	wait_observation(capture, APIDEMO_FORM, "Поиск");
	for (size_t index = 0; index < sizeof(keys) / sizeof(keys[0]); ++index)
		press(capture, 0, keys[index]);
	save_frame(capture, "apidemo-search");
	press(capture, 1, KEY_OK);
	wait_observation(capture, APIDEMO_LIST, "Поиск: СПИСОК");
	save_frame(capture, "apidemo-search-results");
	press(capture, 1, KEY_OK);
	wait_observation(capture, APIDEMO_LIST, "Одиночный список");
	save_frame(capture, "apidemo-russian-search");
	close_and_reap(capture);
	puts("apidemo: Cyrillic uppercase search found translated list title");
}

static void duration_edit(struct capture *capture)
{
	struct apidemo_observation values;

	open_search(capture, "APPEND_DURATION", "Dates and times",
		    APIDEMO_FORM);
	for (unsigned int index = 0; index < 3; index++)
		press(capture, 1, KEY_DOWN);
	press(capture, 1, KEY_OK);
	press(capture, 1, KEY_RIGHT);
	press(capture, 1, KEY_UP);
	press(capture, 1, KEY_OK);
	wait_observation(capture, APIDEMO_LABEL, "Duration: 150 s");
	press(capture, 1, KEY_F13);
	press(capture, 1, KEY_DOWN);
	press(capture, 1, KEY_DOWN);
	press(capture, 1, KEY_OK);
	press(capture, 1, KEY_F13);
	press(capture, 1, KEY_DOWN);
	press(capture, 1, KEY_OK);
	values = wait_observation(capture, APIDEMO_ALERT, "Values");
	assert(strstr(values.text, "Duration: 90 s"));
	save_frame(capture, "apidemo-duration");
	close_and_reap(capture);
	puts("apidemo: Duration edits elapsed seconds and Reset restores 90 s");
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

static void sketch_cursor(struct capture *capture)
{
	open_search(capture, "SKETCH", word(capture, "Sketch pad", "Рисунок"),
		    APIDEMO_CANVAS);
	expect_legends(capture, word(capture, "Paint", "Рисовать"),
		       word(capture, "Back", "Назад"));
	save_frame(capture, "apidemo-sketch-empty");
	press(capture, 1, KEY_OK);
	expect_legends(capture, word(capture, "Erase", "Стереть"),
		       word(capture, "Back", "Назад"));
	save_frame(capture, "apidemo-sketch-painted");
	press(capture, 1, KEY_RIGHT);
	expect_legends(capture, word(capture, "Paint", "Рисовать"),
		       word(capture, "Back", "Назад"));
	save_frame(capture, "apidemo-sketch-neighbour");
	press(capture, 1, KEY_LEFT);
	press(capture, 1, KEY_OK);
	save_frame(capture, "apidemo-sketch-erased");
	press(capture, 1, KEY_F14);
	close_and_reap(capture);
	puts("apidemo: Sketch Paint, move, return and Erase preserve the cursor frame");
}

static void preview_resize(struct capture *capture, bool fullscreen)
{
	int width = dimension(capture->arguments[1]);
	int height = dimension(capture->arguments[2]);
	const struct fplinux_ui_profile_spec *profile = fplinux_ui_profile_spec(
		fplinux_ui_profile_for_screen(width, height));
	unsigned int previous;
	bool options;

	redraw(capture);
	options = !strcmp(capture->left, word(capture, "Options", "Опции"));
	if (options) {
		press(capture, 1, KEY_F13);
		send_press(capture, 1, KEY_OK);
	} else {
		send_press(capture, 1, KEY_F13);
	}

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
	redraw(capture);
	previous = capture->peer.commits;
	if (!fullscreen)
		height -= profile->status_height + profile->softkey_height;
	fplinux_ui_frame_configure(&capture->peer, width, height, fullscreen);
	wait_frame(capture, previous);
}

static void softkey_preview(struct capture *capture)
{
	open_search(capture, "SOFTKEY_BAR_CREATE",
		    word(capture, "Soft-key preview",
			 "Просмотр подписей клавиш"),
		    APIDEMO_CANVAS);
	save_frame(capture, "apidemo-preview-top");
	for (int index = 0; index < 12; ++index)
		send_press(capture, 1, KEY_DOWN);
	save_frame(capture, "apidemo-preview-end");
	press(capture, 1, KEY_OK);
	save_frame(capture, "apidemo-preview-changed");
	preview_resize(capture, true);
	save_frame(capture, "apidemo-preview-fullscreen");
	preview_resize(capture, false);
	save_frame(capture, "apidemo-preview-normal");
	press(capture, 1, KEY_F14);
	close_and_reap(capture);
	puts("apidemo: Soft-key preview accepts navigation, changes legends and returns from full screen");
}

static void canvas_geometry(struct capture *capture)
{
	open_search(capture, "WHOLE SURFACE", "", APIDEMO_CANVAS);
	save_frame(capture, "apidemo-whole-geometry");
	close_and_reap(capture);
	open_search(capture, "MOVING SHAPES",
		    word(capture, "Moving shapes", "Перемещение фигур"),
		    APIDEMO_CANVAS);
	redraw(capture);
	for (int index = 0; index < 32; ++index) {
		send_press(capture, 1, KEY_LEFT);
		send_press(capture, 1, KEY_UP);
	}
	save_frame(capture, "apidemo-moving-first");
	for (int index = 0; index < 32; ++index) {
		send_press(capture, 1, KEY_RIGHT);
		send_press(capture, 1, KEY_DOWN);
	}
	save_frame(capture, "apidemo-moving-last");
	close_and_reap(capture);
	open_search(capture, "CANVAS FIELDS",
		    word(capture, "Canvas fields", "Поля на холсте"),
		    APIDEMO_CANVAS);
	save_frame(capture, "apidemo-fields-geometry");
	close_and_reap(capture);
	puts("apidemo: titled and untitled Canvas bounds keep fields and Moving shapes inside");
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

static unsigned int timer_descriptors(pid_t application)
{
	char directory[64];
	struct dirent *entry;
	unsigned int count = 0;
	DIR *descriptors;

	assert(snprintf(directory, sizeof(directory), "/proc/%ld/fd",
			(long)application) < (int)sizeof(directory));
	descriptors = opendir(directory);
	assert(descriptors);
	while ((entry = readdir(descriptors))) {
		char path[512];
		char target[128];
		ssize_t length;

		if (entry->d_name[0] == '.')
			continue;
		assert(snprintf(path, sizeof(path), "%s/%s", directory,
				entry->d_name) < (int)sizeof(path));
		length = readlink(path, target, sizeof(target) - 1);
		assert(length >= 0);
		target[length] = '\0';
		if (!strcmp(target, "anon_inode:[timerfd]"))
			++count;
	}
	assert(!closedir(descriptors));
	return count;
}

static void search_from_root(struct capture *capture, const char *previous,
			     const char *query, const char *title,
			     enum apidemo_observation_kind kind)
{
	redraw(capture);
	assert(!strcmp(capture->left, "Search"));
	assert(!strcmp(capture->centre, "Open"));
	press(capture, 1, KEY_F13);
	wait_observation(capture, APIDEMO_FORM, "Search");
	for (size_t index = 0; index < strlen(previous); ++index)
		press(capture, 0, KEY_BACKSPACE);
	search_query(capture, query, title, kind);
}

static void activation_root_and_owned_resources(struct capture *capture)
{
	pid_t application;
	size_t bytes;
	uint16_t *confirmation;

	open_search(capture, "WATCHED", "Watched descriptor", APIDEMO_FORM);
	application = capture->application;
	assert(timer_descriptors(application) == 1);
	activate(capture, FPLINUX_UI_SESSION_ACTIVATION_RESUME);
	assert(timer_descriptors(application) == 1);
	activate(capture, FPLINUX_UI_SESSION_ACTIVATION_OPEN);
	assert(timer_descriptors(application) == 0);
	assert(capture->application == application &&
	       kill(application, 0) == 0);
	/* Keep the backdrop title short so its marquee cannot change pixels. */
	search_from_root(capture, "WATCHED", "CONF", "Confirmation",
			 APIDEMO_ALERT);
	redraw(capture);
	bytes = (size_t)capture->peer.width * capture->peer.height *
		sizeof(*capture->peer.pixels);
	confirmation = malloc(bytes);
	assert(confirmation);
	memcpy(confirmation, capture->peer.pixels, bytes);
	activate(capture, FPLINUX_UI_SESSION_ACTIVATION_RESUME);
	assert(!memcmp(confirmation, capture->peer.pixels, bytes));
	activate(capture, FPLINUX_UI_SESSION_ACTIVATION_OPEN);
	assert(!memcmp(confirmation, capture->peer.pixels, bytes));
	free(confirmation);
	press(capture, 1, KEY_OK);
	wait_observation(capture, APIDEMO_ALERT, "Stay chosen");
	activate(capture, FPLINUX_UI_SESSION_ACTIVATION_OPEN);
	search_from_root(capture, "CONF", "PREFERENCES", "Preferences",
			 APIDEMO_FORM);
	type_query(capture, "DRAFT");
	press(capture, 1, KEY_DOWN);
	press(capture, 1, KEY_RIGHT);
	activate(capture, FPLINUX_UI_SESSION_ACTIVATION_OPEN);
	search_from_root(capture, "PREFERENCES", "PREFERENCES", "Preferences",
			 APIDEMO_FORM);
	wait_preference(capture, "In memory", "locale.count = 0");
	wait_preference(capture, "Stored", "locale.count = 0");
	send_press(capture, 1, KEY_OK);
	wait_preference(capture, "In memory", "locale.count = 1");
	wait_preference(capture, "Stored", "locale.name = \"DRAFT\"");
	activate(capture, FPLINUX_UI_SESSION_ACTIVATION_OPEN);
	activate(capture, FPLINUX_UI_SESSION_ACTIVATION_OPEN);
	redraw(capture);
	assert(!strcmp(capture->left, "Search") &&
	       !strcmp(capture->centre, "Open"));
	assert(capture->application == application &&
	       kill(application, 0) == 0);
	close_and_reap(capture);
	puts("apidemo-activation: same process; Resume kept watcher; Open released timerfd; confirmation and preference draft survived");
}

int main(int argc, char **argv)
{
	struct capture capture = { .arguments = argv };

	assert(setvbuf(stdout, NULL, _IONBF, 0) == 0);
	if (argc != 9 && argc != 10) {
		fprintf(stderr,
			"usage: %s WIDTH HEIGHT APP KEYMAP SESSION OUTPUT LOCALE SCENARIO [THEME]\n",
			argv[0]);
		return 2;
	}
	capture.directory = argv[6];
	capture.locale = argv[7];
	capture.theme = argc == 10 ? argv[9] : "dark";
	assert(!strcmp(capture.theme, "light") ||
	       !strcmp(capture.theme, "dark"));
	if (!strcmp(argv[8], "activation")) {
		activation_root_and_owned_resources(&capture);
		return 0;
	}
	if (!strcmp(argv[8], "navigation")) {
		if (!strcmp(capture.locale, "ru"))
			russian_search(&capture);
		else
			catalogue_and_virtual(&capture);
		popup_routes(&capture);
		return 0;
	}
	if (!strcmp(argv[8], "menu-layout")) {
		compact_menu_layout(&capture);
		return 0;
	}
	if (!strcmp(argv[8], "storage-failure")) {
		preference_storage_failure(&capture);
		return 0;
	}
	if (!strcmp(argv[8], "duration")) {
		duration_edit(&capture);
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
	if (!strcmp(argv[8], "sketch-cursor")) {
		sketch_cursor(&capture);
		return 0;
	}
	if (!strcmp(argv[8], "softkey-preview")) {
		softkey_preview(&capture);
		return 0;
	}
	if (!strcmp(argv[8], "canvas-geometry")) {
		canvas_geometry(&capture);
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
