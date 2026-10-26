/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "fplinux-ui-profile.h"
#include "screen-frame.h"
#include "terminal-observer.h"
#include "ui-frame-peer.h"

#include <assert.h>
#include <json-c/json.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static struct terminal_frame_observation observation;
static bool russian;

static void wait_frame(struct fplinux_ui_frame_peer *peer, int observer)
{
	unsigned int previous = peer->commits;
	char frame = 'F';

	assert(send(observer, &frame, 1, 0) == 1);
	fplinux_ui_frame_wait_readable(peer, observer);
	assert(recv(observer, &observation, sizeof(observation), 0) ==
	       (ssize_t)sizeof(observation));
	assert(peer->commits > previous);
}

static void press_repeated(struct fplinux_ui_frame_peer *peer, int observer,
			   unsigned int code, unsigned int count)
{
	for (unsigned int index = 0; index < count; ++index) {
		fplinux_ui_frame_send_key(&peer->keyboards[1], code, true);
		fplinux_ui_frame_send_key(&peer->keyboards[1], code, false);
	}
	wait_frame(peer, observer);
}

static void press(struct fplinux_ui_frame_peer *peer, int observer,
		  unsigned int code)
{
	press_repeated(peer, observer, code, 1);
}

static void external_escape(struct fplinux_ui_frame_peer *peer)
{
	fplinux_ui_frame_send_key(&peer->keyboards[0], KEY_ESC, true);
	fplinux_ui_frame_send_key(&peer->keyboards[0], KEY_ESC, false);
}

static void hold(struct fplinux_ui_frame_peer *peer, int observer,
		 unsigned int code)
{
	uint64_t deadline = fplinux_ui_frame_now_ms() + 600;

	fplinux_ui_frame_send_key(&peer->keyboards[1], code, true);
	while (fplinux_ui_frame_now_ms() < deadline)
		fplinux_ui_frame_pump(peer);
	fplinux_ui_frame_send_key(&peer->keyboards[1], code, false);
	wait_frame(peer, observer);
}

static const char *legend(const char *english)
{
	static const struct {
		const char *english;
		const char *russian;
	} words[] = {
		{ "Options", "Опции" }, { "Enter", "Ввод" },
		{ "Clear", "Стереть" }, { "Select", "Выбрать" },
		{ "Back", "Назад" },	{ "Insert", "Вставить" },
		{ "Cancel", "Отмена" }, { "Send", "Послать" },
		{ "Done", "Готово" },	{ "Mark", "Отметить" },
		{ "Unmark", "Снять" },	{ "Close", "Закрыть" },
	};

	if (russian)
		for (size_t index = 0; index < sizeof(words) / sizeof(words[0]);
		     ++index)
			if (!strcmp(words[index].english, english))
				return words[index].russian;
	return english;
}

static void wait_shell(struct fplinux_ui_frame_peer *peer, int control,
		       char expected)
{
	char byte;

	fplinux_ui_frame_wait_readable(peer, control);
	assert(read(control, &byte, 1) == 1 && byte == expected);
}

static void read_shell_size(struct fplinux_ui_frame_peer *peer, int control,
			    const char *phase, unsigned int columns,
			    unsigned int rows)
{
	struct winsize size;
	unsigned char *bytes = (unsigned char *)&size;

	for (size_t index = 0; index < sizeof(size); ++index) {
		fplinux_ui_frame_wait_readable(peer, control);
		assert(read(control, bytes + index, 1) == 1);
	}
	assert(size.ws_col == columns && size.ws_row == rows);
	printf("terminal-pty %s %dx%d: %ux%u\n", phase, peer->width,
	       peer->height, size.ws_col, size.ws_row);
}

static void wait_legends(struct fplinux_ui_frame_peer *peer, int session,
			 const char *left, const char *centre,
			 const char *right)
{
	uint64_t deadline = fplinux_ui_frame_now_ms() + 3000;

	for (;;) {
		struct fplinux_ui_session_message message =
			fplinux_ui_frame_wait_message(peer, session);

		assert(fplinux_ui_frame_now_ms() < deadline);
		if (message.type == FPLINUX_UI_SESSION_SOFTKEYS &&
		    !strcmp(message.left, legend(left)) &&
		    !strcmp(message.centre, legend(centre)) &&
		    !strcmp(message.right, legend(right)))
			return;
		assert(message.type != FPLINUX_UI_SESSION_EXIT);
	}
}

