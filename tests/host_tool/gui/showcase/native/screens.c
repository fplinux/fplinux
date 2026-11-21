/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "observer.h"
#include "screen-frame.h"
#include "ui-frame-peer.h"
#include "fplinux-ui-profile.h"

#include <assert.h>
#include <errno.h>
#include <linux/input-event-codes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

struct capture {
	struct fplinux_ui_frame_peer peer;
	struct showcase_observation observed;
	pid_t application;
	int observer;
	int session;
	int listener;
	unsigned int exits;
	unsigned int light_applies;
	unsigned int light_clears;
	unsigned int light_resources;
	unsigned int vibrations;
	unsigned int vibration_stops;
	unsigned int mode_requests;
	unsigned int present_requests;
	unsigned int closed;
	unsigned int sequence[4096];
	size_t sequence_count;
	uint64_t now_ms;
	uint64_t advance_ns;
	int screen_width;
	int screen_height;
	int integrated_height;
	bool kept_on;
	bool russian;
	bool measured_clock;
	bool render_timing;
	bool hold_presentations;
	bool gpu_failure;
	bool closing;
	bool close_without_result;
	bool reject_light;
	bool hold_mode;
	bool mode_pending;
	bool requested_fullscreen;
	const char *frame_path;
	char left[128];
	char centre[128];
	char right[128];
};

static const char *word(const struct capture *capture, const char *english,
			const char *russian)
{
	return capture->russian ? russian : english;
}

static bool disconnected_while_closing(const struct capture *capture, int error)
{
	return capture->closing && (error == EPIPE || error == ECONNRESET);
}

static bool session_closed(const struct capture *capture, int error)
{
	char byte;

	if (disconnected_while_closing(capture, error))
		return true;
	/* The Session reader reports both EOF and malformed packets as EPROTO. */
	return capture->closing && error == EPROTO &&
	       recv(capture->session, &byte, sizeof(byte),
		    MSG_PEEK | MSG_DONTWAIT) == 0;
}

static void send_control(struct capture *capture, unsigned int permits)
{
	const struct showcase_control control = {
		.now_ms = capture->now_ms,
		.advance_ns = capture->advance_ns,
		.present_permits = permits,
		.measured_clock = capture->measured_clock,
		.render_timing = capture->render_timing,
		.gpu_failure = capture->gpu_failure,
	};
	ssize_t sent;

	sent = send(capture->observer, &control, sizeof(control), MSG_NOSIGNAL);
	assert(sent == (ssize_t)sizeof(control) ||
	       (sent < 0 && disconnected_while_closing(capture, errno)));
	capture->advance_ns = 0;
}

static void configure_requested_mode(struct capture *capture)
{
	assert(capture->mode_pending && !capture->closing);
	if (capture->peer.configure_pending)
		return;
	fplinux_ui_frame_configure(&capture->peer, capture->screen_width,
				   capture->requested_fullscreen ?
					   capture->screen_height :
					   capture->integrated_height,
				   capture->requested_fullscreen);
	capture->mode_pending = false;
}

static void pump(struct capture *capture)
{
	struct showcase_observation observation;
	struct fplinux_ui_session_message message;
	ssize_t bytes;
	int received;

	fplinux_ui_frame_pump(&capture->peer);
	while ((bytes = recv(capture->observer, &observation,
			     sizeof(observation), MSG_DONTWAIT)) > 0) {
		assert(bytes == (ssize_t)sizeof(observation));
		if (observation.kind == SHOWCASE_PRESENT_REQUEST) {
			++capture->present_requests;
			if (!capture->hold_presentations)
				send_control(capture, 1);
		} else if (observation.kind == SHOWCASE_RENDER) {
			assert(capture->sequence_count < 4096);
			capture->sequence[capture->sequence_count++] =
				observation.frame;
			capture->observed.rendered = observation.rendered;
			capture->observed.frame = observation.frame;
		} else if (observation.kind == SHOWCASE_CLOSE) {
			++capture->closed;
			capture->observed.created = observation.created;
			capture->observed.destroyed = observation.destroyed;
		} else {
			assert(!capture->closing ||
			       !capture->close_without_result ||
			       !strstr(observation.labels,
				       capture->russian ? "\nРезультат\n" :
							  "\nResult\n"));
			capture->observed = observation;
		}
	}
	assert((bytes == 0 && capture->closing) ||
	       (bytes == -1 && (errno == EAGAIN || errno == EWOULDBLOCK ||
				disconnected_while_closing(capture, errno))));
	while ((received = fplinux_ui_session_receive(capture->session,
						      &message)) == 1) {
		if (message.type == FPLINUX_UI_SESSION_KEEP_SCREEN_ON)
			capture->kept_on = message.enabled;
		else if (message.type == FPLINUX_UI_SESSION_EXIT)
			++capture->exits;
		else if (message.type == FPLINUX_UI_SESSION_SOFTKEYS) {
			strcpy(capture->left, message.left);
			strcpy(capture->centre, message.centre);
			strcpy(capture->right, message.right);
		} else if (message.type == FPLINUX_UI_SESSION_LIGHT_EFFECT) {
			if (message.light_effect.resources) {
				++capture->light_applies;
				capture->light_resources |=
					message.light_effect.resources;
				if (capture->reject_light) {
					message = (struct fplinux_ui_session_message){
						.type = FPLINUX_UI_SESSION_ERROR,
						.serial = message.serial,
					};
					strcpy(message.name, "light_effect");
					strcpy(message.reason, "unsupported");
				}
			} else {
				assert(message.light_effect.display_level ==
					       0 &&
				       !message.light_effect.keypad_on);
				++capture->light_clears;
			}
			if (!fplinux_ui_session_send(capture->session,
						     &message))
				assert(disconnected_while_closing(capture,
								  errno));
		} else if (message.type == FPLINUX_UI_SESSION_VIBRATE) {
			if (message.pattern_count) {
				assert(message.pattern_count == 2 &&
				       !message.repeat);
				assert(message.pattern[0] == 0);
				assert(message.pattern[1] >= 50 &&
				       message.pattern[1] <= 3000);
				++capture->vibrations;
			} else {
				++capture->vibration_stops;
			}
		} else if (message.type == FPLINUX_UI_SESSION_MODE) {
			++capture->mode_requests;
			/* MODE may replace an earlier request before its configure. */
			capture->requested_fullscreen =
				message.mode == FPLINUX_UI_SESSION_FULLSCREEN;
			capture->mode_pending = true;
		}
	}
	assert(received == 0 ||
	       (received < 0 && session_closed(capture, errno)));
	if (capture->mode_pending && !capture->hold_mode && !capture->closing)
		configure_requested_mode(capture);
}

