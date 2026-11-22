/* SPDX-License-Identifier: GPL-2.0-only */
#include "fplinux-ui-screen.h"
#include "fplinux-ui.h"
#include "ui-form-boundaries.h"
#include "ui-frame-peer.h"

#include <assert.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

struct form_application {
	struct fplinux_ui_screen *form;
	bool closed;
	unsigned int calls;
};

struct form_observation {
	int32_t before_top, before_bottom;
	int32_t after_top;
	int32_t caption_top, caption_bottom;
	lv_area_t row, name, value, next, viewport;
	size_t focus;
	unsigned int calls;
};

struct form_capture {
	struct fplinux_ui_frame_peer *peer;
	int session;
	int control;
	const char *directory;
	FILE *geometry;
	FILE *properties;
	uint32_t serial;
};

static const char long_value[] = "Line 01\nLine 02\nLine 03\nLine 04\nLine 05\n"
				 "Line 06\nLine 07\nLine 08\nLine 09\nLine 10\n"
				 "Line 11\nLine 12\nLine 13\nLine 14\nLine 15\n"
				 "Line 16\nLine 17\nLine 18\nLine 19\nLine 20";

static const char long_caption[] = "Key\nHeading 2\nHeading 3";

static const char single_line_value[] =
	"Window settings for messages contacts calls and applications "
	"are available under the main menu";

static void value_command(struct fplinux_ui *ui, struct fplinux_ui_screen *form,
			  size_t item, int command, void *context)
{
	struct form_application *application = context;

	(void)ui;
	assert(form == application->form && command == 17 && item == 0);
	++application->calls;
}

static void add_value_command(struct fplinux_ui_screen *form, size_t item)
{
	const struct fplinux_ui_command command = {
		.id = 17,
		.type = FPLINUX_UI_COMMAND_ITEM,
		.label = "Edit",
	};

	assert(fplinux_ui_form_add_item_command(form, item, &command));
}

static lv_obj_t *label_named(lv_obj_t *object, const char *text)
{
	if (lv_obj_check_type(object, &lv_label_class) &&
	    !strcmp(lv_label_get_text(object), text))
		return object;
	for (uint32_t i = 0; i < lv_obj_get_child_count(object); ++i) {
		lv_obj_t *label =
			label_named(lv_obj_get_child(object, i), text);

		if (label)
			return label;
	}
	return NULL;
}

static lv_area_t label_area(struct fplinux_ui *ui, const char *text)
{
	lv_obj_t *label = label_named(fplinux_ui_screen(ui), text);
	lv_area_t area = { -1, -1, -1, -1 };

	if (label)
		lv_obj_get_coords(label, &area);
	return area;
}

static void append_row(struct fplinux_ui_screen *form, const char *name)
{
	const char *choices[] = { "Yes", "No" };

	assert(fplinux_ui_form_append_choice(form, name,
					     FPLINUX_UI_CHOICE_POPUP, choices,
					     2) != SIZE_MAX);
}

static struct fplinux_ui_screen *new_form(struct fplinux_ui *ui,
					  struct form_application *application)
{
	application->form = fplinux_ui_form_new(ui, "Form");
	assert(application->form);
	fplinux_ui_screen_set_item_listener(application->form, value_command,
					    application);
	application->calls = 0;
	return application->form;
}

static void append_text(struct fplinux_ui_screen *form, const char *text)
{
	assert(fplinux_ui_form_append_string(form, NULL, text) != SIZE_MAX);
}

static bool product_input(struct fplinux_ui *ui,
			  struct form_application *application, char command)
{
	struct fplinux_ui_screen *form = application->form;
	bool created = strchr("phdjemosvntbclfg0U", command) != NULL;

