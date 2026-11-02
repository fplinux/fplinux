/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "ui-screen-navigation.h"
#include "ui-frame-peer.h"
#include "fplinux-ui-menu.h"
#include "fplinux-ui-screen.h"
#include "fplinux-ui.h"

#include <assert.h>
#include <errno.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct navigation_state {
	int current;
	size_t chosen;
	unsigned int changes;
	unsigned int results;
	uint32_t request;
	bool choices_visible;
	bool chooser_visible;
	bool full_command_visible;
	bool active;
	bool menu_open;
};

struct navigation_application {
	int control;
	const char *scenario;
	struct fplinux_ui_screen *base;
	struct fplinux_ui_screen *form;
	struct fplinux_ui_screen *candidate;
	size_t choice;
	unsigned int rows;
	bool closed;
	bool final_cleanup;
	char path[64];
	struct navigation_state state;
};

static const char full_command[] = "Move document into the selected folder";

static bool visible_label(lv_obj_t *object, const char *text)
{
	if (!lv_obj_is_visible(object))
		return false;
	if (lv_obj_check_type(object, &lv_label_class) &&
	    !strcmp(lv_label_get_text(object), text))
		return true;
	for (uint32_t i = 0; i < lv_obj_get_child_count(object); ++i)
		if (visible_label(lv_obj_get_child(object, (int32_t)i), text))
			return true;
	return false;
}

static void navigation_active(struct fplinux_ui *ui,
			      struct fplinux_ui_screen *canvas, bool active,
			      void *context)
{
	struct navigation_application *application = context;

	(void)ui;
	assert(canvas == application->base && !application->final_cleanup);
	application->state.active = active;
}

static void navigation_changed(struct fplinux_ui *ui,
			       struct fplinux_ui_screen *form, size_t index,
			       void *context)
{
	struct navigation_application *application = context;

	(void)ui;
	assert(form == application->form && index == application->choice);
	++application->state.changes;
}

static bool navigation_row(struct fplinux_ui *ui,
			   struct fplinux_ui_screen *list, size_t index,
			   struct fplinux_ui_virtual_row *row, void *context)
{
	struct navigation_application *application = context;

	assert(index < 8);
	assert(!row->text && !row->detail && !row->icon && !row->marked);
	assert(row->icon_kind == 0);
	assert(fplinux_ui_current(ui) == list);
	errno = 0;
	assert(!fplinux_ui_push(ui, application->candidate) && errno == EBUSY);
	assert(!fplinux_ui_navigate(ui, application->candidate) &&
	       errno == EBUSY);
	assert(!fplinux_ui_replace(ui, application->candidate) &&
	       errno == EBUSY);
	assert(!fplinux_ui_pop(ui) && errno == EBUSY);
	assert(!fplinux_ui_pop_to(ui, application->base) && errno == EBUSY);
	fplinux_ui_go_back(ui);
	assert(errno == EBUSY && fplinux_ui_current(ui) == list);
	++application->rows;
	row->text = "Retained row";
	row->detail = "Synchronous data";
	return true;
}

static void setup_navigation(struct fplinux_ui *ui,
			     struct navigation_application *application)
{
	static const char *const choices[] = { "First", "Second", "Third" };

	application->base = fplinux_ui_canvas_new(ui, NULL);
	assert(application->base);
	fplinux_ui_canvas_set_active_listener(application->base,
					      navigation_active, application);
	assert(fplinux_ui_push(ui, application->base));
	if (!strcmp(application->scenario, "stack-provider")) {
		struct fplinux_ui_screen *list;

		application->candidate = fplinux_ui_form_new(ui, "Later");
		assert(application->candidate);
		list = fplinux_ui_list_new_virtual(ui, "Rows",
						   FPLINUX_UI_LIST_IMPLICIT, 8,
						   navigation_row, application);
		assert(list && fplinux_ui_push(ui, list));
		lv_obj_update_layout(fplinux_ui_screen(ui));
		lv_refr_now(NULL);
		assert(application->rows && fplinux_ui_current(ui) == list);
		assert(!fplinux_ui_push(ui, list) && errno == EBUSY);
		assert(!fplinux_ui_replace(ui, list) && errno == EBUSY);
		assert(!fplinux_ui_pop_to(ui, application->candidate) &&
		       errno == ENOENT);
		assert(fplinux_ui_current(ui) == list);
		assert(fplinux_ui_pop_to(ui, application->base));
		assert(fplinux_ui_push(ui, application->candidate));
		assert(fplinux_ui_current(ui) == application->candidate);
		assert(fplinux_ui_pop(ui));
		assert(!fplinux_ui_pop(ui) && errno == ENOENT);
		return;
	}
	if (!strcmp(application->scenario, "stack-openwith") ||
	    !strcmp(application->scenario, "stack-canvas"))
		return;
	if (!strcmp(application->scenario, "stack-short-label")) {
		char label[sizeof(full_command)];
		char compact[] = "Here";
		struct fplinux_ui_command command = {
			.id = 71,
			.type = FPLINUX_UI_COMMAND_ITEM,
			.label = label,
			.short_label = compact,
		};
		const struct fplinux_ui_command back = {
			.id = 72,
			.type = FPLINUX_UI_COMMAND_BACK,
			.label = "Return to the previous folder",
			.short_label = "Back",
		};

		strcpy(label, full_command);
		assert(fplinux_ui_screen_add_command(application->base,
						     &command));
		assert(fplinux_ui_screen_add_command(application->base, &back));
		label[0] = compact[0] = 'X';
		return;
	}
	application->form = fplinux_ui_form_new(ui, "Alarm");
	assert(application->form);
	application->choice = fplinux_ui_form_append_choice(
		application->form, "State", FPLINUX_UI_CHOICE_POPUP, choices,
		3);
	assert(application->choice != SIZE_MAX);
	fplinux_ui_screen_set_listener(application->form, NULL,
				       navigation_changed, application);
	fplinux_ui_form_set_focus(application->form, application->choice);
	assert(fplinux_ui_push(ui, application->form));
}