static void pump_for(struct capture *capture, unsigned int milliseconds)
{
	uint64_t deadline = fplinux_ui_frame_now_ms() + milliseconds;

	while (fplinux_ui_frame_now_ms() < deadline)
		pump(capture);
}

static bool has_label(const struct capture *capture, const char *text)
{
	char line[512];

	assert(snprintf(line, sizeof(line), "\n%s\n", text) <
	       (int)sizeof(line));
	return strstr(capture->observed.labels, line) != NULL;
}

static void wait_label(struct capture *capture, const char *text)
{
	uint64_t deadline = fplinux_ui_frame_now_ms() + 3000;

	do {
		pump(capture);
		if (strstr(capture->observed.labels, text))
			return;
	} while (fplinux_ui_frame_now_ms() < deadline);
	fprintf(stderr, "expected label %s; labels:%s\n", text,
		capture->observed.labels);
	abort();
}

static void wait_action(struct capture *capture, bool canvas,
			const char *action)
{
	uint64_t deadline = fplinux_ui_frame_now_ms() + 60000;

	do {
		pump(capture);
		if (capture->observed.paints &&
		    capture->observed.canvas == canvas &&
		    !strcmp(capture->centre, action))
			return;
	} while (fplinux_ui_frame_now_ms() < deadline);
	fprintf(stderr,
		"expected Canvas=%d OK=%s; actual Canvas=%d OK=%s, labels:%s\n",
		canvas, action, capture->observed.canvas, capture->centre,
		capture->observed.labels);
	abort();
}

static void send_key(struct capture *capture, unsigned int code)
{
	fplinux_ui_frame_send_key(&capture->peer.keyboards[1], code, true);
	fplinux_ui_frame_send_key(&capture->peer.keyboards[1], code, false);
}

static void press(struct capture *capture, unsigned int code)
{
	send_key(capture, code);
	pump_for(capture, 40);
}

static void advance(struct capture *capture, uint64_t milliseconds)
{
	capture->now_ms += milliseconds;
	capture->advance_ns += milliseconds * 1000000ULL;
	send_control(capture, 0);
	pump_for(capture, 100);
}

static void wait_frame(struct capture *capture, unsigned int frame)
{
	uint64_t deadline = fplinux_ui_frame_now_ms() + 3000;

	do {
		pump(capture);
		if (capture->observed.rendered &&
		    capture->observed.frame == frame)
			return;
	} while (fplinux_ui_frame_now_ms() < deadline);
	fprintf(stderr, "expected frame %u, got %u\n", frame,
		capture->observed.frame);
	abort();
}

static void focus_row(struct capture *capture, size_t row)
{
	assert(row < capture->observed.rows);
	for (unsigned int attempt = 0;
	     capture->observed.focus != row && attempt < 32; ++attempt) {
		assert(capture->observed.focus != SIZE_MAX);
		press(capture,
		      capture->observed.focus < row ? KEY_DOWN : KEY_UP);
	}
	assert(capture->observed.focus == row);
}

static void wait_choice_list(struct capture *capture)
{
	uint64_t deadline = fplinux_ui_frame_now_ms() + 3000;

	do {
		pump(capture);
		if (capture->observed.rows && capture->observed.chosen &&
		    !has_label(capture, "ARMADA"))
			return;
	} while (fplinux_ui_frame_now_ms() < deadline);
	fprintf(stderr, "expected choice list; labels:%s\n",
		capture->observed.labels);
	abort();
}

static void wait_parent_list(struct capture *capture, size_t rows, size_t focus,
			     size_t first)
{
	uint64_t deadline = fplinux_ui_frame_now_ms() + 3000;

	do {
		pump(capture);
		if ((!rows || capture->observed.rows == rows) &&
		    capture->observed.focus == focus &&
		    (rows ? capture->observed.first_visible == first :
			    capture->observed.first_visible <= first) &&
		    has_label(capture, "ARMADA"))
			return;
	} while (fplinux_ui_frame_now_ms() < deadline);
	fprintf(stderr,
		"expected setup rows=%zu focus=%zu first=%zu; actual rows=%zu focus=%zu first=%zu; labels:%s\n",
		rows, focus, first, capture->observed.rows,
		capture->observed.focus, capture->observed.first_visible,
		capture->observed.labels);
	abort();
}