static void save(struct fplinux_ui_frame_peer *peer, int observer,
		 const char *directory, const char *name)
{
	char path[512];
	struct json_object *frame = json_object_new_object();
	struct json_object *content = json_object_new_array();
	struct json_object *labels = json_object_new_array();
	struct json_object *terminal = json_object_new_object();

	wait_frame(peer, observer);
	assert(snprintf(path, sizeof(path), "%s/%s.rgb565", directory, name) <
	       (int)sizeof(path));
	assert(screen_frame_write(path, peer->pixels, peer->width, peer->height,
				  peer->width * sizeof(*peer->pixels)));
	assert(frame && content && labels && terminal);
	json_object_array_add(content,
			      json_object_new_int(observation.content_left));
	json_object_array_add(content,
			      json_object_new_int(observation.content_top));
	json_object_array_add(content,
			      json_object_new_int(observation.content_width));
	json_object_array_add(content,
			      json_object_new_int(observation.content_height));
	json_object_object_add(frame, "content", content);
	json_object_object_add(
		frame, "labels_truncated",
		json_object_new_boolean(observation.labels_truncated));
	json_object_object_add(frame, "labels", labels);
	json_object_object_add(frame, "terminal", terminal);
	json_object_object_add(
		terminal, "columns",
		json_object_new_int(observation.terminal_columns));
	json_object_object_add(terminal, "rows",
			       json_object_new_int(observation.terminal_rows));
	json_object_object_add(
		terminal, "cursor_x",
		json_object_new_int(observation.terminal_cursor_x));
	json_object_object_add(
		terminal, "cursor_y",
		json_object_new_int(observation.terminal_cursor_y));
	json_object_object_add(terminal, "flags",
			       json_object_new_int(observation.terminal_flags));
	json_object_object_add(
		terminal, "preedit",
		json_object_new_int(observation.terminal_preedit));
	json_object_object_add(
		terminal, "first_character",
		json_object_new_int(observation.terminal_first_character));
	json_object_object_add(
		terminal, "first_background",
		json_object_new_int(observation.terminal_first_background));
	json_object_object_add(
		terminal, "edge_character",
		json_object_new_int(observation.terminal_edge_character));
	json_object_object_add(
		terminal, "cursor_visible",
		json_object_new_boolean(observation.terminal_cursor_visible));
	json_object_object_add(
		frame, "shell_resources_closed",
		json_object_new_boolean(observation.shell_resources_closed));
	json_object_object_add(
		frame, "pty_watched",
		json_object_new_boolean(observation.pty_watched));
	json_object_object_add(
		frame, "final_output_visible",
		json_object_new_boolean(observation.final_output_visible));
	for (size_t index = 0; index < observation.label_count; ++index) {
		const struct terminal_label_observation *label =
			&observation.labels[index];
		struct json_object *item = json_object_new_object();
		struct json_object *bounds = json_object_new_array();
		struct json_object *text_size = json_object_new_array();

		assert(item && bounds && text_size);
		json_object_array_add(bounds, json_object_new_int(label->left));
		json_object_array_add(bounds, json_object_new_int(label->top));
		json_object_array_add(bounds,
				      json_object_new_int(label->width));
		json_object_array_add(bounds,
				      json_object_new_int(label->height));
		json_object_array_add(text_size,
				      json_object_new_int(label->text_width));
		json_object_array_add(text_size,
				      json_object_new_int(label->text_height));
		json_object_object_add(item, "text",
				       json_object_new_string(label->text));
		json_object_object_add(item, "bounds", bounds);
		json_object_object_add(item, "text_size", text_size);
		json_object_object_add(item, "scrolls",
				       json_object_new_boolean(label->scrolls));
		json_object_object_add(
			item, "ellipsizes",
			json_object_new_boolean(label->ellipsizes));
		json_object_array_add(labels, item);
	}
	assert(snprintf(path, sizeof(path), "%s/%s.json", directory, name) <
	       (int)sizeof(path));
	assert(json_object_to_file_ext(path, frame, JSON_C_TO_STRING_PRETTY) ==
	       0);
	json_object_put(frame);
	printf("%s: %dx%d\n", name, peer->width, peer->height);
}