static void navigation_event(struct fplinux_ui *ui,
			     const struct fplinux_ui_event *event,
			     void *context)
{
	struct navigation_application *application = context;

	if (event->type == FPLINUX_UI_EVENT_CLOSE)
		application->closed = true;
	else if (event->type == FPLINUX_UI_EVENT_DOCUMENT_RESULT) {
		assert(event->document_result.request_id ==
		       application->state.request);
		assert(event->document_result.status ==
			       FPLINUX_UI_DOCUMENT_CANCELLED ||
		       event->document_result.status ==
			       FPLINUX_UI_DOCUMENT_ACCEPTED);
		++application->state.results;
	} else if (event->type == FPLINUX_UI_EVENT_ACTIVATION &&
		   event->activation == FPLINUX_UI_ACTIVATION_OPEN) {
		assert(fplinux_ui_pop_to(ui, application->base));
		application->form = NULL;
	}
}

static void navigation_input(struct fplinux_ui *ui, int fd, short events,
			     void *context)
{
	struct navigation_application *application = context;
	struct navigation_state *state = &application->state;
	struct fplinux_ui_screen *current;
	char command;

	assert(events == POLLIN && read(fd, &command, 1) == 1);
	if (command == 'I')
		setup_navigation(ui, application);
	else if (command == 'R') {
		struct fplinux_ui_screen *replacement =
			fplinux_ui_canvas_new(ui, "Replacement");

		assert(replacement && fplinux_ui_replace(ui, replacement));
		application->form = NULL;
	} else if (command == 'D') {
		assert(fplinux_ui_form_delete(application->form,
					      application->choice));
		application->choice = SIZE_MAX;
	} else if (command == 'B') {
		assert(fplinux_ui_pop_to(ui, application->base));
		application->form = NULL;
	} else if (command == 'O') {
		uint32_t request = fplinux_ui_open_file(ui, application->path);

		if (request)
			state->request = request;
		else
			assert(errno == EBUSY);
	} else if (command == 'H') {
		const struct fplinux_ui_command help = {
			.id = 73,
			.type = FPLINUX_UI_COMMAND_HELP,
		};

		assert(fplinux_ui_screen_add_command(application->base, &help));
	} else if (command == 'M') {
		static const struct fplinux_ui_menu_item item = {
			.action = 1,
			.text = "Temporary command",
		};

		assert(fplinux_ui_menu_open(ui, FPLINUX_UI_MENU_ORIGIN_LEFT,
					    NULL, &item, 1, NULL, NULL));
	} else if (command == 'C')
		fplinux_ui_menu_close(ui);
	else if (command == 'F') {
		application->form = fplinux_ui_form_new(ui, "Cover");
		assert(application->form &&
		       fplinux_ui_push(ui, application->form));
	} else
		assert(command == 'Q');
	lv_refr_now(NULL);
	current = fplinux_ui_current(ui);
	state->current = current == application->base ? 0 :
			 current == application->form ? 1 :
							2;
	state->chosen =
		application->form && application->choice != SIZE_MAX ?
			fplinux_ui_form_get_choice(application->form,
						   application->choice) :
			SIZE_MAX;
	state->choices_visible = visible_label(fplinux_ui_screen(ui), "Third");
	state->chooser_visible =
		visible_label(fplinux_ui_screen(ui), "Second handler");
	state->full_command_visible =
		visible_label(lv_layer_top(), full_command);
	state->menu_open = fplinux_ui_menu_is_open(ui);
	assert(write(fd, state, sizeof(*state)) == (ssize_t)sizeof(*state));
}

