/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "fplinux-audio-protocol.h"
#include "fplinux-ui-audio.h"
#include "fplinux-ui-fonts.h"
#include "fplinux-ui-frame.h"
#include "fplinux-ui-menu.h"
#include "fplinux-ui-prefs.h"
#include "fplinux-ui-profile.h"
#include "fplinux-ui-screen.h"
#include "fplinux-ui-session.h"
#include "fplinux-ui-symbol-picker.h"
#include "fplinux-ui-text.h"
#include "fplinux-ui-vibration.h"
#include "fplinux-ui.h"
#include "ui-frame-peer.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <json-c/json.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wayland-server.h>
#include <xkbcommon/xkbcommon.h>

struct application {
	int control;
	bool closed;
	bool initialized;
	unsigned int close_requests;
	lv_obj_t *softkeys;
};

enum observation_type {
	OBSERVATION_KEY,
	OBSERVATION_FILTER,
	OBSERVATION_COMMAND,
	OBSERVATION_VISIBILITY,
	OBSERVATION_CLOSE,
	OBSERVATION_INPUT_CANCEL,
};

struct observation {
	enum observation_type type;
	struct fplinux_ui_key key;
	char command;
	bool visible;
	bool menu_open;
	int32_t position;
	unsigned int callbacks;
	unsigned int activations;
	unsigned int tabs;
	char text[32];
};

struct repeat_application {
	int control;
	bool closed;
	bool bounce_on_press;
	bool picker_enter_held;
	char navigation;
	bool copy_raw_key;
	bool blur_on_change;
	unsigned int callbacks;
	unsigned int activations;
	unsigned int tabs;
	int32_t navigation_position;
	struct fplinux_ui_screen *navigation_screen;
	lv_obj_t *textarea;
	struct fplinux_ui_symbol_picker *picker;
	struct fplinux_ui_text *field;
};

struct media_application {
	int control;
	bool supported;
	bool closed;
	bool root_deleted;
	struct fplinux_audio_stream *streams[2];
	struct fplinux_ui_frame *frame;
	lv_timer_t *timer;
};

/* Generated package fonts are outside the frame/transport boundary. */
const lv_font_t *fplinux_ui_builtin_font(const char *id)
{
	(void)id;
	return &lv_font_montserrat_14;
}

static void paint(struct fplinux_ui *ui, uint32_t colour)
{
	lv_obj_set_style_bg_color(fplinux_ui_screen(ui), lv_color_hex(colour),
				  0);
}

static uint32_t paused_tick(void)
{
	/* Keep a refresh timer which is not due from hiding a queued stale canvas. */
	return 1;
}

static void application_event(struct fplinux_ui *ui,
			      const struct fplinux_ui_event *event, void *data)
{
	struct application *application = data;

	if (event->type == FPLINUX_UI_EVENT_CLOSE) {
		application->closed = true;
		return;
	}
	if (event->type == FPLINUX_UI_EVENT_VISIBILITY) {
		char visible = event->visible ? 'V' : 'H';

		assert(write(application->control, &visible, 1) == 1);
		return;
	}
	if (event->type == FPLINUX_UI_EVENT_ACTIVATION) {
		assert(application->initialized);
		assert(!event->session);
		assert(event->activation == FPLINUX_UI_ACTIVATION_OPEN ||
		       event->activation == FPLINUX_UI_ACTIVATION_RESUME);
		paint(ui, event->activation == FPLINUX_UI_ACTIVATION_OPEN ?
				  0x00ff00 :
				  0x0000ff);
		assert(write(application->control, "a", 1) == 1);
		return;
	}
	if (event->type != FPLINUX_UI_EVENT_SESSION)
		return;
	if (event->session->type == FPLINUX_UI_SESSION_ACTIVE) {
		struct fplinux_ui_session_message ack = {
			.type = FPLINUX_UI_SESSION_ACTIVE_ACK,
			.serial = event->session->serial,
		};

		paint(ui, 0x00ff00);
		assert(fplinux_ui_send(ui, &ack));
	} else if (event->session->type == FPLINUX_UI_SESSION_SETTING) {
		if (!strcmp(event->session->name, "locale"))
			assert(!strcmp(fplinux_ui_locale(ui), "ru"));
		else
			assert(fplinux_ui_theme(ui)->kind ==
			       FPLINUX_UI_THEME_DARK);
		paint(ui, 0x00ff00);
	} else if (event->session->type == FPLINUX_UI_SESSION_ERROR) {
		if (!strcmp(event->session->reason, "applied-theme"))
			assert(fplinux_ui_theme(ui)->kind ==
			       FPLINUX_UI_THEME_DARK);
		else if (!strcmp(event->session->reason, "applied-locale"))
			assert(!strcmp(fplinux_ui_locale(ui), "ru"));
		else if (!strcmp(event->session->reason, "unapplied-theme"))
			assert(fplinux_ui_theme(ui)->kind ==
			       FPLINUX_UI_THEME_LIGHT);
		/* The application renders a visible notice in its normal callback. */
		paint(ui, 0xffff00);
	}
	assert(write(application->control, "a", 1) == 1);
}

static void create_softkeys(struct fplinux_ui *ui,
			    struct application *application)
{
	const struct fplinux_ui_profile_spec *spec =
		fplinux_ui_profile_spec(fplinux_ui_metrics(ui)->profile);
	struct fplinux_ui_screen *canvas = fplinux_ui_canvas_new(ui, NULL);
	lv_area_t bar_area;
	lv_area_t labels[3];
	const char *legends[3] = { "Tasks", "Menu", "Back" };

	assert(canvas && spec);
	fplinux_ui_push(ui, canvas);
	application->softkeys = fplinux_ui_softkey_bar_create(
		ui, fplinux_ui_canvas_object(canvas));
	assert(application->softkeys);
	assert(fplinux_ui_softkey_bar_set(ui, application->softkeys, "Tasks",
					  "Menu", "Back"));
	lv_obj_update_layout(application->softkeys);
	lv_obj_get_coords(application->softkeys, &bar_area);
	/* The 240x320 profile has a 32-pixel bar and 8-pixel legend margins. */
	assert(bar_area.x1 == 0 && bar_area.x2 == 239);
	assert(bar_area.y1 == 288 && bar_area.y2 == 319);
	for (int i = 0; i < 3; ++i) {
		lv_obj_t *label = lv_obj_get_child(application->softkeys, i);

		assert(!strcmp(lv_label_get_text(label), legends[i]));
		lv_obj_get_coords(label, &labels[i]);
		assert(abs(labels[i].y1 + labels[i].y2 - bar_area.y1 -
			   bar_area.y2) <= 1);
	}
	assert(labels[0].x1 == 8);
	assert(abs(labels[1].x1 + labels[1].x2 - 239) <= 1);
	assert(labels[2].x2 == 231);
}

static void create_launcher_screens(struct fplinux_ui *ui,
				    struct application *application)
{
	struct fplinux_ui_screen *canvas, *list, *alert;
	lv_obj_t *status;
	int width, height;

	canvas = fplinux_ui_canvas_new(ui, NULL);
	assert(canvas);
	fplinux_ui_push(ui, canvas);
	application->softkeys = fplinux_ui_softkey_bar_create(
		ui, fplinux_ui_canvas_object(canvas));
	assert(application->softkeys);
	assert(fplinux_ui_softkey_bar_set(ui, application->softkeys, "Tasks",
					  "Menu", "Back"));
	status = lv_obj_create(fplinux_ui_canvas_object(canvas));
	lv_obj_remove_style_all(status);
	lv_obj_set_size(status, 240, 20);
	lv_obj_set_style_bg_color(status, lv_color_hex(0x00ff00), 0);
	lv_obj_set_style_bg_opa(status, LV_OPA_COVER, 0);
	list = fplinux_ui_list_new(ui, "Open apps", FPLINUX_UI_LIST_IMPLICIT);
	assert(list);
	fplinux_ui_list_append(list, "Notes", NULL);
	fplinux_ui_push(ui, list);
	assert(fplinux_ui_screen_get_content_size(list, &width, &height));
	/* Status 20, title 28, content 240 and legends 32 fill the display. */
	assert(width == 240 && height == 240);
	assert(lv_obj_is_visible(status));
	alert = fplinux_ui_alert_new(ui, FPLINUX_UI_ALERT_ERROR, "Open apps",
				     "Unable to close", 0);
	assert(alert);
	fplinux_ui_push(ui, alert);
	assert(lv_obj_is_visible(status));
	assert(fplinux_ui_pop(ui));
	assert(fplinux_ui_current(ui) == list);
	assert(lv_obj_is_visible(status));
	assert(fplinux_ui_pop(ui));
	assert(fplinux_ui_current(ui) == canvas);
	assert(lv_obj_is_visible(status));
	list = fplinux_ui_list_new(ui, "Open apps", FPLINUX_UI_LIST_IMPLICIT);
	assert(list);
	fplinux_ui_list_append(list, "Notes", NULL);
	fplinux_ui_push(ui, list);
}

static void application_input(struct fplinux_ui *ui, int fd, short events,
			      void *data)
{
	struct application *application = data;
	char command;
	char result = 'a';

	assert(events == POLLIN);
	assert(read(fd, &command, 1) == 1);
	if (command == 'c' || command == 'r') {
		assert(!application->closed);
		assert(fplinux_ui_lifecycle(ui) != FPLINUX_UI_CLOSING);
		assert(fplinux_ui_close_reply(
			ui, command == 'c' ? FPLINUX_UI_CLOSE_CANCEL :
					     FPLINUX_UI_CLOSE_READY));
		assert(write(fd, &command, 1) == 1);
		return;
	}
	if (command == 's' || command == 'u' || command == 't' ||
	    command == 'v') {
		if (command == 'v') {
			struct fplinux_ui_screen *alert = fplinux_ui_alert_new(
				ui, FPLINUX_UI_ALERT_ERROR, "Open apps",
				"Unable to close", 0);

			assert(alert);
			fplinux_ui_push(ui, alert);
		} else if (command == 't')
			create_launcher_screens(ui, application);
		else if (command == 's')
			create_softkeys(ui, application);
		else {
			assert(application->softkeys);
			assert(fplinux_ui_softkey_bar_set(
				ui, application->softkeys, "Tasks", "Menu",
				"Back"));
		}
		lv_refr_now(NULL);
		assert(write(fd, &result, 1) == 1);
		return;
	}
	if (command == 'x') {
		/* The manager disappears immediately before the user's Back action. */
		assert(write(fd, "r", 1) == 1);
		assert(read(fd, &command, 1) == 1 && command == 'b');
	}
	paint(ui, command == 'q' ? 0xff00ff : 0x0000ff);
	lv_refr_now(NULL);
	if (command == 'b')
		result = fplinux_ui_back(ui) ? 'b' : 'f';
	else
		assert(command == 'p' || command == 'q');
	assert(write(fd, &result, 1) == 1);
}

static enum fplinux_ui_close_response application_close(struct fplinux_ui *ui,
							void *data)
{
	struct application *application = data;

	assert(!application->closed);
	assert(fplinux_ui_lifecycle(ui) != FPLINUX_UI_CLOSING);
	++application->close_requests;
	paint(ui, 0xffff00);
	lv_refr_now(NULL);
	assert(write(application->control, "w", 1) == 1);
	return FPLINUX_UI_CLOSE_WAIT;
}

static int run_application(int control, const char *scenario)
{
	struct application application = { .control = control };
	struct fplinux_ui *ui = fplinux_ui_open(
		"org.fplinux.frame-test", application_event, &application);
	int result;

	assert(ui);
	if (!strcmp(scenario, "metrics")) {
		const struct fplinux_ui_metrics *metrics =
			fplinux_ui_metrics(ui);

		assert(metrics->profile == FPLINUX_UI_PROFILE_240X320);
		assert(metrics->margin == 8);
		assert(metrics->scrollbar_space == 4);
	}
	if (!strncmp(scenario, "close-", 6)) {
		assert(!fplinux_ui_close_reply(ui, FPLINUX_UI_CLOSE_READY) &&
		       errno == EINVAL);
		fplinux_ui_set_close_handler(ui, application_close,
					     &application);
	}
	lv_tick_set_cb(paused_tick);
	paint(ui, 0xff0000);
	lv_refr_now(NULL);
	fplinux_ui_watch_fd(ui, control, POLLIN, application_input,
			    &application);
	application.initialized = true;
	result = fplinux_ui_run(ui);
	fplinux_ui_close(ui);
	assert(application.closed);
	if (!strcmp(scenario, "close-decisions"))
		assert(application.close_requests == 2);
	else if (!strcmp(scenario, "close-forced"))
		assert(application.close_requests == 1);
	return result;
}

static void send_observation(int control, const struct observation *observation)
{
	assert(write(control, observation, sizeof(*observation)) ==
	       (ssize_t)sizeof(*observation));
}

static lv_obj_t *find_textarea(lv_obj_t *root)
{
	if (lv_obj_check_type(root, &lv_textarea_class))
		return root;
	for (uint32_t i = 0; i < lv_obj_get_child_count(root); ++i) {
		lv_obj_t *area = find_textarea(lv_obj_get_child(root, i));

		if (area)
			return area;
	}
	return NULL;
}