	if (created)
		form = new_form(ui, application);
	switch (command) {
	case 'p':
	case 'b':
	case 'h':
	case 'd':
	case 'j':
	case 'e':
	case 'm':
	case 'o':
	case '0':
		if (command == 'b')
			append_text(form, "Before");
		assert(fplinux_ui_form_append_property(form, "Key", "7") !=
		       SIZE_MAX);
		if (command == 'h' || command == 'd' || command == 'j')
			append_text(form, "Before");
		else if (command == 'e')
			append_text(form, "");
		else if (command == '0')
			assert(fplinux_ui_form_append_string(form, "Section",
							     NULL) != SIZE_MAX);
		assert(fplinux_ui_form_append_property(form, "Other", "9") !=
		       SIZE_MAX);
		if (command == 'm' || command == 'o')
			append_text(form, "Before");
		break;
	case 's':
		assert(fplinux_ui_form_append_string(form, "Key", "7") == 0);
		assert(fplinux_ui_form_append_string(form, "Other", "9") == 1);
		break;
	case 'U':
		assert(fplinux_ui_form_append_value(form, "Key",
						    single_line_value) == 0);
		break;
	case 'c':
	case 'l':
	case 'f':
	case 'g': {
		const char *label = long_caption;
		const char *text = long_value;

		if (command == 'f')
			label = "";
		else if (command == 'g')
			label = NULL;
		else
			text = command == 'c' ? NULL : "";
		assert(fplinux_ui_form_append_value(form, label, text) == 0);
		assert(fplinux_ui_form_append_value(form, "Other", "9") == 1);
		break;
	}
	case 'v':
	case 'n':
	case 't':
		assert(fplinux_ui_form_append_value(
			       form, "Key",
			       command == 'n' ? long_value : "7") == 0);
		assert(fplinux_ui_form_append_value(form, "Other", "9") == 1);
		if (command == 'n' || command == 't') {
			add_value_command(form, 0);
			add_value_command(form, 1);
		}
		break;
	case 'a':
		add_value_command(form, 0);
		fplinux_ui_form_set_focus(form, 0);
		break;
	case 'r':
		fplinux_ui_screen_remove_item_command(form, 0, 17);
		break;
	case 'u':
		assert(fplinux_ui_form_set_value_text(form, 0, long_value));
		break;
	case 'w':
		assert(fplinux_ui_form_set_value_text(form, 0, "7"));
		break;
	case 'x':
		assert(fplinux_ui_form_set_hidden(form, 0, true));
		break;
	case 'y':
		assert(fplinux_ui_form_move_item(form, 0, 1));
		break;
	case 'z':
		assert(fplinux_ui_form_delete(form, 0));
		break;
	case 'i':
		assert(fplinux_ui_form_set_string(form, 1, "Before"));
		break;
	case 'k':
		assert(fplinux_ui_form_set_string(form, 1, ""));
		break;
	case 'q':
		break;
	case '!':
		append_text(form, long_value);
		break;
	default:
		return false;
	}
	if (created)
		assert(fplinux_ui_replace(ui, form));
	if (command == 'h')
		assert(fplinux_ui_form_set_hidden(form, 1, true));
	else if (command == 'd')
		assert(fplinux_ui_form_delete(form, 1));
	else if (command == 'm')
		assert(fplinux_ui_form_move_item(form, 2, 0));
	else if (command == 'o')
		assert(fplinux_ui_form_move_item(form, 2, 1));
	return true;
}