static void choose(struct capture *capture, size_t row, size_t selection)
{
	size_t first;
	size_t rows = capture->observed.rows;

	focus_row(capture, row);
	first = capture->observed.first_visible;
	press(capture, KEY_ENTER);
	wait_choice_list(capture);
	wait_action(capture, false, word(capture, "Select", "Выбрать"));
	assert(capture->observed.chosen == (1U << capture->observed.focus));
	focus_row(capture, selection);
	press(capture, KEY_ENTER);
	wait_parent_list(capture, rows, row, first);
}

static void expect_choice(struct capture *capture, size_t row, size_t selection)
{
	size_t first;
	size_t rows = capture->observed.rows;

	focus_row(capture, row);
	first = capture->observed.first_visible;
	press(capture, KEY_ENTER);
	wait_choice_list(capture);
	wait_action(capture, false, word(capture, "Select", "Выбрать"));
	assert(capture->observed.focus == selection);
	assert(capture->observed.chosen == (1U << selection));
	press(capture, KEY_F14);
	wait_parent_list(capture, rows, row, first);
}

static void start_run(struct capture *capture)
{
	focus_row(capture, 0);
	wait_action(capture, false, word(capture, "Start", "Старт"));
	press(capture, KEY_ENTER);
}

static void wait_setup(struct capture *capture)
{
	uint64_t deadline = fplinux_ui_frame_now_ms() + 60000;

	do {
		pump(capture);
		if (!capture->observed.canvas && capture->observed.rows &&
		    has_label(capture, "ARMADA") && !capture->kept_on)
			return;
	} while (fplinux_ui_frame_now_ms() < deadline);
	fprintf(stderr, "expected setup list; labels:%s\n",
		capture->observed.labels);
	abort();
}

static void wait_running(struct capture *capture)
{
	uint64_t deadline = fplinux_ui_frame_now_ms() + 3000;

	do {
		pump(capture);
		if (capture->observed.scene_visible &&
		    capture->observed.rendered && capture->kept_on &&
		    !strcmp(capture->right, word(capture, "Stop", "Стоп")))
			return;
	} while (fplinux_ui_frame_now_ms() < deadline);
	fprintf(stderr, "expected running Canvas with Stop; labels:%s\n",
		capture->observed.labels);
	abort();
}

static void expect_result(struct capture *capture, const char *status)
{
	const char *metrics[] = {
		"Generated FPS",  "Active time",    "Frames",
		"Average render", "Longest render", "Renderer",
		"Screen mode",	  "Cycles",	    "Effects",
		"Model",	  "Build",
	};
	char status_line[128];

	wait_action(capture, false, word(capture, "Repeat", "Повторить"));
	assert(snprintf(status_line, sizeof(status_line), "\n%s\n%s\n",
			word(capture, "Status", "Статус"),
			status) < (int)sizeof(status_line));
	assert(strstr(capture->observed.labels, status_line));
	if (!capture->russian)
		for (size_t index = 0;
		     index < sizeof(metrics) / sizeof(metrics[0]); ++index)
			assert(has_label(capture, metrics[index]));
	assert(!strcmp(capture->right, word(capture, "Back", "Назад")));
	assert(!capture->kept_on);
}

static void expect_resolution(struct capture *capture, int width, int height)
{
	char line[128];

	assert(snprintf(line, sizeof(line), "\n%s\n%d×%d\n",
			word(capture, "Resolution", "Разрешение"), width,
			height) < (int)sizeof(line));
	assert(strstr(capture->observed.labels, line));
}

static void stop_run(struct capture *capture)
{
	press(capture, KEY_F14);
	expect_result(capture, word(capture, "Stopped", "Остановлен"));
	assert(strstr(capture->observed.labels,
		      word(capture, "Reason\nStopped by user.\n",
			   "Причина\nОстановлен пользователем.\n")));
}

static void visibility(struct capture *capture, bool visible)
{
	struct fplinux_ui_session_message message = {
		.type = FPLINUX_UI_SESSION_VISIBILITY,
		.visible = visible,
	};

	assert(fplinux_ui_session_send(capture->session, &message));
	pump_for(capture, 60);
}

static void lifecycle(struct capture *capture, bool foreground)
{
	struct fplinux_ui_session_message message = {
		.type = FPLINUX_UI_SESSION_LIFECYCLE,
		.lifecycle = foreground ? FPLINUX_UI_SESSION_FOREGROUND :
					  FPLINUX_UI_SESSION_BACKGROUND,
	};

	assert(fplinux_ui_session_send(capture->session, &message));
	pump_for(capture, 60);
}

static void activate(struct capture *capture, bool open)
{
	struct fplinux_ui_session_message message = {
		.type = FPLINUX_UI_SESSION_ACTIVATION,
		.serial = 20,
		.activation = open ? FPLINUX_UI_SESSION_ACTIVATION_OPEN :
				     FPLINUX_UI_SESSION_ACTIVATION_RESUME,
	};

	assert(fplinux_ui_session_send(capture->session, &message));
	pump_for(capture, 60);
}

static void save_frame(struct capture *capture, const char *path)
{
	assert(screen_frame_write(
		path, capture->peer.pixels, (unsigned int)capture->peer.width,
		(unsigned int)capture->peer.height,
		(size_t)capture->peer.width * sizeof(uint16_t)));
}