static int32_t furthest_scroll(lv_obj_t *root)
{
	int32_t position = lv_obj_get_scroll_y(root);

	for (uint32_t i = 0; i < lv_obj_get_child_count(root); ++i) {
		int32_t child = furthest_scroll(lv_obj_get_child(root, i));

		if (child > position)
			position = child;
	}
	return position;
}

static void observe_navigation(struct fplinux_ui *ui,
			       struct repeat_application *application,
			       struct observation *observation)
{
	lv_obj_t *root = fplinux_ui_screen(ui);

	lv_obj_update_layout(root);
	switch (application->navigation) {
	case 'L':
		observation->position = fplinux_ui_list_get_focused(
			application->navigation_screen);
		break;
	case 'F':
		observation->position = fplinux_ui_form_get_value(
			application->navigation_screen, 0);
		break;
	case 'T':
	case 'R':
	case 'V':
		observation->position =
			application->textarea ?
				(int32_t)lv_textarea_get_cursor_pos(
					application->textarea) :
				-1;
		break;
	case 'M':
		root = lv_display_get_layer_top(lv_obj_get_display(root));
		observation->position = furthest_scroll(root);
		break;
	case 'A':
		observation->position = furthest_scroll(root);
		break;
	case 'N':
		observation->position = application->navigation_position;
		observation->activations = application->activations;
		observation->tabs = application->tabs;
		break;
	}
	observation->callbacks = application->callbacks;
}

static void repeat_canvas_key(struct fplinux_ui *ui,
			      struct fplinux_ui_screen *canvas,
			      const struct fplinux_ui_key *key, void *data)
{
	struct repeat_application *application = data;
	struct observation observation = {
		.type = OBSERVATION_KEY,
		.key = *key,
		.visible = fplinux_ui_is_visible(ui),
	};

	(void)canvas;
	if (application->navigation == 'N') {
		if (!key->pressed)
			return;
		if (key->code == KEY_DOWN)
			++application->navigation_position;
		else if (key->code == KEY_ENTER)
			++application->activations;
		else if (key->code == KEY_TAB)
			++application->tabs;
		return;
	}
	if (application->navigation == 'V') {
		if (key->pressed && key->code == KEY_LEFT)
			lv_textarea_cursor_left(application->textarea);
		return;
	}
	if (application->field) {
		assert(fplinux_ui_text_key(application->field, key));
		snprintf(observation.text, sizeof(observation.text), "%s",
			 fplinux_ui_text_get(application->field));
	}
	if (application->bounce_on_press && key->pressed) {
		struct fplinux_ui_screen *cover =
			fplinux_ui_canvas_new(ui, NULL);

		assert(cover);
		application->bounce_on_press = false;
		fplinux_ui_push(ui, cover);
		assert(fplinux_ui_pop(ui));
	}
	send_observation(application->control, &observation);
}

static bool filter_canvas_key(struct fplinux_ui *ui,
			      struct fplinux_ui_screen *canvas,
			      const struct fplinux_ui_key *key, void *data)
{
	struct repeat_application *application = data;
	struct observation observation = {
		.type = OBSERVATION_FILTER,
		.key = *key,
	};

	(void)ui;
	(void)canvas;
	if (key->source == FPLINUX_UI_KEY_KEYBOARD && key->code == KEY_ENTER) {
		if (!key->pressed && application->picker_enter_held) {
			application->picker_enter_held = false;
			return true;
		}
		if (key->pressed && application->picker) {
			assert(fplinux_ui_symbol_picker_is_open(
				application->picker));
			application->picker_enter_held = true;
			fplinux_ui_symbol_picker_delete(application->picker);
			application->picker = NULL;
			send_observation(application->control, &observation);
			return true;
		}
	}
	if (key->source != FPLINUX_UI_KEY_PHONE || key->code != KEY_F14)
		return false;
	send_observation(application->control, &observation);
	return true;
}

static void navigation_menu_chosen(struct fplinux_ui *ui, int action,
				   void *data)
{
	struct repeat_application *application = data;
	struct observation observation = {
		.type = OBSERVATION_COMMAND,
		.command = 'S',
		.position = action,
	};

	(void)ui;
	send_observation(application->control, &observation);
}

static struct fplinux_ui_screen *
new_repeat_canvas(struct fplinux_ui *ui,
		  struct repeat_application *application);

static void prepare_navigation(struct fplinux_ui *ui,
			       struct repeat_application *application,
			       char kind)
{
	const char *text =
		"0123456789012345678901234567890123456789012345678901234567890123";
	struct fplinux_ui_screen *screen = NULL;

	assert(!fplinux_ui_current(ui) && !application->navigation);
	application->navigation = kind;
	switch (kind) {
	case 'L':
		screen = fplinux_ui_list_new(ui, "List",
					     FPLINUX_UI_LIST_IMPLICIT);
		assert(screen);
		for (unsigned int i = 0; i < 64; ++i) {
			char label[24];

			snprintf(label, sizeof(label), "Item %u", i);
			assert(fplinux_ui_list_append(screen, label, NULL) ==
			       i);
		}
		break;
	case 'F':
		screen = fplinux_ui_form_new(ui, "Form");
		assert(screen);
		assert(fplinux_ui_form_append_gauge(screen, "Value", 0, 100, 0,
						    true) == 0);
		break;
	case 'T':
		screen = fplinux_ui_text_box_new(ui, "TextBox", text, 0);
		assert(screen);
		break;
	case 'R':
		application->field = fplinux_ui_text_create(
			ui, fplinux_ui_screen(ui), false, 0);
		assert(application->field);
		lv_obj_set_size(fplinux_ui_text_object(application->field), 220,
				40);
		assert(fplinux_ui_text_set(application->field, text));
		application->textarea = find_textarea(
			fplinux_ui_text_object(application->field));
		assert(application->textarea);
		break;
	case 'V':
		screen = new_repeat_canvas(ui, application);
		fplinux_ui_canvas_set_key_repeat(
			screen, FPLINUX_UI_CANVAS_REPEAT_NAVIGATION);
		application->textarea =
			lv_textarea_create(fplinux_ui_canvas_object(screen));
		assert(application->textarea);
		lv_textarea_set_text(application->textarea, text);
		lv_textarea_set_cursor_pos(application->textarea, 64);
		break;
	case 'N':
		screen = new_repeat_canvas(ui, application);
		fplinux_ui_canvas_set_keyboard_keys(screen, true);
		fplinux_ui_canvas_set_key_repeat(
			screen, FPLINUX_UI_CANVAS_REPEAT_NAVIGATION);
		break;
	case 'A': {
		char message[1600] = "";

		for (unsigned int i = 0; i < 60; ++i)
			strcat(message, "A line of alert text.\n");
		screen = fplinux_ui_alert_new(ui, FPLINUX_UI_ALERT_INFO,
					      "Alert", message, 0);
		assert(screen);
		break;
	}
	case 'M': {
		struct fplinux_ui_menu_item items[64];
		char labels[64][24];

		for (unsigned int i = 0; i < 64; ++i) {
			snprintf(labels[i], sizeof(labels[i]), "Action %u", i);
			items[i] = (struct fplinux_ui_menu_item){
				.action = i,
				.text = labels[i],
			};
		}
		assert(fplinux_ui_menu_open(
			ui, FPLINUX_UI_MENU_ORIGIN_LEFT, "Menu", items, 64,
			navigation_menu_chosen, application));
		break;
	}
	default:
		assert(false);
	}
	application->navigation_screen = screen;
	if (screen)
		fplinux_ui_push(ui, screen);
	if (kind == 'T') {
		application->textarea = find_textarea(fplinux_ui_screen(ui));
		assert(application->textarea);
	}
}

static void canvas_command(struct fplinux_ui *ui,
			   struct fplinux_ui_screen *canvas, int command_id,
			   void *data)
{
	struct repeat_application *application = data;
	struct observation observation = {
		.type = OBSERVATION_COMMAND,
		.command = command_id,
	};

	(void)ui;
	(void)canvas;
	send_observation(application->control, &observation);
}

static struct fplinux_ui_screen *
new_repeat_canvas(struct fplinux_ui *ui, struct repeat_application *application)
{
	struct fplinux_ui_screen *canvas = fplinux_ui_canvas_new(ui, NULL);

	assert(canvas);
	fplinux_ui_canvas_set_key_handler(canvas, repeat_canvas_key,
					  application);
	return canvas;
}

static void repeat_application_event(struct fplinux_ui *ui,
				     const struct fplinux_ui_event *event,
				     void *data)
{
	struct repeat_application *application = data;
	struct observation observation = {
		.type = OBSERVATION_VISIBILITY,
		.visible = event->visible,
	};

	(void)ui;
	if (application->navigation == 'R' &&
	    event->type == FPLINUX_UI_EVENT_KEY) {
		if (application->field) {
			struct fplinux_ui_key copy = event->key;
			const struct fplinux_ui_key *key =
				application->copy_raw_key ? &copy : &event->key;

			fplinux_ui_text_key(application->field, key);
		}
		if (event->key.pressed)
			++application->callbacks;
		return;
	}
	if (event->type == FPLINUX_UI_EVENT_CLOSE) {
		application->closed = true;
		observation.type = OBSERVATION_CLOSE;
		send_observation(application->control, &observation);
	} else if (event->type == FPLINUX_UI_EVENT_INPUT_CANCEL) {
		observation.type = OBSERVATION_INPUT_CANCEL;
		observation.key.source = event->key.source;
		send_observation(application->control, &observation);
	} else if (event->type == FPLINUX_UI_EVENT_VISIBILITY)
		send_observation(application->control, &observation);
}

static void blur_raw_field_on_change(struct fplinux_ui_text *field, void *data)
{
	struct repeat_application *application = data;

	if (!application->blur_on_change)
		return;
	application->blur_on_change = false;
	fplinux_ui_text_set_focused(field, false);
	fplinux_ui_text_set_focused(field, true);
}

static void repeat_application_input(struct fplinux_ui *ui, int fd,
				     short events, void *data)
{
	struct repeat_application *application = data;
	struct fplinux_ui_screen *canvas = fplinux_ui_current(ui);
	struct observation observation = { .type = OBSERVATION_COMMAND };
	char command;

	assert(events == POLLIN);
	assert(read(fd, &command, 1) == 1);
	switch (command) {
	case 'L':
	case 'F':
	case 'T':
	case 'R':
	case 'V':
	case 'A':
	case 'M':
	case 'N':
		prepare_navigation(ui, application, command);
		break;
	case 'I':
		assert(application->field);
		fplinux_ui_text_set_focused(application->field, false);
		break;
	case 'J':
		assert(application->field);
		fplinux_ui_text_set_focused(application->field, true);
		break;
	case 'D':
		assert(application->field);
		fplinux_ui_text_delete(application->field);
		application->field = NULL;
		application->textarea = NULL;
		break;
	case 'O': {
		const struct fplinux_ui_menu_item item = {
			.action = 0,
			.text = "Action",
		};

		errno = 0;
		assert(!fplinux_ui_menu_open(ui,
					     (enum fplinux_ui_menu_origin) - 1,
					     "Menu", &item, 1, NULL, NULL));
		assert(errno == EINVAL && !fplinux_ui_menu_is_open(ui));
		assert(fplinux_ui_menu_open(ui, FPLINUX_UI_MENU_ORIGIN_LEFT,
					    "Menu", &item, 1, NULL, NULL));
		break;
	}
	case 'B':
		fplinux_ui_menu_close(ui);
		break;
	case 'Y': {
		const struct fplinux_ui_key key = {
			.source = FPLINUX_UI_KEY_PHONE,
			.code = KEY_LEFT,
			.pressed = true,
		};

		assert(fplinux_ui_text_key(application->field, &key));
		break;
	}
	case 'q':
		observe_navigation(ui, application, &observation);
		break;
	case 'X':
		application->copy_raw_key = true;
		break;
	case 'P':
		application->blur_on_change = true;
		fplinux_ui_text_set_change_cb(application->field,
					      blur_raw_field_on_change,
					      application);
		break;
	case 'c':
		assert(!canvas);
		fplinux_ui_push(ui, new_repeat_canvas(ui, application));
		break;
	case 'e':
		fplinux_ui_canvas_set_key_repeat(
			canvas, FPLINUX_UI_CANVAS_REPEAT_KEYMAP);
		break;
	case 'd':
		fplinux_ui_canvas_set_key_repeat(canvas,
						 FPLINUX_UI_CANVAS_REPEAT_NONE);
		break;
	case 'g':
		fplinux_ui_canvas_set_key_repeat(
			canvas, FPLINUX_UI_CANVAS_REPEAT_NAVIGATION);
		break;
	case 'w':
		fplinux_ui_canvas_set_keyboard_keys(canvas, true);
		break;
	case 'k':
		assert(!application->picker);
		application->picker = fplinux_ui_symbol_picker_create(ui, 0);
		assert(application->picker);
		assert(fplinux_ui_symbol_picker_is_open(application->picker));
		break;
	case 't':
		assert(!application->field);
		application->field = fplinux_ui_text_create(
			ui, fplinux_ui_canvas_object(canvas), false, 0);
		assert(application->field);
		lv_obj_set_pos(fplinux_ui_text_object(application->field), 8,
			       70);
		lv_obj_set_size(fplinux_ui_text_object(application->field), 224,
				40);
		/* Keep LVGL timers paused while Wayland delivers an overdue release. */
		lv_tick_set_cb(paused_tick);
		break;
	case 'u':
		assert(fplinux_ui_text_set(application->field, ""));
		assert(fplinux_ui_text_set_constraint(application->field,
						      FPLINUX_UI_TEXT_PHONE));
		break;
	case 'f': {
		const struct fplinux_ui_command commands[] = {
			{ .id = 'B', .type = FPLINUX_UI_COMMAND_BACK },
			{ .id = 'O', .type = FPLINUX_UI_COMMAND_OK },
			{ .id = 'M',
			  .type = FPLINUX_UI_COMMAND_SCREEN,
			  .label = "Menu" },
		};

		fplinux_ui_screen_set_key_filter(canvas, filter_canvas_key,
						 application);
		fplinux_ui_screen_set_listener(canvas, canvas_command, NULL,
					       application);
		for (unsigned int i = 0;
		     i < sizeof(commands) / sizeof(commands[0]); ++i)
			assert(fplinux_ui_screen_add_command(canvas,
							     &commands[i]));
		break;
	}
	case 'p':
	case 'v':
		canvas = new_repeat_canvas(ui, application);
		fplinux_ui_canvas_set_key_repeat(
			canvas, FPLINUX_UI_CANVAS_REPEAT_KEYMAP);
		if (command == 'p')
			fplinux_ui_push(ui, canvas);
		else
			fplinux_ui_replace(ui, canvas);
		break;
	case 'b':
		assert(fplinux_ui_pop(ui));
		break;
	case 'o': {
		const struct fplinux_ui_command help = {
			.id = 7,
			.type = FPLINUX_UI_COMMAND_HELP,
		};

		assert(fplinux_ui_screen_add_command(canvas, &help));
		break;
	}
	case 'h':
		assert(fplinux_ui_home(ui));
		break;
	case 'j':
		application->bounce_on_press = true;
		break;
	case 'z':
		/* A controlled callback pause makes several deadlines elapse. */
		observation.command = command;
		send_observation(fd, &observation);
		assert(read(fd, &command, 1) == 1 && command == 'Z');
		/* Keep the next request ready when timer dispatch resumes. */
		{
			char next;

			assert(recv(fd, &next, 1, MSG_PEEK) == 1 &&
			       next == 'n');
		}
		break;
	case 'm':
	case 'n':
		break;
	default:
		assert(false);
	}
	observation.command = command;
	observation.menu_open = fplinux_ui_menu_is_open(ui);
	send_observation(fd, &observation);
}

