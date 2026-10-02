/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "fplinux-drm-session.h"
#include "fplinux-input-session.h"
#include "terminal-keyboard.h"
#include "terminal-pty.h"

#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/*
 * The main loop, renderer and libtsm are real. Device sessions and the shell
 * process are replaced by an in-memory display, scheduled keypad events and a
 * socket peer. A virtual poll clock makes a delayed shell reply deterministic;
 * this does not exercise a kernel PTY, evdev devices or a DRM device.
 */
struct scheduled_key {
	uint64_t time_ms;
	uint64_t delivery_ms;
	unsigned int code;
	bool pressed;
};

static const struct scheduled_key compose_keys[] = {
	{ 1010, 1010, KEY_NUMERIC_2, true },
	{ 1011, 1011, KEY_NUMERIC_2, false },
};

/* Both soft-key events are waiting when the loop resumes at 1700 ms. */
static const struct scheduled_key delayed_hold_keys[] = {
	{ 1010, 1700, KEY_F13, true },
	{ 1610, 1700, KEY_F13, false },
	{ 1710, 1710, KEY_NUMERIC_1, true },
	{ 1711, 1711, KEY_NUMERIC_1, false },
	{ 1720, 1720, KEY_F13, true },
	{ 1721, 1721, KEY_F13, false },
	{ 1730, 1730, KEY_NUMERIC_2, true },
	{ 1731, 1731, KEY_NUMERIC_2, false },
};

/* A short press needs OK to enter Modifiers from the ordinary menu. */
static const struct scheduled_key delayed_tap_keys[] = {
	{ 1010, 1700, KEY_F13, true },
	{ 1110, 1700, KEY_F13, false },
	{ 1702, 1702, KEY_OK, true },
	{ 1703, 1703, KEY_OK, false },
	{ 1710, 1710, KEY_NUMERIC_1, true },
	{ 1711, 1711, KEY_NUMERIC_1, false },
	{ 1720, 1720, KEY_F13, true },
	{ 1721, 1721, KEY_F13, false },
	{ 1730, 1730, KEY_NUMERIC_2, true },
	{ 1731, 1731, KEY_NUMERIC_2, false },
};

static uint64_t now_ms = 1000;
static uint64_t committed_ms;
static uint64_t cleared_ms;
static unsigned int input_index;
static unsigned int polls;
static int peer;
static bool echo_enabled;
static bool echo_sent;
static bool saw_preedit;
static bool saw_echo;
static bool modifier_scenario;
static const struct scheduled_key *input_keys = compose_keys;
static size_t input_count = sizeof(compose_keys) / sizeof(compose_keys[0]);
static const char *keymap_root;

int fplinux_terminal_main(int argc, char **argv);
int fixture_clock_gettime(clockid_t clock, struct timespec *time);
int fixture_poll(struct pollfd *descriptors, nfds_t count, int timeout);
bool __real_fplinux_terminal_keyboard_open(
	struct fplinux_terminal_keyboard *keyboard, const char *data_root,
	char *error, size_t error_size);
bool __wrap_fplinux_terminal_keyboard_open(
	struct fplinux_terminal_keyboard *keyboard, const char *data_root,
	char *error, size_t error_size);

int fixture_clock_gettime(clockid_t clock, struct timespec *time)
{
	assert(clock == CLOCK_MONOTONIC);
	time->tv_sec = (time_t)(now_ms / 1000);
	time->tv_nsec = (long)(now_ms % 1000) * 1000000;
	return 0;
}

int fixture_poll(struct pollfd *descriptors, nfds_t count, int timeout)
{
	uint64_t deadline = 4500;
	int ready;

	assert(++polls < 1000);
	ready = poll(descriptors, count, 0);
	assert(ready >= 0);
	if (ready)
		return ready;
	if (timeout >= 0 && now_ms + (unsigned int)timeout < deadline)
		deadline = now_ms + (unsigned int)timeout;
	if (input_index < input_count &&
	    input_keys[input_index].delivery_ms < deadline)
		deadline = input_keys[input_index].delivery_ms;
	if (committed_ms) {
		if (committed_ms + 40 < deadline)
			deadline = committed_ms + 40;
		if (echo_enabled && !echo_sent && committed_ms + 8 < deadline)
			deadline = committed_ms + 8;
	}
	assert(deadline >= now_ms && deadline < 5000);
	now_ms = deadline;
	if (echo_enabled && committed_ms && !echo_sent &&
	    now_ms >= committed_ms + 8) {
		assert(write(peer, "a", 1) == 1);
		echo_sent = true;
	}
	return poll(descriptors, count, 0);
}

bool fplinux_terminal_pty_open(struct fplinux_terminal_pty *pty,
			       unsigned int columns, unsigned int rows,
			       const char *shell, const char *startup,
			       const char *shell_token)
{
	int pair[2];

	(void)columns;
	(void)rows;
	(void)shell;
	(void)startup;
	(void)shell_token;
	assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, pair) == 0);
	pty->fd = pair[0];
	peer = pair[1];
	/* Keep cursor animation out of the glyph visibility observation. */
	assert(write(peer, "\033[?25l", 6) == 6);
	return true;
}

bool fplinux_terminal_pty_shell_foreground(
	const struct fplinux_terminal_pty *pty)
{
	(void)pty;
	return true;
}

