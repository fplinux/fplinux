/* SPDX-License-Identifier: GPL-2.0-only */

#include <assert.h>
#include <errno.h>
#include <linux/input-event-codes.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "fplinux-ui-text-model.h"

static enum fplinux_ui_text_model_result
phone(struct fplinux_ui_text_model *model, unsigned int code, bool pressed,
      uint64_t time)
{
	struct fplinux_ui_text_model_key key = {
		.code = code,
		.pressed = pressed,
	};

	return fplinux_ui_text_model_key(model, &key, time);
}

static void tap(struct fplinux_ui_text_model *model, unsigned int code,
		uint64_t time)
{
	phone(model, code, true, time);
	phone(model, code, false, time + 1);
}

static void taps(struct fplinux_ui_text_model *model, unsigned int code,
		 unsigned int count, uint64_t time)
{
	for (unsigned int index = 0; index < count; ++index)
		tap(model, code, time + 10 * index);
}

static enum fplinux_ui_text_model_result
keyboard(struct fplinux_ui_text_model *model, unsigned int code,
	 const char *text, bool shortcut)
{
	struct fplinux_ui_text_model_key key = {
		.keyboard = true,
		.code = code,
		.pressed = true,
		.shortcut = shortcut,
		.text = text,
	};
	enum fplinux_ui_text_model_result result =
		fplinux_ui_text_model_key(model, &key, 0);

	key.pressed = false;
	fplinux_ui_text_model_key(model, &key, 0);
	return result;
}

static void shows(const struct fplinux_ui_text_model *model,
		  const char *display, size_t caret)
{
	assert(!strcmp(model->display, display));
	assert(fplinux_ui_text_model_caret(model) == caret);
}

static bool pending_at(const struct fplinux_ui_text_model *model, size_t index)
{
	size_t pending;

	return fplinux_ui_text_model_pending(model, &pending) &&
	       pending == index;
}

/* Language files of the fixture directory: en and ru. */
static const char *languages;

/* Modes as # presses from the first, with English preferred for input. */
enum mode {
	SENTENCE,
	LOWER,
	UPPER,
	RUSSIAN_SENTENCE,
	RUSSIAN_LOWER,
	RUSSIAN_UPPER,
	NUMERIC,
};

static void init(struct fplinux_ui_text_model *model, bool multiline,
		 size_t limit, enum mode mode)
{
	assert(fplinux_ui_text_model_init(model, multiline, limit, languages,
					  "en"));
	for (unsigned int press = 0; press < mode; ++press)
		tap(model, KEY_NUMERIC_POUND, 0);
}

static void verify_sentence_case_punctuation_and_space(void)
{
	struct fplinux_ui_text_model model;
	char status[32];

	init(&model, false, 0, SENTENCE);
	taps(&model, KEY_NUMERIC_4, 2, 0);
	shows(&model, "H", 1);
	assert(pending_at(&model, 0));
	assert(phone(&model, KEY_RIGHT, true, 100) ==
	       FPLINUX_UI_TEXT_MODEL_CONSUMED);
	phone(&model, KEY_RIGHT, false, 101);
	shows(&model, "H", 1);
	assert(!pending_at(&model, 0));
	taps(&model, KEY_NUMERIC_4, 3, 200);
	tap(&model, KEY_NUMERIC_1, 300);
	tap(&model, KEY_NUMERIC_0, 400);
	taps(&model, KEY_NUMERIC_6, 3, 500);
	shows(&model, "Hi. O", 5);
	assert(pending_at(&model, 4));
	fplinux_ui_text_model_tick(&model, 1520);
	assert(!pending_at(&model, 4));
	taps(&model, KEY_NUMERIC_5, 2, 1600);
	shows(&model, "Hi. Ok", 6);
	fplinux_ui_text_model_status(&model, status, sizeof(status));
	assert(!strcmp(status, "Abc"));
	assert(!strcmp(fplinux_ui_text_model_indicator(&model), "Abc"));
	fplinux_ui_text_model_cleanup(&model);
}

static void verify_commit_timeout_boundary(void)
{
	struct fplinux_ui_text_model model;
	uint64_t deadline;

	init(&model, false, 0, LOWER);
	assert(!fplinux_ui_text_model_deadline(&model, &deadline));
	tap(&model, KEY_NUMERIC_2, 1000);
	assert(fplinux_ui_text_model_deadline(&model, &deadline));
	assert(deadline == 2000);
	fplinux_ui_text_model_tick(&model, 1999);
	assert(pending_at(&model, 0));
	fplinux_ui_text_model_tick(&model, 2000);
	assert(!pending_at(&model, 0));
	shows(&model, "a", 1);
	assert(!fplinux_ui_text_model_deadline(&model, &deadline));
	/* The same key after the timeout starts a new character. */
	tap(&model, KEY_NUMERIC_2, 2100);
	tap(&model, KEY_NUMERIC_2, 3100);
	shows(&model, "aaa", 3);
	assert(pending_at(&model, 2));
	fplinux_ui_text_model_cleanup(&model);
}

static void verify_long_digit_press(void)
{
	struct fplinux_ui_text_model model;
	uint64_t deadline;

	init(&model, false, 0, UPPER);
	phone(&model, KEY_NUMERIC_2, true, 0);
	assert(fplinux_ui_text_model_deadline(&model, &deadline));
	assert(deadline == 500);
	fplinux_ui_text_model_tick(&model, 499);
	shows(&model, "A", 1);
	fplinux_ui_text_model_tick(&model, 500);
	shows(&model, "2", 1);
	assert(!pending_at(&model, 0));
	assert(phone(&model, KEY_NUMERIC_2, false, 600) ==
	       FPLINUX_UI_TEXT_MODEL_CONSUMED);

	/* A held second press keeps the first character. */
	tap(&model, KEY_NUMERIC_7, 1000);
	phone(&model, KEY_NUMERIC_7, true, 1100);
	fplinux_ui_text_model_tick(&model, 1600);
	phone(&model, KEY_NUMERIC_7, false, 1700);
	shows(&model, "2P7", 3);

	/* Releasing before 500 ms keeps the letter. */
	phone(&model, KEY_NUMERIC_3, true, 2000);
	phone(&model, KEY_NUMERIC_3, false, 2499);
	fplinux_ui_text_model_tick(&model, 2600);
	shows(&model, "2P7D", 4);
	assert(pending_at(&model, 3));

	/* Digits need no long press in 123 mode. */
	fplinux_ui_text_model_set_text(&model, "");
	for (unsigned int press = 0; press < 4; ++press)
		tap(&model, KEY_NUMERIC_POUND, 3000 + press);
	assert(!strcmp(fplinux_ui_text_model_indicator(&model), "123"));
	tap(&model, KEY_NUMERIC_0, 3100);
	tap(&model, KEY_NUMERIC_1, 3110);
	tap(&model, KEY_NUMERIC_1, 3120);
	shows(&model, "011", 3);
	assert(!pending_at(&model, 2));
	fplinux_ui_text_model_cleanup(&model);
}

static void verify_overdue_digit_release(void)
{
	struct fplinux_ui_text_model model;
	uint64_t deadline;

	init(&model, false, 0, UPPER);
	phone(&model, KEY_NUMERIC_2, true, 0);
	assert(phone(&model, KEY_NUMERIC_2, false, 600) ==
	       FPLINUX_UI_TEXT_MODEL_CONSUMED);
	shows(&model, "2", 1);
	assert(!pending_at(&model, 0));
	fplinux_ui_text_model_tick(&model, 600);
	fplinux_ui_text_model_tick(&model, 1600);
	shows(&model, "2", 1);
	assert(!fplinux_ui_text_model_deadline(&model, &deadline));

	phone(&model, KEY_NUMERIC_3, true, 2000);
	phone(&model, KEY_NUMERIC_3, false, 2499);
	fplinux_ui_text_model_tick(&model, 2600);
	shows(&model, "2D", 2);
	assert(pending_at(&model, 1));
	fplinux_ui_text_model_tick(&model, 3000);
	shows(&model, "2D", 2);
	fplinux_ui_text_model_cleanup(&model);
}

