/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "clock-wire.h"
#include "session-clock.h"
#include "clocks-observer.h"
#include "fplinux-ui-profile.h"
#include "screen-frame.h"
#include "ui-frame-peer.h"

#include <assert.h>
#include <errno.h>
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

struct capture {
	struct fplinux_ui_frame_peer peer;
	struct clock_model owner;
	struct fplinux_session_clock *storage_owner;
	struct fplinux_storage_result storage;
	struct clocks_frame_observation observed;
	struct fplinux_ui_session_message request;
	struct clock_request operation;
	int listener;
	int session;
	int observer;
	pid_t application;
	uint32_t redraw_serial;
	uint32_t redraw_ack;
	uint32_t close_serial;
	uint32_t activation_serial;
	uint32_t activation_ack;
	char close_value[16];
	unsigned int snapshots;
	unsigned int retransmissions;
	bool has_request;
	char left[128];
	char centre[128];
	char right[128];
	const char *directory;
	const char *session_path;
};

static bool fail_directory_sync;
static unsigned int directory_sync_failures;
static void enter_alarm_hour(struct capture *capture, unsigned int digit);

int __real_fsync(int fd);
int __wrap_fsync(int fd);

int __wrap_fsync(int fd)
{
	struct stat status;

	assert(fstat(fd, &status) == 0);
	if (fail_directory_sync && S_ISDIR(status.st_mode)) {
		++directory_sync_failures;
		errno = EIO;
		return -1;
	}
	return __real_fsync(fd);
}

static int dimension(const char *text)
{
	char *end;
	long value = strtol(text, &end, 10);

	assert(*text && !*end && value > 0 && value <= 1024);
	return (int)value;
}

static void reply_result(struct capture *capture,
			 const struct fplinux_ui_session_message *request,
			 const struct clock_result *result)
{
	struct fplinux_ui_session_message response = {
		.type = FPLINUX_UI_SESSION_CLOCK,
		.serial = request->serial,
	};

	strcpy(response.name, request->name);
	assert(clock_wire_snapshot_encode(&capture->owner, result,
					  response.serial, response.clock_data,
					  sizeof(response.clock_data)) ==
	       CLOCK_OK);
	assert(fplinux_ui_session_send(capture->session, &response));
}

static void reply(struct capture *capture,
		  const struct fplinux_ui_session_message *request,
		  enum clock_status status)
{
	const struct clock_result result = {
		.status = status,
		.applied = status == CLOCK_OK,
		.storage = status == CLOCK_STORAGE_ERROR ?
				   (struct fplinux_storage_result){
					   .state = FPLINUX_STORAGE_UNPUBLISHED,
					   .error = EIO,
				   } :
				   capture->storage,
	};

	reply_result(capture, request, &result);
}

/* The peer owns only the session boundary; app, widgets and codecs are real. */
static void pump(struct capture *capture)
{
	struct fplinux_ui_session_message message;
	struct clocks_frame_observation observation;
	ssize_t bytes;
	int received;

	fplinux_ui_frame_pump(&capture->peer);
	while ((bytes = recv(capture->observer, &observation,
			     sizeof(observation), MSG_DONTWAIT)) > 0) {
		assert(bytes == (ssize_t)sizeof(observation));
		capture->observed = observation;
	}
	assert(bytes == -1 && (errno == EAGAIN || errno == EWOULDBLOCK));
	while ((received = fplinux_ui_session_receive(capture->session,
						      &message)) == 1) {
		assert(message.type != FPLINUX_UI_SESSION_EXIT);
		if (message.type == FPLINUX_UI_SESSION_CLOCK) {
			struct clock_request operation;

			assert(clock_wire_request_decode(
				       message.clock_data,
				       strlen(message.clock_data),
				       &operation) == CLOCK_OK);
			assert(!strcmp(message.name,
				       clock_wire_request_name(operation.op)));
			if (operation.op == CLOCK_REQUEST_SNAPSHOT) {
				++capture->snapshots;
				reply(capture, &message, CLOCK_OK);
			} else {
				if (capture->has_request) {
					assert(message.serial ==
					       capture->request.serial);
					assert(!strcmp(message.name,
						       capture->request.name));
					assert(!strcmp(
						message.clock_data,
						capture->request.clock_data));
					++capture->retransmissions;
				} else {
					capture->has_request = true;
					capture->request = message;
					capture->operation = operation;
				}
			}
		} else if (message.type == FPLINUX_UI_SESSION_SOFTKEYS) {
			strcpy(capture->left, message.left);
			strcpy(capture->centre, message.centre);
			strcpy(capture->right, message.right);
		} else if (message.type == FPLINUX_UI_SESSION_REDRAW_ACK) {
			capture->redraw_ack = message.serial;
		} else if (message.type == FPLINUX_UI_SESSION_ACTIVATION_ACK) {
			capture->activation_ack = message.serial;
		} else if (message.type == FPLINUX_UI_SESSION_CLOSE_REPLY) {
			assert(message.serial == capture->close_serial);
			assert(strlen(message.value) <
			       sizeof(capture->close_value));
			strcpy(capture->close_value, message.value);
		}
	}
	assert(received == 0);
}

static void pump_for(struct capture *capture, unsigned int milliseconds)
{
	uint64_t deadline = fplinux_ui_frame_now_ms() + milliseconds;

	while (fplinux_ui_frame_now_ms() < deadline)
		pump(capture);
}

static bool has_label(const struct capture *capture, const char *text)
{
	char line[1024];

	assert(snprintf(line, sizeof(line), "\n%s\n", text) <
	       (int)sizeof(line));
	return strstr(capture->observed.labels, line) != NULL;
}

static void wait_label(struct capture *capture, const char *text)
{
	uint64_t deadline = fplinux_ui_frame_now_ms() + 3000;

	while (!has_label(capture, text) &&
	       fplinux_ui_frame_now_ms() < deadline)
		pump(capture);
	if (!has_label(capture, text))
		fprintf(stderr, "Clock: missing label [%s], actual:%s\n", text,
			capture->observed.labels);
	assert(has_label(capture, text));
}

static void wait_label_absent(struct capture *capture, const char *text)
{
	uint64_t deadline = fplinux_ui_frame_now_ms() + 3000;

	while (has_label(capture, text) && fplinux_ui_frame_now_ms() < deadline)
		pump(capture);
	assert(!has_label(capture, text));
}

static void wait_legends(struct capture *capture, const char *left,
			 const char *centre, const char *right)
{
	uint64_t deadline = fplinux_ui_frame_now_ms() + 3000;

	while ((strcmp(capture->left, left) ||
		strcmp(capture->centre, centre) ||
		strcmp(capture->right, right)) &&
	       fplinux_ui_frame_now_ms() < deadline)
		pump(capture);
	if (strcmp(capture->left, left) || strcmp(capture->centre, centre) ||
	    strcmp(capture->right, right))
		fprintf(stderr,
			"Clock: legends [%s|%s|%s], expected [%s|%s|%s]\n",
			capture->left, capture->centre, capture->right, left,
			centre, right);
	assert(!strcmp(capture->left, left) &&
	       !strcmp(capture->centre, centre) &&
	       !strcmp(capture->right, right));
}

static void redraw(struct capture *capture)
{
	struct fplinux_ui_session_message setting = {
		.type = FPLINUX_UI_SESSION_SETTING,
		.serial = ++capture->redraw_serial,
	};
	uint64_t deadline = fplinux_ui_frame_now_ms() + 3000;

	strcpy(setting.name, "theme");
	strcpy(setting.value, "dark");
	assert(fplinux_ui_session_send(capture->session, &setting));
	while (capture->redraw_ack != setting.serial &&
	       fplinux_ui_frame_now_ms() < deadline)
		pump(capture);
	assert(capture->redraw_ack == setting.serial);
}

static void press(struct capture *capture, unsigned int seat, unsigned int key)
{
	struct fplinux_ui_frame_keyboard *keyboard =
		&capture->peer.keyboards[seat];

	fplinux_ui_frame_send_key(keyboard, key, true);
	fplinux_ui_frame_send_key(keyboard, key, false);
	pump(capture);
}

static void activate(struct capture *capture,
		     enum fplinux_ui_session_activation activation)
{
	const struct fplinux_ui_session_message message = {
		.type = FPLINUX_UI_SESSION_ACTIVATION,
		.serial = ++capture->activation_serial,
		.activation = activation,
	};
	uint64_t deadline = fplinux_ui_frame_now_ms() + 3000;

	assert(fplinux_ui_session_send(capture->session, &message));
	while (capture->activation_ack != message.serial &&
	       fplinux_ui_frame_now_ms() < deadline)
		pump(capture);
	assert(capture->activation_ack == message.serial);
	redraw(capture);
}

static void expect_request(struct capture *capture, enum clock_request_op op)
{
	uint64_t deadline = fplinux_ui_frame_now_ms() + 3000;

	while (!capture->has_request && fplinux_ui_frame_now_ms() < deadline)
		pump(capture);
	if (!capture->has_request)
		fprintf(stderr,
			"Clock: missing request %s; legends [%s|%s|%s]; labels:%s\n",
			clock_wire_request_name(op), capture->left,
			capture->centre, capture->right,
			capture->observed.labels);
	assert(capture->has_request);
	if (capture->operation.op != op)
		fprintf(stderr, "Clock: request %s, expected %s\n",
			clock_wire_request_name(capture->operation.op),
			clock_wire_request_name(op));
	assert(capture->operation.op == op);
}

static struct clock_result apply_received_request(struct capture *capture)
{
	struct clock_result result;

	assert(capture->has_request);
	if (capture->storage_owner) {
		result = fplinux_session_clock_apply(
			capture->storage_owner, &capture->operation,
			capture->owner.wall_utc, capture->owner.monotonic_ms);
		capture->owner = capture->storage_owner->model;
		capture->storage = capture->storage_owner->storage;
	} else {
		result = (struct clock_result){
			.status = clock_wire_request_apply(&capture->owner,
							   &capture->operation),
			.storage = capture->storage,
		};
		result.applied = result.status == CLOCK_OK;
	}
	reply_result(capture, &capture->request, &result);
	capture->has_request = false;
	redraw(capture);
	return result;
}

static void apply_request(struct capture *capture)
{
	assert(apply_received_request(capture).status == CLOCK_OK);
}

static void wait_retransmissions(struct capture *capture, unsigned int count)
{
	uint64_t deadline = fplinux_ui_frame_now_ms() + 5000;

	while (capture->retransmissions < count &&
	       fplinux_ui_frame_now_ms() < deadline)
		pump(capture);
	assert(capture->retransmissions == count);
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
}