int fplinux_ui_test_navigation_client(int control, const char *scenario)
{
	struct navigation_application application = {
		.control = control,
		.scenario = scenario,
		.choice = SIZE_MAX,
		.path = "/tmp/fplinux-navigation-XXXXXX",
	};
	struct fplinux_ui *ui = fplinux_ui_open("org.fplinux.navigation-test",
						navigation_event, &application);
	int file, result;

	assert(ui);
	file = mkstemp(application.path);
	assert(file >= 0 && close(file) == 0);
	lv_obj_set_style_bg_color(fplinux_ui_screen(ui), lv_color_hex(0xff0000),
				  0);
	lv_refr_now(NULL);
	fplinux_ui_watch_fd(ui, control, POLLIN, navigation_input,
			    &application);
	result = fplinux_ui_run(ui);
	application.final_cleanup = true;
	fplinux_ui_close(ui);
	assert(application.closed && unlink(application.path) == 0);
	return result;
}

static struct navigation_state
navigation_command(struct fplinux_ui_frame_peer *peer, int control,
		   char command)
{
	struct navigation_state state;

	assert(write(control, &command, 1) == 1);
	fplinux_ui_frame_wait_readable(peer, control);
	assert(read(control, &state, sizeof(state)) == (ssize_t)sizeof(state));
	return state;
}

static struct navigation_state
wait_navigation(struct fplinux_ui_frame_peer *peer, int control, bool choices,
		bool chooser, unsigned int changes, unsigned int results)
{
	uint64_t deadline = fplinux_ui_frame_now_ms() + 3000;
	struct navigation_state state;

	do {
		assert(fplinux_ui_frame_now_ms() < deadline);
		state = navigation_command(peer, control, 'Q');
	} while (state.choices_visible != choices ||
		 state.chooser_visible != chooser || state.changes != changes ||
		 state.results != results);
	return state;
}

static void navigation_key(struct fplinux_ui_frame_peer *peer,
			   unsigned int code)
{
	fplinux_ui_frame_send_key(&peer->keyboards[0], code, true);
	fplinux_ui_frame_send_key(&peer->keyboards[0], code, false);
}

static struct fplinux_ui_session_message
navigation_message(struct fplinux_ui_frame_peer *peer, int session,
		   enum fplinux_ui_session_message_type type)
{
	struct fplinux_ui_session_message message;

	do {
		message = fplinux_ui_frame_wait_message(peer, session);
	} while (message.type == FPLINUX_UI_SESSION_SOFTKEYS);
	assert(message.type == type);
	return message;
}

static void check_document_choices(struct fplinux_ui_frame_peer *peer,
				   int session, int control)
{
	struct fplinux_ui_session_message choices = {
		.type = FPLINUX_UI_SESSION_DOCUMENT_CHOICES,
		.document_choice_count = 2,
		.document_choices = {
			{ .app_id = "org.fplinux.first", .title = "First handler" },
			{ .app_id = "org.fplinux.second", .title = "Second handler" },
		},
	};
	struct fplinux_ui_session_message result = {
		.type = FPLINUX_UI_SESSION_DOCUMENT_RESULT,
		.document_status = FPLINUX_UI_DOCUMENT_CANCELLED,
	};
	uint32_t previous = 0;

	for (unsigned int attempt = 0; attempt < 3; ++attempt) {
		struct navigation_state state =
			navigation_command(peer, control, 'O');
		struct fplinux_ui_session_message message = navigation_message(
			peer, session, FPLINUX_UI_SESSION_OPEN_FILE);

		assert(state.request && state.request != previous);
		assert(message.serial == state.request);
		choices.serial = state.request;
		assert(fplinux_ui_session_send(session, &choices));
		state = wait_navigation(peer, control, false, true, 0, attempt);
		assert(state.current == 0);
		if (!attempt) {
			navigation_command(peer, control, 'B');
			assert(navigation_command(peer, control, 'O').request ==
			       choices.serial);
		} else {
			if (attempt == 2) {
				navigation_key(peer, KEY_DOWN);
				navigation_key(peer, KEY_ENTER);
			} else
				navigation_key(peer, KEY_ESC);
		}
		state = wait_navigation(peer, control, false, false, 0,
					attempt);
		message = navigation_message(
			peer, session, FPLINUX_UI_SESSION_DOCUMENT_CHOICE);
		assert(message.serial == choices.serial);
		assert(!strcmp(message.app_id,
			       attempt == 2 ? "org.fplinux.second" : ""));
		result.serial = choices.serial;
		if (attempt == 2)
			result.document_status = FPLINUX_UI_DOCUMENT_ACCEPTED;
		assert(fplinux_ui_session_send(session, &result));
		wait_navigation(peer, control, false, false, 0, attempt + 1);
		previous = choices.serial;
	}
}