static void capture_symbols(struct fplinux_ui_frame_peer *peer, int session,
			    int observer, int control, const char *directory)
{
	press(peer, observer, KEY_NUMERIC_STAR);
	wait_legends(peer, session, "", "Insert", "Cancel");
	save(peer, observer, directory, "terminal-symbols-star");
	press(peer, observer, KEY_F14);
	wait_legends(peer, session, "Options", "Enter", "Clear");
	press(peer, observer, KEY_F13);
	wait_legends(peer, session, "", "Select", "Back");
	press(peer, observer, KEY_DOWN);
	press(peer, observer, KEY_OK);
	wait_legends(peer, session, "", "Insert", "Cancel");
	save(peer, observer, directory, "terminal-symbols-menu");
	press(peer, observer, KEY_OK);
	wait_shell(peer, control, '.');
	wait_legends(peer, session, "Options", "Enter", "Clear");
}

static void check_child_navigation(struct fplinux_ui_frame_peer *peer,
				   int session, int observer, int control)
{
	static const char *const navigation[] = {
		"\033[H", "\033[F", "\033[3~", "\033[5~", "\033[6~",
	};

	press(peer, observer, KEY_NUMERIC_2);
	press(peer, observer, KEY_F13);
	wait_legends(peer, session, "", "Select", "Back");
	/* Modifiers, Symbols, then Special keys. */
	press(peer, observer, KEY_DOWN);
	press(peer, observer, KEY_DOWN);
	press(peer, observer, KEY_OK);
	wait_legends(peer, session, "", "Send", "Back");
	press(peer, observer, KEY_F14);
	wait_legends(peer, session, "", "Select", "Back");
	press(peer, observer, KEY_OK);
	wait_legends(peer, session, "", "Send", "Back");
	press(peer, observer, KEY_OK);
	wait_legends(peer, session, "Options", "Enter", "Clear");
	/* Back preserves composition; Send commits it before the first Esc. */
	for (const char *byte = "a\033"; *byte; ++byte)
		wait_shell(peer, control, *byte);
	for (unsigned int index = 0;
	     index < sizeof(navigation) / sizeof(navigation[0]); ++index) {
		press(peer, observer, KEY_F13);
		wait_legends(peer, session, "", "Select", "Back");
		press_repeated(peer, observer, KEY_DOWN, 2);
		press(peer, observer, KEY_OK);
		wait_legends(peer, session, "", "Send", "Back");
		press_repeated(peer, observer, KEY_DOWN, index + 1);
		press(peer, observer, KEY_OK);
		wait_legends(peer, session, "Options", "Enter", "Clear");
		for (const char *byte = navigation[index]; *byte; ++byte)
			wait_shell(peer, control, *byte);
	}
	printf("terminal-special %dx%d: Esc/Home/End/Delete/PageUp/PageDown reached the real PTY; pending text kept its order\n",
	       peer->width, peer->height);
	press(peer, observer, KEY_F13);
	wait_legends(peer, session, "", "Select", "Back");
	for (unsigned int index = 0; index < 8; ++index)
		press(peer, observer, KEY_DOWN);
	press(peer, observer, KEY_OK);
	wait_legends(peer, session, "", "", "Back");
	press(peer, observer, KEY_F14);
	wait_legends(peer, session, "", "Select", "Back");
	press(peer, observer, KEY_OK);
	wait_legends(peer, session, "", "", "Back");
	press(peer, observer, KEY_F14);
	wait_legends(peer, session, "", "Select", "Back");
	press(peer, observer, KEY_F14);
	wait_legends(peer, session, "Options", "Enter", "Clear");
}

static void open_option(struct fplinux_ui_frame_peer *peer, int session,
			int observer, unsigned int index);

static bool has_label(const char *text)
{
	for (size_t index = 0; index < observation.label_count; ++index)
		if (!strcmp(observation.labels[index].text, text))
			return true;
	return false;
}