static void verify_overdue_phone_zero_release(void)
{
	struct fplinux_ui_text_model model;
	uint64_t deadline;

	init(&model, false, 0, SENTENCE);
	assert(fplinux_ui_text_model_set_type(
		&model, FPLINUX_UI_TEXT_MODEL_PHONE, '.'));
	phone(&model, KEY_NUMERIC_0, true, 0);
	assert(phone(&model, KEY_NUMERIC_0, false, 600) ==
	       FPLINUX_UI_TEXT_MODEL_CONSUMED);
	shows(&model, "+", 1);
	fplinux_ui_text_model_tick(&model, 600);
	fplinux_ui_text_model_tick(&model, 1600);
	shows(&model, "+", 1);
	assert(!fplinux_ui_text_model_deadline(&model, &deadline));
	phone(&model, KEY_NUMERIC_0, true, 2000);
	phone(&model, KEY_NUMERIC_0, false, 2499);
	fplinux_ui_text_model_tick(&model, 2600);
	shows(&model, "+0", 2);
	fplinux_ui_text_model_cleanup(&model);
}

static void cycles_through(const char *directory, const char *preferred,
			   const char *const modes[], size_t count)
{
	struct fplinux_ui_text_model model;

	assert(fplinux_ui_text_model_init(&model, false, 0, directory,
					  preferred));
	for (size_t mode = 0; mode < count; ++mode) {
		assert(!strcmp(fplinux_ui_text_model_indicator(&model),
			       modes[mode]));
		assert(phone(&model, KEY_NUMERIC_POUND, true, 10 * mode) ==
		       FPLINUX_UI_TEXT_MODEL_CONSUMED);
		phone(&model, KEY_NUMERIC_POUND, false, 10 * mode + 1);
	}
	fplinux_ui_text_model_cleanup(&model);
}

/*
 * # steps through each input language's cases, the preferred language first and
 * the others by code, then 123; without language files only English remains.
 */
static void verify_mode_cycle_and_russian(void)
{
	static const char *const english_first[] = {
		"Abc", "abc", "ABC", "Абв", "абв", "АБВ", "123", "Abc",
	};
	static const char *const russian_first[] = {
		"Абв", "абв", "АБВ", "Abc", "abc", "ABC", "123", "Абв",
	};
	static const char *const builtin[] = {
		"Abc", "abc", "ABC", "123", "Abc",
	};
	struct fplinux_ui_text_model model;

	cycles_through(languages, "en", english_first,
		       sizeof(english_first) / sizeof(english_first[0]));
	cycles_through(languages, "ru", russian_first,
		       sizeof(russian_first) / sizeof(russian_first[0]));
	/* An unavailable preference leaves the input languages in code order. */
	cycles_through(languages, "de", english_first,
		       sizeof(english_first) / sizeof(english_first[0]));
	cycles_through("/nonexistent/fplinux", "ru", builtin,
		       sizeof(builtin) / sizeof(builtin[0]));

	/* The first mode types the preferred input language in sentence case. */
	assert(fplinux_ui_text_model_init(&model, false, 0, languages, "ru"));
	taps(&model, KEY_NUMERIC_2, 2, 0);
	shows(&model, "Б", 1);
	fplinux_ui_text_model_cleanup(&model);

	init(&model, false, 0, RUSSIAN_SENTENCE);
	taps(&model, KEY_NUMERIC_9, 4, 0);
	tap(&model, KEY_RIGHT, 100);
	tap(&model, KEY_NUMERIC_3, 200);
	shows(&model, "Яд", 2);
	/* Changing mode commits the pending character in its shown case. */
	tap(&model, KEY_NUMERIC_POUND, 300);
	taps(&model, KEY_NUMERIC_3, 3, 400);
	tap(&model, KEY_NUMERIC_POUND, 500);
	taps(&model, KEY_NUMERIC_3, 3, 600);
	shows(&model, "ЯдёЁ", 4);
	/* Leaving Russian commits too; Latin follows. */
	tap(&model, KEY_NUMERIC_POUND, 700);
	tap(&model, KEY_NUMERIC_POUND, 710);
	taps(&model, KEY_NUMERIC_7, 4, 800);
	shows(&model, "ЯдёЁs", 5);
	fplinux_ui_text_model_cleanup(&model);
}

static void verify_clear_and_repeat(void)
{
	struct fplinux_ui_text_model model;
	uint64_t deadline;

	init(&model, false, 0, LOWER);
	assert(phone(&model, KEY_F14, true, 0) ==
	       FPLINUX_UI_TEXT_MODEL_IGNORED);
	assert(phone(&model, KEY_F14, false, 1) ==
	       FPLINUX_UI_TEXT_MODEL_IGNORED);
	assert(fplinux_ui_text_model_set_text(&model, "абв"));
	assert(phone(&model, KEY_F14, true, 1000) ==
	       FPLINUX_UI_TEXT_MODEL_CONSUMED);
	shows(&model, "аб", 2);
	assert(fplinux_ui_text_model_deadline(&model, &deadline));
	assert(deadline == 1500);
	fplinux_ui_text_model_tick(&model, 1499);
	shows(&model, "аб", 2);
	fplinux_ui_text_model_tick(&model, 1500);
	shows(&model, "а", 1);
	assert(fplinux_ui_text_model_deadline(&model, &deadline));
	assert(deadline == 1600);
	fplinux_ui_text_model_tick(&model, 1600);
	shows(&model, "", 0);
	assert(fplinux_ui_text_model_is_empty(&model));
	/* An emptied field stops repeating; the release stays consumed. */
	assert(!fplinux_ui_text_model_deadline(&model, &deadline));
	assert(phone(&model, KEY_F14, false, 1700) ==
	       FPLINUX_UI_TEXT_MODEL_CONSUMED);
	assert(phone(&model, KEY_F14, true, 1800) ==
	       FPLINUX_UI_TEXT_MODEL_IGNORED);
	phone(&model, KEY_F14, false, 1801);

	/* Clear first removes a pending character. */
	assert(fplinux_ui_text_model_set_text(&model, "x"));
	tap(&model, KEY_NUMERIC_2, 2000);
	shows(&model, "xa", 2);
	tap(&model, KEY_F14, 2010);
	shows(&model, "x", 1);
	assert(!fplinux_ui_text_model_deadline(&model, &deadline));

	/* At the start, Clear is consumed without deleting, never Back. */
	assert(fplinux_ui_text_model_set_text(&model, "xy"));
	tap(&model, KEY_LEFT, 2100);
	tap(&model, KEY_LEFT, 2110);
	shows(&model, "xy", 0);
	assert(phone(&model, KEY_F14, true, 2120) ==
	       FPLINUX_UI_TEXT_MODEL_CONSUMED);
	shows(&model, "xy", 0);
	assert(!fplinux_ui_text_model_deadline(&model, &deadline));
	assert(phone(&model, KEY_F14, false, 2130) ==
	       FPLINUX_UI_TEXT_MODEL_CONSUMED);

	/* Held Clear stops at the start and keeps the text after the caret. */
	assert(fplinux_ui_text_model_set_text(&model, "abc"));
	tap(&model, KEY_LEFT, 3000);
	phone(&model, KEY_F14, true, 3100);
	shows(&model, "ac", 1);
	fplinux_ui_text_model_tick(&model, 3600);
	shows(&model, "c", 0);
	assert(!fplinux_ui_text_model_deadline(&model, &deadline));
	fplinux_ui_text_model_tick(&model, 3700);
	shows(&model, "c", 0);
	assert(phone(&model, KEY_F14, false, 3800) ==
	       FPLINUX_UI_TEXT_MODEL_CONSUMED);
	assert(!fplinux_ui_text_model_is_empty(&model));
	fplinux_ui_text_model_cleanup(&model);
}

static void verify_character_limit_and_counter(void)
{
	struct fplinux_ui_text_model model;
	char status[32];

	init(&model, false, 3, RUSSIAN_LOWER);
	errno = 0;
	assert(!fplinux_ui_text_model_set_text(&model, "abcd"));
	assert(errno == E2BIG);
	errno = 0;
	assert(!fplinux_ui_text_model_set_text(&model, "абвг"));
	assert(errno == E2BIG);
	/* The limit counts characters: six bytes of Cyrillic fit three. */
	assert(fplinux_ui_text_model_set_text(&model, "абв"));
	assert(fplinux_ui_text_model_set_text(&model, "аб"));
	fplinux_ui_text_model_status(&model, status, sizeof(status));
	assert(!strcmp(status, "абв  2/3"));
	/* The pending character counts toward the limit and the counter. */
	tap(&model, KEY_NUMERIC_2, 0);
	shows(&model, "аба", 3);
	fplinux_ui_text_model_status(&model, status, sizeof(status));
	assert(!strcmp(status, "абв  3/3"));
	fplinux_ui_text_model_tick(&model, 1000);
	tap(&model, KEY_NUMERIC_3, 1100);
	shows(&model, "аба", 3);
	assert(!pending_at(&model, 3));
	assert(keyboard(&model, KEY_Z, "z", false) ==
	       FPLINUX_UI_TEXT_MODEL_CONSUMED);
	shows(&model, "аба", 3);
	/* A deletion frees one character. */
	tap(&model, KEY_F14, 1200);
	tap(&model, KEY_NUMERIC_POUND, 1300);
	tap(&model, KEY_NUMERIC_POUND, 1310);
	tap(&model, KEY_NUMERIC_POUND, 1320);
	tap(&model, KEY_NUMERIC_POUND, 1330);
	tap(&model, KEY_NUMERIC_7, 1400);
	shows(&model, "абp", 3);
	fplinux_ui_text_model_status(&model, status, sizeof(status));
	assert(!strcmp(status, "abc  3/3"));
	fplinux_ui_text_model_cleanup(&model);
}

