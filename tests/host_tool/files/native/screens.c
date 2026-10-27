/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "observer.h"
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
	}
	assert(bytes == -1 && (errno == EAGAIN || errno == EWOULDBLOCK));
	while ((received = fplinux_ui_session_receive(capture->session,
						      &message)) == 1) {
		if (message.type == FPLINUX_UI_SESSION_STORAGE_REQUEST) {
			struct fplinux_ui_session_message reply = {
				.type = FPLINUX_UI_SESSION_STORAGE_RESULT,
				.serial = message.serial,
				.volume_result = { .request_id =
							   message.serial },
			};

			assert(message.volume_operation ==
			       FPLINUX_VOLUME_REFRESH);
			assert(fplinux_ui_session_send(capture->session,
						       &reply));
		} else if (message.type == FPLINUX_UI_SESSION_EXIT) {
			capture->exits++;
		} else if (message.type == FPLINUX_UI_SESSION_SOFTKEYS) {
			strcpy(capture->left, message.left);
			strcpy(capture->centre, message.centre);
			strcpy(capture->right, message.right);
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
		if (!capture->observed.menu && capture->observed.count == 1 &&
		    capture->observed.focused == 0 &&
		    has_label(capture, "b-dir") &&
		    has_label(capture, "large.bin"))
			return;
	} while (fplinux_ui_frame_now_ms() < deadline);
	fprintf(stderr,
		"Files expected copied destination list; count=%zu focus=%zu, labels:%s\n",
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

static void option(struct capture *capture, const char *text)
{
	size_t index;

	open_menu(capture);
	for (index = 0; index < capture->observed.menu_count; index++) {
		if (!strcmp(capture->observed.menu_items[index], text))
			break;
	}
	assert(index < capture->observed.menu_count);
	for (size_t i = 0; i < index; i++)
		press(capture, KEY_DOWN);
	press(capture, KEY_ENTER);
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
		assert(setenv("LANG", "en_US.UTF-8", 1) == 0);
		assert(setenv("LC_ALL", "en_US.UTF-8", 1) == 0);
		assert(unsetenv("LANGUAGE") == 0);
		execl(arguments[1], arguments[1], (char *)NULL);
		_exit(127);
	}
	close(wayland[1]);
	close(observer[1]);
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

static void stop_and_close(struct capture *capture)
{
	struct fplinux_ui_session_message close_message = {
		.type = FPLINUX_UI_SESSION_PREPARE_CLOSE,
		.serial = 78,
	};
	uint64_t deadline;

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

static void check_directory_menu_groups(const struct capture *capture)
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
	static const bool separators[] = { false, false, true, false,
					   false, false, true, true };

	assert(capture->observed.menu_count == 8);
	for (size_t i = 0; i < 8; ++i) {
		assert(!strcmp(capture->observed.menu_items[i],
			       word(capture, actions[i][0], actions[i][1])));
		assert(capture->observed.menu_separators[i] == separators[i]);
	}
}

int main(int argc, char **argv)
{
	struct capture capture = { 0 };
	struct fplinux_ui_session_message message;
	size_t first;

	assert(argc == 9 || argc == 10);
	start(&capture, argv);
	if (argc == 10) {
		if (!strcmp(argv[9], "stop"))
			stop_and_close(&capture);
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
	check_directory_menu_groups(&capture);
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
	assert(capture.observed.count == 1 && capture.observed.focused == 0);
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
