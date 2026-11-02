/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The real runtime, screens and LVGL wait for work over a list. The client
 * counts every rendered frame that shows the busy alert and stamps the first
 * one and the end of each wait with its own clock; the checker drives keys
 * through the frame peer and reads the soft keys from the session.
 */
#define _GNU_SOURCE
#include "ui-screen-wait.h"
#include "ui-frame-peer.h"
#include "fplinux-ui-screen.h"
#include "fplinux-ui.h"

#include <assert.h>
#include <errno.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define WAIT_TITLE "Copying photos"
#define MARK_COMMAND 7

enum wait_current {
	CURRENT_ORIGIN,
	CURRENT_RESULT,
	CURRENT_QUESTION,
	CURRENT_ALERT,
	CURRENT_OTHER,
};

struct wait_state {
	enum wait_current current;
	unsigned int cancels;
	unsigned int dones;
	unsigned int answers;
	unsigned int origin_commands;
	unsigned int origin_moves;
	unsigned int waiting_frames;
	uint64_t begun_ms;
	uint64_t first_waiting_ms;
	uint64_t done_ms;
	bool waiting_visible;
};

struct wait_application {
	struct fplinux_ui *ui;
	struct fplinux_ui_screen *origin;
	struct fplinux_ui_screen *question;
	struct fplinux_ui_screen *alert;
	struct wait_state state;
	/* The question's answer begins a wait that removes Mark from the origin. */
	bool wait_on_answer;
};

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

static void rendered(lv_event_t *event)
{
	struct wait_application *application = lv_event_get_user_data(event);
	struct wait_state *state = &application->state;

	if (!visible_label(fplinux_ui_screen(application->ui), WAIT_TITLE))
		return;
	if (!state->waiting_frames++)
		state->first_waiting_ms = fplinux_ui_now_ms();
}

static void origin_command(struct fplinux_ui *ui,
			   struct fplinux_ui_screen *screen, int id,
			   void *context)
{
	struct wait_application *application = context;

	(void)ui;
	(void)screen;
	(void)id;
	++application->state.origin_commands;
}

static void origin_moved(struct fplinux_ui *ui,
			 struct fplinux_ui_screen *screen, size_t index,
			 void *context)
{
	struct wait_application *application = context;

	(void)ui;
	(void)screen;
	(void)index;
	++application->state.origin_moves;
}

static void wait_cancelled(struct fplinux_ui *ui, void *context)
{
	struct wait_application *application = context;

	(void)ui;
	++application->state.cancels;
}

static void question_command(struct fplinux_ui *ui,
			     struct fplinux_ui_screen *screen, int id,
			     void *context)
{
	struct wait_application *application = context;

	(void)screen;
	(void)id;
	application->question = NULL;
	++application->state.answers;
	if (!application->wait_on_answer)
		return;
	application->wait_on_answer = false;
	application->alert = fplinux_ui_wait_begin(ui, WAIT_TITLE, NULL,
						   wait_cancelled, application);
	assert(application->alert);
	fplinux_ui_screen_remove_command(application->origin, MARK_COMMAND);
}

static void count_done(struct wait_application *application)
{
	assert(fplinux_ui_current(application->ui) != application->alert);
	application->alert = NULL;
	application->state.done_ms = fplinux_ui_now_ms();
	++application->state.dones;
}

static void stay_done(struct fplinux_ui *ui, void *context)
{
	struct wait_application *application = context;

	assert(fplinux_ui_current(ui) == application->origin);
	count_done(application);
}

static void push_done(struct fplinux_ui *ui, void *context)
{
	struct wait_application *application = context;
	struct fplinux_ui_screen *result = fplinux_ui_form_new(ui, "Result");

	count_done(application);
	assert(result);
	fplinux_ui_form_append_string(result, NULL, "Finished");
	fplinux_ui_screen_set_data(result, application);
	assert(fplinux_ui_push(ui, result));
}

static void setup_origin(struct wait_application *application)
{
	const struct fplinux_ui_command back = {
		.id = 9,
		.type = FPLINUX_UI_COMMAND_BACK,
	};
	struct fplinux_ui *ui = application->ui;

	application->origin =
		fplinux_ui_list_new(ui, "Origin", FPLINUX_UI_LIST_IMPLICIT);
	assert(application->origin);
	for (int row = 0; row < 3; ++row)
		assert(fplinux_ui_list_append(application->origin, "Row",
					      NULL) == (size_t)row);
	assert(fplinux_ui_screen_add_command(application->origin, &back));
	fplinux_ui_screen_set_listener(application->origin, origin_command,
				       NULL, application);
	fplinux_ui_list_set_focus_listener(application->origin, origin_moved,
					   application);
	assert(fplinux_ui_push(ui, application->origin));
	lv_display_add_event_cb(lv_obj_get_display(fplinux_ui_screen(ui)),
				rendered, LV_EVENT_RENDER_READY, application);
}