static void start(struct capture *capture, char **argv, const char *time)
{
	struct fplinux_ui_session_message config = {
		.type = FPLINUX_UI_SESSION_CONFIG,
		.mode = FPLINUX_UI_SESSION_INTEGRATED,
		.theme = FPLINUX_UI_SESSION_THEME_DARK,
		.screen_width = dimension(argv[1]),
		.screen_height = dimension(argv[2]),
	};
	const struct fplinux_ui_profile_spec *profile = fplinux_ui_profile_spec(
		fplinux_ui_profile_for_screen(config.screen_width,
					      config.screen_height));
	struct fplinux_ui_session_message hello;
	int wayland[2], observer[2];

	assert(profile);
	capture->listener = fplinux_ui_frame_listen_session(argv[5]);
	capture->directory = argv[6];
	capture->session_path = argv[5];
	capture->redraw_serial = 10000;
	capture->redraw_ack = 0;
	capture->observed.labels[0] = '\0';
	fplinux_ui_frame_socketpair(wayland);
	assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, observer) == 0);
	capture->application = fork();
	assert(capture->application >= 0);
	if (!capture->application) {
		char descriptor[32], state_directory[512];

		close(capture->listener);
		close(wayland[0]);
		close(observer[0]);
		fplinux_ui_frame_client_environment(wayland[1], argv[5]);
		snprintf(descriptor, sizeof(descriptor), "%d", observer[1]);
		assert(setenv("FPLINUX_CLOCKS_OBSERVER_FD", descriptor, 1) ==
		       0);
		assert(snprintf(state_directory, sizeof(state_directory),
				"%s/state",
				argv[6]) < (int)sizeof(state_directory));
		assert(setenv("XDG_STATE_HOME", state_directory, 1) == 0);
		assert(setenv("LC_ALL", "C", 1) == 0);
		assert(setenv("TZ", "UTC", 1) == 0);
		execl(argv[3], argv[3], (char *)NULL);
		_exit(127);
	}
	close(wayland[1]);
	close(observer[1]);
	capture->observer = observer[0];
	fplinux_ui_frame_init(&capture->peer, wayland[0], config.screen_width,
			      config.screen_height - profile->status_height -
				      profile->softkey_height,
			      argv[4]);
	capture->session = fplinux_ui_frame_accept_session(&capture->peer,
							   capture->listener);
	hello = fplinux_ui_frame_wait_message(&capture->peer, capture->session);
	assert(hello.type == FPLINUX_UI_SESSION_HELLO);
	assert(!strcmp(hello.app_id, "org.fplinux.clock"));
	strcpy(config.locale, "en");
	strcpy(config.input_locale, "en");
	assert(fplinux_ui_session_send(capture->session, &config));
	wait_label(capture, time);
	assert(capture->peer.keyboards[0].keyboard &&
	       capture->peer.keyboards[1].keyboard);
	fplinux_ui_frame_focus_keyboard(&capture->peer.keyboards[0]);
	fplinux_ui_frame_focus_keyboard(&capture->peer.keyboards[1]);
	redraw(capture);
}

static void close_and_reap(struct capture *capture)
{
	struct fplinux_ui_session_message message = {
		.type = FPLINUX_UI_SESSION_CLOSE,
	};
	uint64_t deadline = fplinux_ui_frame_now_ms() + 3000;
	pid_t finished;
	int status;

	strcpy(message.reason, "capture-complete");
	assert(fplinux_ui_session_send(capture->session, &message));
	do {
		fplinux_ui_frame_pump(&capture->peer);
		finished = waitpid(capture->application, &status, WNOHANG);
		assert(finished >= 0);
	} while (!finished && fplinux_ui_frame_now_ms() < deadline);
	assert(finished == capture->application);
	assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	close(capture->observer);
	close(capture->session);
	close(capture->listener);
	fplinux_ui_frame_cleanup(&capture->peer);
	assert(unlink(capture->session_path) == 0);
}

static void repeat_key(struct capture *capture, unsigned int key, size_t count)
{
	for (size_t index = 0; index < count; ++index)
		press(capture, 1, key);
}

static void focus_form_item(struct capture *capture, size_t item)
{
	unsigned int direction;

	redraw(capture);
	direction = capture->observed.focused_item < item ? KEY_DOWN : KEY_UP;
	for (size_t attempt = 0;
	     capture->observed.focused_item != item && attempt < 20;
	     ++attempt) {
		press(capture, 1, direction);
		redraw(capture);
	}
	assert(capture->observed.focused_item == item);
}

static void save_label(struct capture *capture)
{
	const unsigned int letters[] = { KEY_W, KEY_A, KEY_K, KEY_E };

	press(capture, 1, KEY_OK);
	wait_label(capture, "Name");
	for (size_t index = 0; index < sizeof(letters) / sizeof(letters[0]);
	     ++index)
		press(capture, 0, letters[index]);
	wait_label(capture, "wake");
	wait_legends(capture, "Clear", "Done", "Cancel");
	press(capture, 1, KEY_OK);
	wait_label(capture, "New alarm");
}

static void expect_time_bounds(struct capture *capture, const char *expected)
{
	const struct clocks_time_observation *time = &capture->observed.time;

	assert(!strcmp(time->text, expected));
	assert(time->text_width > 0 && time->text_width <= time->width);
	assert(time->text_height > 0 && time->text_height <= time->height);
	assert(time->left >= 0 && time->top >= 0);
	assert(time->left + time->width <= time->parent_width);
	assert(time->top + time->height <= time->parent_height);
	printf("clocks-face %s: glyphs and %dx%d text fit %dx%d label\n",
	       expected, time->text_width, time->text_height, time->width,
	       time->height);
}

static void clock_face(struct capture *capture)
{
	press(capture, 1, KEY_F13);
	wait_label(capture, "Show clock");
	press(capture, 1, KEY_OK);
	wait_label(capture, "00:00:00");
	expect_time_bounds(capture, "00:00:00");
	save_frame(capture, "clocks-digital");
	press(capture, 1, KEY_F14);
	wait_legends(capture, "", "Select", "Back");
	press(capture, 1, KEY_OK);
	wait_label(capture, "00:00:00");
	press(capture, 1, KEY_F14);
	wait_legends(capture, "", "Select", "Back");
	press(capture, 1, KEY_DOWN);
	press(capture, 1, KEY_OK);
	wait_label(capture, "Clock settings");
	press(capture, 1, KEY_OK);
	wait_label(capture, "Clock face");
	press(capture, 1, KEY_DOWN);
	press(capture, 1, KEY_F14);
	wait_label(capture, "Clock settings");
	assert(!capture->has_request);
	press(capture, 1, KEY_OK);
	wait_label(capture, "Clock face");
	press(capture, 1, KEY_DOWN);
	press(capture, 1, KEY_OK);
	expect_request(capture, CLOCK_REQUEST_SET_VIEW);
	assert(capture->operation.view == CLOCK_VIEW_ANALOG);
	apply_request(capture);
	wait_label(capture, "Clock settings");
	press(capture, 1, KEY_F14);
	wait_legends(capture, "", "Select", "Back");
	press(capture, 1, KEY_UP);
	press(capture, 1, KEY_OK);
	wait_label(capture, "00:00");
	save_frame(capture, "clocks-analog");
	press(capture, 1, KEY_F14);
	wait_legends(capture, "", "Select", "Back");
	press(capture, 1, KEY_F14);
	wait_label(capture, "Clock");
}

static void alarm_editor(struct capture *capture)
{
	struct fplinux_ui_session_message mismatched, previous;

	press(capture, 1, KEY_OK);
	wait_label(capture, "New alarm");
	press(capture, 1, KEY_OK);
	wait_label(capture, "New alarm");
	redraw(capture);
	assert(capture->observed.time_fields == 1);
	assert(capture->observed.date_fields == 1);
	assert(capture->observed.duration_fields == 0);
	wait_legends(capture, "Save", "Edit", "Cancel");
	save_frame(capture, "clocks-alarm-edit");
	press(capture, 1, KEY_OK);
	repeat_key(capture, KEY_NUMERIC_0, 2);
	repeat_key(capture, KEY_NUMERIC_0, 2);
	press(capture, 1, KEY_OK);
	focus_form_item(capture, 2);
	press(capture, 1, KEY_OK);
	press(capture, 1, KEY_RIGHT);
	press(capture, 1, KEY_NUMERIC_0);
	press(capture, 1, KEY_NUMERIC_1);
	press(capture, 1, KEY_OK);
	press(capture, 1, KEY_F13);
	wait_label(capture,
		   "This time has passed. Choose another date or time.");
	assert(!capture->has_request);
	press(capture, 1, KEY_F14);
	wait_label(capture, "New alarm");
	focus_form_item(capture, 0);
	press(capture, 1, KEY_OK);
	press(capture, 1, KEY_NUMERIC_0);
	press(capture, 1, KEY_NUMERIC_8);
	press(capture, 1, KEY_NUMERIC_4);
	press(capture, 1, KEY_NUMERIC_5);
	press(capture, 1, KEY_OK);
	repeat_key(capture, KEY_DOWN, 2);
	press(capture, 1, KEY_OK);
	press(capture, 1, KEY_NUMERIC_0);
	press(capture, 1, KEY_NUMERIC_2);
	press(capture, 1, KEY_NUMERIC_2);
	press(capture, 1, KEY_NUMERIC_9);
	press(capture, 1, KEY_NUMERIC_2);
	press(capture, 1, KEY_NUMERIC_0);
	press(capture, 1, KEY_NUMERIC_2);
	press(capture, 1, KEY_NUMERIC_4);
	press(capture, 1, KEY_OK);
	focus_form_item(capture, 4);
	save_label(capture);
	wait_legends(capture, "Save", "Edit", "Cancel");
	press(capture, 1, KEY_F13);
	expect_request(capture, CLOCK_REQUEST_ALARM_ADD);
	assert(capture->operation.alarm.schedule == CLOCK_ALARM_ONCE);
	assert(capture->operation.alarm.once_utc == INT64_C(1709196300));
	assert(!capture->operation.alarm.hour &&
	       !capture->operation.alarm.minute);
	assert(capture->operation.alarm.enabled);
	assert(capture->operation.alarm.repeat_days == 0);
	assert(!strcmp(capture->operation.alarm.label, "wake"));
	assert(capture->owner.alarm_count == 0);
	previous = capture->request;
	mismatched = capture->request;
	++mismatched.serial;
	reply(capture, &mismatched, CLOCK_OK);
	mismatched = capture->request;
	strcpy(mismatched.name, "timer-start");
	reply(capture, &mismatched, CLOCK_OK);
	redraw(capture);
	assert(has_label(capture, "New alarm"));
	save_frame(capture, "clocks-pending");
	press(capture, 1, KEY_F13);
	redraw(capture);
	assert(capture->owner.alarm_count == 0);
	reply(capture, &capture->request, CLOCK_STORAGE_ERROR);
	capture->has_request = false;
	wait_label(capture, "Change not saved");
	wait_label(capture, "Storage input/output error");
	save_frame(capture, "clocks-error");
	press(capture, 1, KEY_F14);
	wait_label(capture, "New alarm");
	press(capture, 1, KEY_F13);
	expect_request(capture, CLOCK_REQUEST_ALARM_ADD);
	assert(capture->operation.alarm.once_utc == INT64_C(1709196300));
	assert(!strcmp(capture->operation.alarm.label, "wake"));
	reply(capture, &previous, CLOCK_OK);
	redraw(capture);
	assert(has_label(capture, "New alarm"));
	assert(capture->retransmissions == 0);
	wait_retransmissions(capture, 1);
	pump_for(capture, 3500);
	wait_label(capture, "Could not confirm the change. Retry?");
	save_frame(capture, "clocks-retry");
	pump_for(capture, 3500);
	assert(capture->retransmissions == 1);
	assert(capture->owner.alarm_count == 0);
	press(capture, 1, KEY_F13);
	wait_retransmissions(capture, 2);
	focus_form_item(capture, 0);
	press(capture, 1, KEY_OK);
	press(capture, 1, KEY_UP);
	redraw(capture);
	assert(capture->observed.editing);
	apply_request(capture);
	assert(capture->owner.alarm_count == 1);
	wait_label(capture, "Edit alarm");
	assert(capture->observed.editing);
	press(capture, 1, KEY_F14);
	redraw(capture);
	assert(!capture->observed.editing);
	press(capture, 1, KEY_F13);
	expect_request(capture, CLOCK_REQUEST_ALARM_EDIT);
	assert(capture->operation.id == capture->owner.alarms[0].id);
	assert(capture->operation.alarm.once_utc == INT64_C(1709196300));
	apply_request(capture);
	assert(capture->owner.alarm_count == 1);
	wait_label(capture, "08:45  wake");
	save_frame(capture, "clocks-alarms");
	puts("clocks-alarm: one TIME and DATE; 2024-02-29 08:45 UTC; visible Save; stale replies ignored; storage retry retains draft");
	puts("clocks-retry: one automatic retry; bounded silence; explicit retry retains serial and payload; one alarm");
	puts("clocks-late-ack: inline edit survives ADD acknowledgment; Cancel then EDIT keeps one alarm");
}