static void save_setup_frame(struct capture *capture, const char *name)
{
	char path[1024];

	assert(snprintf(path, sizeof(path), "%s.%s.rgb565", capture->frame_path,
			name) < (int)sizeof(path));
	save_frame(capture, path);
}

static void start(struct capture *capture, char **arguments)
{
	struct fplinux_ui_session_message config = {
		.type = FPLINUX_UI_SESSION_CONFIG,
		.mode = FPLINUX_UI_SESSION_INTEGRATED,
		.screen_width = atoi(arguments[5]),
		.screen_height = atoi(arguments[6]),
		.theme = !strcmp(arguments[7], "dark") ?
				 FPLINUX_UI_SESSION_THEME_DARK :
				 FPLINUX_UI_SESSION_THEME_LIGHT,
	};
	struct fplinux_ui_session_message hello;
	const struct fplinux_ui_profile_spec *profile = fplinux_ui_profile_spec(
		fplinux_ui_profile_for_screen(config.screen_width,
					      config.screen_height));
	int wayland[2], observer[2];

	capture->russian = !strcmp(arguments[8], "ru");
	capture->frame_path = arguments[4];
	capture->now_ms = 1000;
	capture->measured_clock = true;
	capture->render_timing = !strcmp(arguments[9], "measurement");
	capture->screen_width = config.screen_width;
	capture->screen_height = config.screen_height;
	capture->integrated_height = config.screen_height -
				     profile->status_height -
				     profile->softkey_height;
	if (strstr(arguments[9], "effects")) {
		config.light_effects =
			!strcmp(arguments[9], "effects-subset") ? 2 : 3;
		config.vibration = true;
	}
	capture->listener = fplinux_ui_frame_listen_session(arguments[3]);
	fplinux_ui_frame_socketpair(wayland);
	assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, observer) == 0);
	capture->application = fork();
	assert(capture->application >= 0);
	if (!capture->application) {
		char descriptor[32], path[1024];
		const char *slash = strrchr(arguments[1], '/');

		assert(slash);
		close(capture->listener);
		close(wayland[0]);
		close(observer[0]);
		fplinux_ui_frame_client_environment(wayland[1], arguments[3]);
		snprintf(descriptor, sizeof(descriptor), "%d", observer[1]);
		assert(setenv("FPLINUX_SHOWCASE_OBSERVER_FD", descriptor, 1) ==
		       0);
		assert(snprintf(path, sizeof(path), "%.*s/locale",
				(int)(slash - arguments[1]),
				arguments[1]) < (int)sizeof(path));
		assert(setenv("FPLINUX_SHOWCASE_LOCALE_DIR", path, 1) == 0);
		assert(setenv("LANG", "en_US.UTF-8", 1) == 0);
		assert(setenv("LC_ALL", "en_US.UTF-8", 1) == 0);
		assert(unsetenv("LANGUAGE") == 0);
		if (!strcmp(arguments[9], "gpu-failure"))
			assert(setenv("FPLINUX_SHOWCASE_TEST_GPU", "1", 1) ==
			       0);
		else
			assert(unsetenv("FPLINUX_SHOWCASE_TEST_GPU") == 0);
		execl(arguments[1], arguments[1], (char *)NULL);
		_exit(127);
	}
	close(wayland[1]);
	close(observer[1]);
	capture->observer = observer[0];
	fplinux_ui_frame_init(&capture->peer, wayland[0], config.screen_width,
			      capture->integrated_height, arguments[2]);
	capture->session = fplinux_ui_frame_accept_session(&capture->peer,
							   capture->listener);
	hello = fplinux_ui_frame_wait_message(&capture->peer, capture->session);
	assert(hello.type == FPLINUX_UI_SESSION_HELLO);
	assert(!strcmp(hello.app_id, "org.fplinux.showcase"));
	strcpy(config.locale, arguments[8]);
	strcpy(config.input_locale, "en");
	assert(fplinux_ui_session_send(capture->session, &config));
	send_control(capture, 0);
	wait_setup(capture);
	assert(capture->observed.focus == 0 && !capture->observed.fullscreen);
	assert(capture->observed.created == 0 &&
	       capture->observed.rendered == 0);
	assert(capture->mode_requests == 0 && capture->light_applies == 0 &&
	       capture->vibrations == 0);
	fplinux_ui_frame_focus_keyboard(&capture->peer.keyboards[1]);
	pump_for(capture, 40);
}

static void close_and_reap(struct capture *capture, const char *socket_path)
{
	struct fplinux_ui_session_message message = {
		.type = FPLINUX_UI_SESSION_CLOSE
	};
	uint64_t deadline = fplinux_ui_frame_now_ms() + 3000;
	pid_t finished;
	int status;

	capture->closing = true;
	assert(fplinux_ui_session_send(capture->session, &message));
	do {
		pump(capture);
		finished = waitpid(capture->application, &status, WNOHANG);
		assert(finished >= 0);
	} while (!finished && fplinux_ui_frame_now_ms() < deadline);
	assert(finished == capture->application);
	assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	assert(capture->closed == 1);
	assert(!capture->kept_on);
	close(capture->observer);
	close(capture->session);
	close(capture->listener);
	fplinux_ui_frame_cleanup(&capture->peer);
	assert(unlink(socket_path) == 0);
}

struct viewport_geometry {
	int width;
	int height;
	int fullscreen_height;
	int standard_height;
	int standard_y;
};

