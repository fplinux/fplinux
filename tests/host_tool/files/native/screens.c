/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "observer.h"
#include "fplinux-ui-profile.h"
#include "screen-frame.h"
#include "ui-frame-peer.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

struct capture {
	struct fplinux_ui_frame_peer peer;
	struct files_observation observed;
	pid_t application;
	int observer;
	int session;
	int listener;
	bool russian;
	unsigned int close_waits;
	unsigned int close_cancels;
	unsigned int close_ready;
	uint32_t activation_ack;
	unsigned int exits;
	char left[128];
	char centre[128];
	char right[128];
	const char *directory;
	const char *session_path;
	/* Transition recording: every observed frame and legend message. */
	bool recording;
	unsigned int recorded_frames;
	unsigned int waiting_frames;
	unsigned int failure_frames;
	unsigned int stopping_progress_frames;
	unsigned int legend_messages;
	char legend_log[2048];
	unsigned int unavailable_frames;
	/* Every storage result carries this snapshot. */
	struct fplinux_volume_snapshot volumes;
	/* A refresh held until the scenario chooses its result and delivery. */
	bool hold_refresh;
	uint32_t refresh_serial;
	uint32_t eject_volume;
	/* A connection answered later, so that Files waits for it. */
	uint32_t connect_serial;
	uint32_t connect_volume;
	uint64_t connect_due_ms;
	/* Each read of the slow folder waits for one byte from the test. */
	bool slow_reads;
	int read_gate[2];
};

static const char *word(const struct capture *capture, const char *english,
			const char *russian)
{
	return capture->russian ? russian : english;
}

static bool has_label(const struct capture *capture, const char *text)
{
	char line[1024];

	assert(snprintf(line, sizeof(line), "\n%s\n", text) <
	       (int)sizeof(line));
	return strstr(capture->observed.labels, line) != NULL;
}

static void record_frame(struct capture *capture)
{
	capture->recorded_frames++;
	if (has_label(capture, "Reading folder") ||
	    has_label(capture, "Please wait…"))
		capture->waiting_frames++;
	if (has_label(capture, "Operation failed") ||
	    has_label(capture, "Operation cancelled."))
		capture->failure_frames++;
	if (strstr(capture->observed.labels,
		   "The file or storage is no longer available."))
		capture->unavailable_frames++;
	/* After Stop the busy alert no longer follows copy progress. */
	if (has_label(capture, "Stopping") &&
	    strstr(capture->observed.labels, "% · "))
		capture->stopping_progress_frames++;
}

static void reply_storage(struct capture *capture, uint32_t serial)
{
	struct fplinux_ui_session_message reply = {
		.type = FPLINUX_UI_SESSION_STORAGE_RESULT,
		.serial = serial,
		.volume_result = { .request_id = serial,
				   .snapshot = capture->volumes },
	};

	assert(fplinux_ui_session_send(capture->session, &reply));
}

static void connect_card(struct capture *capture)
{
	for (size_t i = 0; i < capture->volumes.count; i++) {
		struct fplinux_volume *volume = &capture->volumes.volumes[i];

		if (volume->id == capture->connect_volume) {
			/* Session discovery assigns a new ID after a mount change. */
			volume->id += 100;
			volume->state = FPLINUX_VOLUME_MOUNTED;
			volume->mount_id = 1;
			strcpy(volume->path, "/nonexistent-card");
		}
	}
}

static void pump(struct capture *capture)
{
	struct files_observation observation;
	struct fplinux_ui_session_message message;
	ssize_t bytes;
	int received;

	fplinux_ui_frame_pump(&capture->peer);
	while ((bytes = recv(capture->observer, &observation,
			     sizeof(observation), MSG_DONTWAIT)) > 0) {
		assert(bytes == (ssize_t)sizeof(observation));
		capture->observed = observation;
		if (capture->recording)
			record_frame(capture);
	}
	assert(bytes == -1 && (errno == EAGAIN || errno == EWOULDBLOCK));
	while ((received = fplinux_ui_session_receive(capture->session,
						      &message)) == 1) {
		if (message.type == FPLINUX_UI_SESSION_STORAGE_REQUEST &&
		    message.volume_operation == FPLINUX_VOLUME_CONNECT) {
			assert(!capture->connect_serial);
			capture->connect_serial = message.serial;
			capture->connect_volume = message.volume_id;
			capture->connect_due_ms =
				fplinux_ui_frame_now_ms() + 800;
		} else if (message.type == FPLINUX_UI_SESSION_STORAGE_REQUEST) {
			if (message.volume_operation == FPLINUX_VOLUME_EJECT) {
				capture->eject_volume = message.volume_id;
				for (size_t i = 0; i < capture->volumes.count;
				     i++) {
					struct fplinux_volume *volume =
						&capture->volumes.volumes[i];

					if (volume->id == message.volume_id) {
						volume->id += 100;
						volume->state =
							FPLINUX_VOLUME_AVAILABLE;
						volume->mount_id = 0;
						volume->path[0] = '\0';
					}
				}
				reply_storage(capture, message.serial);
			} else {
				assert(message.volume_operation ==
				       FPLINUX_VOLUME_REFRESH);
				if (capture->hold_refresh) {
					assert(!capture->refresh_serial);
					capture->refresh_serial =
						message.serial;
				} else {
					reply_storage(capture, message.serial);
				}
			}
		} else if (message.type == FPLINUX_UI_SESSION_EXIT) {
			capture->exits++;
		} else if (message.type == FPLINUX_UI_SESSION_SOFTKEYS) {
			strcpy(capture->left, message.left);
			strcpy(capture->centre, message.centre);
			strcpy(capture->right, message.right);
			if (capture->recording) {
				size_t used = strlen(capture->legend_log);

				capture->legend_messages++;
				snprintf(capture->legend_log + used,
					 sizeof(capture->legend_log) - used,
					 "[%s|%s|%s]", message.left,
					 message.centre, message.right);
			}
		} else if (message.type == FPLINUX_UI_SESSION_CLOSE_REPLY) {
			if (!strcmp(message.value, "wait"))
				capture->close_waits++;
			else if (!strcmp(message.value, "cancel"))
				capture->close_cancels++;
			else if (!strcmp(message.value, "ready"))
				capture->close_ready++;
		} else if (message.type == FPLINUX_UI_SESSION_ACTIVATION_ACK) {
			capture->activation_ack = message.serial;
		}
	}
	assert(received == 0);
	if (capture->connect_serial &&
	    fplinux_ui_frame_now_ms() >= capture->connect_due_ms) {
		connect_card(capture);
		reply_storage(capture, capture->connect_serial);
		capture->connect_serial = 0;
	}
}