static void timer_actions(struct capture *capture)
{
	unsigned int snapshots;

	press(capture, 1, KEY_F14);
	press(capture, 1, KEY_DOWN);
	press(capture, 1, KEY_OK);
	wait_label(capture, "Timer");
	wait_legends(capture, "Edit", "Start", "Back");
	press(capture, 1, KEY_F13);
	redraw(capture);
	assert(capture->observed.duration_fields == 1);
	assert(capture->observed.time_fields == 0);
	assert(capture->observed.date_fields == 0);
	press(capture, 1, KEY_F13);
	wait_label(capture, "Enter a duration greater than zero.");
	assert(!capture->has_request);
	snapshots = capture->snapshots;
	pump_for(capture, 1200);
	redraw(capture);
	assert(capture->snapshots > snapshots);
	assert(has_label(capture, "Enter a duration greater than zero."));
	press(capture, 1, KEY_OK);
	press(capture, 1, KEY_NUMERIC_0);
	press(capture, 1, KEY_NUMERIC_0);
	press(capture, 1, KEY_NUMERIC_0);
	press(capture, 1, KEY_NUMERIC_1);
	press(capture, 1, KEY_NUMERIC_3);
	press(capture, 1, KEY_NUMERIC_0);
	press(capture, 1, KEY_OK);
	redraw(capture);
	assert(capture->observed.duration_seconds == 90);
	save_frame(capture, "clocks-duration");
	press(capture, 1, KEY_F13);
	expect_request(capture, CLOCK_REQUEST_TIMER_SET);
	assert(capture->operation.seconds == 90);
	apply_request(capture);
	wait_label(capture, "00:01:30");
	wait_legends(capture, "Edit", "Start", "Back");
	expect_time_bounds(capture, "00:01:30");
	press(capture, 1, KEY_OK);
	expect_request(capture, CLOCK_REQUEST_TIMER_START);
	apply_request(capture);
	assert(capture->owner.timer.state == CLOCK_TIMER_RUNNING);
	assert(capture->owner.timer.deadline_ms == 91000);
	wait_legends(capture, "Stop", "Pause", "Back");
	assert(clock_model_tick(&capture->owner, INT64_C(1704067210), 11500) ==
	       CLOCK_OK);
	press(capture, 1, KEY_OK);
	expect_request(capture, CLOCK_REQUEST_TIMER_PAUSE);
	apply_request(capture);
	assert(capture->owner.timer.remaining_ms == 79500);
	wait_label(capture, "Paused");
	wait_label(capture, "00:01:19");
	wait_legends(capture, "Options", "Resume", "Back");
	save_frame(capture, "clocks-timer");
	press(capture, 1, KEY_OK);
	expect_request(capture, CLOCK_REQUEST_TIMER_RESUME);
	apply_request(capture);
	assert(capture->owner.timer.deadline_ms == 91000);
	press(capture, 1, KEY_F13);
	expect_request(capture, CLOCK_REQUEST_TIMER_STOP);
	apply_request(capture);
	wait_label(capture, "00:01:30");
	press(capture, 1, KEY_OK);
	expect_request(capture, CLOCK_REQUEST_TIMER_START);
	apply_request(capture);
	assert(clock_model_tick(&capture->owner, INT64_C(1704067221), 22000) ==
	       CLOCK_OK);
	press(capture, 1, KEY_OK);
	expect_request(capture, CLOCK_REQUEST_TIMER_PAUSE);
	apply_request(capture);
	assert(capture->owner.timer.remaining_ms == 79500);
	puts("clocks-timer: one duration; 90s; pause, resume and stop");
}

static void enter_duration(struct capture *capture,
			   const unsigned int digits[6])
{
	press(capture, 1, KEY_OK);
	for (size_t index = 0; index < 6; ++index)
		press(capture, 1, digits[index]);
	press(capture, 1, KEY_OK);
	redraw(capture);
	assert(!capture->observed.editing);
}

static void timer_save_acknowledgment(struct capture *capture, bool changed)
{
	const unsigned int ninety[] = { KEY_NUMERIC_0, KEY_NUMERIC_0,
					KEY_NUMERIC_0, KEY_NUMERIC_1,
					KEY_NUMERIC_3, KEY_NUMERIC_0 };
	const unsigned int one_twenty[] = { KEY_NUMERIC_0, KEY_NUMERIC_0,
					    KEY_NUMERIC_0, KEY_NUMERIC_2,
					    KEY_NUMERIC_0, KEY_NUMERIC_0 };

	press(capture, 1, KEY_DOWN);
	press(capture, 1, KEY_OK);
	wait_label(capture, "Timer");
	press(capture, 1, KEY_F13);
	wait_label(capture, "Timer duration");
	enter_duration(capture, ninety);
	assert(capture->observed.duration_seconds == 90);
	press(capture, 1, KEY_F13);
	expect_request(capture, CLOCK_REQUEST_TIMER_SET);
	assert(capture->operation.seconds == 90);
	if (changed) {
		enter_duration(capture, one_twenty);
		assert(capture->observed.duration_seconds == 120);
	}
	apply_request(capture);
	assert(capture->owner.timer.duration_seconds == 90);
	if (changed) {
		wait_label(capture, "Timer duration");
		assert(capture->observed.duration_fields == 1);
		assert(capture->observed.duration_seconds == 120);
		assert(!capture->observed.editing);
		press(capture, 1, KEY_F13);
		expect_request(capture, CLOCK_REQUEST_TIMER_SET);
		assert(capture->operation.seconds == 120);
		apply_request(capture);
		assert(capture->owner.timer.duration_seconds == 120);
		wait_label(capture, "00:02:00");
		puts("clocks-timer-late-ack: accepted 120s draft survives 90s ACK; second Save stores 120s");
	} else {
		wait_label(capture, "00:01:30");
		puts("clocks-timer-unchanged-ack: unchanged 90s draft closes on Save ACK");
	}
	wait_label_absent(capture, "Timer duration");
	wait_legends(capture, "Edit", "Start", "Back");
}

static void activation_preserves_running_clocks(struct capture *capture)
{
	pid_t application = capture->application;
	uint64_t timer_deadline = capture->owner.timer.deadline_ms;
	uint64_t stopwatch_elapsed = capture->owner.stopwatch.elapsed_ms;
	size_t laps = capture->owner.stopwatch.lap_count;

	press(capture, 1, KEY_DOWN);
	press(capture, 1, KEY_OK);
	wait_label(capture, "00:01:29");
	activate(capture, FPLINUX_UI_SESSION_ACTIVATION_RESUME);
	wait_label(capture, "00:01:29");
	activate(capture, FPLINUX_UI_SESSION_ACTIVATION_OPEN);
	wait_label(capture, "Alarms");
	wait_label(capture, "World clock");
	press(capture, 1, KEY_F13);
	wait_label(capture, "Show clock");
	activate(capture, FPLINUX_UI_SESSION_ACTIVATION_RESUME);
	wait_label(capture, "Show clock");
	activate(capture, FPLINUX_UI_SESSION_ACTIVATION_OPEN);
	wait_label_absent(capture, "Show clock");
	assert(!capture->has_request);
	assert(capture->application == application);
	assert(capture->owner.timer.state == CLOCK_TIMER_RUNNING);
	assert(capture->owner.timer.deadline_ms == timer_deadline);
	assert(capture->owner.stopwatch.running);
	assert(capture->owner.stopwatch.elapsed_ms == stopwatch_elapsed);
	assert(capture->owner.stopwatch.lap_count == laps && laps == 1);
	assert(clock_model_tick(&capture->owner, 1704067202, 3000) == CLOCK_OK);
	/* The home keeps Timer focused, so the same row opens its ticking state. */
	press(capture, 1, KEY_OK);
	wait_label(capture, "00:01:28");
	assert(capture->owner.timer.deadline_ms == timer_deadline);
	puts("clocks-activation-navigation: OPEN returns home; RESUME keeps the timer; running clocks and laps survive");
}

static void activation_protects_alarm_draft(struct capture *capture)
{
	uint64_t focused;

	press(capture, 1, KEY_OK);
	press(capture, 1, KEY_OK);
	wait_label(capture, "New alarm");
	focus_form_item(capture, 0);
	press(capture, 1, KEY_OK);
	press(capture, 1, KEY_NUMERIC_0);
	redraw(capture);
	assert(capture->observed.editing);
	activate(capture, FPLINUX_UI_SESSION_ACTIVATION_OPEN);
	wait_label(capture, "Return to the start without saving?");
	press(capture, 1, KEY_OK);
	wait_label_absent(capture, "Return to the start without saving?");
	assert(capture->observed.editing &&
	       capture->observed.focused_item == 0);
	press(capture, 1, KEY_NUMERIC_8);
	press(capture, 1, KEY_NUMERIC_0);
	press(capture, 1, KEY_NUMERIC_0);
	press(capture, 1, KEY_OK);
	redraw(capture);
	focused = capture->observed.focused_item;
	activate(capture, FPLINUX_UI_SESSION_ACTIVATION_RESUME);
	assert(capture->observed.time_value == INT64_C(1704096000));
	assert(capture->observed.focused_item == focused);
	activate(capture, FPLINUX_UI_SESSION_ACTIVATION_OPEN);
	wait_label(capture, "Return to the start without saving?");
	activate(capture, FPLINUX_UI_SESSION_ACTIVATION_OPEN);
	activate(capture, FPLINUX_UI_SESSION_ACTIVATION_RESUME);
	press(capture, 1, KEY_OK);
	wait_label(capture, "New alarm");
	wait_label_absent(capture, "Return to the start without saving?");
	assert(capture->observed.time_value == INT64_C(1704096000));
	assert(capture->observed.focused_item == focused);
	activate(capture, FPLINUX_UI_SESSION_ACTIVATION_OPEN);
	press(capture, 1, KEY_F13);
	wait_label(capture, "World clock");
	assert(capture->owner.alarm_count == 0 && !capture->has_request);
	puts("clocks-activation-draft: repeated OPEN keeps one Leave/Stay decision; Stay retains the draft and Leave opens home");
}

