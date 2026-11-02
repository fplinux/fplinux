/* SPDX-License-Identifier: GPL-2.0-only */
#include "fplinux-terminal.h"
#include "fplinux-language.h"
#include "fplinux-ui-screen-model.h"
#include "terminal-help.h"

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
	if (terminal->output_size != size ||
	    memcmp(terminal->output, expected, size)) {
		fputs("expected PTY bytes:", stderr);
		for (size_t index = 0; index < size; ++index)
			fprintf(stderr, " %02x",
				(unsigned char)expected[index]);
		fputs("\nreceived PTY bytes:", stderr);
		for (size_t index = 0; index < terminal->output_size; ++index)
			fprintf(stderr, " %02x",
				(unsigned char)terminal->output[index]);
		fputc('\n', stderr);
	}
	assert(terminal->output_size == size);
	assert(!memcmp(terminal->output, expected, size));
	fplinux_terminal_consume(terminal, size);
}

static void digit(struct fplinux_terminal *terminal, unsigned int key,
		  uint64_t now_ms)
{
	fplinux_terminal_phone(terminal, KEY_NUMERIC_0 + key, true, now_ms);
	fplinux_terminal_phone(terminal, KEY_NUMERIC_0 + key, false,
			       now_ms + 1);
}

static void phone_key(struct fplinux_terminal *terminal, unsigned int code,
		      uint64_t now_ms)
{
	assert(fplinux_terminal_phone(terminal, code, true, now_ms));
	assert(fplinux_terminal_phone(terminal, code, false, now_ms + 1));
}

static void hold_key(struct fplinux_terminal *terminal, unsigned int code,
		     uint64_t now_ms)
{
	assert(fplinux_terminal_phone(terminal, code, true, now_ms));
	assert(fplinux_terminal_phone(terminal, code, false, now_ms + 500));
}

static void
languages_cycle_without_erasing_and_emit_literal_utf8(const char *directory)
{
	struct fplinux_terminal terminal;

	assert(fplinux_terminal_init(&terminal, 12, 4));
	fplinux_terminal_load_languages(&terminal, directory, "en");
	digit(&terminal, 2, 1000);
	fplinux_terminal_tick(&terminal, 1999, true);
	assert(terminal.output_size == 0);
	assert(fplinux_terminal_preedit(&terminal) == 'a');
	fplinux_terminal_tick(&terminal, 2000, true);
	expect_output(&terminal, "a", 1);

	digit(&terminal, 2, 2100);
	digit(&terminal, 2, 2200);
	phone_key(&terminal, KEY_NUMERIC_POUND, 2300);
	expect_output(&terminal, "b", 1);
	digit(&terminal, 2, 2400);
	fplinux_terminal_tick(&terminal, 3400, true);
	expect_output(&terminal, "A", 1);
	phone_key(&terminal, KEY_NUMERIC_POUND, 3500);
	hold_key(&terminal, KEY_NUMERIC_STAR, 3600);
	digit(&terminal, 2, 4200);
	digit(&terminal, 2, 4300);
	fplinux_terminal_tick(&terminal, 5300, true);
	expect_output(&terminal, "\xd0\xb1", 2);
	phone_key(&terminal, KEY_NUMERIC_POUND, 5400);
	digit(&terminal, 8, 5500);
	fplinux_terminal_tick(&terminal, 6500, true);
	expect_output(&terminal, "\xd0\xa8", 2);
	hold_key(&terminal, KEY_NUMERIC_POUND, 6600);
	digit(&terminal, 2, 7200);
	expect_output(&terminal, "2", 1);
	hold_key(&terminal, KEY_NUMERIC_POUND, 7300);
	assert(!strcmp(fplinux_text_input_indicator(&terminal.input), "АБВ"));
	hold_key(&terminal, KEY_NUMERIC_STAR, 7900);
	phone_key(&terminal, KEY_NUMERIC_POUND, 8500);
	digit(&terminal, 2, 8600);
	fplinux_terminal_tick(&terminal, 9600, true);
	expect_output(&terminal, "a", 1);
	/* Shell input remains lowercase even at a new sentence boundary. */
	fplinux_terminal_run(&terminal, FPLINUX_TERMINAL_COMMAND_ENTER);
	expect_output(&terminal, "\r", 1);
	digit(&terminal, 2, 9700);
	fplinux_terminal_tick(&terminal, 10700, true);
	expect_output(&terminal, "a", 1);
	fplinux_terminal_destroy(&terminal);
}