static void pump_for(struct capture *capture, unsigned int milliseconds)
{
	uint64_t deadline = fplinux_ui_frame_now_ms() + milliseconds;

	while (fplinux_ui_frame_now_ms() < deadline)
		pump(capture);
}

static void wait_label(struct capture *capture, const char *text, bool menu)
{
	uint64_t deadline = fplinux_ui_frame_now_ms() + 5000;

	do {
		pump(capture);
		if (has_label(capture, text) && capture->observed.menu == menu)
			return;
	} while (fplinux_ui_frame_now_ms() < deadline);
	fprintf(stderr,
		"Files expected [%s], menu=%d; actual menu=%d, labels:%s\n",
		text, menu, capture->observed.menu, capture->observed.labels);
	assert(false);
}

static void press(struct capture *capture, unsigned int code)
{
	fplinux_ui_frame_send_key(&capture->peer.keyboards[0], code, true);
	fplinux_ui_frame_send_key(&capture->peer.keyboards[0], code, false);
	pump_for(capture, 25);
}

static void move_to(struct capture *capture, size_t row)
{
	uint64_t deadline = fplinux_ui_frame_now_ms() + 3000;

	assert(row < capture->observed.count);
	while (capture->observed.focused != row) {
		size_t next = (capture->observed.focused + 1) %
			      capture->observed.count;
		unsigned int previous_frame = capture->observed.frame;

		press(capture, KEY_DOWN);
		/* Wait for this key before sending the next navigation key. */
		while ((capture->observed.frame <= previous_frame ||
			capture->observed.focused != next) &&
		       fplinux_ui_frame_now_ms() < deadline)
			pump(capture);
		if (capture->observed.frame <= previous_frame ||
		    capture->observed.focused != next)
			fprintf(stderr,
				"Files expected focus=%zu after Down; actual focus=%zu, labels:%s\n",
				next, capture->observed.focused,
				capture->observed.labels);
		assert(capture->observed.frame > previous_frame &&
		       capture->observed.focused == next);
	}
}

static bool menu_has(const struct capture *capture, const char *text)
{
	for (size_t i = 0; i < capture->observed.menu_count; i++) {
		if (!strcmp(capture->observed.menu_items[i], text))
			return true;
	}
	return false;
}

static void open_menu(struct capture *capture)
{
	uint64_t deadline;
	unsigned int previous_frame;

	if (capture->observed.menu)
		press(capture, KEY_ESC);
	previous_frame = capture->observed.frame;
	press(capture, KEY_COMPOSE);
	deadline = fplinux_ui_frame_now_ms() + 3000;
	while ((!capture->observed.menu ||
		capture->observed.frame <= previous_frame) &&
	       fplinux_ui_frame_now_ms() < deadline)
		pump(capture);
	if (!capture->observed.menu ||
	    capture->observed.frame <= previous_frame)
		fprintf(stderr,
			"Files expected Options; softkeys=[%s|%s|%s], labels:%s\n",
			capture->left, capture->centre, capture->right,
			capture->observed.labels);
	assert(capture->observed.menu &&
	       capture->observed.frame > previous_frame);
}

static void wait_copy_result(struct capture *capture)
{
	uint64_t deadline = fplinux_ui_frame_now_ms() + 5000;

	do {
		pump(capture);
		if (!capture->observed.menu && capture->observed.count == 24 &&
		    capture->observed.focused == 2 &&
		    has_label(capture,
			      word(capture, "My files", "Мои файлы")) &&
		    has_label(capture, "large.bin"))
			return;
	} while (fplinux_ui_frame_now_ms() < deadline);
	fprintf(stderr,
		"Files expected source after copy; count=%zu focus=%zu, labels:%s\n",
		capture->observed.count, capture->observed.focused,
		capture->observed.labels);
	assert(false);
}

static void wait_editor_text(struct capture *capture, const char *expected)
{
	uint64_t deadline = fplinux_ui_frame_now_ms() + 3000;

	do {
		pump(capture);
		if (capture->observed.editor && !capture->observed.menu &&
		    !strcmp(capture->observed.text, expected))
			return;
	} while (fplinux_ui_frame_now_ms() < deadline);
	fprintf(stderr,
		"Files expected editor [%s]; actual [%s] cursor=%u, labels:%s\n",
		expected, capture->observed.text, capture->observed.cursor,
		capture->observed.labels);
	assert(false);
}

static void wait_conflict_list(struct capture *capture, const char *title,
			       size_t count, size_t focused)
{
	uint64_t deadline = fplinux_ui_frame_now_ms() + 3000;

	do {
		pump(capture);
		if (!capture->observed.editor && !capture->observed.menu &&
		    capture->observed.count == count &&
		    capture->observed.focused == focused &&
		    has_label(capture, title))
			return;
	} while (fplinux_ui_frame_now_ms() < deadline);
	fprintf(stderr,
		"Files expected conflict context [%s] rows=%zu focus=%zu; actual rows=%zu focus=%zu editor=%d, labels:%s\n",
		title, count, focused, capture->observed.count,
		capture->observed.focused, capture->observed.editor,
		capture->observed.labels);
	assert(false);
}

static void select_option(struct capture *capture, const char *text)
{
	size_t index;

	assert(capture->observed.menu);
	for (index = 0; index < capture->observed.menu_count; index++) {
		if (!strcmp(capture->observed.menu_items[index], text))
			break;
	}
	assert(index < capture->observed.menu_count);
	for (size_t i = 0; i < index; i++)
		press(capture, KEY_DOWN);
	press(capture, KEY_ENTER);
}