static void activation_waits_for_alarm_reply(struct capture *capture,
					     bool failed, bool changed)
{
	struct fplinux_ui_session_message request;

	press(capture, 1, KEY_OK);
	press(capture, 1, KEY_OK);
	wait_label(capture, "New alarm");
	enter_alarm_hour(capture, KEY_NUMERIC_8);
	press(capture, 1, KEY_F13);
	expect_request(capture, CLOCK_REQUEST_ALARM_ADD);
	request = capture->request;
	activate(capture, FPLINUX_UI_SESSION_ACTIVATION_OPEN);
	wait_legends(capture, "Saving…", "Edit", "Cancel");
	activate(capture, FPLINUX_UI_SESSION_ACTIVATION_OPEN);
	assert(capture->request.serial == request.serial);
	assert(capture->retransmissions == 0);
	assert(capture->observed.time_value == INT64_C(1704096000));
	if (changed)
		enter_alarm_hour(capture, KEY_NUMERIC_9);
	if (failed) {
		reply(capture, &capture->request, CLOCK_STORAGE_ERROR);
		capture->has_request = false;
		wait_label(capture, "Change not saved");
		activate(capture, FPLINUX_UI_SESSION_ACTIVATION_OPEN);
		wait_label(capture, "Change not saved");
		press(capture, 1, KEY_F14);
		wait_legends(capture, "Retry", "Edit", "Cancel");
		redraw(capture);
		wait_label(capture, "New alarm");
		assert(capture->observed.time_fields == 1);
		assert(capture->observed.time_value == INT64_C(1704096000));
		assert(capture->owner.alarm_count == 0 &&
		       !capture->has_request);
		puts("clocks-activation-pending-failed: OPEN keeps the request and failed draft; the error remains until dismissed");
	} else {
		apply_request(capture);
		assert(capture->owner.alarm_count == 1);
		assert(capture->owner.alarms[0].spec.once_utc ==
		       INT64_C(1704096000));
		assert(!capture->has_request);
		if (changed) {
			wait_label(capture,
				   "Return to the start without saving?");
			press(capture, 1, KEY_OK);
			wait_legends(capture, "Save", "Edit", "Cancel");
			redraw(capture);
			wait_label(capture, "Edit alarm");
			assert(capture->observed.time_fields == 1);
			assert(capture->observed.time_value ==
			       INT64_C(1704099600));
			puts("clocks-activation-pending-changed: successful older mutation keeps the newer draft behind Leave/Stay");
		} else {
			wait_label(capture, "World clock");
			puts("clocks-activation-pending-success: OPEN defers home until one alarm mutation succeeds");
		}
	}
}

static void
paused_timer_edit_preserves_or_replaces_countdown(struct capture *capture)
{
	const unsigned int nine_minutes[] = { KEY_NUMERIC_0, KEY_NUMERIC_0,
					      KEY_NUMERIC_0, KEY_NUMERIC_9,
					      KEY_NUMERIC_0, KEY_NUMERIC_0 };

	press(capture, 1, KEY_DOWN);
	press(capture, 1, KEY_OK);
	wait_label(capture, "00:07:00");
	for (unsigned int cancel = 0; cancel < 2; ++cancel) {
		press(capture, 1, KEY_F13);
		wait_label(capture, "Edit");
		press(capture, 1, KEY_OK);
		wait_label(capture, "Timer duration");
		press(capture, 1, cancel ? KEY_F14 : KEY_F13);
		wait_label(capture, "00:07:00");
		assert(!capture->has_request);
		assert(capture->owner.timer.state == CLOCK_TIMER_PAUSED);
		assert(capture->owner.timer.remaining_ms == 420123);
	}
	press(capture, 1, KEY_F13);
	press(capture, 1, KEY_OK);
	wait_label(capture, "Timer duration");
	enter_duration(capture, nine_minutes);
	press(capture, 1, KEY_F13);
	wait_label(capture,
		   "Replace the current countdown with this duration?");
	assert(!capture->has_request &&
	       capture->owner.timer.remaining_ms == 420123);
	press(capture, 1, KEY_F14);
	wait_label(capture, "Timer duration");
	press(capture, 1, KEY_F13);
	wait_label(capture,
		   "Replace the current countdown with this duration?");
	press(capture, 1, KEY_F13);
	expect_request(capture, CLOCK_REQUEST_TIMER_SET);
	assert(capture->operation.seconds == 540);
	apply_request(capture);
	wait_label(capture, "00:09:00");
	assert(capture->owner.timer.state == CLOCK_TIMER_STOPPED);
	assert(capture->owner.timer.remaining_ms == 540000);
	puts("clocks-paused-edit: unchanged Done and Cancel retain PAUSED 420123ms; replacement requires confirmation");
}

static void snoozed_alarm_and_disabled_draft(struct capture *capture,
					     bool weekly)
{
	press(capture, 1, KEY_OK);
	wait_label(capture, "Alarms");
	wait_label(capture, "Snoozed until 00:06");
	wait_label(capture, "00:05:00 left");
	press(capture, 1, KEY_DOWN);
	press(capture, 1, KEY_F13);
	wait_label(capture, "Cancel snooze");
	press(capture, 1, KEY_OK);
	expect_request(capture, CLOCK_REQUEST_ALARM_CANCEL_SNOOZE);
	assert(capture->operation.id == 1);
	apply_request(capture);
	wait_label_absent(capture, "Snoozed until 00:06");
	assert(!capture->owner.alarms[0].snoozed);
	assert(capture->owner.alarms[0].spec.enabled == weekly);
	if (!weekly) {
		press(capture, 1, KEY_OK);
		wait_label(capture, "Edit alarm");
		focus_form_item(capture, 6);
		wait_label(capture, "Alarm state");
		wait_label(capture, "Disabled");
		wait_label(capture, "Off. This alarm will not ring.");
		press(capture, 1, KEY_OK);
		press(capture, 1, KEY_DOWN);
		press(capture, 1, KEY_OK);
		press(capture, 1, KEY_F14);
		wait_label(capture, "Leave without saving?");
		press(capture, 1, KEY_F13);
		assert(!capture->owner.alarms[0].spec.enabled &&
		       !capture->has_request);
		press(capture, 1, KEY_OK);
		wait_label(capture, "Edit alarm");
		focus_form_item(capture, 0);
		press(capture, 1, KEY_OK);
		press(capture, 1, KEY_RIGHT);
		press(capture, 1, KEY_DOWN);
		press(capture, 1, KEY_OK);
		press(capture, 1, KEY_F13);
		expect_request(capture, CLOCK_REQUEST_ALARM_EDIT);
		assert(!capture->operation.alarm.enabled);
		assert(capture->operation.alarm.once_utc == 1704067200);
		apply_request(capture);
		assert(!capture->owner.alarms[0].spec.enabled);
	}
	puts(weekly ?
		     "clocks-snooze-weekly: explicit cancellation retains next regular occurrence" :
		     "clocks-snooze-once: pending reminder is visible; cancellation and disabled edits preserve Off");
}

static void prepare_missed_events(struct capture *capture)
{
	const struct clock_alarm_spec alarm = {
		.label = "Morning tea",
		.schedule = CLOCK_ALARM_ONCE,
		.once_utc = INT64_C(1704067320),
		.enabled = true,
	};
	uint32_t id;

	assert(clock_model_timer_set(&capture->owner, 1) == CLOCK_OK);
	assert(clock_model_timer_start(&capture->owner) == CLOCK_OK);
	assert(clock_model_tick(&capture->owner, INT64_C(1704067201), 2000) ==
	       CLOCK_OK);
	assert(capture->owner.active.kind == CLOCK_EVENT_TIMER);
	assert(clock_model_tick(&capture->owner, INT64_C(1704067261), 62000) ==
	       CLOCK_OK);
	assert(capture->owner.missed_count == 1);
	assert(capture->owner.missed[0].missed_reason == CLOCK_MISSED_EXPIRED);
	assert(clock_model_alarm_add(&capture->owner, &alarm, &id) == CLOCK_OK);
	assert(clock_model_tick(&capture->owner, INT64_C(1704067320), 121000) ==
	       CLOCK_OK);
	assert(capture->owner.active.kind == CLOCK_EVENT_ALARM);
	assert(capture->owner.active.source_id == id);
	assert(clock_model_tick(&capture->owner, INT64_C(1704067380), 181000) ==
	       CLOCK_OK);
	assert(capture->owner.missed_count == 2);
	assert(capture->owner.missed[1].missed_reason == CLOCK_MISSED_EXPIRED);
}

static void prepare_confirmed_alarm(struct capture *capture)
{
	const struct clock_alarm_spec alarm = {
		.label = "Wake",
		.schedule = CLOCK_ALARM_WEEKLY,
		.hour = 7,
		.repeat_days = 127,
		.enabled = true,
	};
	uint32_t id;

	assert(clock_model_alarm_add(&capture->owner, &alarm, &id) == CLOCK_OK);
	assert(id == 1);
}

static void enter_alarm_hour(struct capture *capture, unsigned int digit)
{
	focus_form_item(capture, 0);
	press(capture, 1, KEY_OK);
	press(capture, 1, KEY_NUMERIC_0);
	press(capture, 1, digit);
	press(capture, 1, KEY_NUMERIC_0);
	press(capture, 1, KEY_NUMERIC_0);
	press(capture, 1, KEY_OK);
	redraw(capture);
	assert(!capture->observed.editing);
}

static void published_add_retries_accepted_identity(struct capture *capture)
{
	const struct clock_result published = {
		.status = CLOCK_STORAGE_ERROR,
		.applied = true,
		.storage = {
			.state = FPLINUX_STORAGE_PUBLISHED_UNSYNCED,
			.error = EIO,
		},
	};
	uint32_t serial;

	press(capture, 1, KEY_OK);
	press(capture, 1, KEY_OK);
	wait_label(capture, "New alarm");
	press(capture, 1, KEY_F13);
	expect_request(capture, CLOCK_REQUEST_ALARM_ADD);
	serial = capture->request.serial;
	assert(clock_wire_request_apply(&capture->owner, &capture->operation) ==
	       CLOCK_OK);
	assert(capture->owner.alarm_count == 1 &&
	       capture->owner.alarms[0].id == 1);
	capture->storage = published.storage;
	reply_result(capture, &capture->request, &published);
	capture->has_request = false;
	wait_label(capture, "Saving not confirmed");
	wait_label(capture, "Storage input/output error");
	press(capture, 1, KEY_F14);
	wait_label(capture, "Edit alarm");
	redraw(capture);
	assert(capture->observed.time_fields == 1 &&
	       capture->observed.time_value == INT64_C(1704092400));
	wait_legends(capture, "Retry", "Edit", "Cancel");
	press(capture, 1, KEY_F13);
	expect_request(capture, CLOCK_REQUEST_SAVE);
	assert(capture->request.serial != serial);
	assert(capture->owner.alarm_count == 1 &&
	       capture->owner.next_alarm_id == 2);
	enter_alarm_hour(capture, KEY_NUMERIC_8);
	capture->storage = (struct fplinux_storage_result){
		.state = FPLINUX_STORAGE_DURABLE
	};
	apply_request(capture);
	wait_label(capture, "Edit alarm");
	assert(capture->observed.time_value == INT64_C(1704096000));
	assert(capture->owner.alarms[0].spec.once_utc == INT64_C(1704092400));
	press(capture, 1, KEY_F13);
	expect_request(capture, CLOCK_REQUEST_ALARM_EDIT);
	assert(capture->operation.id == 1);
	apply_request(capture);
	assert(capture->owner.alarm_count == 1 &&
	       capture->owner.alarms[0].spec.once_utc == INT64_C(1704096000));
	puts("clocks-published-retry: accepted ADD exposes its ID before synchronization; Retry saves its checkpoint");
}