static void covered_input_restarts_deadline_and_abandons_held_gestures(void)
{
	struct fplinux_terminal terminal;

	assert(fplinux_terminal_init(&terminal, 12, 4));
	digit(&terminal, 2, 1000);
	fplinux_terminal_pause(&terminal, true, 1300);
	fplinux_terminal_tick(&terminal, 4000, true);
	assert(!terminal.output_size &&
	       fplinux_terminal_preedit(&terminal) == 'a');
	fplinux_terminal_pause(&terminal, false, 5000);
	fplinux_terminal_tick(&terminal, 5999, true);
	assert(!terminal.output_size);
	fplinux_terminal_tick(&terminal, 6000, true);
	expect_output(&terminal, "a", 1);

	fplinux_terminal_phone(&terminal, KEY_NUMERIC_2, true, 6100);
	fplinux_terminal_pause(&terminal, true, 6200);
	fplinux_terminal_tick(&terminal, 7000, true);
	fplinux_terminal_pause(&terminal, false, 8000);
	fplinux_terminal_phone(&terminal, KEY_NUMERIC_2, false, 8001);
	fplinux_terminal_tick(&terminal, 8999, true);
	expect_output(&terminal, "", 0);
	fplinux_terminal_tick(&terminal, 9000, true);
	expect_output(&terminal, "a", 1);
	fplinux_terminal_phone(&terminal, KEY_F14, true, 9100);
	expect_output(&terminal, "\177", 1);
	fplinux_terminal_pause(&terminal, true, 9200);
	fplinux_terminal_tick(&terminal, 10000, true);
	fplinux_terminal_pause(&terminal, false, 11000);
	fplinux_terminal_phone(&terminal, KEY_F14, false, 11001);
	expect_output(&terminal, "", 0);

	fplinux_terminal_destroy(&terminal);
}

static void deadlines_keep_cursor_input_and_backpressure_independent(void)
{
	struct fplinux_terminal terminal;
	uint64_t deadline;

	assert(fplinux_terminal_init(&terminal, 12, 4));
	terminal.cursor_ms = 1000;
	assert(fplinux_terminal_deadline(&terminal, true, &deadline));
	assert(deadline == 1500);
	fplinux_terminal_tick(&terminal, 1499, true);
	assert(terminal.cursor_visible);
	fplinux_terminal_tick(&terminal, 1500, false);
	assert(!terminal.cursor_visible && !terminal.output_size);
	assert(fplinux_terminal_deadline(&terminal, false, &deadline));
	assert(deadline == 2000);
	digit(&terminal, 2, 1700);
	assert(fplinux_terminal_deadline(&terminal, true, &deadline));
	assert(deadline == 2700);
	assert(!fplinux_terminal_deadline(&terminal, false, &deadline));
	fplinux_terminal_tick(&terminal, 2700, false);
	assert(!terminal.output_size &&
	       fplinux_terminal_preedit(&terminal) == 'a');
	fplinux_terminal_tick(&terminal, 2700, true);
	expect_output(&terminal, "a", 1);
	assert(fplinux_terminal_phone(&terminal, KEY_F14, true, 3000));
	expect_output(&terminal, "\177", 1);
	assert(fplinux_terminal_deadline(&terminal, true, &deadline));
	assert(deadline == 3500);
	fplinux_terminal_tick(&terminal, 3500, true);
	expect_output(&terminal, "\177", 1);
	assert(fplinux_terminal_deadline(&terminal, true, &deadline));
	assert(deadline == 3600);
	fplinux_terminal_pause(&terminal, true, 3550);
	assert(!fplinux_terminal_deadline(&terminal, true, &deadline));
	fplinux_terminal_pause(&terminal, false, 4000);
	fplinux_terminal_feed(&terminal, "\033[?25l", 6);
	assert(!fplinux_terminal_deadline(&terminal, true, &deadline));
	fplinux_terminal_feed(&terminal, "\033[?25h", 6);
	terminal.history = true;
	assert(!fplinux_terminal_deadline(&terminal, true, &deadline));
	terminal.history = false;
	terminal.stopped = true;
	assert(!fplinux_terminal_deadline(&terminal, true, &deadline));
	fplinux_terminal_destroy(&terminal);
}