static void option(struct capture *capture, const char *text)
{
	open_menu(capture);
	select_option(capture, text);
}

static void save_frame(struct capture *capture, const char *name)
{
	char path[1024];

	pump_for(capture, 30);
	assert(snprintf(path, sizeof(path), "%s/%s.rgb565", capture->directory,
			name) < (int)sizeof(path));
	assert(screen_frame_write(
		path, capture->peer.pixels, (unsigned int)capture->peer.width,
		(unsigned int)capture->peer.height,
		(size_t)capture->peer.width * sizeof(uint16_t)));
}

static void activate(struct capture *capture, bool open, uint32_t serial)
{
	uint64_t deadline;
	struct fplinux_ui_session_message message = {
		.type = FPLINUX_UI_SESSION_LIFECYCLE,
		.lifecycle = FPLINUX_UI_SESSION_FOREGROUND,
	};

	assert(fplinux_ui_session_send(capture->session, &message));
	message = (struct fplinux_ui_session_message){
		.type = FPLINUX_UI_SESSION_ACTIVATION,
		.activation = open ? FPLINUX_UI_SESSION_ACTIVATION_OPEN :
				     FPLINUX_UI_SESSION_ACTIVATION_RESUME,
		.serial = serial,
	};

	assert(fplinux_ui_session_send(capture->session, &message));
	deadline = fplinux_ui_frame_now_ms() + 3000;
	while (capture->activation_ack != serial &&
	       fplinux_ui_frame_now_ms() < deadline)
		pump(capture);
	assert(capture->activation_ack == serial);
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
	const struct fplinux_ui_profile_spec *profile = fplinux_ui_profile_spec(
		fplinux_ui_profile_for_screen(config.screen_width,
					      config.screen_height));
	struct fplinux_ui_session_message hello;
	int wayland[2], observer[2];

	assert(profile);
	capture->directory = arguments[4];
	capture->session_path = arguments[3];
	capture->russian = !strcmp(arguments[8], "ru");
	capture->listener = fplinux_ui_frame_listen_session(arguments[3]);
	fplinux_ui_frame_socketpair(wayland);
	assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, observer) == 0);
	capture->application = fork();
	assert(capture->application >= 0);
	if (!capture->application) {
		char descriptor[32], path[1024];
		const char *separator = strrchr(arguments[1], '/');

		assert(separator);
		close(capture->listener);
		close(wayland[0]);
		close(observer[0]);
		fplinux_ui_frame_client_environment(wayland[1], arguments[3]);
		snprintf(descriptor, sizeof(descriptor), "%d", observer[1]);
		assert(setenv("FPLINUX_FILES_OBSERVER_FD", descriptor, 1) == 0);
		assert(snprintf(path, sizeof(path), "%.*s/locale",
				(int)(separator - arguments[1]),
				arguments[1]) < (int)sizeof(path));
		assert(setenv("FPLINUX_FILES_LOCALE_DIR", path, 1) == 0);
		assert(snprintf(path, sizeof(path),
				"%.*s/icons/usr/share/fplinux/glyphs",
				(int)(separator - arguments[1]),
				arguments[1]) < (int)sizeof(path));
		assert(access(path, R_OK) == 0);
		assert(setenv("FPLINUX_NATIVE_GLYPHS", path, 1) == 0);
		assert(setenv("HOME", arguments[4], 1) == 0);
		if (capture->slow_reads) {
			close(capture->read_gate[1]);
			snprintf(descriptor, sizeof(descriptor), "%d",
				 capture->read_gate[0]);
			assert(setenv("FPLINUX_FILES_READ_GATE_FD", descriptor,
				      1) == 0);
			assert(snprintf(path, sizeof(path), "%s/Files/a-dir",
					arguments[4]) < (int)sizeof(path));
			assert(setenv("FPLINUX_FILES_SLOW_DIRECTORY", path,
				      1) == 0);
		}
		assert(setenv("LANG", "en_US.UTF-8", 1) == 0);
		assert(setenv("LC_ALL", "en_US.UTF-8", 1) == 0);
		assert(unsetenv("LANGUAGE") == 0);
		execl(arguments[1], arguments[1], (char *)NULL);
		_exit(127);
	}
	close(wayland[1]);
	close(observer[1]);
	if (capture->slow_reads)
		close(capture->read_gate[0]);
	capture->observer = observer[0];
	fplinux_ui_frame_init(&capture->peer, wayland[0], config.screen_width,
			      config.screen_height - profile->status_height -
				      profile->softkey_height,
			      arguments[2]);
	capture->session = fplinux_ui_frame_accept_session(&capture->peer,
							   capture->listener);
	hello = fplinux_ui_frame_wait_message(&capture->peer, capture->session);
	assert(hello.type == FPLINUX_UI_SESSION_HELLO);
	assert(!strcmp(hello.app_id, "org.fplinux.files"));
	strcpy(config.locale, arguments[8]);
	strcpy(config.input_locale, "en");
	assert(fplinux_ui_session_send(capture->session, &config));
	wait_label(capture, word(capture, "Files", "Файлы"), false);
	assert(!has_label(capture,
			  word(capture, "Memory card", "Карта памяти")));
	assert(has_label(capture, word(capture, "My files", "Мои файлы")));
	fplinux_ui_frame_focus_keyboard(&capture->peer.keyboards[0]);
	pump_for(capture, 30);
}