static void deliver_background_durable_snapshot(struct capture *capture)
{
	unsigned int snapshots = capture->snapshots;
	uint64_t deadline = fplinux_ui_frame_now_ms() + 3000;

	capture->storage = (struct fplinux_storage_result){
		.state = FPLINUX_STORAGE_DURABLE,
	};
	while (capture->snapshots == snapshots &&
	       fplinux_ui_frame_now_ms() < deadline)
		pump(capture);
	assert(capture->snapshots > snapshots);
	redraw(capture);
}

static void
background_durable_retains_accepted_revision(struct capture *capture)
{
	const struct clock_result published = {
		.status = CLOCK_STORAGE_ERROR,
		.applied = true,
		.storage = {
			.state = FPLINUX_STORAGE_PUBLISHED_UNSYNCED,
			.error = EIO,
		},
	};
	struct clock_model before;

	press(capture, 1, KEY_OK);
	press(capture, 1, KEY_OK);
	wait_label(capture, "New alarm");
	press(capture, 1, KEY_F13);
	expect_request(capture, CLOCK_REQUEST_ALARM_ADD);
	assert(clock_wire_request_apply(&capture->owner, &capture->operation) ==
	       CLOCK_OK);
	assert(capture->owner.alarm_count == 1 &&
	       capture->owner.alarms[0].id == 1 &&
	       capture->owner.alarms[0].generation == 1);
	capture->storage = published.storage;
	reply_result(capture, &capture->request, &published);
	capture->has_request = false;
	wait_label(capture, "Saving not confirmed");
	press(capture, 1, KEY_F14);
	wait_label(capture, "Edit alarm");
	deliver_background_durable_snapshot(capture);
	wait_legends(capture, "Save", "Edit", "Cancel");
	assert(capture->observed.time_value == INT64_C(1704092400));
	before = capture->owner;
	press(capture, 1, KEY_F13);
	expect_request(capture, CLOCK_REQUEST_SAVE);
	apply_request(capture);
	assert(!memcmp(&capture->owner, &before, sizeof(before)));
	wait_label_absent(capture, "Edit alarm");
	wait_label(capture, "07:00  Alarm");
	press(capture, 1, KEY_OK);
	wait_label(capture, "Edit alarm");
	enter_alarm_hour(capture, KEY_NUMERIC_8);
	press(capture, 1, KEY_F13);
	expect_request(capture, CLOCK_REQUEST_ALARM_EDIT);
	assert(capture->operation.id == 1 &&
	       capture->operation.alarm.once_utc == INT64_C(1704096000));
	enter_alarm_hour(capture, KEY_NUMERIC_9);
	assert(clock_wire_request_apply(&capture->owner, &capture->operation) ==
	       CLOCK_OK);
	capture->storage = published.storage;
	reply_result(capture, &capture->request, &published);
	capture->has_request = false;
	wait_label(capture, "Saving not confirmed");
	press(capture, 1, KEY_F14);
	wait_label(capture, "Edit alarm");
	deliver_background_durable_snapshot(capture);
	assert(capture->observed.time_value == INT64_C(1704099600));
	press(capture, 1, KEY_F14);
	wait_label(capture, "Leave without saving?");
	press(capture, 1, KEY_OK);
	wait_label(capture, "Edit alarm");
	press(capture, 1, KEY_F13);
	expect_request(capture, CLOCK_REQUEST_ALARM_EDIT);
	assert(capture->operation.id == 1 &&
	       capture->operation.alarm.once_utc == INT64_C(1704099600));
	apply_request(capture);
	assert(capture->owner.alarm_count == 1 &&
	       capture->owner.alarms[0].id == 1 &&
	       capture->owner.alarms[0].generation == 3);
	puts("clocks-background-durable: unchanged accepted draft saves without mutation; newer draft stays dirty and edits the same ID");
}

static void wait_close_response(struct capture *capture, const char *value)
{
	uint64_t deadline = fplinux_ui_frame_now_ms() + 3000;

	while (strcmp(capture->close_value, value) &&
	       fplinux_ui_frame_now_ms() < deadline)
		pump(capture);
	assert(!strcmp(capture->close_value, value));
}

static void prepare_ordinary_close(struct capture *capture)
{
	struct fplinux_ui_session_message message = {
		.type = FPLINUX_UI_SESSION_PREPARE_CLOSE,
		.serial = ++capture->close_serial,
	};

	capture->close_value[0] = '\0';
	strcpy(message.reason, "capture-close");
	assert(fplinux_ui_session_send(capture->session, &message));
	wait_close_response(capture, "wait");
	wait_label(capture, "Leave without saving?");
}

static void
ordinary_close_keeps_or_discards_a_failed_draft(struct capture *capture)
{
	int status;

	press(capture, 1, KEY_OK);
	press(capture, 1, KEY_OK);
	wait_label(capture, "New alarm");
	enter_alarm_hour(capture, KEY_NUMERIC_8);
	press(capture, 1, KEY_F13);
	expect_request(capture, CLOCK_REQUEST_ALARM_ADD);
	reply(capture, &capture->request, CLOCK_STORAGE_ERROR);
	capture->has_request = false;
	wait_label(capture, "Change not saved");
	wait_label(capture, "Storage input/output error");
	prepare_ordinary_close(capture);
	activate(capture, FPLINUX_UI_SESSION_ACTIVATION_OPEN);
	wait_label(capture, "Leave without saving?");
	press(capture, 1, KEY_OK);
	wait_close_response(capture, "cancel");
	wait_label(capture, "New alarm");
	redraw(capture);
	assert(capture->observed.time_fields == 1);
	if (capture->observed.time_value != INT64_C(1704096000))
		fprintf(stderr,
			"Clock retained draft time: %lld, expected 1704096000\n",
			(long long)capture->observed.time_value);
	assert(capture->observed.time_value == INT64_C(1704096000));
	assert(capture->owner.alarm_count == 0);
	press(capture, 1, KEY_F13);
	expect_request(capture, CLOCK_REQUEST_ALARM_ADD);
	assert(capture->operation.alarm.once_utc == INT64_C(1704096000));
	apply_request(capture);
	wait_label(capture, "08:00  Alarm");
	press(capture, 1, KEY_OK);
	wait_label(capture, "Edit alarm");
	enter_alarm_hour(capture, KEY_NUMERIC_9);
	prepare_ordinary_close(capture);
	press(capture, 1, KEY_F13);
	wait_close_response(capture, "ready");
	assert(waitpid(capture->application, &status, WNOHANG) == 0);
	assert(capture->owner.alarm_count == 1 &&
	       capture->owner.alarms[0].spec.once_utc == INT64_C(1704096000));
	puts("clocks-ordinary-close: failed draft survives Stay and Save retry; Leave permits final close and discards newer draft");
}

static void timer_controls_and_snapshots_survive_directory_sync_failure(
	struct capture *capture)
{
	struct clock_result result;
	unsigned int snapshots;
	uint64_t deadline;

	assert(capture->storage_owner);
	press(capture, 1, KEY_DOWN);
	press(capture, 1, KEY_OK);
	wait_label(capture, "Timer");
	press(capture, 1, KEY_OK);
	expect_request(capture, CLOCK_REQUEST_TIMER_START);
	apply_request(capture);
	wait_legends(capture, "Stop", "Pause", "Back");
	press(capture, 1, KEY_F14);
	press(capture, 1, KEY_F13);
	wait_label(capture, "Show clock");
	press(capture, 1, KEY_DOWN);
	press(capture, 1, KEY_OK);
	wait_label(capture, "Clock settings");
	press(capture, 1, KEY_OK);
	wait_label(capture, "Clock face");
	press(capture, 1, KEY_DOWN);
	press(capture, 1, KEY_OK);
	expect_request(capture, CLOCK_REQUEST_SET_VIEW);
	fail_directory_sync = true;
	result = apply_received_request(capture);
	assert(result.status == CLOCK_STORAGE_ERROR && result.applied);
	assert(result.storage.state == FPLINUX_STORAGE_PUBLISHED_UNSYNCED &&
	       result.storage.error == EIO && directory_sync_failures == 1);
	wait_label(capture, "Saving not confirmed");
	wait_label(capture, "Storage input/output error");
	press(capture, 1, KEY_F14);
	press(capture, 1, KEY_F14);
	press(capture, 1, KEY_F14);
	wait_legends(capture, "", "Select", "Back");
	press(capture, 1, KEY_F14);
	wait_label(capture, "Alarms");
	press(capture, 1, KEY_OK);
	wait_label(capture, "Timer");
	wait_legends(capture, "Stop", "Pause", "Back");
	result = fplinux_session_clock_tick(capture->storage_owner,
					    INT64_C(1704067201), 2000);
	assert(result.status == CLOCK_OK && result.applied);
	assert(result.storage.state == FPLINUX_STORAGE_PUBLISHED_UNSYNCED &&
	       directory_sync_failures == 2);
	capture->owner = capture->storage_owner->model;
	capture->storage = capture->storage_owner->storage;
	snapshots = capture->snapshots;
	deadline = fplinux_ui_frame_now_ms() + 3000;
	while (capture->snapshots == snapshots &&
	       fplinux_ui_frame_now_ms() < deadline)
		pump(capture);
	assert(capture->snapshots > snapshots);
	wait_label(capture, "00:01:29");
	press(capture, 1, KEY_F13);
	expect_request(capture, CLOCK_REQUEST_TIMER_STOP);
	result = apply_received_request(capture);
	assert(result.status == CLOCK_OK && result.applied);
	assert(result.storage.state == FPLINUX_STORAGE_PUBLISHED_UNSYNCED);
	assert(capture->owner.timer.state == CLOCK_TIMER_STOPPED);
	wait_legends(capture, "Edit", "Start", "Back");
	fail_directory_sync = false;
	assert(fplinux_session_clock_retry(capture->storage_owner, 2001)
		       .status == CLOCK_OK);
	puts("clocks-storage-controls: directory sync EIO keeps navigation, ticking snapshots and running timer Stop available");
}