static void external_keyboard_keeps_its_modifiers_and_phone_oneshot(void)
{
	struct fplinux_terminal terminal;

	assert(fplinux_terminal_init(&terminal, 12, 4));
	fplinux_terminal_set_modifiers(&terminal, TSM_CONTROL_MASK);
	fplinux_terminal_key(&terminal, XKB_KEY_x, 'x', 0, 'x');
	expect_output(&terminal, "x", 1);
	assert(terminal.modifier == TSM_CONTROL_MASK);
	fplinux_terminal_key(&terminal, XKB_KEY_c, 'c', TSM_CONTROL_MASK, 'c');
	expect_output(&terminal, "\003", 1);
	assert(terminal.modifier == TSM_CONTROL_MASK);
	digit(&terminal, 2, 1000);
	fplinux_terminal_tick(&terminal, 2000, true);
	expect_output(&terminal, "\001", 1);
	assert(!terminal.modifier);
	digit(&terminal, 2, 2100);
	fplinux_terminal_tick(&terminal, 3100, true);
	expect_output(&terminal, "a", 1);
	fplinux_terminal_set_modifiers(&terminal, TSM_SHIFT_MASK);
	fplinux_terminal_phone(&terminal, KEY_PICKUP_PHONE, true, 3200);
	expect_output(&terminal, "\033[Z", 3);
	assert(!terminal.modifier);
	fplinux_terminal_destroy(&terminal);
}

static void named_control_shortcuts_ignore_unspent_phone_modifiers(void)
{
	static const struct {
		enum fplinux_terminal_command command;
		const char *bytes;
	} shortcuts[] = {
		{ FPLINUX_TERMINAL_COMMAND_INTERRUPT, "\003" },
		{ FPLINUX_TERMINAL_COMMAND_END_INPUT, "\004" },
		{ FPLINUX_TERMINAL_COMMAND_SEARCH, "\022" },
	};
	static const unsigned int modifiers[] = {
		TSM_ALT_MASK,
		TSM_SHIFT_MASK,
		TSM_ALT_MASK | TSM_SHIFT_MASK,
		TSM_CONTROL_MASK | TSM_ALT_MASK | TSM_SHIFT_MASK,
	};
	struct fplinux_terminal terminal;
	uint64_t now_ms = 1000;

	assert(fplinux_terminal_init(&terminal, 12, 4));
	for (size_t shortcut = 0;
	     shortcut < sizeof(shortcuts) / sizeof(shortcuts[0]); ++shortcut) {
		for (size_t modifier = 0;
		     modifier < sizeof(modifiers) / sizeof(modifiers[0]);
		     ++modifier) {
			fplinux_terminal_set_modifiers(&terminal,
						       modifiers[modifier]);
			assert(fplinux_terminal_run(
				&terminal, shortcuts[shortcut].command));
			expect_output(&terminal, shortcuts[shortcut].bytes, 1);
			assert(!terminal.modifier);
		}
		/* A pending phone letter consumes its modifiers before the shortcut. */
		fplinux_terminal_set_modifiers(&terminal,
					       TSM_ALT_MASK | TSM_SHIFT_MASK);
		digit(&terminal, 2, now_ms);
		now_ms += 100;
		assert(fplinux_terminal_run(&terminal,
					    shortcuts[shortcut].command));
		assert(terminal.output_size == 3);
		assert(!memcmp(terminal.output, "\033A", 2));
		assert(terminal.output[2] == shortcuts[shortcut].bytes[0]);
		fplinux_terminal_consume(&terminal, terminal.output_size);
		assert(!fplinux_terminal_preedit(&terminal) &&
		       !terminal.modifier);
	}
	fplinux_terminal_destroy(&terminal);
}

