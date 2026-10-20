/* SPDX-License-Identifier: GPL-2.0-only */
#include "fplinux-terminal.h"
#include "fplinux-ui-screen-model.h"

#include <assert.h>
#include <linux/input-event-codes.h>
#include <locale.h>
#include <stdio.h>
#include <string.h>
#include <xkbcommon/xkbcommon-keysyms.h>

static void expect_output(struct fplinux_terminal *terminal,
			  const char *expected, size_t size)
{
	assert(!terminal->failed);
	assert(terminal->output_size == size);
	assert(!memcmp(terminal->output, expected, size));
	fplinux_terminal_consume(terminal, size);
}

static void digit(struct fplinux_terminal *terminal, unsigned int key,
		  uint64_t now_ms)
{
	fplinux_terminal_phone(terminal, KEY_NUMERIC_0 + key, true, false,
			       now_ms, true);
	fplinux_terminal_phone(terminal, KEY_NUMERIC_0 + key, false, false,
			       now_ms + 1, true);
}

static void languages_emit_literal_utf8_at_700_ms(const char *directory)
{
	struct fplinux_terminal terminal;

	assert(fplinux_terminal_init(&terminal, 12, 4, "input"));
	fplinux_terminal_load_languages(&terminal, directory, "en");
	assert(terminal.languages.count == 2);
	digit(&terminal, 2, 1000);
	fplinux_terminal_tick(&terminal, 1699, true);
	assert(terminal.output_size == 0);
	assert(fplinux_terminal_preedit(&terminal) == 'a');
	fplinux_terminal_tick(&terminal, 1700, true);
	expect_output(&terminal, "a", 1);

	digit(&terminal, 2, 2000);
	digit(&terminal, 2, 2100);
	fplinux_terminal_select_language(&terminal, 1);
	expect_output(&terminal, "b", 1);
	assert(!strcmp(terminal.languages.language[terminal.language].code,
		       "ru"));
	digit(&terminal, 2, 3000);
	digit(&terminal, 2, 3100);
	fplinux_terminal_tick(&terminal, 3800, true);
	expect_output(&terminal, "\xd0\xb1", 2);
	fplinux_terminal_phone(&terminal, KEY_NUMERIC_STAR, true, false, 4000,
			       true);
	fplinux_terminal_phone(&terminal, KEY_NUMERIC_STAR, false, false, 4010,
			       true);
	digit(&terminal, 8, 4100);
	fplinux_terminal_tick(&terminal, 4800, true);
	expect_output(&terminal, "\xd0\xa8", 2);
	fplinux_terminal_destroy(&terminal);
}

static void covered_input_restarts_deadline_and_abandons_held_gestures(void)
{
	struct fplinux_terminal terminal;

	assert(fplinux_terminal_init(&terminal, 12, 4, "input"));
	digit(&terminal, 2, 1000);
	fplinux_terminal_pause(&terminal, true, 1300);
	fplinux_terminal_tick(&terminal, 4000, true);
	assert(!terminal.output_size &&
	       fplinux_terminal_preedit(&terminal) == 'a');
	fplinux_terminal_pause(&terminal, false, 5000);
	fplinux_terminal_tick(&terminal, 5699, true);
	assert(!terminal.output_size);
	fplinux_terminal_tick(&terminal, 5700, true);
	expect_output(&terminal, "a", 1);

	fplinux_terminal_phone(&terminal, KEY_NUMERIC_STAR, true, false, 6000,
			       true);
	fplinux_terminal_pause(&terminal, true, 6100);
	fplinux_terminal_tick(&terminal, 7000, true);
	fplinux_terminal_pause(&terminal, false, 8000);
	fplinux_terminal_phone(&terminal, KEY_NUMERIC_STAR, false, false, 8001,
			       true);
	assert(terminal.mode == FPLINUX_TERMINAL_LOWER);
	fplinux_terminal_phone(&terminal, KEY_NUMERIC_POUND, true, false, 9000,
			       true);
	fplinux_terminal_pause(&terminal, true, 9100);
	fplinux_terminal_tick(&terminal, 10000, true);
	fplinux_terminal_pause(&terminal, false, 11000);
	fplinux_terminal_phone(&terminal, KEY_NUMERIC_POUND, false, false,
			       11001, true);
	expect_output(&terminal, "", 0);

	fplinux_terminal_destroy(&terminal);
}