static const struct viewport_geometry *
expected_geometry(const struct capture *capture)
{
	/* Standard canvas examples include the ARMADA title band. */
	static const struct viewport_geometry examples[] = {
		{ 128, 128, 112, 71, 25 },  { 128, 160, 142, 104, 22 },
		{ 176, 220, 196, 151, 26 }, { 240, 320, 288, 240, 28 },
		{ 320, 240, 218, 171, 28 },
	};

	for (size_t index = 0; index < sizeof(examples) / sizeof(examples[0]);
	     ++index)
		if (capture->screen_width == examples[index].width &&
		    capture->screen_height == examples[index].height)
			return &examples[index];
	abort();
}

static void assert_viewport(struct capture *capture, bool fullscreen)
{
	const struct viewport_geometry *expected = expected_geometry(capture);
	unsigned int height =
		(unsigned int)(fullscreen ? expected->fullscreen_height :
					    expected->standard_height);

	assert(capture->observed.canvas);
	assert(capture->observed.fullscreen == fullscreen);
	assert(capture->observed.width == (unsigned int)capture->screen_width);
	assert(capture->observed.height == height);
	assert(capture->observed.frame_width == capture->screen_width);
	assert(capture->observed.frame_height == (int)height);
	assert(capture->observed.frame_x == 0);
	assert(capture->observed.frame_y ==
	       (fullscreen ? 0 : expected->standard_y));
	if (fullscreen)
		assert(has_label(capture, word(capture, "Stop", "Стоп")));
}

static void setup_geometry(struct capture *capture)
{
	assert(capture->observed.images == 4 && capture->observed.rows == 4);
	for (size_t index = 0; index < 4; ++index) {
		assert(capture->observed.setup_y[index] != INT_MIN);
		if (index)
			assert(capture->observed.setup_y[index] >
			       capture->observed.setup_y[index - 1]);
	}
	assert(has_label(capture, "Auto") && has_label(capture, "Full screen"));
	assert(!has_label(capture, "Preset") && !has_label(capture, "Effects"));
	expect_choice(capture, 1, 0);
	expect_choice(capture, 2, 1);
	expect_choice(capture, 3, 0);
	choose(capture, 1, 1);
	expect_choice(capture, 1, 1);
	assert(!strcmp(capture->left, "Options"));
	press(capture, KEY_F13);
	wait_action(capture, false, "Select");
	press(capture, KEY_ENTER);
	expect_choice(capture, 1, 0);
	advance(capture, 1000000);
	assert(capture->observed.created == 0 &&
	       capture->observed.rendered == 0);
	assert(!capture->kept_on && capture->light_applies == 0 &&
	       capture->vibrations == 0);

	choose(capture, 1, 1);
	capture->hold_mode = true;
	start_run(capture);
	assert(capture->mode_pending && capture->mode_requests == 1);
	assert(capture->observed.created == 0 &&
	       capture->observed.rendered == 0);
	pump_for(capture, 80);
	assert(capture->observed.created == 0);
	assert(!capture->peer.configure_pending);
	configure_requested_mode(capture);
	capture->hold_mode = false;
	wait_running(capture);
	assert_viewport(capture, true);
	assert(capture->observed.mode == 1 &&
	       capture->observed.actual_mode == 1);
	assert(!capture->left[0]);
	assert(!strstr(capture->observed.labels, "Running ·"));
	stop_run(capture);
	expect_resolution(capture, capture->screen_width,
			  expected_geometry(capture)->fullscreen_height);
	assert(capture->light_applies == 0 && capture->vibrations == 0);
	assert(capture->observed.destroyed == 1);
	press(capture, KEY_F14);
	wait_setup(capture);
	assert(has_label(capture, "Last result") &&
	       capture->observed.rows == 5);

	choose(capture, 2, 0);
	start_run(capture);
	wait_running(capture);
	assert_viewport(capture, false);
	stop_run(capture);
	expect_resolution(capture, capture->screen_width,
			  expected_geometry(capture)->standard_height);
	press(capture, KEY_F14);
	wait_setup(capture);
	assert(capture->exits == 0);
}

static void wait_present_request(struct capture *capture, unsigned int count)
{
	uint64_t deadline = fplinux_ui_frame_now_ms() + 3000;

	do {
		pump(capture);
		if (capture->present_requests == count)
			return;
	} while (fplinux_ui_frame_now_ms() < deadline);
	fprintf(stderr, "expected presentation request %u, got %u\n", count,
		capture->present_requests);
	abort();
}