static int run_repeat_application(int control)
{
	struct repeat_application application = { .control = control };
	struct fplinux_ui *ui = fplinux_ui_open("org.fplinux.repeat-test",
						repeat_application_event,
						&application);
	int result;

	assert(ui);
	paint(ui, 0xff0000);
	lv_refr_now(NULL);
	fplinux_ui_watch_fd(ui, control, POLLIN, repeat_application_input,
			    &application);
	result = fplinux_ui_run(ui);
	fplinux_ui_close(ui);
	assert(application.closed);
	return result;
}

static void assert_media_guard(struct fplinux_ui *ui, int error)
{
	const unsigned int pattern[] = { 0, 200 };

	assert(!fplinux_ui_audio_open(ui, 7999, 1) && errno == EINVAL);
	assert(!fplinux_ui_audio_open(ui, 8000, 3) && errno == EINVAL);
	assert(!fplinux_ui_vibration_play(ui, NULL, 0, false) &&
	       errno == EINVAL);
	assert(!fplinux_ui_audio_open(ui, 8000, 1) && errno == error);
	assert(!fplinux_ui_vibration_play(ui, pattern, 2, false) &&
	       errno == error);
}

static void media_application_event(struct fplinux_ui *ui,
				    const struct fplinux_ui_event *event,
				    void *data)
{
	struct media_application *application = data;
	char result;

	if (event->type == FPLINUX_UI_EVENT_CLOSE) {
		assert_media_guard(ui,
				   application->supported ? EAGAIN : ENOTSUP);
		application->closed = true;
		return;
	}
	if (event->type == FPLINUX_UI_EVENT_VISIBILITY) {
		assert(fplinux_ui_is_visible(ui) == event->visible);
		assert(fplinux_ui_audio_supported(ui) ==
		       application->supported);
		assert(fplinux_ui_vibration_supported(ui) ==
		       application->supported);
		assert(!fplinux_ui_audio_background_supported(ui));
		if (!event->visible)
			assert_media_guard(
				ui, application->supported ? EAGAIN : ENOTSUP);
		if (application->frame) {
			assert(lv_timer_get_paused(application->timer) ==
			       !event->visible);
			if (!event->visible) {
				uint16_t *pixels = fplinux_ui_frame_pixels(
					application->frame, NULL, NULL);

				pixels[0] = 0x07e0;
				assert(fplinux_ui_frame_damage(
					application->frame, NULL));
				assert(!fplinux_ui_frame_present(
					       application->frame) &&
				       errno == EAGAIN);
			} else {
				assert(fplinux_ui_frame_present(
					application->frame));
			}
		}
		result = event->visible ? 'V' : 'H';
	} else if (event->type == FPLINUX_UI_EVENT_LIFECYCLE) {
		if (fplinux_ui_lifecycle(ui) == FPLINUX_UI_CLOSING)
			return;
		assert(fplinux_ui_is_visible(ui));
		if (fplinux_ui_lifecycle(ui) == FPLINUX_UI_BACKGROUND) {
			assert_media_guard(ui, EAGAIN);
			result = 'B';
		} else {
			assert(fplinux_ui_lifecycle(ui) ==
			       FPLINUX_UI_FOREGROUND);
			result = 'F';
		}
	} else if (event->type == FPLINUX_UI_EVENT_SESSION &&
		   event->session->type == FPLINUX_UI_SESSION_ERROR) {
		assert(!strcmp(event->session->name, "vibration"));
		assert(!strcmp(event->session->reason, "Input/output error"));
		result = 'e';
	} else {
		return;
	}
	assert(write(application->control, &result, 1) == 1);
}

static void media_application_input(struct fplinux_ui *ui, int fd, short events,
				    void *data)
{
	struct media_application *application = data;
	const unsigned int pattern[] = { 0, 200, 150, 100, 150 };
	const int16_t frames[] = { -1200, 200, 3300 };
	char command;

	assert(events == POLLIN);
	assert(read(fd, &command, 1) == 1);
	switch (command) {
	case 'o':
	case 'p': {
		size_t index = command == 'o' ? 0 : 1;

		assert(!application->streams[index]);
		application->streams[index] =
			fplinux_ui_audio_open(ui, 8000, 1);
		assert(application->streams[index]);
		assert(fplinux_ui_audio_queued(application->streams[index]) ==
		       0);
		assert(fplinux_ui_audio_write(application->streams[index],
					      frames, 3) == 3);
		assert(fplinux_ui_audio_queued(application->streams[index]) ==
		       3);
		break;
	}
	case 'h':
		assert(application->streams[0]);
		assert(fplinux_ui_audio_write(application->streams[0], frames,
					      1) == -1 &&
		       errno == EPIPE);
		assert(fplinux_ui_audio_queued(application->streams[0]) == -1 &&
		       errno == EPIPE);
		break;
	case 'v':
		assert(fplinux_ui_vibration_play(ui, pattern, 5, true));
		break;
	case 's':
		assert(fplinux_ui_vibration_stop(ui));
		break;
	case 'x':
		assert(fplinux_ui_is_visible(ui));
		assert(fplinux_ui_lifecycle(ui) == FPLINUX_UI_FOREGROUND);
		assert(fplinux_ui_back(ui));
		assert_media_guard(ui, EAGAIN);
		break;
	default:
		assert(false);
	}
	assert(write(fd, &command, 1) == 1);
}

static void media_timer(lv_timer_t *timer)
{
	(void)timer;
	assert(false);
}

static void media_root_deleted(lv_event_t *event)
{
	struct media_application *application = lv_event_get_user_data(event);

	/* The root's deletion precedes deletion of its child canvas. */
	assert(lv_timer_get_paused(application->timer));
	application->root_deleted = true;
}

static int run_media_application(int control, const char *scenario)
{
	struct media_application application = {
		.control = control,
		.supported = strcmp(scenario, "media-unsupported") != 0,
	};
	struct fplinux_ui *ui = fplinux_ui_open("org.fplinux.media-test",
						media_application_event,
						&application);
	int result;

	assert(ui);
	lv_tick_set_cb(paused_tick);
	assert(fplinux_ui_audio_supported(ui) == application.supported);
	assert(fplinux_ui_vibration_supported(ui) == application.supported);
	assert(!fplinux_ui_audio_background_supported(ui));
	assert(!fplinux_ui_audio_open(ui, 7999, 1) && errno == EINVAL);
	if (!application.supported)
		assert_media_guard(ui, ENOTSUP);
	paint(ui, 0xff0000);
	if (!strcmp(scenario, "media-lifecycle")) {
		uint16_t *pixels;

		application.frame =
			fplinux_ui_frame_new(ui, 2, 2, FPLINUX_UI_FRAME_STAGED);
		application.timer = lv_timer_create(media_timer, 60000, NULL);
		assert(application.frame && application.timer);
		assert(lv_obj_add_event_cb(fplinux_ui_screen(ui),
					   media_root_deleted, LV_EVENT_DELETE,
					   &application));
		assert(fplinux_ui_frame_set_timer(application.frame,
						  application.timer, true));
		assert(!lv_timer_get_paused(application.timer));
		pixels = fplinux_ui_frame_pixels(application.frame, NULL, NULL);
		for (size_t i = 0; i < 4; ++i)
			pixels[i] = 0xf800;
		assert(fplinux_ui_frame_damage(application.frame, NULL));
		assert(fplinux_ui_frame_present(application.frame));
	} else {
		lv_refr_now(NULL);
	}
	fplinux_ui_watch_fd(ui, control, POLLIN, media_application_input,
			    &application);
	result = fplinux_ui_run(ui);
	fplinux_ui_close(ui);
	assert(application.closed);
	if (application.frame) {
		assert(application.root_deleted);
		assert(lv_timer_get_paused(application.timer));
		assert(!fplinux_ui_frame_object(application.frame) &&
		       errno == ENODEV);
		fplinux_ui_frame_delete(application.frame);
		lv_timer_delete(application.timer);
	}
	return result;
}

static void wait_commits(struct fplinux_ui_frame_peer *compositor,
			 unsigned int count)
{
	uint64_t deadline = fplinux_ui_frame_now_ms() + 3000;

	while (compositor->commits < count &&
	       fplinux_ui_frame_now_ms() < deadline)
		fplinux_ui_frame_pump(compositor);
	assert(compositor->commits == count);
}

static void assert_quiet(struct fplinux_ui_frame_peer *compositor, int session,
			 unsigned int count)
{
	uint64_t deadline = fplinux_ui_frame_now_ms() + 100;
	struct fplinux_ui_session_message message;

	while (fplinux_ui_frame_now_ms() < deadline) {
		fplinux_ui_frame_pump(compositor);
		assert(compositor->commits == count);
		assert(fplinux_ui_session_receive(session, &message) == 0);
	}
}

static struct fplinux_ui_session_message
wait_message(struct fplinux_ui_frame_peer *compositor, int session,
	     enum fplinux_ui_session_message_type type)
{
	struct fplinux_ui_session_message message =
		fplinux_ui_frame_wait_message(compositor, session);

	assert(message.type == type);
	return message;
}

static void wait_input(struct fplinux_ui_frame_peer *compositor, int control,
		       char expected)
{
	char result;

	fplinux_ui_frame_wait_readable(compositor, control);
	assert(read(control, &result, 1) == 1);
	assert(result == expected);
}

static struct observation
wait_observation(struct fplinux_ui_frame_peer *compositor, int control)
{
	struct observation observation;

	fplinux_ui_frame_wait_readable(compositor, control);
	assert(read(control, &observation, sizeof(observation)) ==
	       (ssize_t)sizeof(observation));
	return observation;
}

static void assert_key(const struct observation *observation,
		       enum fplinux_ui_key_source source, unsigned int code,
		       bool pressed, const char *text)
{
	assert(observation->type == OBSERVATION_KEY);
	assert(observation->key.source == source);
	assert(observation->key.code == code);
	assert(observation->key.pressed == pressed);
	assert(!strcmp(observation->key.text, text));
}

static struct fplinux_ui_key wait_key(struct fplinux_ui_frame_peer *compositor,
				      int control,
				      enum fplinux_ui_key_source source,
				      unsigned int code, bool pressed,
				      const char *text)
{
	struct observation observation = wait_observation(compositor, control);

	assert_key(&observation, source, code, pressed, text);
	return observation.key;
}

static void assert_no_input(struct fplinux_ui_frame_peer *compositor,
			    int control, unsigned int duration_ms)
{
	struct pollfd descriptor = { .fd = control, .events = POLLIN };
	uint64_t deadline = fplinux_ui_frame_now_ms() + duration_ms;

	while (fplinux_ui_frame_now_ms() < deadline) {
		fplinux_ui_frame_pump(compositor);
		assert(poll(&descriptor, 1, 0) == 0);
	}
}