bool fplinux_terminal_pty_finished(struct fplinux_terminal_pty *pty,
				   int *status)
{
	char bytes[16];
	ssize_t size = read(peer, bytes, sizeof(bytes));

	(void)pty;
	if (size > 0) {
		assert(!committed_ms && size == 1);
		assert(bytes[0] == (modifier_scenario ? '\001' : 'a'));
		committed_ms = now_ms;
	} else {
		assert(size < 0 && errno == EAGAIN);
	}
	*status = 0;
	return now_ms >= 4500 || (committed_ms && now_ms >= committed_ms + 40);
}

void fplinux_terminal_pty_close(struct fplinux_terminal_pty *pty)
{
	assert(close(pty->fd) == 0);
	assert(close(peer) == 0);
}

bool fplinux_drm_session_open(struct fplinux_drm_session *session,
			      const char *drm_path, const char *tty_path,
			      uint32_t format, char *error, size_t error_size)
{
	(void)drm_path;
	(void)tty_path;
	(void)error;
	(void)error_size;
	assert(format == DRM_FORMAT_RGB565);
	session->width = 128;
	session->height = 60;
	session->stride = 256;
	session->page_bytes = session->stride * session->height;
	session->mapping = calloc(2, session->page_bytes);
	assert(session->mapping);
	return true;
}

bool fplinux_drm_session_set_active_handler(struct fplinux_drm_session *session,
					    fplinux_drm_active_handler handler,
					    void *data)
{
	return !handler || handler(session, true, data);
}

int fplinux_drm_session_fd(const struct fplinux_drm_session *session)
{
	(void)session;
	return -1;
}

bool fplinux_drm_session_dispatch(struct fplinux_drm_session *session)
{
	(void)session;
	assert(false);
	return false;
}

bool fplinux_drm_session_present(struct fplinux_drm_session *session,
				 unsigned int page)
{
	const uint16_t *pixels = (const uint16_t *)(session->mapping +
						    page * session->page_bytes);
	/* The PSF fixture lights the first pixel of 'a'; space stays dark. */
	bool glyph_visible = pixels[0] != pixels[1];

	session->shown_page = page;
	if (modifier_scenario)
		return true;
	if (!committed_ms && glyph_visible)
		saw_preedit = true;
	if (saw_preedit && echo_enabled) {
		if (!glyph_visible)
			fprintf(stderr, "blank frame at %llu ms after commit\n",
				(unsigned long long)(now_ms - committed_ms));
		assert(glyph_visible);
		if (echo_sent)
			saw_echo = true;
	}
	if (committed_ms && !echo_enabled) {
		assert(!glyph_visible);
		if (!cleared_ms)
			cleared_ms = now_ms;
	}
	return true;
}

bool fplinux_drm_session_close(struct fplinux_drm_session *session)
{
	free(session->mapping);
	return true;
}

bool fplinux_input_session_open(struct fplinux_input_session *session,
				unsigned int sources, char *error,
				size_t error_size)
{
	(void)session;
	(void)sources;
	(void)error;
	(void)error_size;
	return true;
}

bool fplinux_input_session_next(struct fplinux_input_session *session,
				struct fplinux_input_event *event)
{
	const struct scheduled_key *key;

	(void)session;
	if (input_index == input_count ||
	    now_ms < input_keys[input_index].delivery_ms)
		return false;
	key = &input_keys[input_index];
	*event = (struct fplinux_input_event){
		.type = FPLINUX_INPUT_EVENT_KEY,
		.source = FPLINUX_INPUT_SOURCE_KEYPAD,
		.device_id = 1,
		.time_ms = key->time_ms,
		.code = key->code,
		.pressed = key->pressed,
	};
	++input_index;
	return true;
}

int fplinux_input_session_get_fd(const struct fplinux_input_session *session)
{
	(void)session;
	return -1;
}

void fplinux_input_session_suspend(struct fplinux_input_session *session)
{
	(void)session;
	assert(false);
}

bool fplinux_input_session_resume(struct fplinux_input_session *session,
				  char *error, size_t error_size)
{
	(void)session;
	(void)error;
	(void)error_size;
	assert(false);
	return false;
}

void fplinux_input_session_close(struct fplinux_input_session *session)
{
	(void)session;
}

bool __wrap_fplinux_terminal_keyboard_open(
	struct fplinux_terminal_keyboard *keyboard, const char *data_root,
	char *error, size_t error_size)
{
	(void)data_root;
	return __real_fplinux_terminal_keyboard_open(keyboard, keymap_root,
						     error, error_size);
}

int main(int argc, char **argv)
{
	char *arguments[] = { "fplinux-terminal", "--font", NULL, NULL };

	assert(argc == 4);
	arguments[2] = argv[1];
	keymap_root = argv[2];
	echo_enabled = !strcmp(argv[3], "echo");
	if (!strcmp(argv[3], "delayed-hold")) {
		modifier_scenario = true;
		input_keys = delayed_hold_keys;
		input_count = sizeof(delayed_hold_keys) /
			      sizeof(delayed_hold_keys[0]);
	} else if (!strcmp(argv[3], "delayed-tap")) {
		modifier_scenario = true;
		input_keys = delayed_tap_keys;
		input_count =
			sizeof(delayed_tap_keys) / sizeof(delayed_tap_keys[0]);
	} else {
		assert(echo_enabled || !strcmp(argv[3], "no-echo"));
	}
	assert(fplinux_terminal_main(3, arguments) == 0);
	assert(committed_ms);
	if (modifier_scenario) {
		assert(input_index == input_count);
		return 0;
	}
	assert(saw_preedit);
	if (echo_enabled)
		assert(saw_echo);
	else
		assert(cleared_ms >= committed_ms &&
		       cleared_ms - committed_ms <= 35);
	return 0;
}