static void push_question(struct wait_application *application)
{
	const struct fplinux_ui_command yes = {
		.id = 1,
		.type = FPLINUX_UI_COMMAND_OK,
		.label = "Yes",
	};
	const struct fplinux_ui_command no = {
		.id = 2,
		.type = FPLINUX_UI_COMMAND_CANCEL,
	};

	application->question = fplinux_ui_alert_new(
		application->ui, FPLINUX_UI_ALERT_CONFIRMATION, "Question",
		"Close now?", 0);
	assert(application->question);
	assert(fplinux_ui_screen_add_command(application->question, &yes));
	assert(fplinux_ui_screen_add_command(application->question, &no));
	fplinux_ui_screen_set_listener(application->question, question_command,
				       NULL, application);
	assert(fplinux_ui_push(application->ui, application->question));
}

static enum wait_current current_screen(struct wait_application *application)
{
	struct fplinux_ui_screen *current = fplinux_ui_current(application->ui);

	if (current == application->origin)
		return CURRENT_ORIGIN;
	if (current && fplinux_ui_screen_get_data(current) == application)
		return CURRENT_RESULT;
	if (current && current == application->question)
		return CURRENT_QUESTION;
	if (current && current == application->alert)
		return CURRENT_ALERT;
	return CURRENT_OTHER;
}

static void wait_input(struct fplinux_ui *ui, int fd, short events,
		       void *context)
{
	struct wait_application *application = context;
	struct wait_state *state = &application->state;
	char command;

	assert(events == POLLIN && read(fd, &command, 1) == 1);
	if (command == 'I') {
		setup_origin(application);
	} else if (command == 'B' || command == 'N') {
		application->alert = fplinux_ui_wait_begin(
			ui, WAIT_TITLE, "Please keep the phone on",
			command == 'B' ? wait_cancelled : NULL, application);
		assert(application->alert);
		assert(!fplinux_ui_wait_begin(ui, "Second", NULL, NULL, NULL) &&
		       errno == EBUSY);
		state->begun_ms = fplinux_ui_now_ms();
		state->first_waiting_ms = 0;
		state->waiting_frames = 0;
	} else if (command == 'E' || command == 'S') {
		assert(fplinux_ui_wait_end(
			ui, command == 'E' ? push_done : stay_done,
			application));
		assert(!fplinux_ui_wait_end(ui, NULL, NULL) && errno == ENOENT);
	} else if (command == 'D' || command == 'P') {
		application->wait_on_answer = command == 'P';
		push_question(application);
	} else if (command == 'A') {
		const struct fplinux_ui_command mark = {
			.id = MARK_COMMAND,
			.type = FPLINUX_UI_COMMAND_SCREEN,
			.label = "Mark",
		};

		assert(fplinux_ui_screen_add_command(application->origin,
						     &mark));
	} else if (command == 'R') {
		fplinux_ui_screen_remove_command(application->origin,
						 MARK_COMMAND);
	} else {
		assert(command == 'Q');
	}
	lv_refr_now(NULL);
	state->current = current_screen(application);
	state->waiting_visible =
		visible_label(fplinux_ui_screen(ui), WAIT_TITLE);
	assert(write(fd, state, sizeof(*state)) == (ssize_t)sizeof(*state));
}

static void wait_event(struct fplinux_ui *ui,
		       const struct fplinux_ui_event *event, void *context)
{
	(void)ui;
	(void)event;
	(void)context;
}

int fplinux_ui_test_wait_client(int control, const char *scenario)
{
	struct wait_application application = { 0 };
	int result;

	(void)scenario;
	application.ui = fplinux_ui_open("org.fplinux.wait-test", wait_event,
					 &application);
	assert(application.ui);
	lv_obj_set_style_bg_color(fplinux_ui_screen(application.ui),
				  lv_color_hex(0xff0000), 0);
	lv_refr_now(NULL);
	fplinux_ui_watch_fd(application.ui, control, POLLIN, wait_input,
			    &application);
	result = fplinux_ui_run(application.ui);
	fplinux_ui_close(application.ui);
	return result;
}

/* The checker: commands, keys, time and the latest soft keys. */