static void digit_hold_symbols_and_clear_have_shared_timing(void)
{
	struct fplinux_terminal terminal;

	assert(fplinux_terminal_init(&terminal, 12, 4));
	assert(fplinux_terminal_phone(&terminal, KEY_NUMERIC_2, true, 1000));
	fplinux_terminal_tick(&terminal, 1499, true);
	expect_output(&terminal, "", 0);
	assert(fplinux_terminal_preedit(&terminal) == 'a');
	fplinux_terminal_tick(&terminal, 1500, true);
	expect_output(&terminal, "2", 1);
	assert(fplinux_terminal_phone(&terminal, KEY_NUMERIC_2, false, 1600));
	assert(fplinux_terminal_phone(&terminal, KEY_NUMERIC_2, true, 1700));
	/* Queued physical events can arrive without an intervening timer tick. */
	assert(fplinux_terminal_phone(&terminal, KEY_NUMERIC_2, false, 2200));
	expect_output(&terminal, "2", 1);
	fplinux_terminal_tick(&terminal, 2600, true);
	expect_output(&terminal, "", 0);

	digit(&terminal, 2, 3000);
	phone_key(&terminal, KEY_NUMERIC_STAR, 3100);
	expect_output(&terminal, "a", 1);
	for (unsigned int index = 0; index < 34; ++index)
		phone_key(&terminal, KEY_RIGHT, 3200 + index * 2);
	phone_key(&terminal, KEY_OK, 3300);
	expect_output(&terminal, "\xe2\x82\xac", 3);
	digit(&terminal, 2, 3400);
	fplinux_terminal_tick(&terminal, 4400, true);
	expect_output(&terminal, "a", 1);
	phone_key(&terminal, KEY_NUMERIC_STAR, 4500);
	for (unsigned int index = 0; index < 36; ++index)
		phone_key(&terminal, KEY_RIGHT, 4510 + index * 2);
	phone_key(&terminal, KEY_OK, 4590);
	expect_output(&terminal, "`", 1);
	phone_key(&terminal, KEY_NUMERIC_STAR, 4600);
	for (unsigned int index = 0; index < 37; ++index)
		phone_key(&terminal, KEY_RIGHT, 4610 + index * 2);
	phone_key(&terminal, KEY_OK, 4690);
	expect_output(&terminal, "^", 1);

	assert(fplinux_terminal_phone(&terminal, KEY_F14, true, 5000));
	expect_output(&terminal, "\177", 1);
	fplinux_terminal_tick(&terminal, 5499, true);
	expect_output(&terminal, "", 0);
	fplinux_terminal_tick(&terminal, 5500, true);
	expect_output(&terminal, "\177", 1);
	fplinux_terminal_tick(&terminal, 5599, true);
	expect_output(&terminal, "", 0);
	fplinux_terminal_tick(&terminal, 5600, true);
	expect_output(&terminal, "\177", 1);
	assert(fplinux_terminal_phone(&terminal, KEY_F14, false, 5601));
	fplinux_terminal_tick(&terminal, 6000, true);
	expect_output(&terminal, "", 0);
	fplinux_terminal_destroy(&terminal);
}

static void blocked_mode_change_preserves_pending_character_and_mode(void)
{
	struct fplinux_terminal terminal;
	size_t accepted = 0;

	assert(fplinux_terminal_init(&terminal, 12, 4));
	while (accepted < 20000 &&
	       fplinux_terminal_key(&terminal, XKB_KEY_x, 'x', 0, 'x'))
		++accepted;
	assert(accepted && accepted < 20000);
	assert(!terminal.failed && terminal.output_size == accepted);
	digit(&terminal, 2, 1000);
	assert(fplinux_terminal_phone(&terminal, KEY_NUMERIC_POUND, true,
				      1100));
	assert(!fplinux_terminal_phone(&terminal, KEY_NUMERIC_POUND, false,
				       1101));
	fplinux_terminal_tick(&terminal, 2000, true);
	assert(!strcmp(fplinux_text_input_indicator(&terminal.input), "abc"));
	assert(fplinux_terminal_preedit(&terminal) == 'a');
	for (size_t index = 0; index < terminal.output_size; ++index)
		assert(terminal.output[index] == 'x');
	fplinux_terminal_consume(&terminal, terminal.output_size);
	assert(fplinux_terminal_phone(&terminal, KEY_NUMERIC_POUND, false,
				      1101));
	expect_output(&terminal, "a", 1);
	digit(&terminal, 2, 2100);
	fplinux_terminal_tick(&terminal, 3100, true);
	expect_output(&terminal, "A", 1);
	fplinux_terminal_destroy(&terminal);
}

static size_t special_index(const char *label)
{
	for (size_t index = 0; index < fplinux_terminal_special_count();
	     ++index)
		if (!strcmp(fplinux_terminal_special(index), label))
			return index;
	assert(false);
	return 0;
}