static void external_keyboard_keeps_its_modifiers_and_phone_oneshot(void)
{
	struct fplinux_terminal terminal;

	assert(fplinux_terminal_init(&terminal, 12, 4, "input"));
	fplinux_terminal_set_modifiers(&terminal, TSM_CONTROL_MASK);
	fplinux_terminal_key(&terminal, XKB_KEY_x, 'x', 0, 'x');
	expect_output(&terminal, "x", 1);
	assert(terminal.modifier == TSM_CONTROL_MASK);
	fplinux_terminal_key(&terminal, XKB_KEY_c, 'c', TSM_CONTROL_MASK, 'c');
	expect_output(&terminal, "\003", 1);
	assert(terminal.modifier == TSM_CONTROL_MASK);
	digit(&terminal, 2, 1000);
	fplinux_terminal_tick(&terminal, 1700, true);
	expect_output(&terminal, "\001", 1);
	assert(!terminal.modifier);
	digit(&terminal, 2, 2000);
	fplinux_terminal_tick(&terminal, 2700, true);
	expect_output(&terminal, "a", 1);
	fplinux_terminal_set_modifiers(&terminal, TSM_SHIFT_MASK);
	fplinux_terminal_phone(&terminal, KEY_PICKUP_PHONE, true, false, 3000,
			       true);
	expect_output(&terminal, "\033[Z", 3);
	assert(!terminal.modifier);
	fplinux_terminal_destroy(&terminal);
}

static enum fplinux_ui_command_role command_role(int type)
{
	/* The public GUI command types share these roles. */
	switch (type) {
	case 0:
	case 1:
	case 2:
		return FPLINUX_UI_ROLE_BACK;
	case 4:
		return FPLINUX_UI_ROLE_OK;
	case 7:
		return FPLINUX_UI_ROLE_HELP;
	default:
		return FPLINUX_UI_ROLE_OTHER;
	}
}

static const char *
command_label(const struct fplinux_terminal_command_info *info)
{
	if (info->label)
		return info->label;
	if (info->type == FPLINUX_TERMINAL_TYPE_BACK)
		return fplinux_ui_label(FPLINUX_UI_LABEL_BACK);
	if (info->type == FPLINUX_TERMINAL_TYPE_HELP)
		return fplinux_ui_label(FPLINUX_UI_LABEL_HELP);
	assert(info->type == FPLINUX_TERMINAL_TYPE_EXIT);
	return fplinux_ui_label(FPLINUX_UI_LABEL_EXIT);
}

static void expect_command_keys(struct fplinux_terminal *terminal,
				const char *right, const char *const *expected,
				size_t expected_count)
{
	struct fplinux_ui_command_entry entries[16];
	struct fplinux_terminal_command_info info[16];
	struct fplinux_ui_command_keys keys;
	size_t menu[16], count = 0;

	for (int id = 0; id < FPLINUX_TERMINAL_COMMAND_COUNT; ++id)
		if (fplinux_terminal_command(terminal, id, &info[count])) {
			entries[count] = (struct fplinux_ui_command_entry){
				.role = command_role(info[count].type),
				.priority = info[count].priority,
			};
			++count;
		}
	fplinux_ui_command_keys_assign(entries, count, false, false, &keys,
				       menu);
	assert(keys.ok < count &&
	       !strcmp(command_label(&info[keys.ok]), "Enter"));
	assert(keys.right < count &&
	       !strcmp(command_label(&info[keys.right]), right));
	assert(keys.options && keys.left == SIZE_MAX);
	assert(keys.menu_count == expected_count);
	for (size_t i = 0; i < expected_count; ++i)
		assert(!strcmp(command_label(&info[menu[i]]), expected[i]));
}