void fplinux_ui_test_navigation_check(struct fplinux_ui_frame_peer *peer,
				      int session, int control,
				      const char *scenario)
{
	struct navigation_state state = navigation_command(peer, control, 'I');

	fplinux_ui_frame_focus_keyboard(&peer->keyboards[0]);
	if (!strcmp(scenario, "stack-provider")) {
		assert(state.current == 0 && state.active);
		return;
	}
	if (!strcmp(scenario, "stack-openwith")) {
		check_document_choices(peer, session, control);
		return;
	}
	if (!strcmp(scenario, "stack-short-label")) {
		struct fplinux_ui_session_message legends;
		uint64_t deadline = fplinux_ui_frame_now_ms() + 3000;

		do {
			legends = fplinux_ui_frame_wait_message(peer, session);
			assert(legends.type == FPLINUX_UI_SESSION_SOFTKEYS);
		} while (strcmp(legends.left, "Here") ||
			 strcmp(legends.right, "Back"));
		assert(!legends.centre[0] && !strcmp(legends.right, "Back"));
		navigation_command(peer, control, 'H');
		navigation_key(peer, KEY_COMPOSE);

		do {
			assert(fplinux_ui_frame_now_ms() < deadline);
			state = navigation_command(peer, control, 'Q');
		} while (!state.menu_open || !state.full_command_visible);
		return;
	}
	if (!strcmp(scenario, "stack-canvas")) {
		assert(state.active);
		state = navigation_command(peer, control, 'M');
		assert(!state.active && state.menu_open);
		state = navigation_command(peer, control, 'C');
		assert(state.active && !state.menu_open);
		state = navigation_command(peer, control, 'F');
		assert(!state.active && state.current == 1);
		state = navigation_command(peer, control, 'B');
		assert(state.active && state.current == 0);
		return;
	}
	assert(state.current == 1 && state.chosen == 0 && !state.changes);
	navigation_key(peer, KEY_ENTER);
	state = wait_navigation(peer, control, true, false, 0, 0);
	assert(state.current == 1 && state.chosen == 0 && !state.active);
	navigation_key(peer, KEY_DOWN);
	if (!strcmp(scenario, "stack-replace")) {
		state = navigation_command(peer, control, 'R');
		assert(state.current == 2 && !state.choices_visible &&
		       !state.changes);
		state = navigation_command(peer, control, 'B');
		assert(state.current == 0 && state.active);
	} else if (!strcmp(scenario, "stack-delete")) {
		state = navigation_command(peer, control, 'D');
		assert(state.current == 1 && state.chosen == SIZE_MAX &&
		       !state.choices_visible && !state.changes);
	} else if (!strcmp(scenario, "stack-resume") ||
		   !strcmp(scenario, "stack-open")) {
		struct fplinux_ui_session_message message = {
			.type = FPLINUX_UI_SESSION_VISIBILITY,
		};

		assert(fplinux_ui_session_send(session, &message));
		message.visible = true;
		assert(fplinux_ui_session_send(session, &message));
		message.type = FPLINUX_UI_SESSION_ACTIVATION;
		message.serial = 97;
		message.activation =
			!strcmp(scenario, "stack-open") ?
				FPLINUX_UI_SESSION_ACTIVATION_OPEN :
				FPLINUX_UI_SESSION_ACTIVATION_RESUME;
		assert(fplinux_ui_session_send(session, &message));
		assert(navigation_message(peer, session,
					  FPLINUX_UI_SESSION_ACTIVATION_ACK)
			       .serial == 97);
		if (!strcmp(scenario, "stack-open")) {
			state = wait_navigation(peer, control, false, false, 0,
						0);
			assert(state.current == 0 && state.active);
		} else {
			fplinux_ui_frame_configure(peer, 128, 128, true);
			navigation_key(peer, KEY_ENTER);
			state = wait_navigation(peer, control, false, false, 1,
						0);
			assert(state.current == 1 && state.chosen == 1);
		}
	} else {
		assert(!strcmp(scenario, "stack-cancel"));
		navigation_key(peer, KEY_ESC);
		state = wait_navigation(peer, control, false, false, 0, 0);
		assert(state.current == 1 && state.chosen == 0);
		navigation_key(peer, KEY_ENTER);
		wait_navigation(peer, control, true, false, 0, 0);
		navigation_key(peer, KEY_ENTER);
		state = wait_navigation(peer, control, false, false, 1, 0);
		assert(state.current == 1 && state.chosen == 0);
	}
}