static void special_keys_encode_navigation_modifiers_and_pending_text(void)
{
	static const struct {
		const char *label;
		const char *bytes;
	} cases[] = {
		{ "Home", "\033[H" },	   { "End", "\033[F" },
		{ "Delete", "\033[3~" },   { "PageUp", "\033[5~" },
		{ "PageDown", "\033[6~" }, { "Esc", "\033" },
		{ "F1", "\033OP" },
	};
	struct fplinux_terminal terminal;
	size_t accepted = 0;

	assert(fplinux_terminal_init(&terminal, 12, 4));
	assert(!strcmp(fplinux_terminal_special(0), "Esc"));
	for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]);
	     ++index) {
		assert(fplinux_terminal_send_special(
			&terminal, special_index(cases[index].label)));
		expect_output(&terminal, cases[index].bytes,
			      strlen(cases[index].bytes));
	}
	fplinux_terminal_set_modifiers(&terminal, TSM_CONTROL_MASK);
	assert(fplinux_terminal_send_special(&terminal, special_index("Home")));
	expect_output(&terminal, "\033[H", 3);
	assert(!terminal.modifier);
	digit(&terminal, 2, 1000);
	assert(fplinux_terminal_send_special(&terminal, special_index("End")));
	expect_output(&terminal, "a\033[F", 4);
	while (accepted < 20000 &&
	       fplinux_terminal_key(&terminal, XKB_KEY_x, 'x', 0, 'x'))
		++accepted;
	assert(accepted && accepted < 20000);
	fplinux_terminal_set_modifiers(&terminal, TSM_ALT_MASK);
	assert(!fplinux_terminal_send_special(&terminal,
					      special_index("Delete")));
	assert(terminal.modifier == TSM_ALT_MASK);
	fplinux_terminal_consume(&terminal, terminal.output_size);
	assert(fplinux_terminal_send_special(&terminal,
					     special_index("Delete")));
	expect_output(&terminal, "\033\033[3~", 5);
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
	const char *const live[] = {
		"Modifiers",	 "Symbols",	     "Special keys",
		"Scrollback",	 "Interrupt Ctrl+C", "End input Ctrl+D",
		"Search Ctrl+R", "Font size",	     "Help",
		"Exit"
	};
	const char *const history[] = { "Modifiers",
					"Symbols",
					"Special keys",
					"Interrupt Ctrl+C",
					"End input Ctrl+D",
					"Search Ctrl+R",
					"Font size",
					"Help",
					"Exit" };

	assert(fplinux_terminal_init(&terminal, 12, 4));
	expect_command_keys(&terminal, "Clear", live, 10);
	fplinux_terminal_run(&terminal, FPLINUX_TERMINAL_COMMAND_SCROLLBACK);
	expect_command_keys(&terminal, "Back", history, 9);
	assert(terminal.history);
	phone_key(&terminal, KEY_F14, 1000);
	assert(!terminal.history);
	fplinux_terminal_tick(&terminal, 2000, true);
	expect_output(&terminal, "", 0);
	expect_command_keys(&terminal, "Clear", live, 10);
	fplinux_terminal_run(&terminal, FPLINUX_TERMINAL_COMMAND_ENTER);
	fplinux_terminal_run(&terminal, FPLINUX_TERMINAL_COMMAND_INTERRUPT);
	fplinux_terminal_run(&terminal, FPLINUX_TERMINAL_COMMAND_END_INPUT);
	fplinux_terminal_run(&terminal, FPLINUX_TERMINAL_COMMAND_SEARCH);
	expect_output(&terminal, "\r\003\004\022", 4);
	fplinux_terminal_destroy(&terminal);
}

static void translated_exit_help(const char *directory, const char *languages)
{
	struct fplinux_language language;
	bool found = false;

	assert(fplinux_language_load(languages, "ru", &language));
	assert(language.system);
	assert(setlocale(LC_ALL, language.lang));
	assert(bindtextdomain("fplinux-terminal", directory));
	assert(bind_textdomain_codeset("fplinux-terminal", "UTF-8"));
	assert(textdomain("fplinux-terminal"));
	for (size_t index = 0; index < fplinux_terminal_help_count(); ++index) {
		const char *title;
		const char *text;

		fplinux_terminal_help_section(index, &title, &text);
		if (strcmp(title, "Выход"))
			continue;
		assert(!found);
		puts(text);
		found = true;
	}
	assert(found);
}

int main(int argc, char **argv)
{
	assert(argc >= 2);
	if (!strcmp(argv[1], "help")) {
		assert(argc == 4);
		translated_exit_help(argv[2], argv[3]);
		return 0;
	}
	assert(argc == 3 && setlocale(LC_ALL, "C"));
	if (!strcmp(argv[1], "languages"))
		languages_cycle_without_erasing_and_emit_literal_utf8(argv[2]);
	else if (!strcmp(argv[1], "pause"))
		covered_input_restarts_deadline_and_abandons_held_gestures();
	else if (!strcmp(argv[1], "deadlines"))
		deadlines_keep_cursor_input_and_backpressure_independent();
	else if (!strcmp(argv[1], "modifiers"))
		external_keyboard_keeps_its_modifiers_and_phone_oneshot();
	else if (!strcmp(argv[1], "shortcuts"))
		named_control_shortcuts_ignore_unspent_phone_modifiers();
	else if (!strcmp(argv[1], "gestures"))
		digit_hold_symbols_and_clear_have_shared_timing();
	else if (!strcmp(argv[1], "blocked"))
		blocked_mode_change_preserves_pending_character_and_mode();
	else if (!strcmp(argv[1], "special"))
		special_keys_encode_navigation_modifiers_and_pending_text();
	else {
		assert(!strcmp(argv[1], "commands"));
		screen_commands_assign_enter_clear_back_and_options();
	}
	printf("terminal-input %s: OK\n", argv[1]);
	return 0;
}