static void failed_shell_stays_readable(struct fplinux_ui_frame_peer *peer,
					int session, int observer, int control,
					const char *directory,
					const char *theme)
{
	const char *status = !strcmp(theme, "light") ?
				     (russian ? "Сигнал 15" : "Signal 15") :
				     (russian ? "Код 37" : "Exit 37");
	const char *request = !strcmp(theme, "light") ? "S" : "F";
	bool keyboard_navigation = !russian && !strcmp(theme, "dark");
	uint64_t deadline = fplinux_ui_frame_now_ms() + 3000;
	char name[128];

	assert(write(control, request, 1) == 1);
	wait_shell(peer, control, 'Q');
	do {
		assert(fplinux_ui_frame_now_ms() < deadline);
		wait_frame(peer, observer);
	} while (!has_label(status));
	wait_legends(peer, session, "Options", "", "Close");
	assert(observation.shell_resources_closed && !observation.pty_watched &&
	       observation.final_output_visible);
	assert(snprintf(name, sizeof(name), "terminal-ended-%s-%s",
			russian ? "ru" : "en", theme) < (int)sizeof(name));
	save(peer, observer, directory, name);
	press_repeated(peer, observer, KEY_NUMERIC_2, 160);
	assert(observation.shell_resources_closed && !observation.pty_watched &&
	       observation.final_output_visible && has_label(status));
	open_option(peer, session, observer, 0);
	wait_legends(peer, session, "Options", "", "Back");
	press(peer, observer, KEY_LEFT);
	assert(!has_label("↑0"));
	assert(snprintf(name, sizeof(name), "terminal-ended-history-%s-%s",
			russian ? "ru" : "en", theme) < (int)sizeof(name));
	save(peer, observer, directory, name);
	if (keyboard_navigation) {
		external_escape(peer);
		wait_frame(peer, observer);
	} else {
		press(peer, observer, KEY_F14);
	}
	assert(has_label(status) && observation.final_output_visible);
	wait_legends(peer, session, "Options", "", "Close");
	/* EXIT preserves the submitted view until the manager sends CLOSE. */
	if (keyboard_navigation) {
		external_escape(peer);
	} else {
		fplinux_ui_frame_send_key(&peer->keyboards[1], KEY_F14, true);
		fplinux_ui_frame_send_key(&peer->keyboards[1], KEY_F14, false);
	}
	for (;;) {
		struct fplinux_ui_session_message message =
			fplinux_ui_frame_wait_message(peer, session);

		if (message.type == FPLINUX_UI_SESSION_EXIT)
			break;
	}
	printf("terminal-ended %dx%d %s %s: final output; no PTY resources/input; Scrollback and Close\n",
	       peer->width, peer->height, russian ? "ru" : "en", theme);
	if (keyboard_navigation)
		printf("terminal-keyboard %dx%d: live Esc reached PTY; stopped Esc Back/Close\n",
		       peer->width, peer->height);
}

static void save_state(struct fplinux_ui_frame_peer *peer, int observer,
		       const char *directory, const char *prefix,
		       const char *state)
{
	char name[192];

	assert(snprintf(name, sizeof(name), "%s-%s", prefix, state) <
	       (int)sizeof(name));
	save(peer, observer, directory, name);
}

static void open_option(struct fplinux_ui_frame_peer *peer, int session,
			int observer, unsigned int index)
{
	press(peer, observer, KEY_F13);
	wait_legends(peer, session, "", "Select", "Back");
	if (index)
		press_repeated(peer, observer, KEY_DOWN, index);
	press(peer, observer, KEY_OK);
}

static void expect_font_selection(size_t focused, size_t selected)
{
	assert(observation.list_count == 2);
	assert(observation.list_focused == focused);
	assert(observation.list_selected[selected]);
	assert(!observation.list_selected[1 - selected]);
}

static void change_font(struct fplinux_ui_frame_peer *peer, int session,
			int observer, int control, bool compact)
{
	struct pollfd input = { .fd = control, .events = POLLIN };
	size_t current = compact ? 0 : 1;
	unsigned int direction = compact ? KEY_DOWN : KEY_UP;

	open_option(peer, session, observer, 7);
	wait_legends(peer, session, "", "Select", "Back");
	expect_font_selection(current, current);
	press(peer, observer, direction);
	expect_font_selection(1 - current, current);
	assert(poll(&input, 1, 0) == 0);
	press(peer, observer, KEY_F14);
	wait_legends(peer, session, "", "Select", "Back");
	press(peer, observer, KEY_OK);
	wait_legends(peer, session, "", "Select", "Back");
	expect_font_selection(current, current);
	assert(poll(&input, 1, 0) == 0);
	press(peer, observer, direction);
	expect_font_selection(1 - current, current);
	press(peer, observer, KEY_OK);
	wait_legends(peer, session, "Options", "Enter", "Clear");
	printf("terminal-font-selection %dx%d: current mark survives focus and Back; OK applies\n",
	       peer->width, peer->height);
}