static struct observation wait_command(struct fplinux_ui_frame_peer *compositor,
				       int control, char command)
{
	uint64_t deadline = fplinux_ui_frame_now_ms() + 3000;
	struct observation observation;

	do {
		assert(fplinux_ui_frame_now_ms() < deadline);
		observation = wait_observation(compositor, control);
		if (observation.type == OBSERVATION_COMMAND) {
			assert(observation.command == command);
			return observation;
		}
		/* A key callback delivered before the command is still valid. */
		assert(observation.type == OBSERVATION_KEY);
		assert(observation.key.source == FPLINUX_UI_KEY_KEYBOARD);
		assert(observation.key.code == KEY_A);
	} while (true);
}

static struct observation
application_command(struct fplinux_ui_frame_peer *compositor, int control,
		    char command)
{
	assert(write(control, &command, 1) == 1);
	return wait_command(compositor, control, command);
}

static void release_key(struct fplinux_ui_frame_peer *compositor, int control,
			struct fplinux_ui_frame_keyboard *peer,
			enum fplinux_ui_key_source source, unsigned int code,
			const char *text)
{
	uint64_t deadline = fplinux_ui_frame_now_ms() + 3000;
	struct observation observation;

	fplinux_ui_frame_send_key(peer, code, false);
	while (true) {
		assert(fplinux_ui_frame_now_ms() < deadline);
		observation = wait_observation(compositor, control);
		assert(observation.type == OBSERVATION_KEY);
		if (!observation.key.pressed) {
			assert_key(&observation, source, code, false, text);
			return;
		}
		assert_key(&observation, source, code, true, text);
	}
}

static void start_keyboard_repeat(struct fplinux_ui_frame_peer *compositor,
				  int control)
{
	uint64_t sent_at = fplinux_ui_frame_now_ms();
	struct fplinux_ui_key initial;
	struct fplinux_ui_key repeated;

	fplinux_ui_frame_send_key(&compositor->keyboards[0], KEY_A, true);
	initial = wait_key(compositor, control, FPLINUX_UI_KEY_KEYBOARD, KEY_A,
			   true, "a");
	repeated = wait_key(compositor, control, FPLINUX_UI_KEY_KEYBOARD, KEY_A,
			    true, "a");
	assert(initial.time_ms >= sent_at &&
	       initial.time_ms <= repeated.time_ms);
	assert(repeated.time_ms - initial.time_ms >= 80);
	assert(repeated.time_ms <= fplinux_ui_frame_now_ms());
}

/* A nonrepeating key after a producer update confirms its ordered delivery. */
static void keyboard_barrier(struct fplinux_ui_frame_peer *compositor,
			     int control,
			     struct fplinux_ui_frame_keyboard *peer,
			     enum fplinux_ui_key_source source)
{
	uint64_t deadline = fplinux_ui_frame_now_ms() + 3000;
	struct observation observation;

	fplinux_ui_frame_send_key(peer, KEY_F24, true);
	do {
		assert(fplinux_ui_frame_now_ms() < deadline);
		observation = wait_observation(compositor, control);
		assert(observation.type == OBSERVATION_KEY);
		if (observation.key.source == source &&
		    observation.key.code == KEY_F24)
			break;
		assert(observation.key.source == FPLINUX_UI_KEY_KEYBOARD);
		assert(observation.key.code == KEY_A);
	} while (true);
	assert_key(&observation, source, KEY_F24, true, "");
	fplinux_ui_frame_send_key(peer, KEY_F24, false);
	wait_key(compositor, control, source, KEY_F24, false, "");
}

static void check_keyboard_repeat(struct fplinux_ui_frame_peer *compositor,
				  int control)
{
	struct fplinux_ui_frame_keyboard *keyboard = &compositor->keyboards[0];
	struct fplinux_ui_key key;
	struct observation observation;
	uint64_t deadline;

	/* Neither a default Canvas nor raw Enter/Escape handling opts into repeat. */
	for (unsigned int raw = 0; raw < 2; ++raw) {
		if (raw)
			application_command(compositor, control, 'w');
		fplinux_ui_frame_send_key(keyboard, KEY_A, true);
		key = wait_key(compositor, control, FPLINUX_UI_KEY_KEYBOARD,
			       KEY_A, true, "a");
		assert(key.keysym == 'a' && key.unicode == 'a' &&
		       key.ascii == 'a');
		assert_no_input(compositor, control, 240);
		fplinux_ui_frame_send_key(keyboard, KEY_A, false);
		wait_key(compositor, control, FPLINUX_UI_KEY_KEYBOARD, KEY_A,
			 false, "a");
	}
	application_command(compositor, control, 'e');
	start_keyboard_repeat(compositor, control);

	/* A held letter resolves fresh modifiers without losing its repeat. */
	wl_keyboard_send_modifiers(keyboard->keyboard, 2, 1, 0, 0, 0);
	fplinux_ui_frame_send_key(keyboard, KEY_LEFTSHIFT, true);
	deadline = fplinux_ui_frame_now_ms() + 3000;
	do {
		assert(fplinux_ui_frame_now_ms() < deadline);
		observation = wait_observation(compositor, control);
		if (observation.key.code == KEY_LEFTSHIFT)
			break;
		assert_key(&observation, FPLINUX_UI_KEY_KEYBOARD, KEY_A, true,
			   "a");
	} while (true);
	assert_key(&observation, FPLINUX_UI_KEY_KEYBOARD, KEY_LEFTSHIFT, true,
		   "");
	assert(observation.key.shift);
	key = wait_key(compositor, control, FPLINUX_UI_KEY_KEYBOARD, KEY_A,
		       true, "A");
	assert(key.shift && !key.control && !key.alt);
	assert(key.keysym == 'A' && key.unicode == 'A' && key.ascii == 'a');

	/* Group 2 uses a distinct literal, so cached text cannot satisfy this. */
	wl_keyboard_send_modifiers(keyboard->keyboard, 3, 1, 0, 0, 1);
	fplinux_ui_frame_send_key(keyboard, KEY_LEFTSHIFT, false);
	deadline = fplinux_ui_frame_now_ms() + 3000;
	do {
		assert(fplinux_ui_frame_now_ms() < deadline);
		observation = wait_observation(compositor, control);
		if (observation.key.code == KEY_LEFTSHIFT)
			break;
		assert_key(&observation, FPLINUX_UI_KEY_KEYBOARD, KEY_A, true,
			   "A");
	} while (true);
	assert_key(&observation, FPLINUX_UI_KEY_KEYBOARD, KEY_LEFTSHIFT, false,
		   "");
	key = wait_key(compositor, control, FPLINUX_UI_KEY_KEYBOARD, KEY_A,
		       true, "B");
	assert(key.shift && key.keysym == 'B' && key.unicode == 'B');
	release_key(compositor, control, keyboard, FPLINUX_UI_KEY_KEYBOARD,
		    KEY_A, "B");
	assert_no_input(compositor, control, 240);

	/* The keymap marks Shift nonrepeating even on an opted-in Canvas. */
	fplinux_ui_frame_send_key(keyboard, KEY_LEFTSHIFT, true);
	wait_key(compositor, control, FPLINUX_UI_KEY_KEYBOARD, KEY_LEFTSHIFT,
		 true, "");
	assert_no_input(compositor, control, 240);
	fplinux_ui_frame_send_key(keyboard, KEY_LEFTSHIFT, false);
	wait_key(compositor, control, FPLINUX_UI_KEY_KEYBOARD, KEY_LEFTSHIFT,
		 false, "");
}

static void check_canvas_filter(struct fplinux_ui_frame_peer *compositor,
				int control)
{
	struct fplinux_ui_frame_keyboard *phone = &compositor->keyboards[1];
	struct fplinux_ui_frame_keyboard *keyboard = &compositor->keyboards[0];
	const unsigned int command_keys[] = { KEY_OK, KEY_F13 };
	const char commands[] = { 'O', 'M' };
	const bool pressed[] = { true, false };
	struct observation observation;

	application_command(compositor, control, 'f');
	for (unsigned int i = 0; i < sizeof(pressed) / sizeof(pressed[0]);
	     ++i) {
		fplinux_ui_frame_send_key(phone, KEY_F14, pressed[i]);
		observation = wait_observation(compositor, control);
		assert(observation.type == OBSERVATION_FILTER);
		assert(observation.key.source == FPLINUX_UI_KEY_PHONE);
		assert(observation.key.code == KEY_F14);
		assert(observation.key.pressed == pressed[i]);
	}
	assert_no_input(compositor, control, 120);
	for (unsigned int i = 0;
	     i < sizeof(command_keys) / sizeof(command_keys[0]); ++i) {
		fplinux_ui_frame_send_key(phone, command_keys[i], true);
		observation = wait_observation(compositor, control);
		assert(observation.type == OBSERVATION_COMMAND);
		assert(observation.command == commands[i]);
		fplinux_ui_frame_send_key(phone, command_keys[i], false);
		assert_no_input(compositor, control, 120);
	}
	fplinux_ui_frame_send_key(keyboard, KEY_ENTER, true);
	observation = wait_observation(compositor, control);
	assert(observation.type == OBSERVATION_COMMAND);
	assert(observation.command == 'O');
	fplinux_ui_frame_send_key(keyboard, KEY_ENTER, false);
	assert_no_input(compositor, control, 120);

	/* An unclaimed ordinary key still reaches the existing void callback. */
	fplinux_ui_frame_send_key(phone, KEY_NUMERIC_2, true);
	wait_key(compositor, control, FPLINUX_UI_KEY_PHONE, KEY_NUMERIC_2, true,
		 "");
	fplinux_ui_frame_send_key(phone, KEY_NUMERIC_2, false);
	wait_key(compositor, control, FPLINUX_UI_KEY_PHONE, KEY_NUMERIC_2,
		 false, "");
	assert_no_input(compositor, control, 120);

	application_command(compositor, control, 'w');
	fplinux_ui_frame_send_key(keyboard, KEY_ENTER, true);
	wait_key(compositor, control, FPLINUX_UI_KEY_KEYBOARD, KEY_ENTER, true,
		 "\r");
	fplinux_ui_frame_send_key(keyboard, KEY_ENTER, false);
	wait_key(compositor, control, FPLINUX_UI_KEY_KEYBOARD, KEY_ENTER, false,
		 "\r");
	assert_no_input(compositor, control, 120);

	application_command(compositor, control, 'k');
	fplinux_ui_frame_send_key(keyboard, KEY_ENTER, true);
	observation = wait_observation(compositor, control);
	assert(observation.type == OBSERVATION_FILTER);
	assert(observation.key.source == FPLINUX_UI_KEY_KEYBOARD);
	assert(observation.key.code == KEY_ENTER);
	assert(observation.key.pressed);
	assert(!strcmp(observation.key.text, "\r"));
	/* Selecting closes the picker before this press could arm a repeat. */
	assert_no_input(compositor, control, 240);
	fplinux_ui_frame_send_key(keyboard, KEY_ENTER, false);
	assert_no_input(compositor, control, 120);
}

static void check_phone_repeat(struct fplinux_ui_frame_peer *compositor,
			       int control)
{
	struct fplinux_ui_frame_keyboard *phone = &compositor->keyboards[1];
	const unsigned int arrows[] = { KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT };
	const unsigned int single[] = { KEY_NUMERIC_2,	   KEY_NUMERIC_STAR,
					KEY_NUMERIC_POUND, KEY_OK,
					KEY_F13,	   KEY_F14 };

	application_command(compositor, control, 'e');
	for (unsigned int i = 0; i < sizeof(arrows) / sizeof(arrows[0]); ++i) {
		struct fplinux_ui_key previous;
		struct fplinux_ui_key repeated;

		fplinux_ui_frame_send_key(phone, arrows[i], true);
		previous = wait_key(compositor, control, FPLINUX_UI_KEY_PHONE,
				    arrows[i], true, "");
		for (unsigned int repeat = 0; repeat < 3; ++repeat) {
			repeated = wait_key(compositor, control,
					    FPLINUX_UI_KEY_PHONE, arrows[i],
					    true, "");
			assert(repeated.time_ms - previous.time_ms >=
			       (repeat ? 200U : 500U));
			previous = repeated;
		}
		release_key(compositor, control, phone, FPLINUX_UI_KEY_PHONE,
			    arrows[i], "");
		assert_no_input(compositor, control, 600);
	}
	for (unsigned int i = 0; i < sizeof(single) / sizeof(single[0]); ++i) {
		fplinux_ui_frame_send_key(phone, single[i], true);
		wait_key(compositor, control, FPLINUX_UI_KEY_PHONE, single[i],
			 true, "");
		assert_no_input(compositor, control, 600);
		fplinux_ui_frame_send_key(phone, single[i], false);
		wait_key(compositor, control, FPLINUX_UI_KEY_PHONE, single[i],
			 false, "");
	}
	check_canvas_filter(compositor, control);
}