static void form_input(struct fplinux_ui *ui, int fd, short events,
		       void *context)
{
	struct form_application *application = context;
	struct fplinux_ui_screen *form = application->form;
	struct form_observation observation;
	lv_area_t before, after, caption;
	bool created = false;
	char command;

	assert(events == POLLIN && read(fd, &command, 1) == 1);
	if (product_input(ui, application, command)) {
		form = application->form;
		goto observe;
	}
	if (strchr("312HBMIDCTZR", command)) {
		form = new_form(ui, application);
		created = true;
	}
	switch (command) {
	case '3':
	case 'D':
	case 'R':
		append_row(form, "One");
		append_row(form, "Two");
		append_row(form, "Three");
		break;
	case '1':
		append_row(form, "One");
		break;
	case '2':
		append_row(form, "One");
		append_row(form, "Three");
		break;
	case 'H':
	case 'I':
		append_row(form, "One");
		append_text(form, "Before");
		append_row(form, "Three");
		break;
	case 'B':
		append_text(form, "Before");
		append_row(form, "One");
		append_row(form, "Two");
		append_text(form, "After");
		break;
	case 'M':
		append_row(form, "One");
		append_text(form, "Before");
		append_row(form, "Two");
		append_text(form, "After");
		break;
	case 'C': {
		const char *choices[] = { "One", "Two" };

		assert(fplinux_ui_form_append_choice(
			       form, "Caption", FPLINUX_UI_CHOICE_EXCLUSIVE,
			       choices, 2) == 0);
		break;
	}
	case 'T':
		append_text(form, "Before");
		append_text(form, "After");
		break;
	case 'Z':
		append_row(form, "One");
		append_text(form, "Before");
		append_row(form, "Three");
		break;
	case 'L':
		form = new_form(ui, application);
		created = true;
		for (unsigned int i = 0; i < 12; ++i) {
			char name[16];

			assert(snprintf(name, sizeof(name), "Row %u", i + 1) <
			       (int)sizeof(name));
			append_row(form, name);
		}
		break;
	case 'F':
		fplinux_ui_form_set_focus(form, 2);
		break;
	case 'S':
	case 'V':
		assert(fplinux_ui_form_set_hidden(form, 1, false));
		break;
	case 'E':
		fplinux_ui_form_set_focus(form, 2);
		break;
	case 'Q':
		break;
	default:
		assert(false);
	}
	if (created)
		assert(fplinux_ui_replace(ui, form));
	if (command == 'H' || command == 'R')
		assert(fplinux_ui_form_set_hidden(form, 1, true));
	else if (command == 'D')
		assert(fplinux_ui_form_delete(form, 1));
	else if (command == 'M')
		assert(fplinux_ui_form_move_item(form, 0, 1));
	else if (command == 'Z') {
		fplinux_ui_form_delete_all(form);
		append_text(form, "Before");
		append_text(form, "After");
	} else if (command == 'L')
		fplinux_ui_form_set_focus(form, 11);
observe:
	lv_obj_update_layout(fplinux_ui_screen(ui));
	lv_refr_now(NULL);
	before = label_area(ui, "Before");
	after = label_area(ui, "After");
	caption = label_area(ui, "Caption");
	observation = (struct form_observation){
		.before_top = before.y1,
		.before_bottom = before.y2,
		.after_top = after.y1,
		.caption_top = caption.y1,
		.caption_bottom = caption.y2,
		.row = { -1, -1, -1, -1 },
		.name = { -1, -1, -1, -1 },
		.value = { -1, -1, -1, -1 },
		.next = { -1, -1, -1, -1 },
		.viewport = { -1, -1, -1, -1 },
		.focus = fplinux_ui_form_get_focus(form),
		.calls = application->calls,
	};
	lv_obj_t *name = label_named(fplinux_ui_screen(ui), "Key");
	lv_obj_t *next = label_named(fplinux_ui_screen(ui), "Other");
	lv_obj_t *row = NULL;

	if (!name)
		name = label_named(fplinux_ui_screen(ui), long_caption);
	if (name) {
		lv_obj_get_coords(name, &observation.name);
		row = lv_obj_get_parent(name);
	} else {
		lv_obj_t *value =
			label_named(fplinux_ui_screen(ui), long_value);

		if (value)
			row = lv_obj_get_parent(value);
	}
	if (row) {
		lv_obj_t *value = lv_obj_get_child(row, -1);

		lv_obj_get_coords(row, &observation.row);
		if (!lv_obj_is_hidden(value))
			lv_obj_get_coords(value, &observation.value);
		lv_obj_get_coords(lv_obj_get_parent(row),
				  &observation.viewport);
	}
	if (next)
		lv_obj_get_coords(lv_obj_get_parent(next), &observation.next);
	assert(write(fd, &observation, sizeof(observation)) ==
	       (ssize_t)sizeof(observation));
}

static void form_event(struct fplinux_ui *ui,
		       const struct fplinux_ui_event *event, void *context)
{
	struct form_application *application = context;

	(void)ui;
	if (event->type == FPLINUX_UI_EVENT_CLOSE)
		application->closed = true;
}