static void measured_stop(struct capture *capture)
{
	char text[8192];
	unsigned int rendered;

	choose(capture, 1, 1);
	capture->hold_presentations = true;
	start_run(capture);
	wait_present_request(capture, 1);
	assert(capture->sequence_count == 1 && capture->sequence[0] == 0);
	send_control(capture, 1);
	wait_present_request(capture, 2);
	assert(capture->sequence_count == 2 && capture->sequence[1] == 0);

	/* Both real renders have finished before 1 s advances at presentation.
	 * Active time includes that second, while render time stays 250/750 us. */
	send_key(capture, KEY_F14);
	capture->now_ms += 1000;
	capture->advance_ns = 1000000000ULL;
	send_control(capture, 1);
	expect_result(capture, "Stopped");
	assert(capture->sequence_count == 2 && capture->sequence[1] == 0);
	assert(strstr(capture->observed.labels, "Frames\n2\n"));
	assert(strstr(capture->observed.labels, "Generated FPS\n2.00\n"));
	assert(strstr(capture->observed.labels, "Active time\n1.001 s\n"));
	assert(strstr(capture->observed.labels, "Average render\n0.500 ms\n"));
	assert(strstr(capture->observed.labels, "Longest render\n0.750 ms\n"));
	expect_resolution(capture, 128, 112);
	assert(strstr(capture->observed.labels,
		      "Model\nComponent test model\n"));
	assert(strstr(capture->observed.labels,
		      "Build\ncomponent-test-build\n"));
	assert(strstr(capture->observed.labels, "Reason\nStopped by user.\n"));
	assert(!strstr(capture->observed.labels, "Warmup"));
	strcpy(text, capture->observed.labels);
	press(capture, KEY_NUMERIC_2);
	assert(!strcmp(capture->observed.labels, text));
	rendered = capture->observed.rendered;
	advance(capture, 1000000);
	visibility(capture, false);
	visibility(capture, true);
	activate(capture, false);
	assert(capture->observed.rendered == rendered &&
	       !strcmp(capture->observed.labels, text));
	save_setup_frame(capture, "result");
}

static unsigned int selected_cycles(const char *scenario, size_t *selection)
{
	const char *value = strrchr(scenario, '-');
	const unsigned int cycles[] = { 1, 2, 3, 5, 10, 0 };
	unsigned int requested;

	assert(value);
	requested = !strcmp(value + 1, "unlimited") ?
			    0 :
			    (unsigned int)strtoul(value + 1, NULL, 10);
	for (size_t index = 0; index < 6; ++index)
		if (cycles[index] == requested) {
			*selection = index;
			return requested;
		}
	abort();
}

static void run_cycles(struct capture *capture, const char *scenario)
{
	size_t selection;
	unsigned int cycles = selected_cycles(scenario, &selection);
	const char *choices[] = { "1", "2", "3", "5", "10", "Until stopped" };
	uint64_t limit_ms = (uint64_t)cycles * 38000;
	const char *duration;
	double actual_seconds;
	unsigned int rendered;

	focus_row(capture, 3);
	press(capture, KEY_ENTER);
	wait_choice_list(capture);
	assert(capture->observed.rows == 6);
	for (size_t index = 0; index < 6; ++index)
		assert(has_label(capture, choices[index]));
	press(capture, KEY_F14);
	choose(capture, 3, selection);
	expect_choice(capture, 3, selection);
	assert(capture->observed.created == 0);
	start_run(capture);
	wait_running(capture);
	assert_viewport(capture, true);
	assert(capture->observed.mode == 0 &&
	       capture->observed.actual_mode == 1);
	advance(capture, 1000);
	wait_frame(capture, 30);
	assert(capture->light_applies == 0 && capture->vibrations == 0);
	if (cycles) {
		advance(capture, limit_ms - 1001);
		wait_frame(capture, 1139);
		assert(capture->observed.canvas && capture->kept_on);
		advance(capture, 1);
		expect_result(capture, "Completed");
	} else {
		advance(capture, 380000);
		wait_frame(capture, 30);
		assert(capture->observed.canvas && capture->kept_on);
		stop_run(capture);
	}
	expect_resolution(capture, 128, 142);
	duration = strstr(capture->observed.labels, "Active time\n");
	assert(duration &&
	       sscanf(duration, "Active time\n%lf s", &actual_seconds) == 1);
	if (cycles) {
		assert(actual_seconds >= (double)cycles * 38.0);
		assert(actual_seconds < (double)cycles * 38.0 + 1.0);
	} else {
		assert(actual_seconds >= 381.0);
	}
	rendered = capture->observed.rendered;
	advance(capture, 1000000);
	activate(capture, false);
	assert(capture->observed.rendered == rendered);
	press(capture, KEY_F14);
	wait_setup(capture);
	expect_choice(capture, 3, selection);
}

static void interrupted(struct capture *capture, const char *scenario)
{
	unsigned long long frames;
	unsigned int rendered;
	const char *measurement;

	start_run(capture);
	wait_running(capture);
	advance(capture, 1000);
	wait_frame(capture, 30);
	if (!strcmp(scenario, "interrupt-visibility"))
		visibility(capture, false);
	else if (!strcmp(scenario, "interrupt-lifecycle"))
		lifecycle(capture, false);
	else
		fplinux_ui_frame_configure(&capture->peer,
					   capture->screen_width - 2,
					   capture->screen_height - 2, true);
	pump_for(capture, 80);
	visibility(capture, true);
	lifecycle(capture, true);
	activate(capture, false);
	expect_result(capture, "Interrupted");
	expect_resolution(capture, 176, 196);
	measurement = strstr(capture->observed.labels, "Frames\n");
	assert(measurement &&
	       sscanf(measurement, "Frames\n%llu", &frames) == 1 && frames > 0);
	rendered = capture->observed.rendered;
	advance(capture, 1000000);
	assert(capture->observed.rendered == rendered &&
	       capture->observed.created == 1 &&
	       capture->observed.destroyed == 1);
}