static void capture_header(struct fplinux_ui_frame_peer *peer, int session,
			   int observer, int control, const char *directory,
			   const char *prefix)
{
	struct pollfd input = { .fd = control, .events = POLLIN };

	save_state(peer, observer, directory, prefix, "live");
	/* Clear composition before arming one-shot modifiers. */
	press(peer, observer, KEY_F14);
	open_option(peer, session, observer, 0);
	wait_legends(peer, session, "Done", "Mark", "Back");
	for (unsigned int index = 0; index < 3; ++index) {
		press(peer, observer, KEY_OK);
		if (index != 2)
			press(peer, observer, KEY_DOWN);
	}
	press(peer, observer, KEY_F13);
	wait_legends(peer, session, "Options", "Enter", "Clear");
	save_state(peer, observer, directory, prefix, "armed");
	open_option(peer, session, observer, 2);
	wait_legends(peer, session, "", "Send", "Back");
	save_state(peer, observer, directory, prefix, "special-modifiers");
	press(peer, observer, KEY_F14);
	wait_legends(peer, session, "", "Select", "Back");
	press(peer, observer, KEY_F14);
	wait_legends(peer, session, "Options", "Enter", "Clear");
	press(peer, observer, KEY_NUMERIC_POUND);
	save_state(peer, observer, directory, prefix, "upper");
	hold(peer, observer, KEY_NUMERIC_STAR);
	save_state(peer, observer, directory, prefix, "ru-upper");
	press(peer, observer, KEY_NUMERIC_POUND);
	save_state(peer, observer, directory, prefix, "ru-lower");
	press(peer, observer, KEY_NUMERIC_POUND);
	hold(peer, observer, KEY_NUMERIC_POUND);
	save_state(peer, observer, directory, prefix, "numeric");
	hold(peer, observer, KEY_NUMERIC_POUND);
	assert(poll(&input, 1, 0) == 0);
	assert(write(control, "H", 1) == 1);
	wait_shell(peer, control, 'H');
	open_option(peer, session, observer, 3);
	wait_legends(peer, session, "Options", "Enter", "Back");
	save_state(peer, observer, directory, prefix, "history-zero");
	press_repeated(peer, observer, KEY_LEFT, 96);
	save_state(peer, observer, directory, prefix, "history-max");
	press(peer, observer, KEY_F14);
	wait_legends(peer, session, "Options", "Enter", "Clear");
	save_state(peer, observer, directory, prefix, "live-after-history");
	/* These changes consume no PTY input and leave the next font in abc. */
	press(peer, observer, KEY_F14);
	hold(peer, observer, KEY_NUMERIC_STAR);
	press(peer, observer, KEY_NUMERIC_POUND);
	assert(poll(&input, 1, 0) == 0);
}

static void capture_pending_letter(struct fplinux_ui_frame_peer *peer,
				   int observer, int control, bool first_font)
{
	if (first_font) {
		/* Production multi-tap sends ec; h remains uncommitted. */
		press_repeated(peer, observer, KEY_NUMERIC_3, 2);
		press(peer, observer, KEY_NUMERIC_2);
		wait_shell(peer, control, 'e');
		press_repeated(peer, observer, KEY_NUMERIC_2, 2);
		press(peer, observer, KEY_NUMERIC_4);
		wait_shell(peer, control, 'c');
		press(peer, observer, KEY_NUMERIC_4);
	} else {
		press_repeated(peer, observer, KEY_NUMERIC_4, 2);
	}
	{
		struct pollfd input = { .fd = control, .events = POLLIN };

		assert(poll(&input, 1, 0) == 0);
	}
}

static void screen_sample(struct fplinux_ui_frame_peer *peer, int observer,
			  int control, char request)
{
	uint64_t deadline = fplinux_ui_frame_now_ms() + 3000;
	unsigned int first = request == 'P' || request == 'L' ? 'g' :
			     request == 'E'		      ? 'W' :
								' ';
	unsigned int background = request == 'I' ? 0xdedfde :
				  request == 'B' ? 0xffffff :
						   0x101010;

	assert(write(control, &request, 1) == 1);
	wait_shell(peer, control, request);
	do {
		assert(fplinux_ui_frame_now_ms() < deadline);
		wait_frame(peer, observer);
	} while (observation.terminal_first_character != first ||
		 observation.terminal_first_background != background);
}