int fplinux_ui_test_form_boundaries_client(int control)
{
	struct form_application application = { 0 };
	struct fplinux_ui *ui = fplinux_ui_open(
		"org.fplinux.form-boundaries-test", form_event, &application);
	int result;

	assert(ui);
	fplinux_ui_watch_fd(ui, control, POLLIN, form_input, &application);
	result = fplinux_ui_run(ui);
	fplinux_ui_close(ui);
	assert(application.closed);
	return result;
}

static void redraw_form(struct form_capture *capture)
{
	struct fplinux_ui_session_message redraw = {
		.type = FPLINUX_UI_SESSION_SETTING,
		.applied = true,
		.serial = ++capture->serial,
		.setting = { .kind = FPLINUX_UI_SETTING_THEME,
			     .operation = FPLINUX_UI_SETTING_RESULT,
			     .value.theme = FPLINUX_UI_SESSION_THEME_LIGHT },
	};

	assert(fplinux_ui_session_send(capture->session, &redraw));
	for (;;) {
		struct fplinux_ui_session_message message =
			fplinux_ui_frame_wait_message(capture->peer,
						      capture->session);

		if (message.type == FPLINUX_UI_SESSION_REDRAW_ACK) {
			assert(message.serial == redraw.serial);
			return;
		}
		assert(message.type == FPLINUX_UI_SESSION_SOFTKEYS);
	}
}

static void capture_form(struct form_capture *capture, char command,
			 const char *name)
{
	struct fplinux_ui_frame_peer *peer = capture->peer;
	struct form_observation observation;
	char path[512];
	FILE *output;

	assert(write(capture->control, &command, 1) == 1);
	fplinux_ui_frame_wait_readable(peer, capture->control);
	assert(read(capture->control, &observation, sizeof(observation)) ==
	       (ssize_t)sizeof(observation));
	redraw_form(capture);
	assert(snprintf(path, sizeof(path), "%s/%s.rgb565", capture->directory,
			name) < (int)sizeof(path));
	output = fopen(path, "wb");
	assert(output);
	assert(fwrite(peer->pixels, sizeof(*peer->pixels),
		      (size_t)peer->width * peer->height,
		      output) == (size_t)peer->width * peer->height);
	assert(fclose(output) == 0);
	assert(fprintf(capture->geometry, "%s\t%d\t%d\t%d\t%d\t%d\n", name,
		       observation.before_top, observation.before_bottom,
		       observation.after_top, observation.caption_top,
		       observation.caption_bottom) > 0);
	assert(fprintf(capture->properties, "%s", name) > 0);
	const lv_area_t areas[] = { observation.row, observation.name,
				    observation.value, observation.next,
				    observation.viewport };

	for (size_t i = 0; i < sizeof(areas) / sizeof(areas[0]); ++i)
		assert(fprintf(capture->properties, "\t%d\t%d\t%d\t%d",
			       areas[i].x1, areas[i].y1, areas[i].x2,
			       areas[i].y2) > 0);
	assert(fprintf(capture->properties, "\t%zu\t%u\n", observation.focus,
		       observation.calls) > 0);
}

static void form_key(struct form_capture *capture, unsigned int code)
{
	fplinux_ui_frame_send_key(&capture->peer->keyboards[0], code, true);
	fplinux_ui_frame_send_key(&capture->peer->keyboards[0], code, false);
	redraw_form(capture);
}