static void saving_a_ringing_checkpoint_preserves_event_and_generation(
	struct capture *capture)
{
	struct fplinux_session_clock restored;
	struct clock_result result;
	uint32_t generation;
	uint64_t event_id;
	char path[512];
	int written;

	assert(capture->storage_owner);
	press(capture, 1, KEY_OK);
	press(capture, 1, KEY_OK);
	wait_label(capture, "New alarm");
	focus_form_item(capture, 0);
	press(capture, 1, KEY_OK);
	press(capture, 1, KEY_NUMERIC_0);
	press(capture, 1, KEY_NUMERIC_0);
	press(capture, 1, KEY_NUMERIC_0);
	press(capture, 1, KEY_NUMERIC_1);
	press(capture, 1, KEY_OK);
	press(capture, 1, KEY_F13);
	expect_request(capture, CLOCK_REQUEST_ALARM_ADD);
	assert(capture->operation.alarm.once_utc == INT64_C(1704067260));
	fail_directory_sync = true;
	result = apply_received_request(capture);
	assert(result.status == CLOCK_STORAGE_ERROR && result.applied &&
	       result.storage.state == FPLINUX_STORAGE_PUBLISHED_UNSYNCED);
	wait_label(capture, "Saving not confirmed");
	generation = capture->owner.alarms[0].generation;
	assert(generation == 1);
	result = fplinux_session_clock_tick(capture->storage_owner,
					    INT64_C(1704067260), 2000);
	assert(result.status == CLOCK_OK && result.applied);
	assert(result.storage.state == FPLINUX_STORAGE_PUBLISHED_UNSYNCED &&
	       result.storage.error == EIO);
	capture->owner = capture->storage_owner->model;
	capture->storage = capture->storage_owner->storage;
	event_id = capture->owner.active.id;
	assert(event_id == 1 &&
	       capture->owner.active.kind == CLOCK_EVENT_ALARM);
	assert(!capture->owner.alarms[0].spec.enabled);
	press(capture, 1, KEY_F14);
	wait_label(capture, "Edit alarm");
	press(capture, 1, KEY_F13);
	expect_request(capture, CLOCK_REQUEST_SAVE);
	fail_directory_sync = false;
	result = apply_received_request(capture);
	assert(result.status == CLOCK_OK && result.applied &&
	       result.storage.state == FPLINUX_STORAGE_DURABLE);
	assert(capture->owner.alarm_count == 1 &&
	       capture->owner.alarms[0].generation == generation);
	assert(capture->owner.active.id == event_id &&
	       capture->owner.active.kind == CLOCK_EVENT_ALARM);
	assert(!capture->owner.alarms[0].spec.enabled &&
	       capture->owner.alarms[0].last_due_utc == INT64_C(1704067260));
	wait_label_absent(capture, "Edit alarm");
	written = snprintf(path, sizeof(path), "%s/owner", capture->directory);
	assert(written > 0 && (size_t)written < sizeof(path));
	assert(fplinux_session_clock_init(&restored, INT64_C(1704067261), 0,
					  path)
		       .applied);
	assert(!restored.model.alarms[0].spec.enabled &&
	       restored.model.alarms[0].last_due_utc == INT64_C(1704067260) &&
	       !restored.model.active.id);
	fplinux_session_clock_cleanup(&restored);
	puts("clocks-storage-ring-retry: checkpoint Retry keeps the ringing event and generation; restart retains the consumed alarm");
}

static void active_confirmation_keeps_original_revision(struct capture *capture,
							bool changed)
{
	press(capture, 1, KEY_OK);
	wait_label(capture, "Alarms");
	press(capture, 1, KEY_DOWN);
	press(capture, 1, KEY_OK);
	wait_label(capture, "Edit alarm");
	enter_alarm_hour(capture, KEY_NUMERIC_8);
	press(capture, 1, KEY_F13);
	expect_request(capture, CLOCK_REQUEST_ALARM_EDIT);
	assert(capture->operation.id == 1 &&
	       capture->operation.alarm.hour == 8);
	assert(!capture->operation.cancel_active);
	if (changed)
		enter_alarm_hour(capture, KEY_NUMERIC_9);
	assert(clock_model_tick(&capture->owner, INT64_C(1704092400),
				25201000) == CLOCK_OK);
	assert(capture->owner.active.kind == CLOCK_EVENT_ALARM);
	assert(clock_wire_request_apply(&capture->owner, &capture->operation) ==
	       CLOCK_ACTIVE_EVENT);
	reply(capture, &capture->request, CLOCK_ACTIVE_EVENT);
	capture->has_request = false;
	wait_label(capture,
		   "This alarm is ringing. Stop it and apply the change?");
	press(capture, 1, KEY_F13);
	expect_request(capture, CLOCK_REQUEST_ALARM_EDIT);
	assert(capture->operation.id == 1 &&
	       capture->operation.alarm.hour == 8);
	assert(capture->operation.cancel_active);
	apply_request(capture);
	assert(capture->owner.alarms[0].spec.hour == 8);
	assert(capture->owner.active.kind == CLOCK_EVENT_NONE);
	if (changed) {
		wait_label(capture, "Edit alarm");
		assert(capture->observed.time_value == INT64_C(1704099600));
		press(capture, 1, KEY_F13);
		expect_request(capture, CLOCK_REQUEST_ALARM_EDIT);
		assert(capture->operation.id == 1 &&
		       capture->operation.alarm.hour == 9);
		apply_request(capture);
		assert(capture->owner.alarms[0].spec.hour == 9);
		puts("clocks-active-late-ack: confirmed 08:00 revision keeps 09:00 draft; next Save stores 09:00");
	} else {
		puts("clocks-active-unchanged-ack: unchanged 08:00 draft closes after confirmation ACK");
	}
	wait_label_absent(capture, "Edit alarm");
	wait_label(capture, changed ? "09:00  Wake" : "08:00  Wake");
}

static void localized_missed_events(struct capture *capture)
{
	press(capture, 1, KEY_F13);
	wait_label(capture, "Show clock");
	repeat_key(capture, KEY_DOWN, 2);
	press(capture, 1, KEY_OK);
	wait_label(capture, "Missed alarms");
	wait_label(capture, "Morning tea");
	wait_label(capture, "Таймер");
	assert(!has_label(capture, "Timer"));
	puts("clocks-missed-localized: expired timer uses Таймер; named alarm keeps Morning tea");
}

static void repeat_schedule(struct capture *capture)
{
	struct fplinux_ui_session_message visibility = {
		.type = FPLINUX_UI_SESSION_VISIBILITY,
		.visible = false,
	};

	press(capture, 1, KEY_OK);
	wait_label(capture, "Edit alarm");
	redraw(capture);
	assert(capture->observed.time_fields == 1);
	assert(capture->observed.date_fields == 1);
	press(capture, 1, KEY_OK);
	press(capture, 1, KEY_UP);
	press(capture, 1, KEY_F14);
	redraw(capture);
	assert(capture->observed.time_value % 86400 == 8 * 3600 + 45 * 60);
	repeat_key(capture, KEY_DOWN, 2);
	press(capture, 1, KEY_OK);
	press(capture, 1, KEY_UP);
	press(capture, 1, KEY_F14);
	redraw(capture);
	assert(capture->observed.date_value == INT64_C(1709196300));
	press(capture, 1, KEY_UP);
	press(capture, 1, KEY_OK);
	wait_label(capture, "Repeat");
	repeat_key(capture, KEY_DOWN, 4);
	press(capture, 1, KEY_OK);
	wait_label(capture, "Choose days");
	wait_legends(capture, "Done", "Mark", "Cancel");
	press(capture, 1, KEY_F13);
	wait_label(capture, "Select at least one day.");
	assert(!capture->has_request);
	press(capture, 1, KEY_F14);
	wait_label(capture, "Choose days");
	press(capture, 1, KEY_DOWN);
	press(capture, 1, KEY_OK);
	repeat_key(capture, KEY_DOWN, 2);
	press(capture, 1, KEY_OK);
	repeat_key(capture, KEY_DOWN, 2);
	press(capture, 1, KEY_OK);
	save_frame(capture, "clocks-days");
	press(capture, 1, KEY_F13);
	wait_label(capture, "Edit alarm");
	redraw(capture);
	assert(capture->observed.date_fields == 0);
	wait_label_absent(capture, "Date");
	wait_legends(capture, "Save", "Edit", "Cancel");
	press(capture, 1, KEY_F13);
	expect_request(capture, CLOCK_REQUEST_ALARM_EDIT);
	assert(capture->operation.alarm.schedule == CLOCK_ALARM_WEEKLY);
	assert(capture->operation.alarm.hour == 8);
	assert(capture->operation.alarm.minute == 45);
	assert(capture->operation.alarm.repeat_days == 0x2a);
	assert(capture->operation.alarm.once_utc == 0);
	apply_request(capture);
	assert(capture->owner.alarms[0].next_due_utc == INT64_C(1704185100));
	wait_label(capture, "08:45  wake");
	press(capture, 1, KEY_OK);
	wait_label(capture, "Edit alarm");
	press(capture, 1, KEY_DOWN);
	press(capture, 1, KEY_OK);
	repeat_key(capture, KEY_DOWN, 4);
	press(capture, 1, KEY_OK);
	press(capture, 1, KEY_OK);
	press(capture, 1, KEY_F14);
	wait_label(capture, "Edit alarm");
	focus_form_item(capture, 0);
	press(capture, 1, KEY_OK);
	press(capture, 1, KEY_UP);
	press(capture, 1, KEY_OK);
	assert(fplinux_ui_session_send(capture->session, &visibility));
	pump_for(capture, 50);
	visibility.visible = true;
	assert(fplinux_ui_session_send(capture->session, &visibility));
	redraw(capture);
	assert(capture->observed.time_value % 86400 == 9 * 3600 + 45 * 60);
	assert(capture->owner.alarms[0].spec.hour == 8);
	press(capture, 1, KEY_F14);
	wait_label(capture, "Leave without saving?");
	wait_legends(capture, "Leave", "Stay", "Cancel");
	press(capture, 1, KEY_F13);
	wait_label(capture, "08:45  wake");
	assert(capture->owner.alarms[0].spec.repeat_days == 0x2a);
	assert(!capture->has_request);
	puts("clocks-repeat: Tue Thu Sat; hidden once date; cancelled draft unchanged");
}

static uint64_t cpu_ns(clockid_t clock)
{
	struct timespec value;

	assert(clock_gettime(clock, &value) == 0);
	return (uint64_t)value.tv_sec * UINT64_C(1000000000) + value.tv_nsec;
}

static void expect_quiet(struct capture *capture, const char *scenario)
{
	clockid_t clock;
	unsigned int frames, snapshots;
	uint64_t before, elapsed;

	pump_for(capture, 100);
	assert(clock_getcpuclockid(capture->application, &clock) == 0);
	frames = capture->peer.commits;
	snapshots = capture->snapshots;
	before = cpu_ns(clock);
	pump_for(capture, 1200);
	elapsed = cpu_ns(clock) - before;
	assert(capture->peer.commits == frames);
	assert(capture->snapshots == snapshots);
	assert(elapsed < UINT64_C(100000000));
	printf("clocks-idle %s: no frames or requests; CPU under 100ms in 1200ms\n",
	       scenario);
}

