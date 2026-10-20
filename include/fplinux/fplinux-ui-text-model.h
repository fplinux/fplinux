/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* LVGL-free editing state of the runtime text field; not installed. */

#ifndef FPLINUX_UI_TEXT_MODEL_H
#define FPLINUX_UI_TEXT_MODEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fplinux-multitap.h"

#define FPLINUX_UI_TEXT_MODEL_COMMIT_MS 1000U
#define FPLINUX_UI_TEXT_MODEL_HOLD_MS 500U
#define FPLINUX_UI_TEXT_MODEL_REPEAT_MS 100U
/* Symbol picker columns until set, and for a count that does not divide. */
#define FPLINUX_UI_TEXT_MODEL_PICKER_COLUMNS 6U
/* Linux KEY_CNT: phone and keyboard codes are tracked below this value. */
#define FPLINUX_UI_TEXT_MODEL_KEY_COUNT 0x300U

/* One key event; code is a Linux KEY_* value for both sources. */
struct fplinux_ui_text_model_key {
	bool keyboard;
	unsigned int code;
	bool pressed;
	/* Control or Alt held: keyboard text is a shortcut, not input. */
	bool shortcut;
	/* Keyboard UTF-8 text, or NULL. */
	const char *text;
};

enum fplinux_ui_text_model_result {
	FPLINUX_UI_TEXT_MODEL_IGNORED,
	FPLINUX_UI_TEXT_MODEL_CONSUMED,
	/* Multi-line Up/Down: the caller moves the caret by a visual line. */
	FPLINUX_UI_TEXT_MODEL_LINE_UP,
	FPLINUX_UI_TEXT_MODEL_LINE_DOWN,
};

/*
 * What the field takes. Every type but ANY types digits at once and shows
 * "123"; # does not change the mode there.
 */
enum fplinux_ui_text_model_type {
	/* Multi-tap letters, modes, symbols and digits. */
	FPLINUX_UI_TEXT_MODEL_ANY,
	/* An optional leading minus, toggled by *, and digits. */
	FPLINUX_UI_TEXT_MODEL_NUMERIC,
	/*
	 * NUMERIC with one decimal separator: * types it, a second * within
	 * the multi-tap timeout takes it back and toggles the minus instead,
	 * and * toggles the minus once the number has a separator.
	 */
	FPLINUX_UI_TEXT_MODEL_DECIMAL,
	/* Digits; * cycles * + p w by multi-tap; # types #; holding 0 types +. */
	FPLINUX_UI_TEXT_MODEL_PHONE,
};

struct fplinux_ui_text_model {
	/* Committed UTF-8 text and the caret as a byte offset into it. */
	char *text;
	size_t length;
	size_t cursor;
	/* Unicode code points in text; the limit counts these. */
	size_t characters;
	/* The text as shown: committed text with the pending character. */
	char *display;
	size_t capacity;
	size_t limit;
	bool multiline;
	bool focused;
	/*
	 * Short # steps through the sentence, lower and upper case of each
	 * input language in order, then to digits, then back to the first.
	 */
	struct fplinux_multitap_languages languages;
	size_t language;
	enum fplinux_language_case letter_case;
	bool numeric;
	enum fplinux_ui_text_model_type type;
	/* The DECIMAL separator, '.' or ','. */
	char separator;
	/* The separator that * typed, which a quick second * takes back. */
	bool separator_typed;
	unsigned int separator_revision;
	uint64_t separator_ms;
	/*
	 * A password shows a dot for every committed character; the pending
	 * one stays readable until it is committed.
	 */
	bool password;
	char *masked;
	size_t masked_capacity;
	/* Letters, or the * cycle of a PHONE field. */
	struct fplinux_multitap compose;
	uint64_t compose_ms;
	/*
	 * Phone digit waiting for its long press; 0 when none. A PHONE field
	 * waits for 0 after typing it, to replace it with +.
	 */
	unsigned int held_code;
	uint64_t held_ms;
	bool clearing;
	uint64_t clear_ms;
	bool picker_open;
	unsigned int picker_index;
	/* Up and Down in the symbol grid move by this many symbols. */
	unsigned int picker_columns;
	/*
	 * Read-only (UNEDITABLE): presses are left to the caller, the status
	 * is empty and the picker stays closed; the text can still be set.
	 */
	bool read_only;
	/* Releases of consumed presses are consumed too: [keyboard][code]. */
	unsigned char consumed[2][FPLINUX_UI_TEXT_MODEL_KEY_COUNT / 8];
	bool dirty;
	/* Incremented on every visible change. */
	unsigned int revision;
};