static void verify_symbol_picker(void)
{
	struct fplinux_ui_text_model model;

	init(&model, false, 0, LOWER);
	tap(&model, KEY_NUMERIC_2, 0);
	tap(&model, KEY_NUMERIC_STAR, 10);
	assert(model.input.picker_open);
	shows(&model, "a", 1);
	tap(&model, KEY_RIGHT, 20);
	tap(&model, KEY_DOWN, 30);
	/* The picker is modal: digits and # do nothing. */
	tap(&model, KEY_NUMERIC_5, 40);
	tap(&model, KEY_NUMERIC_POUND, 50);
	assert(!strcmp(fplinux_ui_text_model_indicator(&model), "abc"));
	assert(phone(&model, KEY_F13, true, 60) ==
	       FPLINUX_UI_TEXT_MODEL_CONSUMED);
	phone(&model, KEY_F13, false, 61);
	tap(&model, KEY_OK, 70);
	assert(!model.input.picker_open);
	shows(&model, "a(", 2);

	tap(&model, KEY_NUMERIC_STAR, 100);
	tap(&model, KEY_UP, 110);
	tap(&model, KEY_OK, 120);
	tap(&model, KEY_NUMERIC_STAR, 130);
	tap(&model, KEY_LEFT, 140);
	tap(&model, KEY_OK, 150);
	shows(&model, "a(`^", 4);

	/* The right soft key and * close the picker without inserting. */
	tap(&model, KEY_NUMERIC_STAR, 200);
	tap(&model, KEY_F14, 210);
	assert(!model.input.picker_open);
	tap(&model, KEY_NUMERIC_STAR, 220);
	tap(&model, KEY_NUMERIC_STAR, 230);
	assert(!model.input.picker_open);
	shows(&model, "a(`^", 4);

	/* Keyboard Enter inserts; other keyboard input closes and applies. */
	tap(&model, KEY_NUMERIC_STAR, 300);
	keyboard(&model, KEY_RIGHT, "", false);
	keyboard(&model, KEY_ENTER, "\r", false);
	tap(&model, KEY_NUMERIC_STAR, 310);
	keyboard(&model, KEY_Q, "q", false);
	assert(!model.input.picker_open);
	shows(&model, "a(`^,q", 6);
	fplinux_ui_text_model_cleanup(&model);
}

/* Opens the picker, moves with the arrows in codes and inserts the symbol. */
static void pick(struct fplinux_ui_text_model *model, const unsigned int *codes,
		 size_t count)
{
	tap(model, KEY_NUMERIC_STAR, 0);
	assert(model->input.picker_open);
	for (size_t index = 0; index < count; ++index)
		tap(model, codes[index], 10 + 10 * index);
	tap(model, KEY_OK, 1000);
	assert(!model->input.picker_open);
}

static void picks(unsigned int columns, const unsigned int *codes, size_t count,
		  const char *symbol)
{
	struct fplinux_ui_text_model model;

	init(&model, false, 0, LOWER);
	fplinux_ui_text_model_set_picker_columns(&model, columns);
	pick(&model, codes, count);
	shows(&model, symbol, 1);
	fplinux_ui_text_model_cleanup(&model);
}

static void verify_failed_picker_view(void)
{
	struct fplinux_ui_text_model model;
	unsigned int revision;

	init(&model, false, 0, LOWER);
	assert(fplinux_ui_text_model_set_text(&model, "a"));
	assert(phone(&model, KEY_NUMERIC_STAR, true, 10) ==
	       FPLINUX_UI_TEXT_MODEL_CONSUMED);
	assert(model.input.picker_open);
	revision = model.revision;
	fplinux_ui_text_model_cancel_picker(&model);
	assert(!model.input.picker_open);
	assert(model.focused);
	assert(model.revision != revision);
	shows(&model, "a", 1);
	/* Rollback preserves the routing of the press that opened the picker. */
	assert(phone(&model, KEY_NUMERIC_STAR, false, 11) ==
	       FPLINUX_UI_TEXT_MODEL_CONSUMED);
	tap(&model, KEY_NUMERIC_3, 20);
	shows(&model, "ad", 2);
	revision = model.revision;
	fplinux_ui_text_model_cancel_picker(&model);
	assert(model.revision == revision);
	fplinux_ui_text_model_cleanup(&model);
}

/* Vertical movement keeps its column even with an incomplete final row. */
static void verify_picker_columns(void)
{
	static const unsigned int right_down[] = { KEY_RIGHT, KEY_DOWN };
	static const unsigned int up[] = { KEY_UP };
	static const unsigned int down[] = { KEY_DOWN };
	static const unsigned int down_4[] = { KEY_DOWN, KEY_DOWN, KEY_DOWN,
					       KEY_DOWN };
	static const unsigned int left_up[] = { KEY_LEFT, KEY_UP };
	/* Six columns have a final row containing the two shell symbols. */
	picks(6, right_down, 2, "(");
	picks(6, up, 1, "`");
	picks(6, left_up, 2, "»");
	picks(6, down_4, 4, "[");
	/* Nine columns preserve the same wrap for the incomplete final row. */
	picks(9, right_down, 2, "/");
	picks(9, up, 1, "`");
	picks(9, left_up, 2, "\\");
	picks(9, down_4, 4, "`");
	/* One row: Up and Down stay; one column: they move as Left and Right. */
	picks(38, down, 1, ".");
	picks(1, down, 1, ",");
	picks(1, up, 1, "^");
	/* Other widths work; zero falls back to six columns. */
	picks(7, down, 1, "(");
	picks(0, down, 1, "-");
}