static void stopwatch_actions(struct capture *capture)
{
	struct fplinux_ui_session_message message = {
		.type = FPLINUX_UI_SESSION_VISIBILITY,
		.visible = false,
	};

	press(capture, 1, KEY_F14);
	press(capture, 1, KEY_DOWN);
	press(capture, 1, KEY_OK);
	wait_label(capture, "Stopwatch");
	wait_label(capture, "00:00.00");
	press(capture, 1, KEY_OK);
	expect_request(capture, CLOCK_REQUEST_STOPWATCH_START);
	apply_request(capture);
	assert(clock_model_tick(&capture->owner, INT64_C(1704067223), 24000) ==
	       CLOCK_OK);
	wait_label(capture, "00:02.00");
	assert(fplinux_ui_session_send(capture->session, &message));
	expect_quiet(capture, "hidden");
	message.visible = true;
	assert(fplinux_ui_session_send(capture->session, &message));
	redraw(capture);
	message.type = FPLINUX_UI_SESSION_LIFECYCLE;
	message.lifecycle = FPLINUX_UI_SESSION_BACKGROUND;
	assert(fplinux_ui_session_send(capture->session, &message));
	redraw(capture);
	expect_quiet(capture, "background");
	message.lifecycle = FPLINUX_UI_SESSION_FOREGROUND;
	assert(fplinux_ui_session_send(capture->session, &message));
	redraw(capture);
	press(capture, 1, KEY_F13);
	expect_request(capture, CLOCK_REQUEST_STOPWATCH_LAP);
	pump_for(capture, 100);
	assert(!has_label(capture, "Lap 1 · 00:02.00"));
	assert(capture->owner.stopwatch.lap_count == 0);
	apply_request(capture);
	assert(capture->owner.stopwatch.lap_count == 1);
	assert(capture->owner.stopwatch.laps[0].elapsed_ms == 2000);
	wait_label(capture, "Lap 1 · 00:02.00");
	wait_label(capture, "Laps →");
	wait_legends(capture, "Lap", "Pause", "Back");
	press(capture, 1, KEY_RIGHT);
	wait_label(capture, "Laps");
	wait_label(capture, "1  00:02.00");
	assert(capture->owner.stopwatch.running);
	assert(!capture->has_request);
	assert(clock_model_tick(&capture->owner, INT64_C(1704067223), 24500) ==
	       CLOCK_OK);
	press(capture, 1, KEY_F14);
	wait_label(capture, "00:02.50");
	wait_label(capture, "Lap 1 · 00:02.00");
	wait_legends(capture, "Lap", "Pause", "Back");
	assert(capture->owner.stopwatch.running &&
	       capture->owner.stopwatch.lap_count == 1);
	assert(clock_model_tick(&capture->owner, INT64_C(1704067223), 24750) ==
	       CLOCK_OK);
	press(capture, 1, KEY_OK);
	expect_request(capture, CLOCK_REQUEST_STOPWATCH_PAUSE);
	apply_request(capture);
	assert(capture->owner.stopwatch.elapsed_ms == 2750);
	wait_label(capture, "00:02.75");
	wait_label_absent(capture, "1/20");
	expect_time_bounds(capture, "00:02.75");
	save_frame(capture, "clocks-stopwatch");
	press(capture, 1, KEY_F13);
	wait_label(capture, "Laps");
	press(capture, 1, KEY_OK);
	wait_label(capture, "1  00:02.00");
	wait_label(capture, "Total 00:02.00");
	save_frame(capture, "clocks-laps");
	press(capture, 1, KEY_F14);
	wait_legends(capture, "", "Select", "Back");
	press(capture, 1, KEY_F14);
	puts("clocks-stopwatch: direct lap=2000ms; confirmed last lap; Right history keeps running; pause=2750ms");
}

static void choose_offset(struct capture *capture, unsigned int direction,
			  size_t steps, int offset_minutes)
{
	repeat_key(capture, KEY_LEFT, CLOCK_WORLD_LIMIT);
	press(capture, 1, KEY_OK);
	wait_label(capture, "UTC offset");
	wait_label(capture, "UTC+00:00");
	repeat_key(capture, direction, steps);
	press(capture, 1, KEY_OK);
	expect_request(capture, CLOCK_REQUEST_WORLD_ADD);
	assert(capture->operation.offset_minutes == offset_minutes);
}

static uint32_t add_offset(struct capture *capture, unsigned int direction,
			   size_t steps, int offset_minutes)
{
	size_t count = capture->owner.world_count;

	choose_offset(capture, direction, steps, offset_minutes);
	apply_request(capture);
	wait_label(capture, "World clock");
	assert(capture->owner.world_count == count + 1);
	assert(capture->owner.world[count].offset_minutes == offset_minutes);
	return capture->owner.world[count].id;
}

static void open_selected_offset(struct capture *capture, const char *name,
				 const char *time, const char *date)
{
	press(capture, 1, KEY_OK);
	wait_label(capture, "Time");
	wait_label(capture, name);
	wait_label(capture, time);
	press(capture, 1, KEY_DOWN);
	wait_label(capture, date);
}

static void set_world_time(struct capture *capture, int64_t wall_utc,
			   uint64_t monotonic_ms)
{
	unsigned int snapshots = capture->snapshots;
	uint64_t deadline = fplinux_ui_frame_now_ms() + 3000;

	assert(clock_model_tick(&capture->owner, wall_utc, monotonic_ms) ==
	       CLOCK_OK);
	while (capture->snapshots == snapshots &&
	       fplinux_ui_frame_now_ms() < deadline)
		pump(capture);
	assert(capture->snapshots > snapshots);
	redraw(capture);
}

static void world_late_acknowledgments(struct capture *capture)
{
	unsigned int retransmissions = capture->retransmissions;
	size_t count = capture->owner.world_count;
	uint32_t added_id;

	choose_offset(capture, KEY_DOWN, 4, 60);
	wait_retransmissions(capture, retransmissions + 1);
	pump_for(capture, 3500);
	wait_label(capture, "Could not confirm the change. Retry?");
	save_frame(capture, "clocks-world-retry-add");
	apply_request(capture);
	assert(capture->owner.world_count == count + 1);
	added_id = capture->owner.world[count].id;
	assert(capture->owner.world[count].offset_minutes == 60);
	wait_label(capture, "Could not confirm the change. Retry?");
	press(capture, 1, KEY_F14);
	wait_label(capture, "World clock");
	open_selected_offset(capture, "UTC+01:00", "01:00",
			     "Monday, January 1");
	press(capture, 1, KEY_F13);
	wait_label(capture, "Delete this UTC offset?");
	press(capture, 1, KEY_F13);
	expect_request(capture, CLOCK_REQUEST_WORLD_DELETE);
	assert(capture->operation.id == added_id);
	retransmissions = capture->retransmissions;
	wait_retransmissions(capture, retransmissions + 1);
	pump_for(capture, 3500);
	wait_label(capture, "Could not confirm the change. Retry?");
	save_frame(capture, "clocks-world-retry-delete");
	apply_request(capture);
	assert(capture->owner.world_count == count);
	wait_label(capture, "Could not confirm the change. Retry?");
	press(capture, 1, KEY_F14);
	wait_label(capture, "World clock");
	wait_label_absent(capture, "UTC+01:00");
	assert(!capture->has_request);
	puts("clocks-world-late-ack: add selects new stable ID after Retry closes; delete leaves no stale detail");
}

static void world_edit(struct capture *capture, uint32_t id)
{
	unsigned int retransmissions;
	size_t count = capture->owner.world_count;

	repeat_key(capture, KEY_LEFT, CLOCK_WORLD_LIMIT);
	repeat_key(capture, KEY_DOWN, 2);
	open_selected_offset(capture, "UTC+05:30", "05:30",
			     "Monday, January 1");
	press(capture, 1, KEY_OK);
	wait_label(capture, "UTC offset");
	repeat_key(capture, KEY_DOWN, 4);
	press(capture, 1, KEY_OK);
	expect_request(capture, CLOCK_REQUEST_WORLD_EDIT);
	assert(capture->operation.id == id);
	assert(capture->operation.offset_minutes == 390);
	apply_request(capture);
	wait_label(capture, "UTC+06:30");
	press(capture, 1, KEY_UP);
	wait_label(capture, "06:30");
	assert(capture->owner.world_count == count);
	assert(capture->owner.world[1].id == id);
	assert(capture->owner.world[1].offset_minutes == 390);
	save_frame(capture, "clocks-offset-edited");
	press(capture, 1, KEY_OK);
	wait_label(capture, "UTC offset");
	press(capture, 1, KEY_DOWN);
	press(capture, 1, KEY_F14);
	wait_label(capture, "Time");
	wait_label(capture, "UTC+06:30");
	assert(!capture->has_request);
	assert(capture->owner.world[1].offset_minutes == 390);
	press(capture, 1, KEY_OK);
	wait_label(capture, "UTC offset");
	repeat_key(capture, KEY_UP, 3);
	press(capture, 1, KEY_OK);
	expect_request(capture, CLOCK_REQUEST_WORLD_EDIT);
	assert(capture->operation.id == id);
	assert(capture->operation.offset_minutes == 345);
	assert(clock_wire_request_apply(&capture->owner, &capture->operation) ==
	       CLOCK_INVALID);
	reply(capture, &capture->request, CLOCK_INVALID);
	capture->has_request = false;
	wait_label(capture, "This UTC offset is already in your list.");
	assert(capture->owner.world[1].id == id);
	assert(capture->owner.world[1].offset_minutes == 390);
	press(capture, 1, KEY_F14);
	wait_label(capture, "UTC offset");
	press(capture, 1, KEY_F14);
	wait_label(capture, "Time");
	press(capture, 1, KEY_OK);
	wait_label(capture, "UTC offset");
	repeat_key(capture, KEY_DOWN, 2);
	press(capture, 1, KEY_OK);
	expect_request(capture, CLOCK_REQUEST_WORLD_EDIT);
	assert(capture->operation.id == id);
	assert(capture->operation.offset_minutes == 420);
	retransmissions = capture->retransmissions;
	wait_retransmissions(capture, retransmissions + 1);
	pump_for(capture, 3500);
	wait_label(capture, "Could not confirm the change. Retry?");
	apply_request(capture);
	assert(capture->owner.world_count == count);
	assert(capture->owner.world[1].id == id);
	assert(capture->owner.world[1].offset_minutes == 420);
	press(capture, 1, KEY_F14);
	wait_label(capture, "UTC+07:00");
	wait_label(capture, "Time");
	wait_label(capture, "07:00");
	press(capture, 1, KEY_F14);
	wait_label(capture, "World clock");
	press(capture, 1, KEY_OK);
	wait_label(capture, "UTC+07:00");
	wait_label(capture, "Time");
	press(capture, 1, KEY_F14);
	wait_label(capture, "World clock");
	puts("clocks-world-edit: +05:30 to +06:30; Cancel and duplicate keep value; late +07:00 ACK keeps stable ID and order");
}