static void wait_menu(struct fplinux_ui_frame_peer *compositor, int control,
		      bool open)
{
	uint64_t deadline = fplinux_ui_frame_now_ms() + 3000;

	while (application_command(compositor, control, 'm').menu_open != open)
		assert(fplinux_ui_frame_now_ms() < deadline);
}

static void wait_cancelled_keyboard(struct fplinux_ui_frame_peer *compositor,
				    int control)
{
	struct observation observation;
	uint64_t deadline = fplinux_ui_frame_now_ms() + 3000;

	for (;;) {
		assert(fplinux_ui_frame_now_ms() < deadline);
		observation = wait_observation(compositor, control);
		if (observation.type == OBSERVATION_INPUT_CANCEL) {
			assert(observation.key.source ==
			       FPLINUX_UI_KEY_KEYBOARD);
			break;
		}
		assert_key(&observation, FPLINUX_UI_KEY_KEYBOARD, KEY_A, true,
			   "a");
	}
	wait_key(compositor, control, FPLINUX_UI_KEY_KEYBOARD, KEY_A, false,
		 "");
}

static void check_repeat_cancellation(struct fplinux_ui_frame_peer *compositor,
				      int session, int control,
				      const char *scenario)
{
	struct fplinux_ui_frame_keyboard *keyboard = &compositor->keyboards[0];
	struct fplinux_ui_frame_keyboard *phone = &compositor->keyboards[1];
	struct fplinux_ui_session_message visibility = {
		.type = FPLINUX_UI_SESSION_VISIBILITY,
	};
	struct observation observation;
	uint64_t deadline;
	bool cancelled[2] = { false, false };

	application_command(compositor, control, 'e');
	if (!strcmp(scenario, "repeat-options"))
		application_command(compositor, control, 'o');
	start_keyboard_repeat(compositor, control);
	if (!strcmp(scenario, "repeat-leave")) {
		wl_keyboard_send_leave(keyboard->keyboard, 2,
				       compositor->surface);
		wait_cancelled_keyboard(compositor, control);
		keyboard_barrier(compositor, control, phone,
				 FPLINUX_UI_KEY_PHONE);
		assert_no_input(compositor, control, 240);
		fplinux_ui_frame_focus_keyboard(keyboard);
		keyboard_barrier(compositor, control, phone,
				 FPLINUX_UI_KEY_PHONE);
	} else if (!strcmp(scenario, "repeat-visibility")) {
		assert(fplinux_ui_session_send(session, &visibility));
		deadline = fplinux_ui_frame_now_ms() + 3000;
		do {
			assert(fplinux_ui_frame_now_ms() < deadline);
			observation = wait_observation(compositor, control);
			if (observation.type == OBSERVATION_VISIBILITY)
				break;
			if (observation.type == OBSERVATION_INPUT_CANCEL) {
				assert(observation.key.source ==
					       FPLINUX_UI_KEY_PHONE ||
				       observation.key.source ==
					       FPLINUX_UI_KEY_KEYBOARD);
				cancelled[observation.key.source] = true;
				continue;
			}
			assert(observation.type == OBSERVATION_KEY);
			assert(observation.key.code == KEY_A);
			if (!observation.key.pressed) {
				assert(cancelled[FPLINUX_UI_KEY_KEYBOARD]);
				assert(!observation.visible);
			}
		} while (true);
		assert(!observation.visible);
		assert(cancelled[FPLINUX_UI_KEY_PHONE] &&
		       cancelled[FPLINUX_UI_KEY_KEYBOARD]);
		assert_no_input(compositor, control, 240);
		visibility.visible = true;
		assert(fplinux_ui_session_send(session, &visibility));
		observation = wait_observation(compositor, control);
		assert(observation.type == OBSERVATION_VISIBILITY &&
		       observation.visible);
	} else if (!strcmp(scenario, "repeat-options")) {
		fplinux_ui_frame_send_key(keyboard, KEY_COMPOSE, true);
		fplinux_ui_frame_send_key(keyboard, KEY_COMPOSE, false);
		wait_menu(compositor, control, true);
		assert_no_input(compositor, control, 240);
		fplinux_ui_frame_send_key(keyboard, KEY_ESC, true);
		fplinux_ui_frame_send_key(keyboard, KEY_ESC, false);
		wait_menu(compositor, control, false);
	} else if (!strcmp(scenario, "repeat-stack")) {
		application_command(compositor, control, 'p');
		assert_no_input(compositor, control, 240);
		application_command(compositor, control, 'b');
	} else if (!strcmp(scenario, "repeat-replace")) {
		application_command(compositor, control, 'v');
	} else if (!strcmp(scenario, "repeat-disable")) {
		application_command(compositor, control, 'd');
		assert_no_input(compositor, control, 240);
		application_command(compositor, control, 'e');
	} else if (!strcmp(scenario, "repeat-policy")) {
		application_command(compositor, control, 'g');
		assert_no_input(compositor, control, 240);
		application_command(compositor, control, 'e');
	} else if (!strcmp(scenario, "repeat-keymap")) {
		fplinux_ui_frame_send_keymap(keyboard);
		keyboard_barrier(compositor, control, keyboard,
				 FPLINUX_UI_KEY_KEYBOARD);
	} else if (!strcmp(scenario, "repeat-seat")) {
		wl_global_destroy(keyboard->global);
		keyboard->global = NULL;
		wait_cancelled_keyboard(compositor, control);
		keyboard_barrier(compositor, control, phone,
				 FPLINUX_UI_KEY_PHONE);
	} else if (!strcmp(scenario, "repeat-rate-zero")) {
		wl_keyboard_send_repeat_info(keyboard->keyboard, 0, 80);
		keyboard_barrier(compositor, control, keyboard,
				 FPLINUX_UI_KEY_KEYBOARD);
		assert_no_input(compositor, control, 240);
		wl_keyboard_send_repeat_info(keyboard->keyboard, 20, 80);
		keyboard_barrier(compositor, control, keyboard,
				 FPLINUX_UI_KEY_KEYBOARD);
	} else {
		assert(!strcmp(scenario, "repeat-home"));
		application_command(compositor, control, 'h');
	}
	/* Returning focus, Back or reenabling repeat cannot revive the old hold. */
	assert_no_input(compositor, control, 240);
}

static void check_repeat_pause(struct fplinux_ui_frame_peer *compositor,
			       int control, bool phone)
{
	struct observation observation;
	unsigned int repeats = 0;
	uint64_t deadline;
	enum fplinux_ui_key_source source = phone ? FPLINUX_UI_KEY_PHONE :
						    FPLINUX_UI_KEY_KEYBOARD;
	unsigned int code = phone ? KEY_DOWN : KEY_A;
	const char *text = phone ? "" : "a";
	struct fplinux_ui_frame_keyboard *keyboard =
		&compositor->keyboards[phone ? 1 : 0];

	application_command(compositor, control, 'e');
	if (phone) {
		fplinux_ui_frame_send_key(keyboard, code, true);
		wait_key(compositor, control, source, code, true, text);
		wait_key(compositor, control, source, code, true, text);
	} else {
		start_keyboard_repeat(compositor, control);
	}
	application_command(compositor, control, 'z');
	assert_no_input(compositor, control, phone ? 800 : 400);
	assert(write(control, "Z", 1) == 1);
	assert(write(control, "n", 1) == 1);
	wait_command(compositor, control, 'Z');
	deadline = fplinux_ui_frame_now_ms() + 3000;
	do {
		assert(fplinux_ui_frame_now_ms() < deadline);
		observation = wait_observation(compositor, control);
		if (observation.type == OBSERVATION_COMMAND)
			break;
		assert_key(&observation, source, code, true, text);
		++repeats;
	} while (true);
	assert(observation.command == 'n');
	/* Overdue intervals must not arrive as a catch-up burst. */
	assert(repeats <= 2);
	if (!repeats)
		wait_key(compositor, control, source, code, true, text);
	release_key(compositor, control, keyboard, source, code, text);
	assert_no_input(compositor, control, 600);
}

static void assert_text_key(struct fplinux_ui_frame_peer *compositor,
			    int control, unsigned int code, bool pressed,
			    const char *text)
{
	struct observation observation = wait_observation(compositor, control);

	assert_key(&observation, FPLINUX_UI_KEY_PHONE, code, pressed, "");
	assert(!strcmp(observation.text, text));
}

static void
check_raw_text_cancellation(struct fplinux_ui_frame_peer *compositor,
			    int session, int control)
{
	struct fplinux_ui_frame_keyboard *phone = &compositor->keyboards[1];
	const unsigned int codes[] = { KEY_NUMERIC_2, KEY_NUMERIC_0 };
	const char *const expected[] = { "A", "0" };

	application_command(compositor, control, 't');
	{
		struct fplinux_ui_session_message redraw = {
			.type = FPLINUX_UI_SESSION_SETTING,
			.serial = 100,
		};
		int left = 240, top = 320, right = -1, bottom = -1;

		fplinux_ui_frame_send_key(phone, KEY_NUMERIC_STAR, true);
		assert_text_key(compositor, control, KEY_NUMERIC_STAR, true,
				"");
		fplinux_ui_frame_send_key(phone, KEY_NUMERIC_STAR, false);
		assert_text_key(compositor, control, KEY_NUMERIC_STAR, false,
				"");
		strcpy(redraw.name, "theme");
		strcpy(redraw.value, "light");
		assert(fplinux_ui_session_send(session, &redraw));
		for (;;) {
			struct fplinux_ui_session_message message =
				fplinux_ui_frame_wait_message(compositor,
							      session);

			if (message.type == FPLINUX_UI_SESSION_REDRAW_ACK) {
				assert(message.serial == 100);
				break;
			}
			assert(message.type == FPLINUX_UI_SESSION_SOFTKEYS);
		}
		for (int y = 0; y < 320; ++y)
			for (int x = 0; x < 240; ++x) {
				if (compositor->pixels[y * 240 + x] != 0x3b0e)
					continue;
				if (x < left)
					left = x;
				if (x > right)
					right = x;
				if (y < top)
					top = y;
				if (y > bottom)
					bottom = y;
			}
		/* The 192x180 grid is centered in the launcher's 240x288 modal area. */
		assert(left == 24 && top == 54 && right == 215 &&
		       bottom == 233);
		fplinux_ui_frame_send_key(phone, KEY_NUMERIC_STAR, true);
		assert_text_key(compositor, control, KEY_NUMERIC_STAR, true,
				"");
		fplinux_ui_frame_send_key(phone, KEY_NUMERIC_STAR, false);
		assert_text_key(compositor, control, KEY_NUMERIC_STAR, false,
				"");
	}
	for (size_t index = 0; index < 2; ++index) {
		struct observation observation;

		if (index)
			application_command(compositor, control, 'u');
		fplinux_ui_frame_focus_keyboard(phone);
		fplinux_ui_frame_send_key(phone, codes[index], true);
		assert_text_key(compositor, control, codes[index], true,
				expected[index]);
		assert_no_input(compositor, control, 600);
		wl_keyboard_send_leave(phone->keyboard, 2, compositor->surface);
		observation = wait_observation(compositor, control);
		assert(observation.type == OBSERVATION_INPUT_CANCEL);
		assert(observation.key.source == FPLINUX_UI_KEY_PHONE);
		assert_text_key(compositor, control, codes[index], false,
				expected[index]);
		assert_no_input(compositor, control, 100);
	}
}

static void check_repeat_bounce(struct fplinux_ui_frame_peer *compositor,
				int control)
{
	application_command(compositor, control, 'e');
	application_command(compositor, control, 'j');
	fplinux_ui_frame_send_key(&compositor->keyboards[0], KEY_A, true);
	wait_key(compositor, control, FPLINUX_UI_KEY_KEYBOARD, KEY_A, true,
		 "a");
	/* The first handler returned to the same Canvas after a push and Back. */
	assert_no_input(compositor, control, 240);
	fplinux_ui_frame_send_key(&compositor->keyboards[0], KEY_A, false);
	wait_key(compositor, control, FPLINUX_UI_KEY_KEYBOARD, KEY_A, false,
		 "a");
}

static void check_repeat_closed(struct fplinux_ui_frame_peer *compositor,
				int control, const char *scenario)
{
	struct observation observation;
	uint64_t deadline = fplinux_ui_frame_now_ms() + 3000;

	do {
		assert(fplinux_ui_frame_now_ms() < deadline);
		observation = wait_observation(compositor, control);
		if (observation.type == OBSERVATION_CLOSE)
			break;
		assert(!strcmp(scenario, "repeat-close"));
		assert_key(&observation, FPLINUX_UI_KEY_KEYBOARD, KEY_A, true,
			   "a");
	} while (true);
	assert(read(control, &observation, sizeof(observation)) == 0);
}

static void pump_duration(struct fplinux_ui_frame_peer *compositor,
			  unsigned int duration_ms)
{
	uint64_t deadline = fplinux_ui_frame_now_ms() + duration_ms;

	while (fplinux_ui_frame_now_ms() < deadline)
		fplinux_ui_frame_pump(compositor);
}