/* Read-only: every press goes to the caller, no status, no picker. */
static void verify_read_only(void)
{
	static const unsigned int phone_codes[] = {
		KEY_NUMERIC_0,
		KEY_NUMERIC_2,
		KEY_NUMERIC_9,
		KEY_NUMERIC_STAR,
		KEY_NUMERIC_POUND,
		KEY_F14,
		KEY_F13,
		KEY_OK,
		KEY_LEFT,
		KEY_RIGHT,
		KEY_UP,
		KEY_DOWN,
	};
	static const unsigned int keyboard_codes[] = {
		KEY_BACKSPACE, KEY_DELETE, KEY_LEFT, KEY_RIGHT, KEY_UP,
		KEY_DOWN,      KEY_HOME,   KEY_END,  KEY_ENTER, KEY_ESC,
	};
	struct fplinux_ui_text_model model;
	unsigned int revision;
	uint64_t deadline;
	char status[32];

	init(&model, false, 10, LOWER);
	assert(fplinux_ui_text_model_set_text(&model, "ab"));
	/* A pending character is committed; nothing stays scheduled. */
	tap(&model, KEY_NUMERIC_2, 0);
	assert(fplinux_ui_text_model_deadline(&model, &deadline));
	revision = model.revision;
	fplinux_ui_text_model_set_read_only(&model, true);
	assert(model.read_only);
	assert(model.revision != revision);
	shows(&model, "aba", 3);
	assert(!pending_at(&model, 2));
	assert(!fplinux_ui_text_model_deadline(&model, &deadline));
	fplinux_ui_text_model_status(&model, status, sizeof(status));
	assert(!strcmp(status, ""));

	revision = model.revision;
	for (size_t index = 0;
	     index < sizeof(phone_codes) / sizeof(phone_codes[0]); ++index) {
		assert(phone(&model, phone_codes[index], true, 100) ==
		       FPLINUX_UI_TEXT_MODEL_IGNORED);
		assert(!model.input.picker_open);
		assert(!fplinux_ui_text_model_deadline(&model, &deadline));
		assert(phone(&model, phone_codes[index], false, 2000) ==
		       FPLINUX_UI_TEXT_MODEL_IGNORED);
	}
	for (size_t index = 0;
	     index < sizeof(keyboard_codes) / sizeof(keyboard_codes[0]);
	     ++index)
		assert(keyboard(&model, keyboard_codes[index], "", false) ==
		       FPLINUX_UI_TEXT_MODEL_IGNORED);
	assert(keyboard(&model, KEY_X, "x", false) ==
	       FPLINUX_UI_TEXT_MODEL_IGNORED);
	assert(keyboard(&model, KEY_2, "2", false) ==
	       FPLINUX_UI_TEXT_MODEL_IGNORED);
	fplinux_ui_text_model_tick(&model, 5000);
	assert(model.revision == revision);
	shows(&model, "aba", 3);
	/* # changed no mode. */
	assert(!strcmp(fplinux_ui_text_model_indicator(&model), "abc"));

	/* The text can still be set, within the type and the limit. */
	assert(fplinux_ui_text_model_set_text(&model, "read only"));
	shows(&model, "read only", 9);
	errno = 0;
	assert(!fplinux_ui_text_model_set_text(&model, "longer than ten"));
	assert(errno == E2BIG);
	fplinux_ui_text_model_status(&model, status, sizeof(status));
	assert(!strcmp(status, ""));

	/* Editable again: typing, status and the picker return. */
	fplinux_ui_text_model_set_read_only(&model, false);
	assert(!model.read_only);
	tap(&model, KEY_NUMERIC_2, 6000);
	shows(&model, "read onlya", 10);
	fplinux_ui_text_model_status(&model, status, sizeof(status));
	assert(!strcmp(status, "abc  10/10"));
	tap(&model, KEY_NUMERIC_STAR, 6100);
	assert(model.input.picker_open);
	/* Read-only closes an open picker without inserting. */
	fplinux_ui_text_model_set_read_only(&model, true);
	assert(!model.input.picker_open);
	shows(&model, "read onlya", 10);
	fplinux_ui_text_model_cleanup(&model);

	/* The release of a press consumed before stays consumed. */
	init(&model, false, 0, LOWER);
	assert(phone(&model, KEY_NUMERIC_5, true, 0) ==
	       FPLINUX_UI_TEXT_MODEL_CONSUMED);
	fplinux_ui_text_model_set_read_only(&model, true);
	assert(phone(&model, KEY_NUMERIC_5, false, 10) ==
	       FPLINUX_UI_TEXT_MODEL_CONSUMED);
	assert(phone(&model, KEY_NUMERIC_5, true, 20) ==
	       FPLINUX_UI_TEXT_MODEL_IGNORED);
	assert(phone(&model, KEY_NUMERIC_5, false, 30) ==
	       FPLINUX_UI_TEXT_MODEL_IGNORED);
	shows(&model, "j", 1);
	fplinux_ui_text_model_cleanup(&model);

	/* Multi-line: Up and Down are not caret moves, OK no newline. */
	init(&model, true, 0, LOWER);
	assert(fplinux_ui_text_model_set_text(&model, "one\ntwo"));
	fplinux_ui_text_model_set_read_only(&model, true);
	assert(phone(&model, KEY_UP, true, 0) == FPLINUX_UI_TEXT_MODEL_IGNORED);
	assert(phone(&model, KEY_DOWN, true, 10) ==
	       FPLINUX_UI_TEXT_MODEL_IGNORED);
	assert(phone(&model, KEY_OK, true, 20) ==
	       FPLINUX_UI_TEXT_MODEL_IGNORED);
	assert(keyboard(&model, KEY_ENTER, "\r", false) ==
	       FPLINUX_UI_TEXT_MODEL_IGNORED);
	shows(&model, "one\ntwo", 7);
	fplinux_ui_text_model_cleanup(&model);

	/* Any type: a read-only number has no status either. */
	init(&model, false, 4, SENTENCE);
	assert(fplinux_ui_text_model_set_type(
		&model, FPLINUX_UI_TEXT_MODEL_NUMERIC, '.'));
	fplinux_ui_text_model_set_read_only(&model, true);
	assert(fplinux_ui_text_model_set_text(&model, "-42"));
	assert(phone(&model, KEY_NUMERIC_STAR, true, 0) ==
	       FPLINUX_UI_TEXT_MODEL_IGNORED);
	shows(&model, "-42", 3);
	fplinux_ui_text_model_status(&model, status, sizeof(status));
	assert(!strcmp(status, ""));
	fplinux_ui_text_model_cleanup(&model);
}

static void verify_external_keyboard(void)
{
	struct fplinux_ui_text_model model;

	init(&model, false, 0, SENTENCE);
	keyboard(&model, KEY_G, "П", false);
	keyboard(&model, KEY_H, "р", false);
	keyboard(&model, KEY_J, "и", false);
	shows(&model, "При", 3);
	assert(keyboard(&model, KEY_LEFT, "", false) ==
	       FPLINUX_UI_TEXT_MODEL_CONSUMED);
	keyboard(&model, KEY_BACKSPACE, "\b", false);
	shows(&model, "Пи", 1);
	keyboard(&model, KEY_DELETE, "\177", false);
	shows(&model, "П", 1);
	keyboard(&model, KEY_HOME, "", false);
	shows(&model, "П", 0);
	keyboard(&model, KEY_END, "", false);
	shows(&model, "П", 1);
	assert(keyboard(&model, KEY_C, "c", true) ==
	       FPLINUX_UI_TEXT_MODEL_IGNORED);
	assert(keyboard(&model, KEY_ESC, "\033", false) ==
	       FPLINUX_UI_TEXT_MODEL_IGNORED);
	assert(keyboard(&model, KEY_ENTER, "\r", false) ==
	       FPLINUX_UI_TEXT_MODEL_IGNORED);
	assert(keyboard(&model, KEY_UP, "", false) ==
	       FPLINUX_UI_TEXT_MODEL_IGNORED);
	/* A keyboard key commits a pending multi-tap character first. */
	taps(&model, KEY_NUMERIC_2, 2, 0);
	keyboard(&model, KEY_X, "x", false);
	shows(&model, "Пbx", 3);
	keyboard(&model, KEY_BACKSPACE, "\b", false);
	keyboard(&model, KEY_BACKSPACE, "\b", false);
	keyboard(&model, KEY_BACKSPACE, "\b", false);
	assert(keyboard(&model, KEY_BACKSPACE, "\b", false) ==
	       FPLINUX_UI_TEXT_MODEL_IGNORED);
	fplinux_ui_text_model_cleanup(&model);
}

static void verify_keyboard_shortcuts(void)
{
	static const unsigned int codes[] = {
		KEY_BACKSPACE, KEY_DELETE,  KEY_LEFT, KEY_RIGHT,
		KEY_HOME,      KEY_END,	    KEY_UP,   KEY_DOWN,
		KEY_ENTER,     KEY_KPENTER, KEY_ESC,  KEY_A,
	};
	struct fplinux_ui_text_model model;
	struct fplinux_ui_text_model_key key = {
		.keyboard = true,
		.shortcut = true,
		.text = "a",
	};
	unsigned int revision;
	unsigned int picker_index;

	init(&model, true, 0, LOWER);
	assert(fplinux_ui_text_model_set_text(&model, "ab\ncd"));
	fplinux_ui_text_model_set_caret(&model, 1);
	revision = model.revision;
	for (size_t i = 0; i < sizeof(codes) / sizeof(codes[0]); ++i) {
		key.code = codes[i];
		key.pressed = true;
		assert(fplinux_ui_text_model_key(&model, &key, 0) ==
		       FPLINUX_UI_TEXT_MODEL_IGNORED);
		key.pressed = false;
		assert(fplinux_ui_text_model_key(&model, &key, 1) ==
		       FPLINUX_UI_TEXT_MODEL_IGNORED);
		shows(&model, "ab\ncd", 1);
		assert(model.revision == revision);
	}
	/* A shortcut neither commits multi-tap nor changes a visible picker. */
	assert(fplinux_ui_text_model_set_text(&model, ""));
	tap(&model, KEY_NUMERIC_2, 10);
	revision = model.revision;
	assert(keyboard(&model, KEY_A, "a", true) ==
	       FPLINUX_UI_TEXT_MODEL_IGNORED);
	assert(pending_at(&model, 0));
	assert(model.revision == revision);
	tap(&model, KEY_NUMERIC_STAR, 20);
	picker_index = model.input.picker_index;
	revision = model.revision;
	for (size_t i = 0; i < sizeof(codes) / sizeof(codes[0]); ++i) {
		assert(keyboard(&model, codes[i], "a", true) ==
		       FPLINUX_UI_TEXT_MODEL_IGNORED);
		assert(model.input.picker_open);
		assert(model.input.picker_index == picker_index);
		assert(model.revision == revision);
		shows(&model, "a", 1);
	}
	fplinux_ui_text_model_cancel_picker(&model);
	/* Modifiers can change between a consumed press and its release. */
	key.code = KEY_BACKSPACE;
	key.shortcut = false;
	key.pressed = true;
	assert(fplinux_ui_text_model_key(&model, &key, 30) ==
	       FPLINUX_UI_TEXT_MODEL_CONSUMED);
	key.shortcut = true;
	assert(fplinux_ui_text_model_key(&model, &key, 31) ==
	       FPLINUX_UI_TEXT_MODEL_IGNORED);
	key.pressed = false;
	assert(fplinux_ui_text_model_key(&model, &key, 32) ==
	       FPLINUX_UI_TEXT_MODEL_CONSUMED);
	assert(fplinux_ui_text_model_key(&model, &key, 33) ==
	       FPLINUX_UI_TEXT_MODEL_IGNORED);
	fplinux_ui_text_model_cleanup(&model);
}