static void capture_rendering(struct fplinux_ui_frame_peer *peer, int observer,
			      int control, const char *directory,
			      const char *prefix)
{
	static const struct {
		const char *name;
		unsigned int key;
		unsigned int presses;
	} letters[] = {
		{ "g", KEY_NUMERIC_4, 1 },	     { "p", KEY_NUMERIC_7, 1 },
		{ "q", KEY_NUMERIC_7, 2 },	     { "y", KEY_NUMERIC_9, 3 },
		{ "underscore", KEY_NUMERIC_1, 14 },
	};
	struct pollfd input = { .fd = control, .events = POLLIN };
	char state[64];

	screen_sample(peer, observer, control, 'P');
	save_state(peer, observer, directory, prefix, "reference");
	for (size_t index = 0; index < sizeof(letters) / sizeof(letters[0]);
	     ++index) {
		press_repeated(peer, observer, letters[index].key,
			       letters[index].presses);
		assert(snprintf(state, sizeof(state), "pending-%s",
				letters[index].name) < (int)sizeof(state));
		save_state(peer, observer, directory, prefix, state);
		assert(poll(&input, 1, 0) == 0);
		press(peer, observer, KEY_F14);
		assert(!observation.terminal_preedit);
		assert(poll(&input, 1, 0) == 0);
	}
	for (unsigned int bright = 0; bright < 2; ++bright) {
		screen_sample(peer, observer, control, bright ? 'B' : 'I');
		/* Cancelling composition resets the blink without sending a key. */
		press(peer, observer, KEY_NUMERIC_2);
		press(peer, observer, KEY_F14);
		save_state(peer, observer, directory, prefix,
			   bright ? "white-cursor" : "inverse-cursor");
		assert(observation.terminal_cursor_visible);
		assert(poll(&input, 1, 0) == 0);
	}
	for (unsigned int bottom = 0; bottom < 2; ++bottom) {
		unsigned int cursor_y = bottom ? observation.terminal_rows - 1 :
						 0;

		screen_sample(peer, observer, control, bottom ? 'L' : 'E');
		press(peer, observer, KEY_NUMERIC_2);
		press(peer, observer, KEY_F14);
		save_state(peer, observer, directory, prefix,
			   bottom ? "bottom-before" : "edge-before");
		assert(observation.terminal_cursor_x ==
		       observation.terminal_columns);
		assert(observation.terminal_cursor_y == cursor_y);
		press(peer, observer, KEY_NUMERIC_2);
		save_state(peer, observer, directory, prefix,
			   bottom ? "bottom-pending" : "edge-pending");
		assert(observation.terminal_preedit == 'a');
		assert(observation.terminal_cursor_x ==
		       observation.terminal_columns);
		assert(observation.terminal_cursor_y == cursor_y);
		if (bottom)
			assert(observation.terminal_edge_character == 'Z');
		press(peer, observer, KEY_F14);
		save_state(peer, observer, directory, prefix,
			   bottom ? "bottom-cancel" : "edge-cancel");
		assert(!observation.terminal_preedit);
		assert(observation.terminal_cursor_x ==
		       observation.terminal_columns);
		assert(observation.terminal_cursor_y == cursor_y);
		assert(poll(&input, 1, 0) == 0);
	}
}

static void capture_showcase(struct fplinux_ui_frame_peer *peer, int session,
			     int observer, const char *directory)
{
	save(peer, observer, directory, "terminal-keypad");
	press(peer, observer, KEY_F13);
	wait_legends(peer, session, "", "Select", "Back");
	save(peer, observer, directory, "terminal-menu");
	press(peer, observer, KEY_OK);
	wait_legends(peer, session, "Done", "Mark", "Back");
	press(peer, observer, KEY_OK);
	wait_legends(peer, session, "Done", "Unmark", "Back");
	save(peer, observer, directory, "terminal-modifiers");
	/* Back discards the unconfirmed mark and retains the pending h. */
	press(peer, observer, KEY_F14);
	wait_legends(peer, session, "", "Select", "Back");
	press(peer, observer, KEY_F14);
	wait_legends(peer, session, "Options", "Enter", "Clear");
}

static int dimension(const char *text)
{
	char *end;
	long value = strtol(text, &end, 10);

	assert(*text && !*end && value > 0 && value <= 1024);
	return (int)value;
}