static void gpu_unavailable(struct capture *capture)
{
	choose(capture, 1, 2);
	start_run(capture);
	wait_label(capture, "test GPU unavailable");
	assert(!capture->observed.canvas && !capture->kept_on);
	assert(has_label(capture, "Preparation failed"));
	assert(capture->observed.created == 1 &&
	       capture->observed.rendered == 0);
	press(capture, KEY_F14);
	wait_setup(capture);
	assert(!has_label(capture, "Last result"));
	choose(capture, 1, 0);
	start_run(capture);
	wait_running(capture);
	assert(capture->observed.mode == 0 &&
	       capture->observed.actual_mode == 1);
	stop_run(capture);
	assert(strstr(capture->observed.labels, "Renderer\nsoftware\n"));
	press(capture, KEY_ENTER);
	wait_running(capture);
	assert(capture->observed.created == 3 && capture->observed.mode == 1 &&
	       capture->observed.actual_mode == 1);
	assert_viewport(capture, true);
	stop_run(capture);
}

static void gpu_failure(struct capture *capture)
{
	start_run(capture);
	wait_running(capture);
	assert(capture->observed.mode == 0 &&
	       capture->observed.actual_mode == 2);
	capture->gpu_failure = true;
	send_control(capture, 0);
	expect_result(capture, "Error");
	assert(strstr(capture->observed.labels, "test GPU render failure"));
	assert(capture->observed.destroyed == 1);
	capture->gpu_failure = false;
	send_control(capture, 0);
	press(capture, KEY_ENTER);
	wait_running(capture);
	assert(capture->observed.mode == 2 &&
	       capture->observed.actual_mode == 2);
	assert_viewport(capture, true);
	stop_run(capture);
}

static void preparation_stop(struct capture *capture)
{
	capture->hold_mode = true;
	start_run(capture);
	assert(capture->mode_pending && capture->observed.created == 0);
	stop_run(capture);
	assert(strstr(capture->observed.labels, "Frames\n0\n"));
	assert(strstr(capture->observed.labels, "Active time\n0.000 s\n"));
	assert(strstr(capture->observed.labels, "Generated FPS\n0.00\n"));
	assert(strstr(capture->observed.labels, "Average render\n0.000 ms\n"));
	assert(strstr(capture->observed.labels, "Longest render\n0.000 ms\n"));
	assert(strstr(capture->observed.labels, "Renderer\nNot started\n"));
	assert(!strstr(capture->observed.labels, "\nResolution\n"));
	assert(capture->observed.created == 0 &&
	       capture->observed.rendered == 0);
	capture->hold_mode = false;
	pump_for(capture, 80);
}

static void effects_selection(struct capture *capture, const char *scenario)
{
	bool subset = !strcmp(scenario, "effects-subset");
	unsigned int clears;
	size_t first;

	assert(capture->observed.rows == 5);
	focus_row(capture, 4);
	first = capture->observed.first_visible;
	press(capture, KEY_ENTER);
	assert(has_label(capture, "Keypad light") &&
	       has_label(capture, "Vibration"));
	if (subset) {
		assert(!has_label(capture, "Scene brightness") &&
		       capture->observed.rows == 2 &&
		       capture->observed.chosen == 3);
		press(capture, KEY_DOWN);
		press(capture, KEY_ENTER);
		assert(capture->observed.chosen == 1);
	} else {
		assert(capture->observed.rows == 3 &&
		       capture->observed.chosen == 7);
		press(capture, KEY_ENTER);
		assert(capture->observed.chosen == 6);
	}
	press(capture, KEY_F14);
	wait_parent_list(capture, 5, 4, first);
	start_run(capture);
	wait_running(capture);
	advance(capture, 24040);
	assert(capture->light_applies > 0 && capture->light_resources == 2);
	assert(capture->vibrations == (subset ? 0U : 1U));
	clears = capture->light_clears;
	stop_run(capture);
	assert(capture->light_clears > clears);
	if (!subset)
		assert(capture->vibration_stops > 0);
	press(capture, KEY_F14);
	wait_setup(capture);
	focus_row(capture, 4);
	press(capture, KEY_ENTER);
	assert(capture->observed.chosen == (subset ? 1U : 6U));
}

static void effects_off(struct capture *capture)
{
	focus_row(capture, 4);
	press(capture, KEY_ENTER);
	assert(capture->observed.chosen == 7);
	for (unsigned int index = 0; index < 3; ++index) {
		focus_row(capture, index);
		press(capture, KEY_ENTER);
	}
	assert(capture->observed.chosen == 0);
	press(capture, KEY_F14);
	start_run(capture);
	wait_running(capture);
	advance(capture, 24040);
	stop_run(capture);
	assert(capture->light_applies == 0 && capture->vibrations == 0);
	assert(strstr(capture->observed.labels, "Effects\nOff\n"));
}

static void effects_cleanup(struct capture *capture, bool close)
{
	unsigned int clears;

	start_run(capture);
	wait_running(capture);
	advance(capture, 24040);
	assert(capture->light_applies > 0 && capture->light_resources == 3);
	assert(capture->vibrations == 1);
	clears = capture->light_clears;
	if (close) {
		capture->close_without_result = true;
		return;
	}
	visibility(capture, false);
	visibility(capture, true);
	expect_result(capture, "Interrupted");
	assert(capture->light_clears > clears && capture->vibration_stops > 0);
}

static void restored_effects(struct capture *capture)
{
	focus_row(capture, 4);
	press(capture, KEY_ENTER);
	assert(capture->observed.chosen == 6);
	press(capture, KEY_F14);
	start_run(capture);
	wait_running(capture);
	advance(capture, 24040);
	assert(capture->light_resources == 2 && capture->vibrations == 1);
	stop_run(capture);
}