static void verify_single_and_multiple_lines(void)
{
	struct fplinux_ui_text_model model;

	init(&model, false, 0, LOWER);
	errno = 0;
	assert(!fplinux_ui_text_model_set_text(&model, "a\nb"));
	assert(errno == EINVAL);
	tap(&model, KEY_NUMERIC_2, 0);
	/* OK is the application's in one line, but it still commits. */
	assert(phone(&model, KEY_OK, true, 10) ==
	       FPLINUX_UI_TEXT_MODEL_IGNORED);
	assert(phone(&model, KEY_OK, false, 11) ==
	       FPLINUX_UI_TEXT_MODEL_IGNORED);
	assert(!pending_at(&model, 0));
	assert(phone(&model, KEY_DOWN, true, 20) ==
	       FPLINUX_UI_TEXT_MODEL_IGNORED);
	shows(&model, "a", 1);
	fplinux_ui_text_model_cleanup(&model);

	init(&model, true, 0, SENTENCE);
	taps(&model, KEY_NUMERIC_2, 1, 0);
	tap(&model, KEY_OK, 10);
	taps(&model, KEY_NUMERIC_2, 1, 20);
	shows(&model, "A\nA", 3);
	assert(phone(&model, KEY_UP, true, 30) ==
	       FPLINUX_UI_TEXT_MODEL_LINE_UP);
	assert(phone(&model, KEY_UP, false, 31) ==
	       FPLINUX_UI_TEXT_MODEL_IGNORED);
	assert(phone(&model, KEY_DOWN, true, 40) ==
	       FPLINUX_UI_TEXT_MODEL_LINE_DOWN);
	keyboard(&model, KEY_ENTER, "\r", false);
	shows(&model, "A\nA\n", 4);
	fplinux_ui_text_model_set_caret(&model, 1);
	keyboard(&model, KEY_END, "", false);
	shows(&model, "A\nA\n", 1);
	fplinux_ui_text_model_set_caret(&model, 99);
	shows(&model, "A\nA\n", 4);
	fplinux_ui_text_model_cleanup(&model);
}

static void verify_caret_moves_by_character(void)
{
	struct fplinux_ui_text_model model;

	init(&model, false, 0, LOWER);
	assert(fplinux_ui_text_model_set_text(&model, "жё€x"));
	fplinux_ui_text_model_set_caret(&model, 2);
	tap(&model, KEY_NUMERIC_2, 0);
	shows(&model, "жёa€x", 3);
	assert(pending_at(&model, 2));
	/* Right first commits only; the next Right moves. */
	tap(&model, KEY_RIGHT, 10);
	shows(&model, "жёa€x", 3);
	tap(&model, KEY_RIGHT, 20);
	shows(&model, "жёa€x", 4);
	tap(&model, KEY_LEFT, 30);
	tap(&model, KEY_LEFT, 40);
	tap(&model, KEY_LEFT, 50);
	tap(&model, KEY_LEFT, 60);
	tap(&model, KEY_LEFT, 70);
	shows(&model, "жёa€x", 0);
	fplinux_ui_text_model_cleanup(&model);
}

static void verify_focus_and_revisions(void)
{
	struct fplinux_ui_text_model model;
	unsigned int revision;
	uint64_t deadline;

	init(&model, false, 0, LOWER);
	revision = model.revision;
	assert(phone(&model, KEY_F13, true, 0) ==
	       FPLINUX_UI_TEXT_MODEL_IGNORED);
	assert(model.revision == revision);
	tap(&model, KEY_NUMERIC_8, 10);
	assert(model.revision != revision);
	phone(&model, KEY_NUMERIC_9, true, 20);
	fplinux_ui_text_model_set_focused(&model, false);
	shows(&model, "tw", 2);
	assert(!pending_at(&model, 1));
	assert(!fplinux_ui_text_model_deadline(&model, &deadline));
	/* The release of a press made before unfocusing is not consumed. */
	assert(phone(&model, KEY_NUMERIC_9, false, 30) ==
	       FPLINUX_UI_TEXT_MODEL_IGNORED);
	assert(phone(&model, KEY_NUMERIC_2, true, 40) ==
	       FPLINUX_UI_TEXT_MODEL_IGNORED);
	shows(&model, "tw", 2);
	fplinux_ui_text_model_set_focused(&model, true);
	tap(&model, KEY_NUMERIC_2, 50);
	shows(&model, "twa", 3);
	fplinux_ui_text_model_cleanup(&model);
}

static void shows_view(const struct fplinux_ui_text_model *model,
		       const char *display, const char *view)
{
	assert(!strcmp(model->display, display));
	assert(!strcmp(fplinux_ui_text_model_view(model), view));
}

/* NUMERIC: digits at once, * toggles a leading minus, # is not taken. */
static void verify_numeric_minus(void)
{
	struct fplinux_ui_text_model model;
	uint64_t deadline;

	init(&model, false, 0, SENTENCE);
	assert(fplinux_ui_text_model_set_type(
		&model, FPLINUX_UI_TEXT_MODEL_NUMERIC, '.'));
	assert(!strcmp(fplinux_ui_text_model_indicator(&model), "123"));
	tap(&model, KEY_NUMERIC_1, 0);
	tap(&model, KEY_NUMERIC_2, 10);
	shows(&model, "12", 2);
	assert(!pending_at(&model, 1));
	tap(&model, KEY_NUMERIC_STAR, 20);
	shows(&model, "-12", 3);
	tap(&model, KEY_NUMERIC_STAR, 30);
	shows(&model, "12", 2);
	/* The caret stays by its digit; typing never goes before the minus. */
	tap(&model, KEY_LEFT, 40);
	tap(&model, KEY_LEFT, 50);
	tap(&model, KEY_NUMERIC_STAR, 60);
	shows(&model, "-12", 1);
	tap(&model, KEY_LEFT, 70);
	shows(&model, "-12", 0);
	tap(&model, KEY_NUMERIC_7, 80);
	shows(&model, "-712", 2);
	assert(phone(&model, KEY_NUMERIC_POUND, true, 90) ==
	       FPLINUX_UI_TEXT_MODEL_IGNORED);
	phone(&model, KEY_NUMERIC_POUND, false, 91);
	assert(!strcmp(fplinux_ui_text_model_indicator(&model), "123"));
	/* A held digit is typed once; there is nothing to wait for. */
	phone(&model, KEY_NUMERIC_3, true, 100);
	assert(!fplinux_ui_text_model_deadline(&model, &deadline));
	fplinux_ui_text_model_tick(&model, 700);
	phone(&model, KEY_NUMERIC_3, false, 800);
	shows(&model, "-7312", 3);
	/* The keyboard types digits and toggles the minus with -. */
	keyboard(&model, KEY_MINUS, "-", false);
	shows(&model, "7312", 2);
	assert(keyboard(&model, KEY_A, "a", false) ==
	       FPLINUX_UI_TEXT_MODEL_IGNORED);
	assert(keyboard(&model, KEY_DOT, ".", false) ==
	       FPLINUX_UI_TEXT_MODEL_IGNORED);
	keyboard(&model, KEY_9, "9", false);
	shows(&model, "73912", 3);

	errno = 0;
	assert(!fplinux_ui_text_model_set_text(&model, "12a"));
	assert(errno == EINVAL);
	assert(!fplinux_ui_text_model_set_text(&model, "1-2"));
	assert(!fplinux_ui_text_model_set_text(&model, "--1"));
	assert(!fplinux_ui_text_model_set_text(&model, "1.5"));
	assert(fplinux_ui_text_model_set_text(&model, "-"));
	assert(fplinux_ui_text_model_set_text(&model, "-40"));
	shows(&model, "-40", 3);
	fplinux_ui_text_model_cleanup(&model);

	/* The minus counts toward the limit. */
	init(&model, false, 2, SENTENCE);
	assert(fplinux_ui_text_model_set_type(
		&model, FPLINUX_UI_TEXT_MODEL_NUMERIC, '.'));
	tap(&model, KEY_NUMERIC_4, 0);
	tap(&model, KEY_NUMERIC_2, 10);
	tap(&model, KEY_NUMERIC_STAR, 20);
	shows(&model, "42", 2);
	tap(&model, KEY_F14, 30);
	tap(&model, KEY_NUMERIC_STAR, 40);
	shows(&model, "-4", 2);
	fplinux_ui_text_model_cleanup(&model);

	/* Text of another type keeps the field as it was. */
	init(&model, false, 0, LOWER);
	assert(fplinux_ui_text_model_set_text(&model, "ab"));
	errno = 0;
	assert(!fplinux_ui_text_model_set_type(
		&model, FPLINUX_UI_TEXT_MODEL_NUMERIC, '.'));
	assert(errno == EINVAL);
	assert(model.type == FPLINUX_UI_TEXT_MODEL_ANY);
	assert(!strcmp(fplinux_ui_text_model_indicator(&model), "abc"));
	assert(!fplinux_ui_text_model_set_type(
		&model, FPLINUX_UI_TEXT_MODEL_DECIMAL, ';'));
	fplinux_ui_text_model_cleanup(&model);
}