static void save_dimensions(const struct fplinux_ui_frame_peer *peer,
			    const char *directory)
{
	char path[512];
	FILE *file;

	assert(snprintf(path, sizeof(path), "%s/frame-size", directory) <
	       (int)sizeof(path));
	file = fopen(path, "w");
	assert(file);
	assert(fprintf(file, "%d %d\n", peer->width, peer->height) > 0);
	assert(!fclose(file));
}

static void finish(struct fplinux_ui_frame_peer *peer, int session,
		   pid_t application)
{
	struct fplinux_ui_session_message close = {
		.type = FPLINUX_UI_SESSION_CLOSE,
	};
	uint64_t deadline = fplinux_ui_frame_now_ms() + 3000;
	pid_t finished;
	int status;

	strcpy(close.reason, "capture-complete");
	assert(fplinux_ui_session_send(session, &close));
	do {
		fplinux_ui_frame_pump(peer);
		finished = waitpid(application, &status, WNOHANG);
		assert(finished >= 0);
	} while (!finished && fplinux_ui_frame_now_ms() < deadline);
	assert(finished == application);
	assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

int main(int argc, char **argv)
{
	struct fplinux_ui_frame_peer peer;
	struct fplinux_ui_session_message config = {
		.type = FPLINUX_UI_SESSION_CONFIG,
		.mode = FPLINUX_UI_SESSION_INTEGRATED,
		.theme = FPLINUX_UI_SESSION_THEME_DARK,
	};
	const struct fplinux_ui_profile_spec *profile;
	struct fplinux_ui_session_message hello;
	int wayland[2], control[2], observer[2];
	int listener, session;
	char descriptor[32];
	char state_directory[512];
	char prefix[128];
	unsigned int columns, rows, other_columns, other_rows;
	pid_t application;
	bool showcase;

	if (argc != 13) {
		fprintf(stderr,
			"usage: %s WIDTH HEIGHT TERMINAL KEYMAP SESSION OUTPUT ISSUE HOSTNAME LANGUAGES THEME LOCALE CATALOGUES\n",
			argv[0]);
		return 2;
	}
	config.screen_width = dimension(argv[1]);
	config.screen_height = dimension(argv[2]);
	if (!strcmp(argv[10], "light"))
		config.theme = FPLINUX_UI_SESSION_THEME_LIGHT;
	else
		assert(!strcmp(argv[10], "dark"));
	russian = !strcmp(argv[11], "ru");
	assert(russian || !strcmp(argv[11], "en"));
	showcase = !russian && config.theme == FPLINUX_UI_SESSION_THEME_DARK;
	assert(setvbuf(stdout, NULL, _IOLBF, 0) == 0);
	profile = fplinux_ui_profile_spec(fplinux_ui_profile_for_screen(
		config.screen_width, config.screen_height));
	assert(profile);
	if (config.screen_width == 128) {
		columns = 21;
		rows = config.screen_height == 128 ? 6 : 9;
		other_columns = 16;
		other_rows = config.screen_height == 128 ? 4 : 6;
	} else if (config.screen_width == 176) {
		columns = 29;
		rows = 12;
		other_columns = 22;
		other_rows = 9;
	} else if (config.screen_width == 240) {
		columns = 30;
		rows = 15;
		other_columns = 40;
		other_rows = 20;
	} else {
		assert(config.screen_width == 320);
		columns = 40;
		rows = 10;
		other_columns = 53;
		other_rows = 14;
	}
	listener = fplinux_ui_frame_listen_session(argv[5]);
	fplinux_ui_frame_socketpair(wayland);
	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, control) == 0);
	assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, observer) == 0);
	application = fork();
	assert(application >= 0);
	if (!application) {
		close(listener);
		close(wayland[0]);
		close(control[0]);
		close(observer[0]);
		fplinux_ui_frame_client_environment(wayland[1], argv[5]);
		snprintf(descriptor, sizeof(descriptor), "%d", control[1]);
		assert(setenv("FPLINUX_SCREEN_CONTROL_FD", descriptor, 1) == 0);
		snprintf(descriptor, sizeof(descriptor), "%d", observer[1]);
		assert(setenv("FPLINUX_TERMINAL_OBSERVER_FD", descriptor, 1) ==
		       0);
		assert(setenv("FPLINUX_SCREEN_ISSUE", argv[7], 1) == 0);
		assert(setenv("FPLINUX_SCREEN_HOSTNAME", argv[8], 1) == 0);
		assert(setenv("FPLINUX_SCREEN_LANGUAGES", argv[9], 1) == 0);
		assert(setenv("FPLINUX_UI_LOCALE_DIRECTORY", argv[12], 1) == 0);
		assert(snprintf(state_directory, sizeof(state_directory),
				"%s/state",
				argv[6]) < (int)sizeof(state_directory));
		assert(setenv("XDG_STATE_HOME", state_directory, 1) == 0);
		execl(argv[3], argv[3], (char *)NULL);
		_exit(127);
	}
	close(wayland[1]);
	close(control[1]);
	close(observer[1]);
	fplinux_ui_frame_init(&peer, wayland[0], config.screen_width,
			      config.screen_height - profile->status_height -
				      profile->softkey_height,
			      argv[4]);
	save_dimensions(&peer, argv[6]);
	session = fplinux_ui_frame_accept_session(&peer, listener);
	hello = fplinux_ui_frame_wait_message(&peer, session);
	assert(hello.type == FPLINUX_UI_SESSION_HELLO);
	assert(!strcmp(hello.app_id, "org.fplinux.terminal"));
	strcpy(config.locale, argv[11]);
	strcpy(config.input_locale, "en");
	assert(fplinux_ui_session_send(session, &config));
	read_shell_size(&peer, control[0], "default", columns, rows);
	wait_shell(&peer, control[0], 'R');
	wait_legends(&peer, session, "Options", "Enter", "Clear");
	assert(peer.keyboards[0].keyboard && peer.keyboards[1].keyboard);
	fplinux_ui_frame_focus_keyboard(&peer.keyboards[0]);
	fplinux_ui_frame_focus_keyboard(&peer.keyboards[1]);
	if (showcase) {
		capture_symbols(&peer, session, observer[0], control[0],
				argv[6]);
	}
	for (unsigned int font = 0; font < 2; ++font) {
		bool compact = (config.screen_width < 240) != (font != 0);

		assert(snprintf(prefix, sizeof(prefix),
				"terminal-header-%s-%s-%s", argv[11], argv[10],
				compact ? "6x12" : "8x16") <
		       (int)sizeof(prefix));
		press(&peer, observer[0], KEY_NUMERIC_STAR);
		wait_legends(&peer, session, "", "Insert", "Cancel");
		save_state(&peer, observer[0], argv[6], prefix, "symbols");
		if (showcase && font)
			save(&peer, observer[0], argv[6],
			     "terminal-symbols-font-other");
		if (!russian && !font && !showcase)
			save(&peer, observer[0], argv[6],
			     "terminal-symbols-light");
		press(&peer, observer[0], KEY_F14);
		wait_legends(&peer, session, "Options", "Enter", "Clear");
		capture_pending_letter(&peer, observer[0], control[0], !font);
		if (showcase) {
			if (!font)
				capture_showcase(&peer, session, observer[0],
						 argv[6]);
			else
				save(&peer, observer[0], argv[6],
				     "terminal-font-other");
		}
		capture_header(&peer, session, observer[0], control[0], argv[6],
			       prefix);
		assert(snprintf(prefix, sizeof(prefix),
				"terminal-render-%s-%s-%s", argv[11], argv[10],
				compact ? "6x12" : "8x16") <
		       (int)sizeof(prefix));
		capture_rendering(&peer, observer[0], control[0], argv[6],
				  prefix);
		if (!font) {
			change_font(&peer, session, observer[0], control[0],
				    compact);
			read_shell_size(&peer, control[0], "other",
					other_columns, other_rows);
		}
	}
	if (showcase) {
		external_escape(&peer);
		wait_shell(&peer, control[0], '\033');
		check_child_navigation(&peer, session, observer[0], control[0]);
	}
	failed_shell_stays_readable(&peer, session, observer[0], control[0],
				    argv[6], argv[10]);
	finish(&peer, session, application);
	close(control[0]);
	close(observer[0]);
	close(session);
	close(listener);
	printf("terminal-capture %dx%d %s %s: input=ec; both fonts; all modifiers; history=0,512; live restored; close=ok\n",
	       peer.width, peer.height, argv[11], argv[10]);
	fplinux_ui_frame_cleanup(&peer);
	assert(unlink(argv[5]) == 0);
	return 0;
}