struct wait_check {
	struct fplinux_ui_frame_peer *peer;
	int session;
	int control;
	char left[FPLINUX_UI_SESSION_TEXT_BYTES];
	char right[FPLINUX_UI_SESSION_TEXT_BYTES];
	unsigned int softkeys;
};

static void receive_legends(struct wait_check *check)
{
	struct fplinux_ui_session_message message;
	int received;

	while ((received = fplinux_ui_session_receive(check->session,
						      &message)) == 1)
		if (message.type == FPLINUX_UI_SESSION_SOFTKEYS) {
			strcpy(check->left, message.left);
			strcpy(check->right, message.right);
			++check->softkeys;
		}
	assert(received == 0);
}

static struct wait_state wait_command(struct wait_check *check, char command)
{
	struct wait_state state;

	assert(write(check->control, &command, 1) == 1);
	fplinux_ui_frame_wait_readable(check->peer, check->control);
	assert(read(check->control, &state, sizeof(state)) ==
	       (ssize_t)sizeof(state));
	receive_legends(check);
	return state;
}

static void pump_for(struct wait_check *check, unsigned int milliseconds)
{
	uint64_t deadline = fplinux_ui_frame_now_ms() + milliseconds;

	while (fplinux_ui_frame_now_ms() < deadline) {
		fplinux_ui_frame_pump(check->peer);
		receive_legends(check);
	}
}

static void press(struct wait_check *check, unsigned int code)
{
	fplinux_ui_frame_send_key(&check->peer->keyboards[0], code, true);
	fplinux_ui_frame_send_key(&check->peer->keyboards[0], code, false);
	pump_for(check, 30);
}

static struct wait_state wait_visible(struct wait_check *check, bool visible)
{
	uint64_t deadline = fplinux_ui_frame_now_ms() + 3000;
	struct wait_state state;

	do {
		assert(fplinux_ui_frame_now_ms() < deadline);
		state = wait_command(check, 'Q');
	} while (state.waiting_visible != visible);
	return state;
}

static struct wait_state wait_dones(struct wait_check *check,
				    unsigned int dones)
{
	uint64_t deadline = fplinux_ui_frame_now_ms() + 3000;
	struct wait_state state;

	do {
		assert(fplinux_ui_frame_now_ms() < deadline);
		state = wait_command(check, 'Q');
	} while (state.dones != dones);
	return state;
}

/* Work under the delay: no frame shows the alert, done pushes at once. */
static void check_fast(struct wait_check *check)
{
	struct wait_state state = wait_command(check, 'B');

	assert(state.current == CURRENT_ORIGIN && !state.waiting_visible);
	pump_for(check, 100);
	state = wait_command(check, 'E');
	state = wait_dones(check, 1);
	assert(state.current == CURRENT_RESULT);
	assert(!state.waiting_frames && !state.cancels);
}

/*
 * The alert appears only after the delay and stays its minimum time even
 * when the work ends at once; the tolerances leave room for a loaded host.
 */
static void check_slow(struct wait_check *check)
{
	struct wait_state state = wait_command(check, 'B');

	state = wait_visible(check, true);
	assert(state.current == CURRENT_ALERT);
	assert(state.first_waiting_ms - state.begun_ms >= 400);
	assert(!strcmp(check->right, "Cancel"));
	state = wait_command(check, 'E');
	state = wait_dones(check, 1);
	assert(state.current == CURRENT_RESULT && !state.waiting_visible);
	assert(state.done_ms - state.first_waiting_ms >= 400);
	assert(!state.cancels);
}

static void check_cancel(struct wait_check *check)
{
	struct wait_state state = wait_command(check, 'B');

	/* Before the alert: Back cancels and the origin ignores other keys. */
	press(check, KEY_DOWN);
	press(check, KEY_ENTER);
	press(check, KEY_ESC);
	state = wait_command(check, 'Q');
	assert(state.cancels == 1 && state.current == CURRENT_ORIGIN);
	assert(!state.origin_commands && !state.origin_moves);
	state = wait_visible(check, true);
	assert(!check->right[0]);
	press(check, KEY_ESC);
	state = wait_command(check, 'S');
	state = wait_dones(check, 1);
	assert(state.cancels == 1 && state.current == CURRENT_ORIGIN);
	press(check, KEY_DOWN);
	assert(wait_command(check, 'Q').origin_moves == 1);

	/* The alert's Cancel keeps it until the end of the work. */
	state = wait_command(check, 'B');
	state = wait_visible(check, true);
	assert(!strcmp(check->right, "Cancel"));
	press(check, KEY_ESC);
	state = wait_command(check, 'Q');
	assert(state.cancels == 2 && state.waiting_visible &&
	       state.current == CURRENT_ALERT && !check->right[0]);
	state = wait_command(check, 'S');
	state = wait_dones(check, 2);
	assert(state.current == CURRENT_ORIGIN && !state.waiting_visible);

	/* Without a cancel callback Back is ignored as well. */
	state = wait_command(check, 'N');
	press(check, KEY_ESC);
	state = wait_command(check, 'Q');
	assert(state.cancels == 2 && state.current == CURRENT_ORIGIN);
	state = wait_command(check, 'S');
	state = wait_dones(check, 3);
	assert(state.current == CURRENT_ORIGIN && state.origin_moves == 1);
}