/* DECIMAL: * types the one separator; a quick second * means minus. */
static void verify_decimal_separator(void)
{
	struct fplinux_ui_text_model model;

	init(&model, false, 0, SENTENCE);
	assert(fplinux_ui_text_model_set_type(
		&model, FPLINUX_UI_TEXT_MODEL_DECIMAL, ','));
	assert(!strcmp(fplinux_ui_text_model_indicator(&model), "123"));
	tap(&model, KEY_NUMERIC_1, 0);
	tap(&model, KEY_NUMERIC_STAR, 10);
	tap(&model, KEY_NUMERIC_5, 20);
	shows(&model, "1,5", 3);
	/* With a separator in place, * toggles the minus. */
	tap(&model, KEY_NUMERIC_STAR, 30);
	shows(&model, "-1,5", 4);
	tap(&model, KEY_NUMERIC_STAR, 40);
	shows(&model, "1,5", 3);

	/* * * within the multi-tap timeout: no separator, but a minus. */
	assert(fplinux_ui_text_model_set_text(&model, "3"));
	tap(&model, KEY_NUMERIC_STAR, 1000);
	shows(&model, "3,", 2);
	tap(&model, KEY_NUMERIC_STAR, 1100);
	shows(&model, "-3", 2);
	tap(&model, KEY_NUMERIC_STAR, 1200);
	shows(&model, "-3,", 3);
	tap(&model, KEY_NUMERIC_STAR, 1300);
	shows(&model, "3", 1);

	/* After the timeout the second * keeps the separator. */
	tap(&model, KEY_NUMERIC_STAR, 2000);
	tap(&model, KEY_NUMERIC_STAR, 3000);
	shows(&model, "-3,", 3);

	/* So does any change in between, such as a caret move. */
	assert(fplinux_ui_text_model_set_text(&model, "3"));
	tap(&model, KEY_NUMERIC_STAR, 4000);
	tap(&model, KEY_LEFT, 4010);
	tap(&model, KEY_RIGHT, 4020);
	tap(&model, KEY_NUMERIC_STAR, 4030);
	shows(&model, "-3,", 3);

	/* A separator at the start follows the minus: "-,5" means -0.5. */
	assert(fplinux_ui_text_model_set_text(&model, "-5"));
	fplinux_ui_text_model_set_caret(&model, 0);
	tap(&model, KEY_NUMERIC_STAR, 5000);
	shows(&model, "-,5", 2);

	/* Either separator is taken and shown as the field's own. */
	assert(fplinux_ui_text_model_set_text(&model, "2.5"));
	shows(&model, "2,5", 3);
	assert(fplinux_ui_text_model_set_text(&model, ".5"));
	shows(&model, ",5", 2);
	errno = 0;
	assert(!fplinux_ui_text_model_set_text(&model, "1.2.3"));
	assert(errno == EINVAL);
	assert(!fplinux_ui_text_model_set_text(&model, "1,2.3"));
	assert(!fplinux_ui_text_model_set_text(&model, "1e3"));

	/* The keyboard types either separator once, and the minus. */
	assert(fplinux_ui_text_model_set_text(&model, "7"));
	keyboard(&model, KEY_DOT, ".", false);
	keyboard(&model, KEY_COMMA, ",", false);
	keyboard(&model, KEY_2, "2", false);
	keyboard(&model, KEY_MINUS, "-", false);
	shows(&model, "-7,2", 4);
	assert(keyboard(&model, KEY_SPACE, " ", false) ==
	       FPLINUX_UI_TEXT_MODEL_IGNORED);

	/* Another separator converts the text. */
	assert(fplinux_ui_text_model_set_type(
		&model, FPLINUX_UI_TEXT_MODEL_DECIMAL, '.'));
	shows(&model, "-7.2", 4);
	fplinux_ui_text_model_cleanup(&model);
}

/* PHONE: digits, * cycles * + p w, # types #, a held 0 becomes +. */
static void verify_phone_keys(void)
{
	static const char *const cycle[] = { "2*", "2+", "2p", "2w", "2*" };
	struct fplinux_ui_text_model model;
	uint64_t deadline;

	init(&model, false, 0, UPPER);
	assert(fplinux_ui_text_model_set_type(
		&model, FPLINUX_UI_TEXT_MODEL_PHONE, '.'));
	assert(!strcmp(fplinux_ui_text_model_indicator(&model), "123"));
	tap(&model, KEY_NUMERIC_2, 0);
	for (unsigned int press = 0; press < 5; ++press) {
		tap(&model, KEY_NUMERIC_STAR, 100 + 100 * press);
		shows(&model, cycle[press], 2);
		assert(pending_at(&model, 1));
	}
	assert(fplinux_ui_text_model_deadline(&model, &deadline));
	assert(deadline == 1500);
	fplinux_ui_text_model_tick(&model, 1500);
	shows(&model, "2*", 2);
	assert(!pending_at(&model, 1));
	tap(&model, KEY_NUMERIC_POUND, 1600);
	shows(&model, "2*#", 3);
	assert(!strcmp(fplinux_ui_text_model_indicator(&model), "123"));
	/* Another key commits the cycle at once; p and w stay lower case. */
	tap(&model, KEY_NUMERIC_STAR, 1700);
	tap(&model, KEY_NUMERIC_STAR, 1710);
	tap(&model, KEY_NUMERIC_STAR, 1720);
	tap(&model, KEY_NUMERIC_5, 1730);
	shows(&model, "2*#p5", 5);

	/* A held 0 turns into +; a short press stays 0. */
	assert(fplinux_ui_text_model_set_text(&model, ""));
	phone(&model, KEY_NUMERIC_0, true, 2000);
	shows(&model, "0", 1);
	assert(fplinux_ui_text_model_deadline(&model, &deadline));
	assert(deadline == 2500);
	fplinux_ui_text_model_tick(&model, 2499);
	shows(&model, "0", 1);
	fplinux_ui_text_model_tick(&model, 2500);
	shows(&model, "+", 1);
	assert(phone(&model, KEY_NUMERIC_0, false, 2600) ==
	       FPLINUX_UI_TEXT_MODEL_CONSUMED);
	tap(&model, KEY_NUMERIC_0, 2700);
	assert(!fplinux_ui_text_model_deadline(&model, &deadline));
	fplinux_ui_text_model_tick(&model, 3500);
	shows(&model, "+0", 2);
	/* The limit still holds for the * cycle. */
	fplinux_ui_text_model_cleanup(&model);

	init(&model, false, 3, SENTENCE);
	assert(fplinux_ui_text_model_set_type(
		&model, FPLINUX_UI_TEXT_MODEL_PHONE, '.'));
	keyboard(&model, KEY_EQUAL, "+", false);
	keyboard(&model, KEY_P, "P", false);
	assert(keyboard(&model, KEY_X, "x", false) ==
	       FPLINUX_UI_TEXT_MODEL_IGNORED);
	keyboard(&model, KEY_W, "w", false);
	shows(&model, "+pw", 3);
	tap(&model, KEY_NUMERIC_STAR, 0);
	shows(&model, "+pw", 3);
	assert(!pending_at(&model, 3));
	errno = 0;
	assert(!fplinux_ui_text_model_set_text(&model, "1 5"));
	assert(errno == EINVAL);
	assert(fplinux_ui_text_model_set_text(&model, "*#0"));
	fplinux_ui_text_model_cleanup(&model);

	/* ANY returns to the first mode and to multi-tap letters. */
	init(&model, false, 0, SENTENCE);
	assert(fplinux_ui_text_model_set_type(
		&model, FPLINUX_UI_TEXT_MODEL_PHONE, '.'));
	tap(&model, KEY_NUMERIC_STAR, 0);
	assert(fplinux_ui_text_model_set_type(&model, FPLINUX_UI_TEXT_MODEL_ANY,
					      '.'));
	shows(&model, "*", 1);
	assert(!strcmp(fplinux_ui_text_model_indicator(&model), "Abc"));
	taps(&model, KEY_NUMERIC_2, 2, 100);
	shows(&model, "*b", 2);
	fplinux_ui_text_model_cleanup(&model);
}