static struct observation
wait_navigation_move(struct fplinux_ui_frame_peer *compositor, int control,
		     char kind, bool repeated, int32_t first_position)
{
	uint64_t deadline = fplinux_ui_frame_now_ms() + 1800;
	struct observation observation;
	bool moved;

	do {
		observation = application_command(compositor, control, 'q');
		if (kind == 'T' || kind == 'R' || kind == 'V')
			moved = observation.position <= (repeated ? 60 : 63);
		else if (kind == 'A')
			moved = observation.position > first_position;
		else if (kind == 'M')
			moved = observation.position > 0;
		else if (kind == 'F')
			moved = observation.position >= (repeated ? 20 : 5);
		else
			moved = observation.position >= (repeated ? 4 : 1);
		if (moved)
			return observation;
		pump_duration(compositor, 10);
	} while (fplinux_ui_frame_now_ms() < deadline);
	fprintf(stderr, "held %c navigation stopped at %d\n", kind,
		observation.position);
	assert(moved);
	return observation;
}

static char navigation_kind(const char *scenario)
{
	if (!strcmp(scenario, "repeat-list"))
		return 'L';
	if (!strcmp(scenario, "repeat-form"))
		return 'F';
	if (!strcmp(scenario, "repeat-textbox"))
		return 'T';
	if (!strcmp(scenario, "repeat-menu"))
		return 'M';
	if (!strcmp(scenario, "repeat-alert"))
		return 'A';
	if (!strcmp(scenario, "repeat-lvgl-textarea"))
		return 'V';
	if (!strncmp(scenario, "repeat-raw-", 11))
		return 'R';
	return 0;
}

static void check_navigation_repeat(struct fplinux_ui_frame_peer *compositor,
				    int control, const char *scenario,
				    char kind)
{
	struct fplinux_ui_frame_keyboard *phone = &compositor->keyboards[1];
	unsigned int code = kind == 'F' ? KEY_RIGHT :
			    kind == 'T' || kind == 'R' || kind == 'V' ?
					  KEY_LEFT :
					  KEY_DOWN;
	struct observation observation;
	int32_t first_position = 0;
	int32_t stopped;

	application_command(compositor, control, kind);
	observation = application_command(compositor, control, 'q');
	assert(observation.position ==
	       (kind == 'T' || kind == 'R' || kind == 'V' ? 64 : 0));
	if (!strcmp(scenario, "repeat-raw-synthetic")) {
		application_command(compositor, control, 'Y');
		pump_duration(compositor, 600);
		observation = application_command(compositor, control, 'q');
		assert(observation.position == 63 && !observation.callbacks);
		return;
	}
	if (!strcmp(scenario, "repeat-raw-copy"))
		application_command(compositor, control, 'X');
	if (!strcmp(scenario, "repeat-raw-pending-blur"))
		application_command(compositor, control, 'P');
	fplinux_ui_frame_send_key(phone, code, true);
	if (kind != 'M') {
		observation = wait_navigation_move(compositor, control, kind,
						   false, 0);
		if (kind == 'A')
			first_position = observation.position;
		else if (kind == 'F')
			assert(observation.position == 5);
		else
			assert(observation.position ==
			       (kind == 'T' || kind == 'R' || kind == 'V' ? 63 :
									    1));
	}
	if (!strcmp(scenario, "repeat-raw-copy") ||
	    !strcmp(scenario, "repeat-raw-pending-blur")) {
		pump_duration(compositor, 600);
		observation = application_command(compositor, control, 'q');
		assert(observation.position == 63 &&
		       observation.callbacks == 1);
		fplinux_ui_frame_send_key(phone, code, false);
		return;
	}
	observation = wait_navigation_move(compositor, control, kind, true,
					   first_position);
	if (kind == 'R')
		assert(observation.callbacks >= 4);
	if (!strcmp(scenario, "repeat-raw-blur")) {
		application_command(compositor, control, 'I');
		application_command(compositor, control, 'J');
	} else if (!strcmp(scenario, "repeat-raw-delete")) {
		application_command(compositor, control, 'D');
	} else if (!strcmp(scenario, "repeat-raw-menu")) {
		application_command(compositor, control, 'O');
		pump_duration(compositor, 160);
		application_command(compositor, control, 'B');
	} else
		fplinux_ui_frame_send_key(phone, code, false);
	/* Let the release reach the client before observing the stopped view. */
	pump_duration(compositor, 120);
	observation = application_command(compositor, control, 'q');
	stopped = observation.position;
	pump_duration(compositor, 600);
	observation = application_command(compositor, control, 'q');
	assert(observation.position == stopped);
	if (kind == 'M') {
		fplinux_ui_frame_send_key(phone, KEY_OK, true);
		observation = wait_command(compositor, control, 'S');
		assert(observation.position > 1);
		fplinux_ui_frame_send_key(phone, KEY_OK, false);
	}
	if (!strcmp(scenario, "repeat-raw-blur") ||
	    !strcmp(scenario, "repeat-raw-delete") ||
	    !strcmp(scenario, "repeat-raw-menu")) {
		unsigned int callbacks = observation.callbacks;

		pump_duration(compositor, 600);
		observation = application_command(compositor, control, 'q');
		assert(observation.callbacks == callbacks);
		fplinux_ui_frame_send_key(phone, code, false);
	}
}

static void
check_canvas_navigation_policy(struct fplinux_ui_frame_peer *compositor,
			       int control)
{
	struct fplinux_ui_frame_keyboard *keyboard = &compositor->keyboards[0];
	const unsigned int commands[] = { KEY_ENTER, KEY_TAB };
	struct observation observation;
	int32_t stopped;

	application_command(compositor, control, 'N');
	fplinux_ui_frame_send_key(keyboard, KEY_DOWN, true);
	observation = wait_navigation_move(compositor, control, 'N', false, 0);
	assert(observation.position == 1);
	wait_navigation_move(compositor, control, 'N', true, 0);
	fplinux_ui_frame_send_key(keyboard, KEY_DOWN, false);
	pump_duration(compositor, 120);
	stopped = application_command(compositor, control, 'q').position;
	pump_duration(compositor, 200);
	assert(application_command(compositor, control, 'q').position ==
	       stopped);
	for (unsigned int i = 0; i < sizeof(commands) / sizeof(commands[0]);
	     ++i) {
		uint64_t deadline = fplinux_ui_frame_now_ms() + 1800;
		unsigned int actions;

		fplinux_ui_frame_send_key(keyboard, commands[i], true);
		do {
			observation =
				application_command(compositor, control, 'q');
			actions = i ? observation.tabs :
				      observation.activations;
			if (actions)
				break;
			pump_duration(compositor, 10);
		} while (fplinux_ui_frame_now_ms() < deadline);
		assert(actions == 1);
		pump_duration(compositor, 240);
		observation = application_command(compositor, control, 'q');
		assert((i ? observation.tabs : observation.activations) == 1);
		fplinux_ui_frame_send_key(keyboard, commands[i], false);
		pump_duration(compositor, 120);
	}
}

static void check_repeat(struct fplinux_ui_frame_peer *compositor, int session,
			 int control, const char *scenario)
{
	char kind = navigation_kind(scenario);

	assert(compositor->keyboards[0].keyboard);
	assert(compositor->keyboards[1].keyboard);
	fplinux_ui_frame_focus_keyboard(&compositor->keyboards[0]);
	fplinux_ui_frame_focus_keyboard(&compositor->keyboards[1]);
	if (!strcmp(scenario, "repeat-navigation-policy")) {
		check_canvas_navigation_policy(compositor, control);
		return;
	}
	if (kind) {
		check_navigation_repeat(compositor, control, scenario, kind);
		return;
	}
	application_command(compositor, control, 'c');
	if (!strcmp(scenario, "repeat-keyboard"))
		check_keyboard_repeat(compositor, control);
	else if (!strcmp(scenario, "repeat-phone"))
		check_phone_repeat(compositor, control);
	else if (!strcmp(scenario, "repeat-pause"))
		check_repeat_pause(compositor, control, false);
	else if (!strcmp(scenario, "repeat-phone-pause"))
		check_repeat_pause(compositor, control, true);
	else if (!strcmp(scenario, "repeat-text-cancel"))
		check_raw_text_cancellation(compositor, session, control);
	else if (!strcmp(scenario, "repeat-bounce"))
		check_repeat_bounce(compositor, control);
	else if (!strcmp(scenario, "repeat-close")) {
		application_command(compositor, control, 'e');
		start_keyboard_repeat(compositor, control);
	} else
		check_repeat_cancellation(compositor, session, control,
					  scenario);
}

static void check_softkeys(struct fplinux_ui_frame_peer *compositor,
			   int session, int control)
{
	struct fplinux_ui_session_message legends;

	assert(write(control, "s", 1) == 1);
	wait_input(compositor, control, 'a');
	/* Pushing the Canvas reports its empty commands; its drawn bar sends none. */
	legends =
		wait_message(compositor, session, FPLINUX_UI_SESSION_SOFTKEYS);
	assert(!legends.left[0] && !legends.centre[0] && !legends.right[0]);
	wait_commits(compositor, 2);
	fplinux_ui_frame_release_buffers(compositor);
	assert(write(control, "u", 1) == 1);
	wait_input(compositor, control, 'a');
	assert_quiet(compositor, session, 2);
}

static void assert_colour(const struct fplinux_ui_frame_peer *compositor,
			  uint16_t colour)
{
	assert(compositor->commits);
	for (int y = 0; y < compositor->height; ++y)
		for (int x = 0; x < compositor->width; ++x)
			assert(compositor->pixels[(size_t)y * compositor->width +
						  x] == colour);
}

static void check_launcher_screens(struct fplinux_ui_frame_peer *compositor,
				   int control)
{
	const char commands[] = { 't', 'v' };

	for (size_t i = 0; i < sizeof(commands); ++i) {
		unsigned int commits = compositor->commits;

		fplinux_ui_frame_release_buffers(compositor);
		assert(write(control, &commands[i], 1) == 1);
		wait_input(compositor, control, 'a');
		wait_commits(compositor, commits + 1);
		/* Neither Tasks nor its alert scrim may cover the status bar. */
		for (int y = 0; y < 20; ++y)
			for (int x = 0; x < 240; ++x)
				assert(compositor->pixels[(size_t)y * 240 + x] ==
				       0x07e0);
		assert(compositor->pixels[100 * 240] != 0x07e0);
	}
}

static void check_exit(struct fplinux_ui_frame_peer *compositor, int session,
		       int control, pid_t child)
{
	unsigned int retained = compositor->commits;
	int status;

	fplinux_ui_frame_release_buffers(compositor);
	assert(write(control, "b", 1) == 1);
	wait_input(compositor, control, 'b');
	wait_message(compositor, session, FPLINUX_UI_SESSION_EXIT);
	assert_quiet(compositor, session, retained);
	assert(waitpid(child, &status, WNOHANG) == 0);
	assert_colour(compositor, 0xf800);
}

static void check_ack(struct fplinux_ui_frame_peer *compositor, int session,
		      int control, const char *scenario)
{
	struct fplinux_ui_session_message update = {
		.type = FPLINUX_UI_SESSION_SETTING,
		.serial = 73,
	};
	struct fplinux_ui_session_message ack;
	enum fplinux_ui_session_message_type expected =
		FPLINUX_UI_SESSION_REDRAW_ACK;
	uint16_t colour = 0x07e0;

	/* Both client buffers become busy with actual, differently drawn frames. */
	assert(write(control, "p", 1) == 1);
	wait_input(compositor, control, 'a');
	wait_commits(compositor, 2);
	assert_colour(compositor, 0x001f);
	/* A third, completed old canvas is queued while both buffers are busy. */
	assert(write(control, "q", 1) == 1);
	wait_input(compositor, control, 'a');
	if (!strcmp(scenario, "active")) {
		update.type = FPLINUX_UI_SESSION_ACTIVE;
		strcpy(update.locale, "en");
		strcpy(update.input_locale, "en");
		strcpy(update.value, "home");
		strcpy(update.name, "grid");
		update.battery = -1;
		expected = FPLINUX_UI_SESSION_ACTIVE_ACK;
	} else if (!strcmp(scenario, "activation-open") ||
		   !strcmp(scenario, "activation-resume")) {
		update.type = FPLINUX_UI_SESSION_ACTIVATION;
		update.activation =
			!strcmp(scenario, "activation-open") ?
				FPLINUX_UI_SESSION_ACTIVATION_OPEN :
				FPLINUX_UI_SESSION_ACTIVATION_RESUME;
		expected = FPLINUX_UI_SESSION_ACTIVATION_ACK;
		if (update.activation == FPLINUX_UI_SESSION_ACTIVATION_RESUME)
			colour = 0x001f;
	} else if (!strcmp(scenario, "error")) {
		update.type = FPLINUX_UI_SESSION_ERROR;
		strcpy(update.reason, "save-failed");
		colour = 0xffe0;
	} else if (!strcmp(scenario, "applied-theme") ||
		   !strcmp(scenario, "applied-locale") ||
		   !strcmp(scenario, "unapplied-theme")) {
		char packet[512];
		bool locale = !strcmp(scenario, "applied-locale");

		snprintf(
			packet, sizeof(packet),
			"{\"type\":\"ERROR\",\"name\":\"%s\",\"value\":\"%s\","
			"\"reason\":\"%s\",\"serial\":73,\"applied\":%s,"
			"\"storage_state\":\"unpublished\",\"storage_error\":28}",
			locale ? "locale" : "theme", locale ? "ru" : "dark",
			scenario,
			!strcmp(scenario, "unapplied-theme") ? "false" :
							       "true");
		assert(send(session, packet, strlen(packet), 0) ==
		       (ssize_t)strlen(packet));
		colour = 0xffe0;
	} else {
		assert(!strcmp(scenario, "setting") ||
		       !strcmp(scenario, "locale"));
		strcpy(update.name,
		       !strcmp(scenario, "locale") ? "locale" : "theme");
		strcpy(update.value,
		       !strcmp(scenario, "locale") ? "ru" : "dark");
	}
	if (strcmp(scenario, "applied-theme") &&
	    strcmp(scenario, "applied-locale") &&
	    strcmp(scenario, "unapplied-theme"))
		assert(fplinux_ui_session_send(session, &update));
	wait_input(compositor, control, 'a');
	assert_quiet(compositor, session, 2);
	fplinux_ui_frame_release_buffers(compositor);
	{
		struct pollfd descriptor = { .fd = session, .events = POLLIN };

		/* The new buffer can commit, but the server has not handled its sync. */
		assert(poll(&descriptor, 1, 100) == 0);
	}
	ack = wait_message(compositor, session, expected);
	assert(ack.serial == 73);
	assert(compositor->commits == 3);
	assert_colour(compositor, colour);
}

