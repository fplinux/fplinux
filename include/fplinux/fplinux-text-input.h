/* SPDX-License-Identifier: GPL-2.0-only */
/* Shared numeric-keypad policy, independent of text storage and rendering. */

#ifndef FPLINUX_TEXT_INPUT_H
#define FPLINUX_TEXT_INPUT_H

#include "fplinux-multitap.h"

#define FPLINUX_TEXT_INPUT_COMMIT_MS 1000U
#define FPLINUX_TEXT_INPUT_HOLD_MS 500U
#define FPLINUX_TEXT_INPUT_REPEAT_MS 100U
#define FPLINUX_TEXT_INPUT_PICKER_COLUMNS 6U
#define FPLINUX_TEXT_INPUT_UTF8_BYTES 5U

enum fplinux_text_input_gesture {
	FPLINUX_TEXT_INPUT_UNDECIDED,
	FPLINUX_TEXT_INPUT_TAP,
	FPLINUX_TEXT_INPUT_HOLD,
};

struct fplinux_text_input {
	struct fplinux_multitap_languages languages;
	size_t language;
	enum fplinux_language_case letter_case;
	bool numeric;
	bool allow_sentence;
	struct fplinux_multitap compose;
	uint64_t compose_ms;
	/* ASCII digit held by a physical phone key, or 0. */
	unsigned char held_digit;
	uint64_t held_ms;
	/* # or *; the resolved gesture survives a blocked emission. */
	unsigned char held_control;
	uint64_t control_ms;
	enum fplinux_text_input_gesture control_gesture;
	bool clearing;
	uint64_t clear_ms;
	bool picker_open;
	unsigned int picker_index;
	unsigned int picker_columns;
};

enum fplinux_text_input_direction {
	FPLINUX_TEXT_INPUT_LEFT,
	FPLINUX_TEXT_INPUT_RIGHT,
	FPLINUX_TEXT_INPUT_UP,
	FPLINUX_TEXT_INPUT_DOWN,
};

/* ACCEPTED permits another repeat, REJECTED stops, BLOCKED waits unchanged. */
typedef enum fplinux_multitap_emit_result (*fplinux_text_input_erase_fn)(
	void *context);

void fplinux_text_input_init(struct fplinux_text_input *input,
			     const char *directory, const char *first,
			     bool allow_sentence);
/* The caller commits or cancels pending input before reloading languages. */
void fplinux_text_input_load_languages(struct fplinux_text_input *input,
				       const char *directory,
				       const char *first);
void fplinux_text_input_set_sentence_case(struct fplinux_text_input *input,
					  bool allowed);
void fplinux_text_input_reset_mode(struct fplinux_text_input *input,
				   bool numeric);
uint32_t fplinux_text_input_apply_case(const struct fplinux_text_input *input,
				       uint32_t character,
				       bool starts_sentence);
const char *
fplinux_text_input_indicator(const struct fplinux_text_input *input);

enum fplinux_multitap_result
fplinux_text_input_commit(struct fplinux_text_input *input,
			  fplinux_multitap_emit_fn emit, void *context);
void fplinux_text_input_cancel(struct fplinux_text_input *input);
/* Digits compose or emit; # and * wait for release or the hold deadline. */
enum fplinux_multitap_result
fplinux_text_input_press(struct fplinux_text_input *input, unsigned char key,
			 uint64_t now_ms, fplinux_multitap_emit_fn emit,
			 void *context);
/* Composition without a physical digit hold, for a field-owned key map. */
enum fplinux_multitap_result
fplinux_text_input_compose(struct fplinux_text_input *input, unsigned char key,
			   uint64_t now_ms, fplinux_multitap_emit_fn emit,
			   void *context);
/* A normal release resolves an overdue hold; BLOCKED keeps it for retry. */
enum fplinux_multitap_result
fplinux_text_input_release(struct fplinux_text_input *input, unsigned char key,
			   uint64_t now_ms, fplinux_multitap_emit_fn emit,
			   void *context);
enum fplinux_multitap_emit_result fplinux_text_input_clear_press(
	struct fplinux_text_input *input, uint64_t now_ms,
	fplinux_text_input_erase_fn erase, void *context);
void fplinux_text_input_clear_release(struct fplinux_text_input *input);
/* Stops physical holds and closes the picker; pending composition remains. */
void fplinux_text_input_cancel_held(struct fplinux_text_input *input);
enum fplinux_multitap_result
fplinux_text_input_tick(struct fplinux_text_input *input, uint64_t now_ms,
			fplinux_multitap_emit_fn emit,
			fplinux_text_input_erase_fn erase, void *context);
bool fplinux_text_input_deadline(const struct fplinux_text_input *input,
				 uint64_t *deadline_ms);

enum fplinux_multitap_result
fplinux_text_input_open_picker(struct fplinux_text_input *input,
			       fplinux_multitap_emit_fn emit, void *context);
void fplinux_text_input_close_picker(struct fplinux_text_input *input);
void fplinux_text_input_set_picker_columns(struct fplinux_text_input *input,
					   unsigned int columns);
/* Vertical movement wraps in the same column, skipping absent final cells. */
void fplinux_text_input_move_picker(struct fplinux_text_input *input,
				    enum fplinux_text_input_direction direction);
enum fplinux_multitap_emit_result
fplinux_text_input_select_symbol(struct fplinux_text_input *input,
				 fplinux_multitap_emit_fn emit, void *context);
size_t fplinux_text_input_symbol_count(void);
uint32_t fplinux_text_input_symbol(size_t index);
/* The caller owns the NUL-terminated label; NULL for an invalid index. */
const char *
fplinux_text_input_symbol_label(size_t index,
				char label[FPLINUX_TEXT_INPUT_UTF8_BYTES]);
/* Encodes one scalar value and terminates it; 0 for an invalid value. */
size_t
fplinux_text_input_encode_utf8(uint32_t character,
			       char bytes[FPLINUX_TEXT_INPUT_UTF8_BYTES]);

#endif