/* Only a multi-line field of any text takes a new line from OK or Enter. */
static void verify_number_and_phone_lines(void)
{
	static const enum fplinux_ui_text_model_type types[] = {
		FPLINUX_UI_TEXT_MODEL_NUMERIC,
		FPLINUX_UI_TEXT_MODEL_DECIMAL,
		FPLINUX_UI_TEXT_MODEL_PHONE,
	};
	struct fplinux_ui_text_model model;

	for (size_t type = 0; type < sizeof(types) / sizeof(types[0]); ++type) {
		init(&model, true, 0, SENTENCE);
		assert(fplinux_ui_text_model_set_type(&model, types[type],
						      '.'));
		assert(!fplinux_ui_text_model_takes_newline(&model));
		tap(&model, KEY_NUMERIC_1, 0);
		tap(&model, KEY_NUMERIC_2, 10);
		assert(phone(&model, KEY_OK, true, 20) ==
		       FPLINUX_UI_TEXT_MODEL_IGNORED);
		assert(phone(&model, KEY_OK, false, 21) ==
		       FPLINUX_UI_TEXT_MODEL_IGNORED);
		assert(keyboard(&model, KEY_ENTER, "\r", false) ==
		       FPLINUX_UI_TEXT_MODEL_IGNORED);
		assert(keyboard(&model, KEY_KPENTER, "\r", false) ==
		       FPLINUX_UI_TEXT_MODEL_IGNORED);
		shows(&model, "12", 2);
		fplinux_ui_text_model_cleanup(&model);
	}

	/* OK is the application's, but it still commits the * cycle. */
	init(&model, true, 0, SENTENCE);
	assert(fplinux_ui_text_model_set_type(
		&model, FPLINUX_UI_TEXT_MODEL_PHONE, '.'));
	tap(&model, KEY_NUMERIC_2, 0);
	tap(&model, KEY_NUMERIC_STAR, 100);
	tap(&model, KEY_NUMERIC_STAR, 200);
	shows(&model, "2+", 2);
	assert(pending_at(&model, 1));
	assert(phone(&model, KEY_OK, true, 300) ==
	       FPLINUX_UI_TEXT_MODEL_IGNORED);
	phone(&model, KEY_OK, false, 301);
	shows(&model, "2+", 2);
	assert(!pending_at(&model, 1));
	fplinux_ui_text_model_cleanup(&model);

	init(&model, true, 0, LOWER);
	assert(fplinux_ui_text_model_takes_newline(&model));
	tap(&model, KEY_NUMERIC_2, 0);
	assert(phone(&model, KEY_OK, true, 10) ==
	       FPLINUX_UI_TEXT_MODEL_CONSUMED);
	phone(&model, KEY_OK, false, 11);
	assert(keyboard(&model, KEY_KPENTER, "\r", false) ==
	       FPLINUX_UI_TEXT_MODEL_CONSUMED);
	shows(&model, "a\n\n", 3);
	fplinux_ui_text_model_set_read_only(&model, true);
	assert(!fplinux_ui_text_model_takes_newline(&model));
	fplinux_ui_text_model_cleanup(&model);

	init(&model, false, 0, LOWER);
	assert(!fplinux_ui_text_model_takes_newline(&model));
	fplinux_ui_text_model_cleanup(&model);
}

/* PASSWORD: dots for committed characters, the pending one readable. */
static void verify_password_masking(void)
{
	static const char *const modes[] = {
		"abc", "ABC", "абв", "АБВ", "123", "abc",
	};
	struct fplinux_ui_text_model model;

	init(&model, false, 0, SENTENCE);
	fplinux_ui_text_model_set_password(&model, true);
	/* No sentence case: nothing is capitalized on its own. */
	for (size_t mode = 0; mode < sizeof(modes) / sizeof(modes[0]); ++mode) {
		assert(!strcmp(fplinux_ui_text_model_indicator(&model),
			       modes[mode]));
		tap(&model, KEY_NUMERIC_POUND, 10 * mode);
	}
	assert(!strcmp(fplinux_ui_text_model_indicator(&model), "ABC"));
	tap(&model, KEY_NUMERIC_POUND, 100);
	tap(&model, KEY_NUMERIC_POUND, 110);
	tap(&model, KEY_NUMERIC_POUND, 120);
	tap(&model, KEY_NUMERIC_POUND, 130);
	assert(!strcmp(fplinux_ui_text_model_indicator(&model), "abc"));
	shows_view(&model, "", "");
	tap(&model, KEY_NUMERIC_2, 1000);
	shows_view(&model, "a", "a");
	fplinux_ui_text_model_tick(&model, 2000);
	shows_view(&model, "a", "•");
	taps(&model, KEY_NUMERIC_3, 2, 2100);
	shows_view(&model, "ae", "•e");
	tap(&model, KEY_RIGHT, 2200);
	shows_view(&model, "ae", "••");
	/* In the middle of the text the indices of both strings match. */
	tap(&model, KEY_LEFT, 2300);
	tap(&model, KEY_NUMERIC_9, 2400);
	shows_view(&model, "awe", "•w•");
	assert(pending_at(&model, 1));
	keyboard(&model, KEY_X, "X", false);
	shows_view(&model, "awXe", "••••");
	shows(&model, "awXe", 3);
	fplinux_ui_text_model_set_password(&model, false);
	shows_view(&model, "awXe", "awXe");
	fplinux_ui_text_model_cleanup(&model);

	/* A PIN: NUMERIC with PASSWORD masks digits at once. */
	init(&model, false, 4, SENTENCE);
	assert(fplinux_ui_text_model_set_type(
		&model, FPLINUX_UI_TEXT_MODEL_NUMERIC, '.'));
	fplinux_ui_text_model_set_password(&model, true);
	tap(&model, KEY_NUMERIC_1, 0);
	tap(&model, KEY_NUMERIC_9, 10);
	shows_view(&model, "19", "••");
	assert(fplinux_ui_text_model_set_text(&model, "1234"));
	shows_view(&model, "1234", "••••");
	tap(&model, KEY_F14, 20);
	shows_view(&model, "123", "•••");
	fplinux_ui_text_model_cleanup(&model);
}

struct input_sink {
	uint32_t characters[8];
	size_t count;
	bool blocked;
	bool rejected;
	unsigned int remaining;
};

static enum fplinux_multitap_emit_result sink_character(void *context,
							uint32_t character)
{
	struct input_sink *sink = context;

	if (sink->blocked)
		return FPLINUX_MULTITAP_EMIT_BLOCKED;
	if (sink->rejected)
		return FPLINUX_MULTITAP_EMIT_REJECTED;
	assert(sink->count <
	       sizeof(sink->characters) / sizeof(sink->characters[0]));
	sink->characters[sink->count++] = character;
	return FPLINUX_MULTITAP_EMIT_ACCEPTED;
}

static enum fplinux_multitap_emit_result sink_erase(void *context)
{
	struct input_sink *sink = context;

	if (sink->blocked)
		return FPLINUX_MULTITAP_EMIT_BLOCKED;
	if (!sink->remaining)
		return FPLINUX_MULTITAP_EMIT_REJECTED;
	--sink->remaining;
	return FPLINUX_MULTITAP_EMIT_ACCEPTED;
}