static void wait_application(struct fplinux_ui_frame_peer *compositor,
			     pid_t child, int exit_status)
{
	uint64_t deadline = fplinux_ui_frame_now_ms() + 3000;
	int status;
	pid_t finished;

	do {
		fplinux_ui_frame_pump(compositor);
		finished = waitpid(child, &status, WNOHANG);
		assert(finished >= 0);
	} while (!finished && fplinux_ui_frame_now_ms() < deadline);
	assert(finished == child);
	assert(WIFEXITED(status) && WEXITSTATUS(status) == exit_status);
}

static void finish_application(struct fplinux_ui_frame_peer *compositor,
			       int session, pid_t child)
{
	struct fplinux_ui_session_message close_message = {
		.type = FPLINUX_UI_SESSION_CLOSE,
	};

	strcpy(close_message.reason, "test-complete");
	assert(fplinux_ui_session_send(session, &close_message));
	wait_application(compositor, child, 0);
}

static void check_failed_exit(struct fplinux_ui_frame_peer *compositor,
			      int session, int control, pid_t child)
{
	assert(write(control, "x", 1) == 1);
	wait_input(compositor, control, 'r');
	assert(close(session) == 0);
	assert(write(control, "b", 1) == 1);
	wait_input(compositor, control, 'f');
	/* A failed protocol send ends the connection and must reach CLOSE locally. */
	wait_application(compositor, child, 255);
}

static void check_repeat_keymap(const char *path)
{
	struct xkb_context *context =
		xkb_context_new(XKB_CONTEXT_NO_DEFAULT_INCLUDES |
				XKB_CONTEXT_NO_ENVIRONMENT_NAMES);
	FILE *source = fopen(path, "r");
	struct xkb_keymap *keymap;

	assert(context && source);
	keymap = xkb_keymap_new_from_file(context, source,
					  XKB_KEYMAP_FORMAT_TEXT_V1, 0);
	assert(fclose(source) == 0);
	assert(keymap);
	assert(xkb_keymap_mod_get_index(keymap, "Shift") == 0);
	xkb_keymap_unref(keymap);
	xkb_context_unref(context);
}

static void send_visibility(struct fplinux_ui_frame_peer *compositor,
			    int session, int control, bool visible)
{
	struct fplinux_ui_session_message message = {
		.type = FPLINUX_UI_SESSION_VISIBILITY,
		.visible = visible,
	};

	assert(fplinux_ui_session_send(session, &message));
	wait_input(compositor, control, visible ? 'V' : 'H');
}

static void assert_vibration_packet(struct fplinux_ui_frame_peer *compositor,
				    int session, bool stop)
{
	const unsigned int expected[] = { 0, 200, 150, 100, 150 };
	char packet[4096];
	struct json_object *object, *type, *pattern, *repeat;
	ssize_t length;

	fplinux_ui_frame_wait_readable(compositor, session);
	length = recv(session, packet, sizeof(packet) - 1, 0);
	assert(length > 0 && (size_t)length < sizeof(packet) - 1);
	packet[length] = '\0';
	object = json_tokener_parse(packet);
	assert(object);
	assert(json_object_object_get_ex(object, "type", &type));
	assert(!strcmp(json_object_get_string(type), "VIBRATE"));
	assert(json_object_object_get_ex(object, "pattern", &pattern));
	assert(json_object_is_type(pattern, json_type_array));
	assert(json_object_array_length(pattern) == (stop ? 0U : 5U));
	for (size_t i = 0; !stop && i < 5; ++i)
		assert(json_object_get_int64(json_object_array_get_idx(
			       pattern, i)) == expected[i]);
	assert(json_object_object_get_ex(object, "repeat", &repeat));
	assert(json_object_is_type(repeat, json_type_boolean));
	assert(json_object_get_boolean(repeat) == !stop);
	json_object_put(object);
}

static void check_media_lifecycle(struct fplinux_ui_frame_peer *compositor,
				  int session, int control, bool supported)
{
	struct fplinux_ui_session_message lifecycle = {
		.type = FPLINUX_UI_SESSION_LIFECYCLE,
		.lifecycle = FPLINUX_UI_SESSION_BACKGROUND,
	};
	struct fplinux_ui_session_message error = {
		.type = FPLINUX_UI_SESSION_ERROR,
	};
	unsigned int before = compositor->commits;

	send_visibility(compositor, session, control, false);
	/* Rejected hidden requests must not reach the manager or renderer. */
	assert_quiet(compositor, session, before);
	send_visibility(compositor, session, control, true);
	wait_commits(compositor, before + 1);
	if (!supported)
		return;
	assert(compositor->pixels[0] == 0x07e0);
	assert(compositor->pixels[1] == 0xf800);
	assert(fplinux_ui_session_send(session, &lifecycle));
	wait_input(compositor, control, 'B');
	assert_quiet(compositor, session, compositor->commits);
	lifecycle.lifecycle = FPLINUX_UI_SESSION_FOREGROUND;
	assert(fplinux_ui_session_send(session, &lifecycle));
	wait_input(compositor, control, 'F');
	assert(write(control, "v", 1) == 1);
	wait_input(compositor, control, 'v');
	assert_vibration_packet(compositor, session, false);
	assert(write(control, "s", 1) == 1);
	wait_input(compositor, control, 's');
	assert_vibration_packet(compositor, session, true);
	strcpy(error.name, "vibration");
	strcpy(error.reason, "Input/output error");
	assert(fplinux_ui_session_send(session, &error));
	wait_input(compositor, control, 'e');
	assert(write(control, "x", 1) == 1);
	wait_input(compositor, control, 'x');
	wait_message(compositor, session, FPLINUX_UI_SESSION_EXIT);
	assert_quiet(compositor, session, compositor->commits);
}

/* Only the external audio service is replaced; UI transport remains real. */
struct media_audio_peer {
	int fd;
	struct fplinux_audio_ring *rings[2];
	unsigned int opened;
};

static void wait_audio_packet(struct fplinux_ui_frame_peer *compositor, int fd,
			      uint32_t packet[8])
{
	fplinux_ui_frame_wait_readable(compositor, fd);
	assert(recv(fd, packet, 32, 0) == 32);
}

static void answer_audio_open(struct fplinux_ui_frame_peer *compositor,
			      struct media_audio_peer *peer)
{
	uint32_t request[8];
	uint32_t answer[] = { 3, 0, 41 + peer->opened, 8000, 1, 0, 0, 0 };
	int fd = memfd_create("ui-media-peer", MFD_CLOEXEC | MFD_ALLOW_SEALING);
	struct fplinux_audio_ring *ring;
	union {
		struct cmsghdr alignment;
		char bytes[CMSG_SPACE(sizeof(fd))];
	} control = { 0 };
	struct iovec vector = { .iov_base = answer, .iov_len = sizeof(answer) };
	struct msghdr header = {
		.msg_iov = &vector,
		.msg_iovlen = 1,
		.msg_control = control.bytes,
		.msg_controllen = sizeof(control.bytes),
	};
	struct cmsghdr *rights = CMSG_FIRSTHDR(&header);

	wait_audio_packet(compositor, peer->fd, request);
	assert(!memcmp(request, (uint32_t[8]){ 2, 0, 0, 8000, 1, 0, 0, 0 },
		       sizeof(request)));
	/* A mono 8 kHz stream has a 64-byte header and 2048 sample frames. */
	assert(fd >= 0 && ftruncate(fd, 4160) == 0);
	ring = mmap(NULL, 4160, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	assert(ring != MAP_FAILED);
	ring->magic = 0x31415046;
	ring->rate = 8000;
	ring->channels = 1;
	ring->capacity = 2048;
	assert(fcntl(fd, F_ADD_SEALS,
		     F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL) == 0);
	rights->cmsg_level = SOL_SOCKET;
	rights->cmsg_type = SCM_RIGHTS;
	rights->cmsg_len = CMSG_LEN(sizeof(fd));
	memcpy(CMSG_DATA(rights), &fd, sizeof(fd));
	assert(sendmsg(peer->fd, &header, MSG_NOSIGNAL) ==
	       (ssize_t)sizeof(answer));
	assert(close(fd) == 0);
	assert(peer->opened < 2);
	peer->rings[peer->opened++] = ring;
}

static void check_media_audio(struct fplinux_ui_frame_peer *compositor,
			      int listener, int control,
			      struct media_audio_peer *peer, bool hang_up)
{
	uint32_t hello[8];
	unsigned int count = hang_up ? 1 : 2;