/*
 * limit counts code points, 0 for none; the UTF-8 buffers grow as needed.
 * The input languages come from the language files in directory with the
 * preferred input language first; the first
 * mode is the sentence case of the first language.
 */
bool fplinux_ui_text_model_init(struct fplinux_ui_text_model *model,
				bool multiline, size_t limit,
				const char *directory,
				const char *input_language);
void fplinux_ui_text_model_cleanup(struct fplinux_ui_text_model *model);

enum fplinux_ui_text_model_result
fplinux_ui_text_model_key(struct fplinux_ui_text_model *model,
			  const struct fplinux_ui_text_model_key *key,
			  uint64_t now_ms);
/* Runs the commit, long-press and Clear repeat deadlines due at now_ms. */
void fplinux_ui_text_model_tick(struct fplinux_ui_text_model *model,
				uint64_t now_ms);
/* Returns false when nothing is scheduled. */
bool fplinux_ui_text_model_deadline(const struct fplinux_ui_text_model *model,
				    uint64_t *deadline_ms);
/*
 * Fails with E2BIG above the limit, or EINVAL for a newline in one line or a
 * text that the type does not take. A DECIMAL field takes '.' or ',' as the
 * separator and stores its own.
 */
bool fplinux_ui_text_model_set_text(struct fplinux_ui_text_model *model,
				    const char *text);
/*
 * Commits pending input and selects the type and, for DECIMAL, the separator
 * ('.' or ','). ANY returns to the first mode. An unknown type or separator
 * fails with EINVAL before changing the model. Text that does not suit the
 * type fails with EINVAL after pending input is committed; the type stays.
 */
bool fplinux_ui_text_model_set_type(struct fplinux_ui_text_model *model,
				    enum fplinux_ui_text_model_type type,
				    char separator);
/* A password leaves out the sentence-case modes, so nothing is capitalized. */
void fplinux_ui_text_model_set_password(struct fplinux_ui_text_model *model,
					bool password);
/* Unfocusing commits pending input, closes the picker and stops repeats. */
void fplinux_ui_text_model_set_focused(struct fplinux_ui_text_model *model,
				       bool focused);
/* Cancel without insertion when the picker view could not be created. */
void fplinux_ui_text_model_cancel_picker(struct fplinux_ui_text_model *model);
/*
 * Columns of the symbol grid: a divisor of the symbol count (1, 2, 3, 4, 6, 9,
 * 12, 18 or 36); any other count selects FPLINUX_UI_TEXT_MODEL_PICKER_COLUMNS.
 */
void fplinux_ui_text_model_set_picker_columns(
	struct fplinux_ui_text_model *model, unsigned int columns);
/*
 * Read-only commits pending input, closes the picker and stops repeats; from
 * then on every press is ignored and not consumed, so that the caller can
 * use it. The release of a press consumed before stays consumed.
 */
void fplinux_ui_text_model_set_read_only(struct fplinux_ui_text_model *model,
					 bool read_only);
/* Moves the caret to a character index of the display text. */
void fplinux_ui_text_model_set_caret(struct fplinux_ui_text_model *model,
				     size_t index);

bool fplinux_ui_text_model_is_empty(const struct fplinux_ui_text_model *model);
/*
 * Whether OK and Enter start a new line: only in an editable multi-line field
 * of ANY. Elsewhere they are ignored, after committing pending input.
 */
bool fplinux_ui_text_model_takes_newline(
	const struct fplinux_ui_text_model *model);
/*
 * The text to draw: display, or for a password a dot for every committed
 * character around the readable pending one. Character indices match.
 */
const char *
fplinux_ui_text_model_view(const struct fplinux_ui_text_model *model);
/* Character index of the caret in the display text, after a pending one. */
size_t fplinux_ui_text_model_caret(const struct fplinux_ui_text_model *model);
/* Character index of the pending character in the display text. */
bool fplinux_ui_text_model_pending(const struct fplinux_ui_text_model *model,
				   size_t *index);
const char *
fplinux_ui_text_model_indicator(const struct fplinux_ui_text_model *model);
/*
 * The indicator, followed by "used/limit" characters when there is a limit;
 * empty for a read-only field.
 */
void fplinux_ui_text_model_status(const struct fplinux_ui_text_model *model,
				  char *status, size_t size);

size_t fplinux_ui_text_model_symbol_count(void);
const char *fplinux_ui_text_model_symbol(size_t index);

#endif