/* A question above the origin keeps its place until it is answered. */
static void check_question(struct wait_check *check)
{
	struct wait_state state = wait_command(check, 'B');

	state = wait_command(check, 'D');
	assert(state.current == CURRENT_QUESTION);
	pump_for(check, 800);
	state = wait_command(check, 'E');
	pump_for(check, 700);
	state = wait_command(check, 'Q');
	assert(state.current == CURRENT_QUESTION && !state.dones);
	assert(!state.waiting_frames);
	press(check, KEY_ESC);
	state = wait_dones(check, 1);
	assert(state.answers == 1 && state.current == CURRENT_RESULT);
	assert(!state.waiting_frames && !state.cancels);

	/* Above a shown alert, the question stays after the work ends. */
	state = wait_command(check, 'B');
	state = wait_visible(check, true);
	state = wait_command(check, 'D');
	assert(state.current == CURRENT_QUESTION);
	state = wait_command(check, 'E');
	pump_for(check, 700);
	state = wait_command(check, 'Q');
	assert(state.current == CURRENT_QUESTION && state.dones == 1);
	press(check, KEY_ESC);
	state = wait_dones(check, 2);
	assert(state.answers == 2 && state.current == CURRENT_RESULT &&
	       !state.waiting_visible && !state.cancels);
}

/*
 * The origin shows the legends it had when the wait began, also when the
 * application removes its commands meanwhile and after a question above it.
 */
static void check_legends(struct wait_check *check)
{
	struct wait_state state = wait_command(check, 'A');
	unsigned int softkeys;

	/* Each command's legends precede the reply to the next one. */
	state = wait_command(check, 'Q');
	assert(!strcmp(check->left, "Mark"));
	state = wait_command(check, 'B');
	softkeys = check->softkeys;
	state = wait_command(check, 'R');
	state = wait_command(check, 'Q');
	assert(state.current == CURRENT_ORIGIN && check->softkeys == softkeys);
	state = wait_command(check, 'D');
	assert(state.current == CURRENT_QUESTION);
	press(check, KEY_ESC);
	state = wait_command(check, 'Q');
	assert(state.answers == 1 && state.current == CURRENT_ORIGIN);
	assert(!strcmp(check->left, "Mark") && !strcmp(check->right, "Back"));
	state = wait_command(check, 'S');
	state = wait_dones(check, 1);
	assert(state.current == CURRENT_ORIGIN && !check->left[0]);

	/* A question's listener begins the wait, then the origin loses Mark. */
	state = wait_command(check, 'A');
	state = wait_command(check, 'Q');
	assert(!strcmp(check->left, "Mark"));
	state = wait_command(check, 'P');
	assert(state.current == CURRENT_QUESTION);
	press(check, KEY_ESC);
	state = wait_command(check, 'Q');
	assert(state.answers == 2 && state.current == CURRENT_ORIGIN);
	assert(!strcmp(check->left, "Mark") && !strcmp(check->right, "Back"));
	state = wait_command(check, 'S');
	state = wait_dones(check, 2);
	assert(state.current == CURRENT_ORIGIN && !check->left[0]);
}

void fplinux_ui_test_wait_check(struct fplinux_ui_frame_peer *peer, int session,
				int control, const char *scenario)
{
	struct wait_check check = {
		.peer = peer,
		.session = session,
		.control = control,
	};
	struct wait_state state = wait_command(&check, 'I');

	assert(state.current == CURRENT_ORIGIN);
	fplinux_ui_frame_focus_keyboard(&peer->keyboards[0]);
	if (!strcmp(scenario, "wait-fast"))
		check_fast(&check);
	else if (!strcmp(scenario, "wait-slow"))
		check_slow(&check);
	else if (!strcmp(scenario, "wait-cancel"))
		check_cancel(&check);
	else if (!strcmp(scenario, "wait-legends"))
		check_legends(&check);
	else {
		assert(!strcmp(scenario, "wait-question"));
		check_question(&check);
	}
}