static void close_and_reap(struct capture *capture)
{
	struct fplinux_ui_session_message close_message = {
		.type = FPLINUX_UI_SESSION_CLOSE,
	};
	uint64_t deadline = fplinux_ui_frame_now_ms() + 3000;
	pid_t finished;
	int status;

	assert(fplinux_ui_session_send(capture->session, &close_message));
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

/* large.bin from My files into b-dir, until its slow copy shows progress. */
static void start_copy(struct capture *capture)
{
	press(capture, KEY_ENTER);
	wait_label(capture, "a-dir", false);
	move_to(capture, 2);
	option(capture, word(capture, "Copy", "Копировать"));
	press(capture, KEY_ENTER);
	wait_label(capture, "b-dir", false);
	move_to(capture, 1);
	press(capture, KEY_ENTER);
	wait_label(capture,
		   word(capture,
			"No subfolders. Select this folder with the left key.",
			"Подпапок нет. Выберите эту папку левой клавишей."),
		   false);
	press(capture, KEY_COMPOSE);
	wait_label(capture, word(capture, "Copying", "Копирование"), false);
}

static void stop_and_close(struct capture *capture)
{
	struct fplinux_ui_session_message close_message = {
		.type = FPLINUX_UI_SESSION_PREPARE_CLOSE,
		.serial = 78,
	};
	uint64_t deadline;

	start_copy(capture);
	strcpy(close_message.reason, "user");
	assert(fplinux_ui_session_send(capture->session, &close_message));
	wait_label(capture, word(capture, "Close Files?", "Закрыть «Файлы»?"),
		   false);
	assert(fplinux_ui_session_send(capture->session, &close_message));
	pump_for(capture, 40);
	assert(capture->close_waits == 2 && !capture->close_ready);
	press(capture, KEY_ENTER);
	deadline = fplinux_ui_frame_now_ms() + 3000;
	while (!capture->close_ready && fplinux_ui_frame_now_ms() < deadline)
		pump(capture);
	assert(capture->close_ready == 1);
	close_and_reap(capture);
	puts("files-stop: repeated Close has one decision; asynchronous cancellation cleans the unpublished copy before READY");
}

static void copy_to_existing_name(struct capture *capture)
{
	press(capture, KEY_ENTER);
	wait_conflict_list(capture, word(capture, "My files", "Мои файлы"), 25,
			   0);
	move_to(capture, 2);
	option(capture, word(capture, "Copy", "Копировать"));
	wait_conflict_list(capture, word(capture, "Copy to", "Копировать в"), 1,
			   0);
	press(capture, KEY_ENTER);
	wait_conflict_list(capture, word(capture, "My files", "Мои файлы"), 2,
			   0);
	press(capture, KEY_COMPOSE);
	wait_label(capture,
		   word(capture, "Operation failed", "Операция не выполнена"),
		   false);
}

static void edit_conflicting_name(struct capture *capture)
{
	press(capture, KEY_ENTER);
	wait_editor_text(capture, "large.bin");
	press(capture, KEY_Z);
	wait_editor_text(capture, "large.binz");
	option(capture, word(capture, "Save", "Сохранить"));
	wait_label(capture,
		   word(capture, "Operation failed", "Операция не выполнена"),
		   false);
}

static void conflict_and_cancel(struct capture *capture)
{
	copy_to_existing_name(capture);
	edit_conflicting_name(capture);
	press(capture, KEY_ENTER);
	wait_editor_text(capture, "large.binz");
	activate(capture, true, 91);
	assert(capture->observed.editor &&
	       !strcmp(capture->observed.text, "large.binz"));
	option(capture, word(capture, "Cancel", "Отмена"));
	wait_conflict_list(capture, word(capture, "My files", "Мои файлы"), 2,
			   0);
	press(capture, KEY_ESC);
	wait_conflict_list(capture, word(capture, "Copy to", "Копировать в"), 1,
			   0);
	press(capture, KEY_ESC);
	wait_conflict_list(capture, word(capture, "My files", "Мои файлы"), 25,
			   2);
	activate(capture, true, 92);
	wait_conflict_list(capture, word(capture, "Files", "Файлы"), 1, 0);
	copy_to_existing_name(capture);
	edit_conflicting_name(capture);
	press(capture, KEY_ESC);
	wait_conflict_list(capture, word(capture, "My files", "Мои файлы"), 25,
			   2);
	activate(capture, true, 93);
	wait_conflict_list(capture, word(capture, "Files", "Файлы"), 1, 0);
	close_and_reap(capture);
	puts("files-conflict: repeated Another name reuses the draft; OPEN retains editing; cancelling editor or destination clears protection and preserves source selection");
}

static void begin_recording(struct capture *capture)
{
	pump_for(capture, 100);
	capture->recording = true;
	capture->recorded_frames = 0;
	capture->waiting_frames = 0;
	capture->failure_frames = 0;
	capture->stopping_progress_frames = 0;
	capture->legend_messages = 0;
	capture->legend_log[0] = '\0';
	capture->unavailable_frames = 0;
}

/* Settles late legends, then reports what the user saw during the step. */
static void end_recording(struct capture *capture, const char *step)
{
	pump_for(capture, 100);
	capture->recording = false;
	printf("%s: frames=%u waiting=%u legends=%u %s focus=%zu\n", step,
	       capture->recorded_frames, capture->waiting_frames,
	       capture->legend_messages, capture->legend_log,
	       capture->observed.focused);
}

static void folder_transitions(struct capture *capture)
{
	begin_recording(capture);
	press(capture, KEY_ENTER);
	wait_label(capture, "a-dir", false);
	end_recording(capture, "open");
	move_to(capture, 2);
	begin_recording(capture);
	option(capture, "Refresh");
	wait_label(capture, "a-dir", false);
	end_recording(capture, "refresh");
	move_to(capture, 0);
	begin_recording(capture);
	press(capture, KEY_ENTER);
	wait_label(capture, "This folder is empty.", false);
	end_recording(capture, "subfolder");
	begin_recording(capture);
	press(capture, KEY_ESC);
	wait_label(capture, "a-dir", false);
	end_recording(capture, "back");
	close_and_reap(capture);
}

static void move_up_to(struct capture *capture, size_t row)
{
	uint64_t deadline = fplinux_ui_frame_now_ms() + 3000;

	while (capture->observed.focused > row) {
		size_t next = capture->observed.focused - 1;

		press(capture, KEY_UP);
		while (capture->observed.focused != next &&
		       fplinux_ui_frame_now_ms() < deadline)
			pump(capture);
		assert(capture->observed.focused == next);
	}
}

/* A change without a busy alert or a reading screen. */
static void assert_quiet_change(const struct capture *capture)
{
	assert(!capture->waiting_frames);
	assert(!strstr(capture->legend_log, "[||Cancel]"));
}

static void keep_positions(struct capture *capture)
{
	char path[1024];
	size_t first;
	FILE *file;

	press(capture, KEY_ENTER);
	wait_conflict_list(capture, "My files", 24, 0);
	/* Up from the bottom of the view: a fresh list would scroll less. */
	move_to(capture, 20);
	move_up_to(capture, 17);
	first = capture->observed.first_visible;
	assert(first > 0 && first < 17);

	assert(snprintf(path, sizeof(path), "%s/Files/zz-new.txt",
			capture->directory) < (int)sizeof(path));
	file = fopen(path, "w");
	assert(file && fclose(file) == 0);
	begin_recording(capture);
	option(capture, "Refresh");
	wait_conflict_list(capture, "z-13-long-visible-name.txt", 25, 17);
	pump_for(capture, 100);
	assert(capture->observed.first_visible == first);
	assert_quiet_change(capture);

	begin_recording(capture);
	option(capture, "Delete");
	wait_label(capture, "Delete z-13-long-visible-name.txt?", false);
	press(capture, KEY_ENTER);
	wait_conflict_list(capture, "z-14-long-visible-name.txt", 24, 17);
	pump_for(capture, 100);
	assert(!has_label(capture, "z-13-long-visible-name.txt"));
	assert(capture->observed.first_visible == first);
	assert_quiet_change(capture);

	begin_recording(capture);
	option(capture, "Rename");
	wait_editor_text(capture, "z-14-long-visible-name.txt");
	press(capture, KEY_Z);
	wait_editor_text(capture, "z-14-long-visible-name.txtz");
	option(capture, "Save");
	wait_conflict_list(capture, "z-14-long-visible-name.txtz", 24, 17);
	pump_for(capture, 100);
	assert(capture->observed.first_visible == first);
	assert_quiet_change(capture);

	/* A change from Search results keeps the query and its results. */
	option(capture, "Find in folder");
	wait_editor_text(capture, "");
	press(capture, KEY_Z);
	press(capture, KEY_Z);
	wait_editor_text(capture, "zz");
	option(capture, "Find");
	wait_conflict_list(capture, "zz-new.txt", 2, 0);
	move_to(capture, 1);
	begin_recording(capture);
	option(capture, "Delete");
	wait_label(capture, "Delete zz-new.txt?", false);
	press(capture, KEY_ENTER);
	wait_conflict_list(capture, "Search results", 1, 0);
	assert(has_label(capture, "unknown.zzz"));
	assert(!has_label(capture, "zz-new.txt"));
	assert_quiet_change(capture);
	press(capture, KEY_ESC);
	wait_editor_text(capture, "zz");
	option(capture, "Back");
	wait_conflict_list(capture, "z-14-long-visible-name.txtz", 23, 17);
	close_and_reap(capture);
	puts("files-positions: Refresh, Delete and Rename keep the focused name or its row and the first visible row; a change from search results keeps the query");
}

static void cancel_copy(struct capture *capture)
{
	start_copy(capture);
	begin_recording(capture);
	press(capture, KEY_ESC);
	wait_conflict_list(capture, "My files", 24, 2);
	pump_for(capture, 200);
	assert(!capture->failure_frames && !capture->stopping_progress_frames);
	assert(!has_label(capture, "Stopping"));
	close_and_reap(capture);
	puts("files-cancel-copy: Cancel stops the copy and returns to the source without an error");
}

static void question_survives_copy(struct capture *capture)
{
	struct fplinux_ui_session_message close_message = {
		.type = FPLINUX_UI_SESSION_PREPARE_CLOSE,
		.serial = 79,
	};
	char path[1024];
	uint64_t deadline;

	start_copy(capture);
	strcpy(close_message.reason, "user");
	assert(fplinux_ui_session_send(capture->session, &close_message));
	wait_label(capture, "Close Files?", false);
	assert(snprintf(path, sizeof(path), "%s/Files/b-dir/large.bin",
			capture->directory) < (int)sizeof(path));
	deadline = fplinux_ui_frame_now_ms() + 10000;
	while (access(path, F_OK)) {
		assert(fplinux_ui_frame_now_ms() < deadline);
		pump(capture);
	}
	/* The copy and the source folder's reading finish meanwhile. */
	pump_for(capture, 1000);
	assert(has_label(capture, "Close Files?") && !capture->observed.menu);
	assert(capture->close_waits == 1 && !capture->close_cancels &&
	       !capture->close_ready);
	press(capture, KEY_ESC);
	deadline = fplinux_ui_frame_now_ms() + 3000;
	while (!capture->close_cancels && fplinux_ui_frame_now_ms() < deadline)
		pump(capture);
	assert(capture->close_cancels == 1 && !capture->close_ready);
	wait_copy_result(capture);
	close_and_reap(capture);
	puts("files-question: Close Files? stays after the copy ends; Continue then shows the source");
}

static void places_without_system_cards(struct capture *capture)
{
	capture->volumes = (struct fplinux_volume_snapshot){
		.count = 2,
		.home_persistent = true,
		.volumes = {
			{
				.id = 7,
				.state = FPLINUX_VOLUME_PROTECTED,
				.protection = FPLINUX_VOLUME_SYSTEM_ROOT,
				.label = "FPLROOT",
			},
			{
				.id = 8,
				.state = FPLINUX_VOLUME_AVAILABLE,
				.label = "PHOTOS",
			},
		},
	};
	option(capture, "Refresh");
	wait_label(capture, "Select to connect", false);
	assert(capture->observed.count == 2);
	assert(has_label(capture, "On the memory card"));
	assert(!has_label(capture, "Home directory"));
	assert(!has_label(capture, "Used by the system"));
	move_to(capture, 1);
	pump_for(capture, 50);
	assert(!strcmp(capture->centre, "Connect"));
	press(capture, KEY_ENTER);
	wait_label(capture, "Connecting…", false);
	wait_label(capture, "Connected", false);
	pump_for(capture, 50);
	assert(capture->connect_volume == 8);
	assert(capture->volumes.volumes[1].id == 108);
	assert(capture->observed.count == 2 && capture->observed.focused == 1);
	assert(!has_label(capture, "Connecting…"));
	assert(!strcmp(capture->centre, "Open"));
	close_and_reap(capture);
	puts("files-places: no system card row; My files on the memory card; Connect waits and keeps focus after the card ID changes");
}

/* A real host directory supplies the card's path and observed mount ID. */
static void set_card(struct capture *capture)
{
	struct fplinux_volume *volume = &capture->volumes.volumes[0];
	struct statx metadata;

	capture->volumes = (struct fplinux_volume_snapshot){ .count = 1 };
	volume->id = 11;
	volume->state = FPLINUX_VOLUME_MOUNTED;
	strcpy(volume->label, "Documents");
	assert(snprintf(volume->path, sizeof(volume->path), "%s/card",
			capture->directory) < (int)sizeof(volume->path));
	assert(statx(AT_FDCWD, volume->path, 0, STATX_MNT_ID, &metadata) == 0);
	assert(metadata.stx_mask & STATX_MNT_ID);
	assert(metadata.stx_mnt_id && metadata.stx_mnt_id <= UINT32_MAX);
	volume->mount_id = (uint32_t)metadata.stx_mnt_id;
	option(capture, "Refresh");
	wait_conflict_list(capture, "Connected", 2, 0);
}

static void deliver_refresh(struct capture *capture)
{
	assert(capture->refresh_serial);
	reply_storage(capture, capture->refresh_serial);
	capture->refresh_serial = 0;
	capture->hold_refresh = false;
	pump_for(capture, 200);
}

static void places_continuity(struct capture *capture)
{
	struct fplinux_volume swap;

	set_card(capture);
	capture->volumes.count = 2;
	capture->volumes.volumes[1] = capture->volumes.volumes[0];
	capture->volumes.volumes[1].id = 12;
	option(capture, "Refresh");
	wait_conflict_list(capture, "Connected", 3, 0);
	move_to(capture, 2);
	press(capture, KEY_ENTER);
	wait_label(capture, "card-note.txt", false);
	assert(has_label(capture, "Memory card") &&
	       !has_label(capture, "card"));
	option(capture, "Places");
	wait_conflict_list(capture, "Files", 3, 2);

	/* A late unchanged result must leave the open Options menu alone. */
	capture->hold_refresh = true;
	activate(capture, false, 111);
	open_menu(capture);
	deliver_refresh(capture);
	assert(capture->observed.menu && capture->observed.focused == 2);
	press(capture, KEY_ESC);

	/* Reordering the snapshot keeps the selected card, including Eject. */
	capture->hold_refresh = true;
	activate(capture, false, 112);
	open_menu(capture);
	swap = capture->volumes.volumes[0];
	capture->volumes.volumes[0] = capture->volumes.volumes[1];
	capture->volumes.volumes[1] = swap;
	deliver_refresh(capture);
	assert(capture->observed.menu && capture->observed.focused == 2);
	press(capture, KEY_ESC);
	wait_conflict_list(capture, "Files", 3, 1);

	/* Help covers Places before its deferred snapshot is applied. */
	capture->hold_refresh = true;
	activate(capture, false, 113);
	open_menu(capture);
	swap = capture->volumes.volumes[0];
	capture->volumes.volumes[0] = capture->volumes.volumes[1];
	capture->volumes.volumes[1] = swap;
	deliver_refresh(capture);
	assert(capture->observed.menu && capture->observed.focused == 1);
	select_option(capture, "Help");
	wait_label(capture, "Help", false);
	pump_for(capture, 100);
	press(capture, KEY_ESC);
	wait_label(capture, "Help", true);
	press(capture, KEY_ESC);
	wait_conflict_list(capture, "Files", 3, 2);
	option(capture, "Safely eject");
	wait_label(capture, "Select to connect", false);
	pump_for(capture, 100);
	assert(capture->eject_volume == 12 && capture->observed.focused == 2);
	assert(capture->volumes.volumes[1].id == 112);
	assert(!strcmp(capture->centre, "Connect"));
	close_and_reap(capture);
	puts("files-places-continuity: Places keeps the card; unchanged and reordered snapshots keep Options; Help keeps the deferred snapshot; Eject keeps focus after the card ID changes; card root has its place name");
}

static void removed_card(struct capture *capture, bool opened)
{
	char original[1024], removed[1024];

	set_card(capture);
	move_to(capture, 1);
	if (opened) {
		press(capture, KEY_ENTER);
		wait_label(capture, "card-note.txt", false);
	}
	assert(snprintf(original, sizeof(original), "%s/card",
			capture->directory) < (int)sizeof(original));
	assert(snprintf(removed, sizeof(removed), "%s/card-away",
			capture->directory) < (int)sizeof(removed));
	assert(rename(original, removed) == 0);
	capture->volumes.count = 0;
	capture->hold_refresh = true;
	begin_recording(capture);
	if (opened)
		option(capture, "Refresh");
	else
		press(capture, KEY_ENTER);
	pump_for(capture, 800);
	assert(!capture->unavailable_frames);
	deliver_refresh(capture);
	wait_label(capture, "The file or storage is no longer available.",
		   false);
	assert(has_label(capture, "My files"));
	press(capture, KEY_ESC);
	wait_conflict_list(capture, "Files", 1, 0);
	pump_for(capture, 200);
	assert(!has_label(capture,
			  "The file or storage is no longer available."));
	close_and_reap(capture);
	puts("files-removed-card: failed reading waits for storage; one error over Places closes once");
}

static void return_after_transfer(struct capture *capture, bool move)
{
	size_t first, root_first;

	press(capture, KEY_ENTER);
	wait_conflict_list(capture, "My files", 24, 0);
	move_to(capture, 20);
	move_up_to(capture, 17);
	root_first = capture->observed.first_visible;
	option(capture, "Find in folder");
	wait_editor_text(capture, "");
	press(capture, KEY_Z);
	option(capture, "Find");
	wait_conflict_list(capture, "Search results", 20, 0);
	move_to(capture, 17);
	move_up_to(capture, 14);
	first = capture->observed.first_visible;
	assert(first > 0 && first < 14);
	option(capture, move ? "Move" : "Copy");
	press(capture, KEY_ENTER);
	wait_conflict_list(capture, "My files", 2, 0);
	move_to(capture, 1);
	press(capture, KEY_ENTER);
	wait_label(capture,
		   "No subfolders. Select this folder with the left key.",
		   false);
	press(capture, KEY_COMPOSE);
	wait_conflict_list(capture, "Search results", move ? 19 : 20, 14);
	assert(has_label(capture, move ? "z-14-long-visible-name.txt" :
					 "z-13-long-visible-name.txt"));
	assert(capture->observed.first_visible == first);
	press(capture, KEY_ESC);
	wait_editor_text(capture, "z");
	option(capture, "Back");
	wait_conflict_list(capture, "My files", move ? 23 : 24, 17);
	assert(capture->observed.first_visible == root_first);
	close_and_reap(capture);
	puts("files-return-transfer: completion returns to the source; folder and search keep focus and scroll; Back keeps the query");
}

static void inspect_special_files(struct capture *capture)
{
	press(capture, KEY_ENTER);
	wait_conflict_list(capture, "My files", 26, 0);
	for (size_t row = 2; row <= 3; row++) {
		move_to(capture, row);
		pump_for(capture, 50);
		assert(!strcmp(capture->centre, "Details"));
		press(capture, KEY_ENTER);
		wait_label(capture, row == 2 ? "Symbolic link" : "Special file",
			   false);
		press(capture, KEY_ESC);
		wait_conflict_list(capture, "My files", 26, row);
	}
	close_and_reap(capture);
	puts("files-special: Centre says Details for symbolic links and special files and opens their information");
}

static void release_read(struct capture *capture)
{
	char byte = 1;

	assert(write(capture->read_gate[1], &byte, 1) == 1);
}

static void wait_reading_gone(struct capture *capture)
{
	uint64_t deadline = fplinux_ui_frame_now_ms() + 3000;

	do
		pump(capture);
	while (has_label(capture, "Reading folder") &&
	       fplinux_ui_frame_now_ms() < deadline);
	assert(!has_label(capture, "Reading folder"));
}

/*
 * Each a-dir read waits for release_read(). Times come from frames as the
 * peer receives them; the bounds leave room for a loaded host.
 */
static void slow_folder(struct capture *capture)
{
	uint64_t pressed, waiting, opened;

	press(capture, KEY_ENTER);
	wait_conflict_list(capture, "My files", 24, 0);
	begin_recording(capture);
	pressed = fplinux_ui_frame_now_ms();
	press(capture, KEY_ENTER);
	wait_label(capture, "Reading folder", false);
	waiting = fplinux_ui_frame_now_ms();
	assert(waiting - pressed >= 300);
	assert(has_label(capture, "My files") && has_label(capture, "a-dir"));
	release_read(capture);
	wait_label(capture, "This folder is empty.", false);
	opened = fplinux_ui_frame_now_ms();
	assert(opened - waiting >= 300);
	press(capture, KEY_ESC);
	wait_conflict_list(capture, "My files", 24, 0);

	/* Back before the alert stops the read; the folder stays. */
	begin_recording(capture);
	press(capture, KEY_ENTER);
	press(capture, KEY_ESC);
	release_read(capture);
	pump_for(capture, 1000);
	assert(!capture->waiting_frames && !capture->failure_frames);
	assert(capture->observed.count == 24 && capture->observed.focused == 0);
	assert(!has_label(capture, "This folder is empty."));

	/* Cancel on the shown alert does the same. */
	press(capture, KEY_ENTER);
	wait_label(capture, "Reading folder", false);
	assert(!strcmp(capture->right, "Cancel"));
	press(capture, KEY_ESC);
	release_read(capture);
	wait_reading_gone(capture);
	pump_for(capture, 100);
	assert(capture->observed.count == 24 && capture->observed.focused == 0);
	assert(has_label(capture, "a-dir"));
	assert(!has_label(capture, "This folder is empty."));
	close_and_reap(capture);
	puts("files-slow: the busy alert waits, then stays; Back or Cancel keeps the folder");
}

static void check_directory_menu_actions(const struct capture *capture)
{
	static const char *const actions[][2] = {
		{ "Details", "Сведения" },
		{ "Rename", "Переименовать" },
		{ "Places", "Места" },
		{ "Refresh", "Обновить" },
		{ "Find in folder", "Найти в папке" },
		{ "New folder", "Новая папка" },
		{ "Help", "Справка" },
		{ "Delete", "Удалить" },
	};

	assert(capture->observed.menu_count == 8);
	for (size_t i = 0; i < 8; ++i) {
		assert(!strcmp(capture->observed.menu_items[i],
			       word(capture, actions[i][0], actions[i][1])));
	}
}

int main(int argc, char **argv)
{
	struct capture capture = { 0 };
	struct fplinux_ui_session_message message;
	size_t first;

	assert(argc == 9 || argc == 10);
	capture.slow_reads = argc == 10 && !strcmp(argv[9], "slow");
	if (capture.slow_reads)
		assert(pipe(capture.read_gate) == 0);
	start(&capture, argv);
	if (argc == 10) {
		if (!strcmp(argv[9], "stop"))
			stop_and_close(&capture);
		else if (!strcmp(argv[9], "transitions"))
			folder_transitions(&capture);
		else if (!strcmp(argv[9], "positions"))
			keep_positions(&capture);
		else if (!strcmp(argv[9], "cancel-copy"))
			cancel_copy(&capture);
		else if (!strcmp(argv[9], "question"))
			question_survives_copy(&capture);
		else if (!strcmp(argv[9], "places"))
			places_without_system_cards(&capture);
		else if (!strcmp(argv[9], "places-continuity"))
			places_continuity(&capture);
		else if (!strcmp(argv[9], "removed-card"))
			removed_card(&capture, true);
		else if (!strcmp(argv[9], "removed-card-open"))
			removed_card(&capture, false);
		else if (!strcmp(argv[9], "copy-return") ||
			 !strcmp(argv[9], "move-return"))
			return_after_transfer(&capture,
					      !strcmp(argv[9], "move-return"));
		else if (!strcmp(argv[9], "special"))
			inspect_special_files(&capture);
		else if (!strcmp(argv[9], "slow"))
			slow_folder(&capture);
		else {
			assert(!strcmp(argv[9], "conflict"));
			conflict_and_cancel(&capture);
		}
		return 0;
	}
	press(&capture, KEY_ENTER);
	wait_label(&capture, "a-dir", false);
	assert(capture.observed.count == 24);
	assert(capture.observed.glyphs > 0);
	save_frame(&capture, "files-directory");
	/* Every focused row down to the last entry and back to the first. */
	for (size_t row = 1; row < 24; ++row) {
		char name[48];

		move_to(&capture, row);
		snprintf(name, sizeof(name), "files-rows-down-%02zu", row);
		save_frame(&capture, name);
	}
	for (size_t row = 23; row-- > 0;) {
		char name[48];

		move_up_to(&capture, row);
		snprintf(name, sizeof(name), "files-rows-up-%02zu", row);
		save_frame(&capture, name);
	}
	move_to(&capture, 1);
	first = capture.observed.first_visible;
	press(&capture, KEY_ENTER);
	wait_label(&capture,
		   word(&capture, "This folder is empty.", "Папка пуста."),
		   false);
	press(&capture, KEY_ESC);
	wait_label(&capture, "a-dir", false);
	assert(capture.observed.focused == 1 &&
	       capture.observed.first_visible == first);
	option(&capture, word(&capture, "Details", "Сведения"));
	wait_label(&capture, word(&capture, "Path", "Путь"), false);
	press(&capture, KEY_ESC);
	wait_label(&capture, word(&capture, "Details", "Сведения"), true);
	assert(capture.observed.menu_glyphs > 0);
	check_directory_menu_actions(&capture);
	save_frame(&capture, "files-menu");
	press(&capture, KEY_ESC);
	option(&capture, word(&capture, "Find in folder", "Найти в папке"));
	wait_label(&capture, word(&capture, "Find in folder", "Найти в папке"),
		   false);
	press(&capture, KEY_Z);
	press(&capture, KEY_Z);
	wait_editor_text(&capture, "zz");
	activate(&capture, true, 51);
	assert(!strcmp(capture.observed.text, "zz"));
	save_frame(&capture, "files-input");
	press(&capture, KEY_COMPOSE);
	assert(capture.observed.menu);
	assert(menu_has(&capture, word(&capture, "Find", "Найти")));
	press(&capture, KEY_ENTER);
	wait_label(&capture, "unknown.zzz", false);
	assert(capture.observed.count == 1);
	save_frame(&capture, "files-results");
	press(&capture, KEY_ESC);
	assert(!strcmp(capture.observed.text, "zz"));
	option(&capture, word(&capture, "Back", "Назад"));
	if (capture.observed.menu)
		press(&capture, KEY_ESC);
	wait_label(&capture, "a-dir", false);
	move_to(&capture, 2);
	option(&capture, word(&capture, "Copy", "Копировать"));
	wait_label(&capture, word(&capture, "Copy to", "Копировать в"), false);
	assert(capture.observed.count == 1);
	press(&capture, KEY_ENTER);
	wait_label(&capture, "b-dir", false);
	assert(capture.observed.count == 2);
	fprintf(stderr, "Files destination softkeys=[%s|%s|%s]\n", capture.left,
		capture.centre, capture.right);
	assert(capture.left[0] &&
	       strcmp(capture.left, word(&capture, "Options", "Опции")));
	save_frame(&capture, "files-destination");
	move_to(&capture, 1);
	press(&capture, KEY_ENTER);
	wait_label(&capture,
		   word(&capture,
			"No subfolders. Select this folder with the left key.",
			"Подпапок нет. Выберите эту папку левой клавишей."),
		   false);
	press(&capture, KEY_COMPOSE);
	wait_label(&capture, word(&capture, "Copying", "Копирование"), false);
	save_frame(&capture, "files-progress");
	message = (struct fplinux_ui_session_message){
		.type = FPLINUX_UI_SESSION_LIFECYCLE,
		.lifecycle = FPLINUX_UI_SESSION_BACKGROUND,
	};
	assert(fplinux_ui_session_send(capture.session, &message));
	activate(&capture, false, 52);
	wait_label(&capture, word(&capture, "Copying", "Копирование"), false);
	message = (struct fplinux_ui_session_message){
		.type = FPLINUX_UI_SESSION_PREPARE_CLOSE,
		.serial = 77,
	};
	strcpy(message.reason, "user");
	assert(fplinux_ui_session_send(capture.session, &message));
	wait_label(&capture, word(&capture, "Close Files?", "Закрыть «Файлы»?"),
		   false);
	assert(capture.close_waits == 1);
	press(&capture, KEY_ESC);
	assert(capture.close_cancels == 1);
	wait_copy_result(&capture);
	assert(capture.observed.count == 24 && capture.observed.focused == 2);
	activate(&capture, true, 53);
	wait_label(&capture, word(&capture, "Files", "Файлы"), false);
	option(&capture, word(&capture, "System files", "Системные файлы"));
	wait_label(&capture, word(&capture, "System files", "Системные файлы"),
		   false);
	open_menu(&capture);
	assert(!menu_has(&capture, word(&capture, "Copy", "Копировать")));
	assert(!menu_has(&capture, word(&capture, "Move", "Переместить")));
	assert(!menu_has(&capture, word(&capture, "Rename", "Переименовать")));
	assert(!menu_has(&capture, word(&capture, "Delete", "Удалить")));
	assert(!menu_has(&capture,
			 word(&capture, "New folder", "Новая папка")));
	save_frame(&capture, "files-system-menu");
	close_and_reap(&capture);
	puts("files-screens: no card; parent focus; returning Options; retained search; explicit copy destination; background progress; close WAIT/Continue; readonly system view");
	return 0;
}