static void verify_shared_input_blocked_events_can_be_retried(void)
{
	struct fplinux_text_input input;
	struct input_sink sink = { 0 };
	char label[FPLINUX_TEXT_INPUT_UTF8_BYTES];
	uint64_t deadline;

	fplinux_text_input_init(&input, NULL, NULL, false);
	fplinux_text_input_press(&input, '2', 0, sink_character, &sink);
	fplinux_text_input_release(&input, '2', 1, sink_character, &sink);
	sink.blocked = true;
	assert(fplinux_text_input_press(&input, '3', 100, sink_character,
					&sink) == FPLINUX_MULTITAP_BLOCKED);
	assert(fplinux_multitap_candidate(&input.compose) == 'a');
	assert(fplinux_text_input_deadline(&input, &deadline));
	assert(deadline == 1000);
	assert(sink.count == 0);
	sink.blocked = false;
	fplinux_text_input_press(&input, '3', 100, sink_character, &sink);
	fplinux_text_input_release(&input, '3', 101, sink_character, &sink);
	assert(sink.count == 1 && sink.characters[0] == 'a');
	assert(fplinux_multitap_candidate(&input.compose) == 'd');
	sink.blocked = true;
	assert(fplinux_text_input_next_mode(&input, sink_character, &sink) ==
	       FPLINUX_MULTITAP_BLOCKED);
	assert(!strcmp(fplinux_text_input_indicator(&input), "abc"));
	assert(fplinux_multitap_candidate(&input.compose) == 'd');
	assert(fplinux_text_input_open_picker(&input, sink_character, &sink) ==
	       FPLINUX_MULTITAP_BLOCKED);
	assert(!input.picker_open);
	sink.blocked = false;
	fplinux_text_input_open_picker(&input, sink_character, &sink);
	assert(sink.count == 2 && sink.characters[1] == 'd');
	input.picker_index = 34;
	assert(!strcmp(fplinux_text_input_symbol_label(34, label), "€"));
	sink.blocked = true;
	assert(fplinux_text_input_select_symbol(&input, sink_character,
						&sink) ==
	       FPLINUX_MULTITAP_EMIT_BLOCKED);
	assert(input.picker_open && input.picker_index == 34);
	sink.blocked = false;
	fplinux_text_input_select_symbol(&input, sink_character, &sink);
	assert(!input.picker_open);
	assert(sink.count == 3 && sink.characters[2] == 0x20ac);
	fplinux_text_input_reset_mode(&input, true);
	sink.blocked = true;
	assert(fplinux_text_input_press(&input, '9', 200, sink_character,
					&sink) == FPLINUX_MULTITAP_BLOCKED);
	assert(sink.count == 3);
	sink.blocked = false;
	fplinux_text_input_press(&input, '9', 200, sink_character, &sink);
	assert(sink.count == 4 && sink.characters[3] == '9');
}

static void verify_shared_clear_deadlines_preserve_blocked_repeat(void)
{
	struct fplinux_text_input input;
	struct input_sink sink = { .remaining = 4 };
	uint64_t deadline;

	fplinux_text_input_init(&input, NULL, NULL, false);
	fplinux_text_input_clear_press(&input, 100, sink_erase, &sink);
	assert(sink.remaining == 3);
	assert(fplinux_text_input_deadline(&input, &deadline) &&
	       deadline == 600);
	sink.blocked = true;
	assert(fplinux_text_input_tick(&input, 600, sink_character, sink_erase,
				       &sink) == FPLINUX_MULTITAP_BLOCKED);
	assert(sink.remaining == 3);
	assert(fplinux_text_input_deadline(&input, &deadline) &&
	       deadline == 600);
	sink.blocked = false;
	fplinux_text_input_tick(&input, 600, sink_character, sink_erase, &sink);
	assert(sink.remaining == 2);
	assert(fplinux_text_input_deadline(&input, &deadline) &&
	       deadline == 700);
	fplinux_text_input_clear_release(&input);
	fplinux_text_input_tick(&input, 900, sink_character, sink_erase, &sink);
	assert(sink.remaining == 2);
	assert(!fplinux_text_input_deadline(&input, &deadline));
}

static void verify_shared_release_preserves_blocked_and_rejected_emission(void)
{
	struct fplinux_text_input input;
	struct input_sink sink = { .blocked = true };
	uint64_t deadline;

	fplinux_text_input_init(&input, NULL, NULL, false);
	fplinux_text_input_press(&input, '2', 0, sink_character, &sink);
	assert(fplinux_text_input_release(&input, '2', 600, sink_character,
					  &sink) == FPLINUX_MULTITAP_BLOCKED);
	assert(!sink.count);
	assert(fplinux_text_input_deadline(&input, &deadline));
	assert(deadline == 500);
	assert(fplinux_multitap_candidate(&input.compose) == '2');
	sink.blocked = false;
	assert(fplinux_text_input_release(&input, '2', 600, sink_character,
					  &sink) == FPLINUX_MULTITAP_COMMITTED);
	fplinux_text_input_tick(&input, 1600, sink_character, sink_erase,
				&sink);
	assert(sink.count == 1 && sink.characters[0] == '2');
	assert(!fplinux_text_input_deadline(&input, &deadline));

	fplinux_text_input_press(&input, '3', 2000, sink_character, &sink);
	sink.rejected = true;
	assert(fplinux_text_input_release(&input, '3', 2600, sink_character,
					  &sink) == FPLINUX_MULTITAP_REJECTED);
	sink.rejected = false;
	fplinux_text_input_tick(&input, 3600, sink_character, sink_erase,
				&sink);
	assert(sink.count == 1 && sink.characters[0] == '2');
	assert(!fplinux_text_input_deadline(&input, &deadline));
}

static void verify_cancelled_hold_does_not_edit_on_synthetic_release(void)
{
	struct fplinux_ui_text_model model;

	init(&model, false, 0, UPPER);
	phone(&model, KEY_NUMERIC_2, true, 0);
	fplinux_ui_text_model_cancel_held(&model);
	phone(&model, KEY_NUMERIC_2, false, 600);
	fplinux_ui_text_model_tick(&model, 600);
	shows(&model, "A", 1);
	fplinux_ui_text_model_tick(&model, 1000);
	shows(&model, "A", 1);
	assert(!pending_at(&model, 0));
	assert(fplinux_ui_text_model_set_text(&model, "abc"));
	phone(&model, KEY_F14, true, 2000);
	fplinux_ui_text_model_cancel_held(&model);
	phone(&model, KEY_F14, false, 2600);
	fplinux_ui_text_model_tick(&model, 2600);
	shows(&model, "ab", 2);
	assert(fplinux_ui_text_model_set_text(&model, ""));
	assert(fplinux_ui_text_model_set_type(
		&model, FPLINUX_UI_TEXT_MODEL_PHONE, '.'));
	phone(&model, KEY_NUMERIC_0, true, 3000);
	fplinux_ui_text_model_cancel_held(&model);
	phone(&model, KEY_NUMERIC_0, false, 3600);
	fplinux_ui_text_model_tick(&model, 3600);
	shows(&model, "0", 1);
	fplinux_ui_text_model_cleanup(&model);
}

int main(int argc, char **argv)
{
	assert(argc == 2 || argc == 3);
	languages = argv[1];
	if (argc == 3) {
		if (!strcmp(argv[2], "overdue-digit-release"))
			verify_overdue_digit_release();
		else if (!strcmp(argv[2], "overdue-phone-zero-release"))
			verify_overdue_phone_zero_release();
		else
			assert(!"unknown text model scenario");
		return 0;
	}
	verify_sentence_case_punctuation_and_space();
	verify_commit_timeout_boundary();
	verify_long_digit_press();
	verify_mode_cycle_and_russian();
	verify_clear_and_repeat();
	verify_character_limit_and_counter();
	verify_symbol_picker();
	verify_failed_picker_view();
	verify_picker_columns();
	verify_read_only();
	verify_external_keyboard();
	verify_keyboard_shortcuts();
	verify_single_and_multiple_lines();
	verify_caret_moves_by_character();
	verify_focus_and_revisions();
	verify_numeric_minus();
	verify_decimal_separator();
	verify_phone_keys();
	verify_number_and_phone_lines();
	verify_password_masking();
	verify_shared_input_blocked_events_can_be_retried();
	verify_shared_clear_deadlines_preserve_blocked_repeat();
	verify_shared_release_preserves_blocked_and_rejected_emission();
	verify_cancelled_hold_does_not_edit_on_synthetic_release();
	return 0;
}