static void unavailable_effect(struct capture *capture)
{
	capture->reject_light = true;
	start_run(capture);
	expect_result(capture, "Error");
	assert(strstr(capture->observed.labels,
		      "A selected physical effect is unavailable."));
	assert(strstr(capture->observed.labels, "Scene brightness"));
	assert(!strstr(capture->observed.labels, "Status\nCompleted"));
	assert(capture->light_applies > 0 && capture->light_clears > 0);
}

static void persist_settings(struct capture *capture)
{
	choose(capture, 3, 5);
	choose(capture, 1, 1);
	choose(capture, 2, 0);
	focus_row(capture, 0);
	assert(capture->observed.created == 0);
}

static void restored_settings(struct capture *capture)
{
	expect_choice(capture, 1, 1);
	expect_choice(capture, 2, 0);
	expect_choice(capture, 3, 5);
	advance(capture, 1000000);
	assert(capture->observed.created == 0 &&
	       capture->observed.rendered == 0);
	assert(!has_label(capture, "Last result"));
	start_run(capture);
	wait_running(capture);
	assert_viewport(capture, false);
	assert(capture->observed.mode == 1);
	advance(capture, 380000);
	assert(capture->observed.canvas && capture->kept_on);
	stop_run(capture);
}

static void default_settings(struct capture *capture)
{
	expect_choice(capture, 1, 0);
	expect_choice(capture, 2, 1);
	expect_choice(capture, 3, 0);
	assert(capture->observed.rows == 4 && capture->observed.created == 0);
}

static void activation_navigation(struct capture *capture)
{
	unsigned int created;
	char result[8192];

	/* The phone's KEY_OK starts the same action as keyboard Enter. */
	press(capture, KEY_OK);
	wait_running(capture);
	advance(capture, 1000);
	wait_frame(capture, 30);
	created = capture->observed.created;
	press(capture, KEY_OK);
	activate(capture, false);
	wait_running(capture);
	assert(capture->observed.created == created);
	lifecycle(capture, false);
	visibility(capture, false);
	advance(capture, 1000000);
	activate(capture, false);
	lifecycle(capture, true);
	visibility(capture, true);
	expect_result(capture, "Interrupted");
	strcpy(result, capture->observed.labels);
	activate(capture, true);
	wait_setup(capture);
	assert(capture->observed.rows == 5 &&
	       has_label(capture, "Last result"));
	advance(capture, 1000000);
	assert(capture->observed.created == created && !capture->kept_on);
	focus_row(capture, 4);
	wait_action(capture, false, "Open");
	press(capture, KEY_ENTER);
	expect_result(capture, "Interrupted");
	assert(!strcmp(capture->observed.labels, result));
	activate(capture, false);
	assert(!strcmp(capture->observed.labels, result));
	press(capture, KEY_F14);
	wait_setup(capture);
}

static void localized_run(struct capture *capture)
{
	assert(has_label(capture, "ARMADA"));
	assert(has_label(capture, word(capture, "Auto", "Авто")));
	start_run(capture);
	wait_running(capture);
	advance(capture, 1000);
	wait_frame(capture, 30);
	save_setup_frame(capture, "running");
	advance(capture, 37000);
	expect_result(capture, word(capture, "Completed", "Завершено"));
	press(capture, KEY_F14);
	wait_setup(capture);
}

int main(int argc, char **argv)
{
	struct capture capture = { 0 };
	const char *scenario;

	assert(argc == 10);
	scenario = argv[9];
	start(&capture, argv);
	save_frame(&capture, argv[4]);
	if (!strcmp(scenario, "geometry"))
		setup_geometry(&capture);
	else if (!strcmp(scenario, "measurement"))
		measured_stop(&capture);
	else if (!strncmp(scenario, "cycles-", 7))
		run_cycles(&capture, scenario);
	else if (!strncmp(scenario, "interrupt-", 10))
		interrupted(&capture, scenario);
	else if (!strcmp(scenario, "gpu-unavailable"))
		gpu_unavailable(&capture);
	else if (!strcmp(scenario, "gpu-failure"))
		gpu_failure(&capture);
	else if (!strcmp(scenario, "preparation-stop"))
		preparation_stop(&capture);
	else if (!strcmp(scenario, "effects") ||
		 !strcmp(scenario, "effects-subset"))
		effects_selection(&capture, scenario);
	else if (!strcmp(scenario, "effects-off"))
		effects_off(&capture);
	else if (!strcmp(scenario, "effects-interrupted"))
		effects_cleanup(&capture, false);
	else if (!strcmp(scenario, "effects-close"))
		effects_cleanup(&capture, true);
	else if (!strcmp(scenario, "effects-unavailable"))
		unavailable_effect(&capture);
	else if (!strcmp(scenario, "persist-settings"))
		persist_settings(&capture);
	else if (!strcmp(scenario, "restored-settings"))
		restored_settings(&capture);
	else if (!strcmp(scenario, "default-settings"))
		default_settings(&capture);
	else if (!strcmp(scenario, "activation"))
		activation_navigation(&capture);
	else if (!strcmp(scenario, "restored-effects"))
		restored_effects(&capture);
	else if (!strcmp(scenario, "localized"))
		localized_run(&capture);
	else
		abort();
	close_and_reap(&capture, argv[3]);
	if (!strcmp(scenario, "effects-close"))
		assert(capture.light_clears > 0 && capture.vibration_stops > 0);
	puts("showcase: actual App setup, software scene, controlled Session and Wayland");
	return 0;
}