static void capture_properties(struct form_capture *capture)
{
	capture_form(capture, 'p', "properties");
	form_key(capture, KEY_RIGHT);
	capture_form(capture, 'q', "properties-bottom");
	capture_form(capture, 'h', "properties-hidden");
	capture_form(capture, 'd', "properties-deleted");
	capture_form(capture, 'j', "properties-separated");
	capture_form(capture, 'o', "properties-moved-apart");
	capture_form(capture, 'b', "properties-after-text");
	capture_form(capture, 'm', "properties-moved-together");
	capture_form(capture, 'e', "properties-empty");
	capture_form(capture, 'i', "properties-empty-filled");
	capture_form(capture, 'k', "properties-empty-cleared");
	capture_form(capture, 's', "strings");
	capture_form(capture, '0', "property-heading");
	capture_form(capture, 'c', "value-empty-null");
	capture_form(capture, 'l', "value-empty-string");
	capture_form(capture, 'f', "caption-empty-string");
	capture_form(capture, 'q', "caption-empty-settled");
	capture_form(capture, 'g', "caption-empty-null");
	capture_form(capture, 'v', "values");
	capture_form(capture, 'a', "values-command");
	form_key(capture, KEY_ENTER);
	capture_form(capture, 'q', "values-activated");
	capture_form(capture, 'r', "values-command-removed");
	capture_form(capture, 'a', "values-command-restored");
	capture_form(capture, 'u', "values-long-update");
	capture_form(capture, 'q', "values-long-settled");
	capture_form(capture, 'w', "values-short-update");
	capture_form(capture, 't', "values-two-actions");
	capture_form(capture, 'x', "values-action-hidden");
	capture_form(capture, 't', "values-two-actions-reset");
	capture_form(capture, 'y', "values-action-moved");
	capture_form(capture, 'z', "values-action-deleted");
	capture_form(capture, 'n', "values-long-action");
	form_key(capture, KEY_DOWN);
	capture_form(capture, 'q', "values-long-step");
	form_key(capture, KEY_RIGHT);
	capture_form(capture, 'q', "values-long-page");
	for (unsigned int i = 0; i < 40; ++i)
		form_key(capture, KEY_DOWN);
	capture_form(capture, 'q', "values-long-next");
	form_key(capture, KEY_UP);
	capture_form(capture, 'q', "values-long-return");
	for (unsigned int i = 0; i < 40; ++i)
		form_key(capture, KEY_UP);
	capture_form(capture, 'q', "values-long-top");
	capture_form(capture, '!', "values-long-tail");
	for (unsigned int i = 0; i < 12; ++i)
		form_key(capture, KEY_RIGHT);
	capture_form(capture, 'q', "values-action-offscreen");
	form_key(capture, KEY_ENTER);
	capture_form(capture, 'q', "values-offscreen-enter");
	capture_form(capture, 'U', "values-width-wrap");
}

void fplinux_ui_test_form_boundaries_check(struct fplinux_ui_frame_peer *peer,
					   int session, int control,
					   const char *directory)
{
	struct form_capture capture = {
		.peer = peer,
		.session = session,
		.control = control,
		.directory = directory,
	};
	char path[512];

	assert(snprintf(path, sizeof(path), "%s/geometry.tsv", directory) <
	       (int)sizeof(path));
	capture.geometry = fopen(path, "w");
	assert(capture.geometry);
	assert(snprintf(path, sizeof(path), "%s/properties.tsv", directory) <
	       (int)sizeof(path));
	capture.properties = fopen(path, "w");
	assert(capture.properties);
	capture_form(&capture, '3', "three-first");
	capture_form(&capture, 'F', "three-last");
	capture_form(&capture, '1', "single");
	capture_form(&capture, '2', "two-direct");
	capture_form(&capture, 'R', "hidden-row");
	capture_form(&capture, 'V', "shown-row");
	capture_form(&capture, 'H', "hidden-text");
	capture_form(&capture, 'S', "shown-text");
	capture_form(&capture, 'I', "text-between");
	capture_form(&capture, 'D', "deleted-row");
	capture_form(&capture, 'B', "mixed-first");
	capture_form(&capture, 'M', "mixed-moved");
	capture_form(&capture, 'E', "mixed-second");
	fplinux_ui_frame_focus_keyboard(&peer->keyboards[0]);
	for (unsigned int i = 0; i < 12; ++i) {
		fplinux_ui_frame_send_key(&peer->keyboards[0], KEY_RIGHT, true);
		fplinux_ui_frame_send_key(&peer->keyboards[0], KEY_RIGHT,
					  false);
	}
	redraw_form(&capture);
	capture_form(&capture, 'Q', "mixed-bottom");
	capture_form(&capture, 'L', "long-bottom");
	capture_form(&capture, 'C', "caption");
	capture_form(&capture, 'T', "text-only");
	capture_form(&capture, 'Z', "reset-text");
	capture_properties(&capture);
	assert(fclose(capture.geometry) == 0);
	assert(fclose(capture.properties) == 0);
}