static void screen_commands_assign_enter_clear_back_and_options(void)
{
	struct fplinux_terminal terminal;
	const char *const live[] = { "Modifiers",
				     "Symbols",
				     "Special keys",
				     "Language: EN",
				     "Scrollback",
				     "Interrupt Ctrl+C",
				     "End input Ctrl+D",
				     "Search Ctrl+R",
				     "Help",
				     "Exit" };
	const char *const history[] = { "Modifiers",
					"Symbols",
					"Special keys",
					"Language: EN",
					"Interrupt Ctrl+C",
					"End input Ctrl+D",
					"Search Ctrl+R",
					"Help",
					"Exit" };

	assert(fplinux_terminal_init(&terminal, 12, 4, "input"));
	expect_command_keys(&terminal, "Clear", live, 10);
	fplinux_terminal_run(&terminal, FPLINUX_TERMINAL_COMMAND_SCROLLBACK);
	expect_command_keys(&terminal, "Back", history, 9);
	fplinux_terminal_run(&terminal, FPLINUX_TERMINAL_COMMAND_LIVE);
	expect_command_keys(&terminal, "Clear", live, 10);
	fplinux_terminal_run(&terminal, FPLINUX_TERMINAL_COMMAND_ENTER);
	fplinux_terminal_run(&terminal, FPLINUX_TERMINAL_COMMAND_INTERRUPT);
	fplinux_terminal_run(&terminal, FPLINUX_TERMINAL_COMMAND_END_INPUT);
	fplinux_terminal_run(&terminal, FPLINUX_TERMINAL_COMMAND_SEARCH);
	expect_output(&terminal, "\r\003\004\022", 4);
	fplinux_terminal_destroy(&terminal);
}

static bool title_fits(const char *text, void *context)
{
	const size_t *capacity = context;

	return strlen(text) <= *capacity;
}

static void
title_preserves_mode_when_name_and_modifiers_do_not_fit(const char *directory)
{
	struct fplinux_terminal terminal;
	char title[128];
	size_t capacity = 100;

	assert(fplinux_terminal_init(&terminal, 12, 4, "input"));
	fplinux_terminal_title(&terminal, title_fits, &capacity, title,
			       sizeof(title));
	assert(!strcmp(title, "Terminal  abc"));
	fplinux_terminal_set_modifiers(&terminal,
				       TSM_CONTROL_MASK | TSM_SHIFT_MASK);
	fplinux_terminal_title(&terminal, title_fits, &capacity, title,
			       sizeof(title));
	assert(!strcmp(title, "Terminal  Ctrl+Shift  abc"));
	capacity = 15;
	fplinux_terminal_title(&terminal, title_fits, &capacity, title,
			       sizeof(title));
	assert(!strcmp(title, "Ctrl+Shift  abc"));
	capacity = 3;
	fplinux_terminal_title(&terminal, title_fits, &capacity, title,
			       sizeof(title));
	assert(!strcmp(title, "abc"));
	fplinux_terminal_set_modifiers(&terminal, 0);
	fplinux_terminal_load_languages(&terminal, directory, "ru");
	capacity = 100;
	fplinux_terminal_title(&terminal, title_fits, &capacity, title,
			       sizeof(title));
	assert(!strcmp(title, "Terminal  абв"));
	capacity = 1;
	fplinux_terminal_title(&terminal, title_fits, &capacity, title,
			       sizeof(title));
	assert(!strcmp(title, "абв"));
	fplinux_terminal_destroy(&terminal);
}

int main(int argc, char **argv)
{
	assert(argc == 3 && setlocale(LC_ALL, "C"));
	if (!strcmp(argv[1], "languages"))
		languages_emit_literal_utf8_at_700_ms(argv[2]);
	else if (!strcmp(argv[1], "pause"))
		covered_input_restarts_deadline_and_abandons_held_gestures();
	else if (!strcmp(argv[1], "modifiers"))
		external_keyboard_keeps_its_modifiers_and_phone_oneshot();
	else if (!strcmp(argv[1], "commands"))
		screen_commands_assign_enter_clear_back_and_options();
	else {
		assert(!strcmp(argv[1], "title"));
		title_preserves_mode_when_name_and_modifiers_do_not_fit(
			argv[2]);
	}
	printf("terminal-input %s: OK\n", argv[1]);
	return 0;
}
