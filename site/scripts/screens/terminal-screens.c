/* SPDX-License-Identifier: GPL-2.0-only */
/* Drive the production terminal through keypad states and save its frames. */
#include "fplinux-terminal.h"
#include "screen-frame.h"
#include "terminal-render.h"

#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SHELL_TOKEN "docs"
#define PROMPT_MARKER_EDITING "\033]777;" SHELL_TOKEN ";B\a"
#define PROMPT_MARKER_COMMAND "\033]777;" SHELL_TOKEN ";C\a"

struct screen_session {
	struct fplinux_terminal terminal;
	struct fplinux_font font;
	struct fplinux_terminal_surface surface;
	char prompt[96];
	uint64_t now_ms;
};

static void require(bool condition, const char *state)
{
	if (!condition) {
		fprintf(stderr, "terminal did not reach state: %s\n", state);
		exit(EXIT_FAILURE);
	}
}

static void read_text(const char *path, char *text, size_t size)
{
	FILE *file = fopen(path, "rb");
	size_t length;

	require(file != NULL, path);
	length = fread(text, 1, size - 1, file);
	require(!ferror(file) && feof(file), path);
	fclose(file);
	text[length] = '\0';
}

/* Feed shell output the way the PTY line discipline delivers it (onlcr). */
static void feed_output(struct screen_session *session, const char *text)
{
	for (; *text; ++text) {
		if (*text == '\n')
			fplinux_terminal_feed(&session->terminal, "\r\n", 2);
		else
			fplinux_terminal_feed(&session->terminal, text, 1);
	}
}

static void prompt(struct screen_session *session)
{
	feed_output(session, session->prompt);
	feed_output(session, PROMPT_MARKER_EDITING);
}

static void command(struct screen_session *session, const char *line,
		    const char *output)
{
	feed_output(session, line);
	feed_output(session, "\n" PROMPT_MARKER_COMMAND);
	feed_output(session, output);
	prompt(session);
}

/* Readline echoes printable input; return the queued PTY bytes as that echo. */
static void echo_input(struct screen_session *session)
{
	char bytes[FPLINUX_TERMINAL_OUTPUT_BYTES];
	size_t size = session->terminal.output_size;

	memcpy(bytes, session->terminal.output, size);
	fplinux_terminal_consume(&session->terminal, size);
	fplinux_terminal_feed(&session->terminal, bytes, size);
}

static void press(struct screen_session *session, unsigned int code)
{
	fplinux_terminal_phone(&session->terminal, code, true, false,
			       session->now_ms, true);
	fplinux_terminal_phone(&session->terminal, code, false, false,
			       session->now_ms + 80U, true);
	session->now_ms += 200U;
	echo_input(session);
}

static void wait_ms(struct screen_session *session, unsigned int duration)
{
	session->now_ms += duration;
	fplinux_terminal_tick(&session->terminal, session->now_ms, true);
	echo_input(session);
}

static void save(struct screen_session *session, const char *directory,
		 const char *name)
{
	char path[512];

	require(snprintf(path, sizeof(path), "%s/%s", directory, name) <
			(int)sizeof(path),
		"output path");
	fplinux_terminal_render(&session->terminal, &session->font,
				&session->surface);
	require(screen_frame_write(
			path, session->surface.pixels, session->surface.width,
			session->surface.height, session->surface.stride_bytes),
		path);
}

static unsigned int dimension(const char *text)
{
	char *end;
	unsigned long value = strtoul(text, &end, 10);

	require(*text && !*end && value && value <= 1024U, "screen size");
	return (unsigned int)value;
}

static const char *focused_label(const struct screen_session *session)
{
	return fplinux_terminal_menu_label(&session->terminal,
					   session->terminal.menu_index);
}

int main(int argc, char **argv)
{
	static struct screen_session session = { .now_ms = 1000U };
	char issue[128];
	char hostname[64];
	char hostname_output[66];
	unsigned int columns;
	unsigned int rows;

	if (argc != 7) {
		fprintf(stderr,
			"usage: %s WIDTH HEIGHT FONT.psf ISSUE HOSTNAME OUTPUT-DIRECTORY\n",
			argv[0]);
		return EXIT_FAILURE;
	}
	session.surface.width = dimension(argv[1]);
	session.surface.height = dimension(argv[2]);
	session.surface.stride_bytes =
		session.surface.width * sizeof(*session.surface.pixels);
	session.surface.pixels =
		calloc((size_t)session.surface.width * session.surface.height,
		       sizeof(*session.surface.pixels));
	require(session.surface.pixels != NULL, "frame allocation");
	require(fplinux_font_open(&session.font, argv[3]), argv[3]);
	read_text(argv[4], issue, sizeof(issue));
	read_text(argv[5], hostname, sizeof(hostname));
	hostname[strcspn(hostname, "\n")] = '\0';
	snprintf(session.prompt, sizeof(session.prompt), "root@%s:~# ",
		 hostname);
	snprintf(hostname_output, sizeof(hostname_output), "%s\n", hostname);
	/* Same grid as the terminal program: one font row stays for status. */
	columns = session.surface.width / session.font.width;
	rows = session.surface.height / session.font.height - 1U;
	require(fplinux_terminal_init(&session.terminal, columns, rows,
				      SHELL_TOKEN),
		"terminal init");

	prompt(&session);
	command(&session, "cat /etc/issue", issue);
	command(&session, "hostname", hostname_output);
	command(&session, "uname -s", "Linux\n");
	/* Multi-tap "ec", then "h" stays pending under the cursor. */
	press(&session, KEY_NUMERIC_3);
	press(&session, KEY_NUMERIC_3);
	press(&session, KEY_NUMERIC_2);
	press(&session, KEY_NUMERIC_2);
	press(&session, KEY_NUMERIC_2);
	press(&session, KEY_NUMERIC_4);
	press(&session, KEY_NUMERIC_4);
	require(fplinux_terminal_preedit(&session.terminal) == 'h',
		"pending multi-tap h");
	save(&session, argv[6], "terminal-keypad.rgb565");

	press(&session, KEY_F13);
	require(session.terminal.menu == FPLINUX_TERMINAL_MENU_MAIN &&
			!strcmp(focused_label(&session), "Modifiers"),
		"main menu with Modifiers focused");
	save(&session, argv[6], "terminal-menu.rgb565");

	press(&session, KEY_F14);
	wait_ms(&session, FPLINUX_MULTITAP_TIMEOUT_MS);
	require(session.terminal.menu == FPLINUX_TERMINAL_MENU_CLOSED &&
			!fplinux_terminal_preedit(&session.terminal),
		"closed menu with committed input");
	press(&session, KEY_F13);
	press(&session, KEY_F13);
	press(&session, KEY_NUMERIC_1);
	require(session.terminal.modifier_panel &&
			session.terminal.modifier_selection ==
				TSM_CONTROL_MASK &&
			session.terminal.modifier_index == 0U,
		"modifier panel with Ctrl selected");
	save(&session, argv[6], "terminal-modifiers.rgb565");

	fplinux_terminal_destroy(&session.terminal);
	fplinux_font_close(&session.font);
	free(session.surface.pixels);
	return EXIT_SUCCESS;
}