static void world_view(struct capture *capture)
{
	uint32_t half_hour_id;
	size_t count;

	press(capture, 1, KEY_F14);
	press(capture, 1, KEY_DOWN);
	press(capture, 1, KEY_OK);
	wait_label(capture, "World clock");
	press(capture, 1, KEY_OK);
	wait_label(capture, "UTC offset");
	wait_label(capture, "UTC+00:00");
	save_frame(capture, "clocks-offset-picker");
	press(capture, 1, KEY_OK);
	expect_request(capture, CLOCK_REQUEST_WORLD_ADD);
	assert(capture->operation.offset_minutes == 0);
	apply_request(capture);
	assert(capture->owner.world_count == 1);
	wait_label(capture, "World clock");
	open_selected_offset(capture, "UTC+00:00", "00:00",
			     "Monday, January 1");
	press(capture, 1, KEY_F14);
	half_hour_id = add_offset(capture, KEY_DOWN, 22, 330);
	open_selected_offset(capture, "UTC+05:30", "05:30",
			     "Monday, January 1");
	press(capture, 1, KEY_F14);
	add_offset(capture, KEY_DOWN, 23, 345);
	open_selected_offset(capture, "UTC+05:45", "05:45",
			     "Monday, January 1");
	save_frame(capture, "clocks-offset-detail");
	press(capture, 1, KEY_F14);
	add_offset(capture, KEY_UP, 14, -210);
	open_selected_offset(capture, "UTC-03:30", "20:30",
			     "Sunday, December 31");
	press(capture, 1, KEY_F14);
	wait_label(capture, "20:30 · Yesterday");
	save_frame(capture, "clocks-world");
	add_offset(capture, KEY_UP, 48, -720);
	open_selected_offset(capture, "UTC-12:00", "12:00",
			     "Sunday, December 31");
	press(capture, 1, KEY_F14);
	add_offset(capture, KEY_DOWN, 56, 840);
	open_selected_offset(capture, "UTC+14:00", "14:00",
			     "Monday, January 1");
	press(capture, 1, KEY_F14);
	count = capture->owner.world_count;
	choose_offset(capture, KEY_DOWN, 23, 345);
	assert(clock_wire_request_apply(&capture->owner, &capture->operation) ==
	       CLOCK_INVALID);
	reply(capture, &capture->request, CLOCK_INVALID);
	capture->has_request = false;
	wait_label(capture, "This UTC offset is already in your list.");
	assert(capture->owner.world_count == count);
	save_frame(capture, "clocks-world-duplicate");
	press(capture, 1, KEY_F14);
	wait_label(capture, "UTC offset");
	press(capture, 1, KEY_F14);
	wait_label(capture, "World clock");
	world_edit(capture, half_hour_id);
	world_late_acknowledgments(capture);
	repeat_key(capture, KEY_LEFT, CLOCK_WORLD_LIMIT);
	repeat_key(capture, KEY_DOWN, 3);
	open_selected_offset(capture, "UTC+05:45", "05:45",
			     "Monday, January 1");
	set_world_time(capture, INT64_C(1706744700), 25000);
	wait_label(capture, "05:30");
	wait_label(capture, "Thursday, February 1");
	press(capture, 1, KEY_F14);
	wait_label(capture, "05:30 · Tomorrow");
	save_frame(capture, "clocks-world-tomorrow");
	press(capture, 1, KEY_OK);
	wait_label(capture, "Time");
	set_world_time(capture, INT64_C(1735689000), 26000);
	wait_label(capture, "05:35");
	wait_label(capture, "Wednesday, January 1");
	press(capture, 1, KEY_F14);
	wait_label(capture, "05:35 · Tomorrow");
	puts("clocks-world: UTC default; +05:30; +05:45; -03:30 yesterday; -12:00 and +14:00 endpoints; duplicate rejected");
	puts("clocks-world-calendar: Jan 31 23:45 +05:45 = Feb 1 05:30; Dec 31 23:50 +05:45 = Jan 1 05:35; Tomorrow");
}

static void reopened_state(struct capture *capture)
{
	press(capture, 1, KEY_OK);
	press(capture, 1, KEY_DOWN);
	wait_label(capture, "08:45  wake");
	press(capture, 1, KEY_F14);
	press(capture, 1, KEY_DOWN);
	press(capture, 1, KEY_OK);
	wait_label(capture, "00:01:19");
	wait_label(capture, "Paused");
	press(capture, 1, KEY_F14);
	press(capture, 1, KEY_DOWN);
	press(capture, 1, KEY_OK);
	wait_label(capture, "00:02.75");
	wait_label_absent(capture, "1/20");
	puts("clocks-reopen: owner retains alarm, paused timer and stopwatch lap");
}

int main(int argc, char **argv)
{
	struct capture capture = {
		.storage.state = FPLINUX_STORAGE_DURABLE,
	};
	FILE *dimensions;
	char path[512];

	assert(setvbuf(stdout, NULL, _IONBF, 0) == 0);
	if (argc != 7 && argc != 8) {
		fprintf(stderr,
			"usage: %s WIDTH HEIGHT CLOCKS KEYMAP SESSION OUTPUT [SCENARIO]\n",
			argv[0]);
		return 2;
	}
	assert(clock_model_init(&capture.owner, INT64_C(1704067200), 1000) ==
	       CLOCK_OK);
	if (argc == 8) {
		struct fplinux_session_clock storage_owner;
		bool localized = !strcmp(argv[7], "missed-timer-localized");
		bool storage_controls = !strcmp(argv[7], "storage-controls");
		bool storage_ring = !strcmp(argv[7], "storage-ring-retry");
		bool paused_edit = !strcmp(argv[7], "paused-timer-edit");
		bool activation_navigation =
			!strcmp(argv[7], "activation-navigation");
		bool snooze = !strncmp(argv[7], "snooze-", 7);
		bool weekly = !strcmp(argv[7], "snooze-weekly");
		bool active_confirmation =
			!strcmp(argv[7], "active-confirm-late-ack") ||
			!strcmp(argv[7], "active-confirm-unchanged-ack");

		if (localized)
			prepare_missed_events(&capture);
		if (active_confirmation)
			prepare_confirmed_alarm(&capture);
		if (activation_navigation) {
			assert(clock_model_timer_set(&capture.owner, 90) ==
			       CLOCK_OK);
			assert(clock_model_timer_start(&capture.owner) ==
			       CLOCK_OK);
			assert(clock_model_stopwatch_start(&capture.owner) ==
			       CLOCK_OK);
			assert(clock_model_tick(&capture.owner, 1704067201,
						2000) == CLOCK_OK);
			assert(clock_model_stopwatch_lap(&capture.owner) ==
			       CLOCK_OK);
		}
		if (paused_edit) {
			assert(clock_model_timer_set(&capture.owner, 600) ==
			       CLOCK_OK);
			assert(clock_model_timer_start(&capture.owner) ==
			       CLOCK_OK);
			assert(clock_model_tick(&capture.owner, 1704067380,
						180877) == CLOCK_OK);
			assert(clock_model_timer_pause(&capture.owner) ==
			       CLOCK_OK);
			assert(capture.owner.timer.remaining_ms == 420123);
		}
		if (snooze) {
			const struct clock_alarm_spec spec = {
				.schedule = weekly ? CLOCK_ALARM_WEEKLY :
						     CLOCK_ALARM_ONCE,
				.once_utc = weekly ? 0 : 1704067260,
				.minute = weekly ? 1 : 0,
				.repeat_days = weekly ? 127 : 0,
				.enabled = true,
			};
			assert(clock_model_alarm_add(&capture.owner, &spec,
						     NULL) == CLOCK_OK);
			assert(clock_model_tick(&capture.owner, 1704067260,
						61000) == CLOCK_OK);
			assert(clock_model_snooze(&capture.owner,
						  capture.owner.active.id,
						  5) == CLOCK_OK);
		}
		if (storage_controls || storage_ring) {
			const struct clock_request request = {
				.op = CLOCK_REQUEST_TIMER_SET,
				.seconds = 90,
			};
			int written = snprintf(path, sizeof(path), "%s/owner",
					       argv[6]);

			assert(written > 0 && (size_t)written < sizeof(path));
			assert(fplinux_session_clock_init(&storage_owner,
							  INT64_C(1704067200),
							  1000, path)
				       .applied);
			if (storage_controls)
				assert(fplinux_session_clock_apply(
					       &storage_owner, &request,
					       INT64_C(1704067200), 1000)
					       .status == CLOCK_OK);
			capture.storage_owner = &storage_owner;
			capture.owner = storage_owner.model;
		}
		start(&capture, argv, "Alarms");
		if (activation_navigation)
			activation_preserves_running_clocks(&capture);
		else if (!strcmp(argv[7], "activation-draft"))
			activation_protects_alarm_draft(&capture);
		else if (!strncmp(argv[7], "activation-pending-", 19))
			activation_waits_for_alarm_reply(
				&capture, !strcmp(argv[7] + 19, "failed"),
				!strcmp(argv[7] + 19, "changed"));
		else if (localized)
			localized_missed_events(&capture);
		else if (paused_edit)
			paused_timer_edit_preserves_or_replaces_countdown(
				&capture);
		else if (snooze)
			snoozed_alarm_and_disabled_draft(&capture, weekly);
		else if (active_confirmation)
			active_confirmation_keeps_original_revision(
				&capture,
				!strcmp(argv[7], "active-confirm-late-ack"));
		else if (!strcmp(argv[7], "published-add-retry"))
			published_add_retries_accepted_identity(&capture);
		else if (!strcmp(argv[7], "published-background-durable"))
			background_durable_retains_accepted_revision(&capture);
		else if (!strcmp(argv[7], "ordinary-close-draft"))
			ordinary_close_keeps_or_discards_a_failed_draft(
				&capture);
		else if (storage_controls)
			timer_controls_and_snapshots_survive_directory_sync_failure(
				&capture);
		else if (storage_ring)
			saving_a_ringing_checkpoint_preserves_event_and_generation(
				&capture);
		else {
			assert(!strcmp(argv[7], "timer-late-ack") ||
			       !strcmp(argv[7], "timer-unchanged-ack"));
			timer_save_acknowledgment(
				&capture, !strcmp(argv[7], "timer-late-ack"));
		}
		close_and_reap(&capture);
		if (storage_controls || storage_ring)
			fplinux_session_clock_cleanup(&storage_owner);
		return 0;
	}
	start(&capture, argv, "Alarms");
	assert(snprintf(path, sizeof(path), "%s/frame-size", argv[6]) <
	       (int)sizeof(path));
	dimensions = fopen(path, "w");
	assert(dimensions);
	assert(fprintf(dimensions, "%d %d\n", capture.peer.width,
		       capture.peer.height) > 0);
	assert(!fclose(dimensions));
	save_frame(&capture, "clocks-home");
	clock_face(&capture);
	alarm_editor(&capture);
	repeat_schedule(&capture);
	timer_actions(&capture);
	stopwatch_actions(&capture);
	world_view(&capture);
	close_and_reap(&capture);
	/* Reopening creates a new UI process, while the same owner model survives. */
	start(&capture, argv, "Alarms");
	reopened_state(&capture);
	close_and_reap(&capture);
	puts("clocks: actual app, shared widgets and model wire completed");
	return 0;
}