	assert(write(control, "o", 1) == 1);
	peer->fd = fplinux_ui_frame_accept_session(compositor, listener);
	wait_audio_packet(compositor, peer->fd, hello);
	assert(!memcmp(hello, (uint32_t[8]){ 1, 1, 0, 0, 0, 0, 0, 0 },
		       sizeof(hello)));
	for (unsigned int i = 0; i < count; ++i) {
		const int16_t expected[] = { -1200, 200, 3300 };
		const int16_t *samples;

		if (i)
			assert(write(control, "p", 1) == 1);
		answer_audio_open(compositor, peer);
		wait_input(compositor, control, i ? 'p' : 'o');
		assert(atomic_load(&peer->rings[i]->write) == 3);
		assert(atomic_load(&peer->rings[i]->state) == 0);
		samples = (const int16_t *)((const char *)peer->rings[i] + 64);
		assert(!memcmp(samples, expected, sizeof(expected)));
	}
	if (hang_up) {
		assert(close(peer->fd) == 0);
		peer->fd = -1;
		/* The ring still says PLAYING: only runtime HUP dispatch can end it. */
		assert(write(control, "h", 1) == 1);
		wait_input(compositor, control, 'h');
		assert(atomic_load(&peer->rings[0]->state) == 0);
	}
}

static void close_media_audio(struct fplinux_ui_frame_peer *compositor,
			      struct media_audio_peer *peer)
{
	if (peer->fd >= 0) {
		uint32_t closed[8];
		bool seen[2] = { false, false };

		/* Both streams were left open for fplinux_ui_close() to own. */
		for (unsigned int i = 0; i < 2; ++i) {
			wait_audio_packet(compositor, peer->fd, closed);
			assert(closed[2] == 41 || closed[2] == 42);
			assert(!seen[closed[2] - 41]);
			seen[closed[2] - 41] = true;
			assert(!memcmp(closed,
				       (uint32_t[8]){ 5, 0, closed[2], 0, 0, 0,
						      0, 0 },
				       sizeof(closed)));
		}
		assert(recv(peer->fd, closed, sizeof(closed), 0) == 0);
		assert(close(peer->fd) == 0);
	}
	for (unsigned int i = 0; i < peer->opened; ++i)
		assert(munmap(peer->rings[i], 4160) == 0);
}

static void check_prepare_close(struct fplinux_ui_frame_peer *compositor,
				int session, pid_t child)
{
	const char *packet = "{\"type\":\"PREPARE_CLOSE\",\"serial\":19,"
			     "\"reason\":\"user-close\"}";
	struct fplinux_ui_session_message reply;
	int status;

	assert(send(session, packet, strlen(packet), 0) ==
	       (ssize_t)strlen(packet));
	fplinux_ui_frame_wait_readable(compositor, session);
	assert(fplinux_ui_session_receive(session, &reply) == 1);
	assert(reply.serial == 19 && !strcmp(reply.value, "ready"));
	assert(reply.type == FPLINUX_UI_SESSION_CLOSE_REPLY);
	assert(waitpid(child, &status, WNOHANG) == 0);
}

static void assert_close_reply(struct fplinux_ui_frame_peer *compositor,
			       int session, uint32_t serial, const char *value)
{
	struct fplinux_ui_session_message reply = wait_message(
		compositor, session, FPLINUX_UI_SESSION_CLOSE_REPLY);

	assert(reply.serial == serial && !strcmp(reply.value, value));
}

static void check_close_decisions(struct fplinux_ui_frame_peer *compositor,
				  int session, int control, pid_t child,
				  bool forced)
{
	struct fplinux_ui_session_message prepare = {
		.type = FPLINUX_UI_SESSION_PREPARE_CLOSE,
		.serial = 31,
	};
	int status;
	unsigned int before;

	/* A user's Back first holds the frame while the manager prepares close. */
	assert(write(control, "b", 1) == 1);
	wait_input(compositor, control, 'b');
	wait_message(compositor, session, FPLINUX_UI_SESSION_EXIT);
	assert(fplinux_ui_session_send(session, &prepare));
	wait_input(compositor, control, 'w');
	assert_close_reply(compositor, session, 31, "wait");
	wait_commits(compositor, 2);
	assert_colour(compositor, 0xffe0);
	assert(wait_message(compositor, session, FPLINUX_UI_SESSION_CLOSE_ACK)
		       .serial == 31);
	assert(waitpid(child, &status, WNOHANG) == 0);
	if (forced)
		return;
	/* A duplicate request must not save twice or stack another decision. */
	before = compositor->commits;
	assert(fplinux_ui_session_send(session, &prepare));
	assert_close_reply(compositor, session, 31, "wait");
	assert(wait_message(compositor, session, FPLINUX_UI_SESSION_CLOSE_ACK)
		       .serial == 31);
	wait_commits(compositor, before + 1);
	assert(write(control, "c", 1) == 1);
	wait_input(compositor, control, 'c');
	assert_close_reply(compositor, session, 31, "cancel");
	assert(wait_message(compositor, session, FPLINUX_UI_SESSION_CLOSE_ACK)
		       .serial == 31);
	before = compositor->commits;
	assert(write(control, "p", 1) == 1);
	wait_input(compositor, control, 'a');
	wait_commits(compositor, before + 1);
	assert_colour(compositor, 0x001f);
	prepare.serial = 32;
	before = compositor->commits;
	assert(fplinux_ui_session_send(session, &prepare));
	wait_input(compositor, control, 'w');
	assert_close_reply(compositor, session, 32, "wait");
	wait_commits(compositor, before + 1);
	assert(wait_message(compositor, session, FPLINUX_UI_SESSION_CLOSE_ACK)
		       .serial == 32);
	assert_colour(compositor, 0xffe0);
	assert(write(control, "r", 1) == 1);
	wait_input(compositor, control, 'r');
	assert_close_reply(compositor, session, 32, "ready");
	assert(waitpid(child, &status, WNOHANG) == 0);
}

static void
check_close_acknowledgments(struct fplinux_ui_frame_peer *compositor,
			    int session, int control, const char *scenario)
{
	struct fplinux_ui_session_message prepare = {
		.type = FPLINUX_UI_SESSION_PREPARE_CLOSE,
		.serial = 31,
	};
	struct fplinux_ui_session_message appearance = {
		.type = FPLINUX_UI_SESSION_SETTING,
		.name = "theme",
		.value = "dark",
		.serial = 73,
	};
	struct fplinux_ui_session_message activation = {
		.type = FPLINUX_UI_SESSION_ACTIVATION,
		.activation = FPLINUX_UI_SESSION_ACTIVATION_OPEN,
		.serial = 91,
	};
	bool appearance_first = !strcmp(scenario, "close-appearance-first");
	bool hidden = !strcmp(scenario, "close-hidden");
	bool activating = !strcmp(scenario, "close-activation-independent");
	bool close_ack = false, redraw_ack = false, activation_ack = false;

	compositor->release_frames = false;
	assert(write(control, "p", 1) == 1);
	wait_input(compositor, control, 'a');
	wait_commits(compositor, 2);
	assert_colour(compositor, 0x001f);
	/* A completed old canvas must not acknowledge either new request. */
	assert(write(control, "q", 1) == 1);
	wait_input(compositor, control, 'a');
	if (hidden)
		send_visibility(compositor, session, control, false);
	if (appearance_first)
		assert(fplinux_ui_session_send(session, &appearance));
	assert(fplinux_ui_session_send(session, &prepare));
	if (!appearance_first && !hidden)
		assert(fplinux_ui_session_send(session, &appearance));
	if (activating)
		assert(fplinux_ui_session_send(session, &activation));
	if (appearance_first)
		wait_input(compositor, control, 'a');
	wait_input(compositor, control, 'w');
	if (!appearance_first && !hidden)
		wait_input(compositor, control, 'a');
	if (activating)
		wait_input(compositor, control, 'a');
	assert_close_reply(compositor, session, 31, "wait");
	assert_quiet(compositor, session, 2);
	fplinux_ui_frame_release_buffers(compositor);
	if (hidden) {
		assert_quiet(compositor, session, 2);
		send_visibility(compositor, session, control, true);
	}
	for (unsigned int i = 0; i < (hidden	 ? 1U :
				      activating ? 3U :
						   2U);
	     ++i) {
		struct fplinux_ui_session_message ack;

		fplinux_ui_frame_wait_readable(compositor, session);
		assert(fplinux_ui_session_receive(session, &ack) == 1);
		if (ack.type == FPLINUX_UI_SESSION_CLOSE_ACK) {
			assert(!close_ack && ack.serial == 31);
			close_ack = true;
		} else if (ack.type == FPLINUX_UI_SESSION_ACTIVATION_ACK) {
			assert(activating && !activation_ack &&
			       ack.serial == 91);
			activation_ack = true;
		} else {
			assert(ack.type == FPLINUX_UI_SESSION_REDRAW_ACK);
			assert(!redraw_ack && ack.serial == 73);
			redraw_ack = true;
		}
	}
	assert(close_ack && (hidden || redraw_ack));
	assert(!activating || activation_ack);
	assert(compositor->commits == 3);
	assert_colour(compositor, appearance_first || hidden ? 0xffe0 : 0x07e0);
}

static void check_lazy_preferences(const char *directory)
{
	struct fplinux_ui_prefs *prefs;
	char path[4096];

	assert(setenv("XDG_STATE_HOME", directory, 1) == 0);
	assert(snprintf(path, sizeof(path), "%s/fplinux/org.fplinux.lazy-test",
			directory) < (int)sizeof(path));
	prefs = fplinux_ui_prefs_open("org.fplinux.lazy-test");
	assert(prefs);
	assert(access(directory, F_OK) < 0 && errno == ENOENT);
	assert(access(path, F_OK) < 0 && errno == ENOENT);
	fplinux_ui_prefs_close(prefs);
}

static void check_storage_errors(void)
{
	static const struct {
		int error;
		const char *text;
	} cases[] = {
		{ ENOSPC, "Not enough storage space" },
		{ EDQUOT, "Storage limit reached" },
		{ EROFS, "Storage is read-only" },
		{ EACCES, "Storage access denied" },
		{ EPERM, "Storage access denied" },
		{ ENOMEM, "Not enough memory to save changes" },
		{ EIO, "Storage input/output error" },
		{ EMFILE, "Could not save changes" },
	};

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
		assert(!strcmp(
			fplinux_ui_storage_error((struct fplinux_storage_result){
				FPLINUX_STORAGE_UNPUBLISHED, cases[i].error }),
			cases[i].text));
	assert(!strcmp(fplinux_ui_storage_error((struct fplinux_storage_result){
			       FPLINUX_STORAGE_PUBLISHED_UNSYNCED, ENOSPC }),
		       "Not enough storage space"));
	assert(!strcmp(fplinux_ui_storage_error((struct fplinux_storage_result){
			       FPLINUX_STORAGE_DURABLE, 0 }),
		       ""));
}

int main(int argc, char **argv)
{
	struct fplinux_ui_frame_peer compositor = { 0 };
	struct media_audio_peer audio_peer = { .fd = -1 };
	struct fplinux_ui_session_message config = {
		.type = FPLINUX_UI_SESSION_CONFIG,
		.mode = FPLINUX_UI_SESSION_LAUNCHER,
		.screen_width = 240,
		.screen_height = 320,
	};
	int wayland[2], control[2];
	int listener, session;
	int audio_listener = -1;
	pid_t child;
	bool repeat, media, audio;
	bool cold_activation;

	assert(argc == 3 || argc == 4);
	if (!strcmp(argv[1], "prefs-lazy")) {
		check_lazy_preferences(argv[2]);
		puts("prefs-lazy: ok");
		return 0;
	}
	if (!strcmp(argv[1], "storage-errors")) {
		check_storage_errors();
		puts("storage-errors: ok");
		return 0;
	}
	repeat = !strncmp(argv[1], "repeat-", 7);
	media = !strncmp(argv[1], "media-", 6);
	audio = !strncmp(argv[1], "media-audio-", 12);
	cold_activation = !strcmp(argv[1], "activation-cold");
	assert((repeat || audio) == (argc == 4));
	if (repeat)
		check_repeat_keymap(argv[3]);
	if (audio)
		audio_listener = fplinux_ui_frame_listen_session(argv[3]);
	listener = fplinux_ui_frame_listen_session(argv[2]);
	fplinux_ui_frame_socketpair(wayland);
	assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, control) == 0);
	child = fork();
	assert(child >= 0);
	if (!child) {
		close(listener);
		close(wayland[0]);
		close(control[0]);
		if (audio) {
			close(audio_listener);
			assert(setenv("FPLINUX_AUDIO_STREAM_SOCKET", argv[3],
				      1) == 0);
		}
		fplinux_ui_frame_client_environment(wayland[1], argv[2]);
		if (media)
			return run_media_application(control[1], argv[1]);
		return repeat ? run_repeat_application(control[1]) :
				run_application(control[1], argv[1]);
	}
	close(wayland[1]);
	close(control[1]);
	fplinux_ui_frame_init(&compositor, wayland[0], 240, 320,
			      repeat ? argv[3] : NULL);
	compositor.release_frames = repeat || media ||
				    !strcmp(argv[1], "close-decisions") ||
				    !strcmp(argv[1], "close-forced");
	session = fplinux_ui_frame_accept_session(&compositor, listener);
	wait_message(&compositor, session, FPLINUX_UI_SESSION_HELLO);
	strcpy(config.locale, "en");
	strcpy(config.input_locale, "en");
	config.audio = config.vibration =
		media && strcmp(argv[1], "media-unsupported") != 0;
	assert(fplinux_ui_session_send(session, &config));
	if (cold_activation) {
		struct fplinux_ui_session_message activation = {
			.type = FPLINUX_UI_SESSION_ACTIVATION,
			.activation = FPLINUX_UI_SESSION_ACTIVATION_OPEN,
			.serial = 91,
		};

		assert(fplinux_ui_session_send(session, &activation));
	}
	wait_message(&compositor, session, FPLINUX_UI_SESSION_READY);
	wait_commits(&compositor, 1);
	assert_colour(&compositor, cold_activation ? 0x07e0 : 0xf800);
	if (!strcmp(argv[1], "disconnect")) {
		check_failed_exit(&compositor, session, control[0], child);
	} else {
		if (cold_activation) {
			wait_input(&compositor, control[0], 'a');
			assert(wait_message(&compositor, session,
					    FPLINUX_UI_SESSION_ACTIVATION_ACK)
				       .serial == 91);
			assert(compositor.commits == 1);
		} else if (audio)
			check_media_audio(&compositor, audio_listener,
					  control[0], &audio_peer,
					  !strcmp(argv[1], "media-audio-hup"));
		else if (media)
			check_media_lifecycle(&compositor, session, control[0],
					      !strcmp(argv[1],
						      "media-lifecycle"));
		else if (repeat)
			check_repeat(&compositor, session, control[0], argv[1]);
		else if (!strcmp(argv[1], "exit") ||
			 !strcmp(argv[1], "metrics"))
			check_exit(&compositor, session, control[0], child);
		else if (!strcmp(argv[1], "softkeys"))
			check_softkeys(&compositor, session, control[0]);
		else if (!strcmp(argv[1], "launcher-screens"))
			check_launcher_screens(&compositor, control[0]);
		else if (!strcmp(argv[1], "prepare-close"))
			check_prepare_close(&compositor, session, child);
		else if (!strcmp(argv[1], "close-decisions") ||
			 !strcmp(argv[1], "close-forced"))
			check_close_decisions(&compositor, session, control[0],
					      child,
					      !strcmp(argv[1], "close-forced"));
		else if (!strcmp(argv[1], "close-ack-first") ||
			 !strcmp(argv[1], "close-appearance-first") ||
			 !strcmp(argv[1], "close-activation-independent") ||
			 !strcmp(argv[1], "close-hidden"))
			check_close_acknowledgments(&compositor, session,
						    control[0], argv[1]);
		else
			check_ack(&compositor, session, control[0], argv[1]);
		finish_application(&compositor, session, child);
		if (audio)
			close_media_audio(&compositor, &audio_peer);
		if (repeat)
			check_repeat_closed(&compositor, control[0], argv[1]);
		close(session);
	}
	close(control[0]);
	close(listener);
	if (audio) {
		close(audio_listener);
		assert(unlink(argv[3]) == 0);
	}
	fplinux_ui_frame_cleanup(&compositor);
	assert(unlink(argv[2]) == 0);
	printf("%s: ok\n", argv[1]);
	return 0;
}
