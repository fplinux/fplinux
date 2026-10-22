/* SPDX-License-Identifier: GPL-2.0-only */

#include "fplinux-text-input.h"

#include <string.h>

static const uint32_t symbols[] = {
	'.',  ',',  '?',    '!',    '\'',   '"',  '-', '(', ')',  '@',
	'/',  ':',  ';',    '_',    '+',    '=',  '*', '#', '%',  '&',
	'$',  '<',  '>',    '~',    '[',    ']',  '{', '}', '\\', '|',
	0xab, 0xbb, 0x2014, 0x2116, 0x20ac, 0xb0, '`', '^',
};

static enum fplinux_language_case
first_case(const struct fplinux_text_input *input)
{
	return input->allow_sentence ? FPLINUX_LANGUAGE_SENTENCE :
				       FPLINUX_LANGUAGE_LOWER;
}

void fplinux_text_input_load_languages(struct fplinux_text_input *input,
				       const char *directory, const char *first)
{
	fplinux_multitap_load_languages(&input->languages, directory, first);
	fplinux_text_input_reset_mode(input, false);
}

void fplinux_text_input_init(struct fplinux_text_input *input,
			     const char *directory, const char *first,
			     bool allow_sentence)
{
	memset(input, 0, sizeof(*input));
	input->allow_sentence = allow_sentence;
	input->picker_columns = FPLINUX_TEXT_INPUT_PICKER_COLUMNS;
	fplinux_multitap_init(&input->compose, NULL,
			      FPLINUX_TEXT_INPUT_COMMIT_MS);
	fplinux_text_input_load_languages(input, directory, first);
}

void fplinux_text_input_set_sentence_case(struct fplinux_text_input *input,
					  bool allowed)
{
	input->allow_sentence = allowed;
	if (!allowed && input->letter_case == FPLINUX_LANGUAGE_SENTENCE)
		input->letter_case = FPLINUX_LANGUAGE_LOWER;
}

void fplinux_text_input_reset_mode(struct fplinux_text_input *input,
				   bool numeric)
{
	input->language = 0;
	input->letter_case = first_case(input);
	input->numeric = numeric;
	input->held_digit = 0;
	fplinux_multitap_set_language(&input->compose,
				      &input->languages.language[0]);
}

uint32_t fplinux_text_input_apply_case(const struct fplinux_text_input *input,
				       uint32_t character, bool starts_sentence)
{
	if (!input->numeric &&
	    (input->letter_case == FPLINUX_LANGUAGE_UPPER ||
	     (input->letter_case == FPLINUX_LANGUAGE_SENTENCE &&
	      starts_sentence)))
		return fplinux_multitap_uppercase(character);
	return character;
}

const char *fplinux_text_input_indicator(const struct fplinux_text_input *input)
{
	return input->numeric ? "123" :
				input->languages.language[input->language]
					.indicator[input->letter_case];
}

enum fplinux_multitap_result
fplinux_text_input_commit(struct fplinux_text_input *input,
			  fplinux_multitap_emit_fn emit, void *context)
{
	enum fplinux_multitap_result result =
		fplinux_multitap_commit(&input->compose, emit, context);

	if (result != FPLINUX_MULTITAP_BLOCKED)
		input->held_digit = 0;
	return result;
}

void fplinux_text_input_cancel(struct fplinux_text_input *input)
{
	input->held_digit = 0;
	fplinux_multitap_cancel(&input->compose);
}

enum fplinux_multitap_result
fplinux_text_input_next_mode(struct fplinux_text_input *input,
			     fplinux_multitap_emit_fn emit, void *context)
{
	enum fplinux_multitap_result result =
		fplinux_text_input_commit(input, emit, context);

	if (result == FPLINUX_MULTITAP_BLOCKED)
		return result;
	if (input->numeric) {
		input->numeric = false;
		input->language = 0;
		input->letter_case = first_case(input);
	} else if (input->letter_case != FPLINUX_LANGUAGE_UPPER) {
		++input->letter_case;
	} else if (input->language + 1 < input->languages.count) {
		++input->language;
		input->letter_case = first_case(input);
	} else {
		input->numeric = true;
	}
	fplinux_multitap_set_language(
		&input->compose, &input->languages.language[input->language]);
	return result;
}

enum fplinux_multitap_result
fplinux_text_input_compose(struct fplinux_text_input *input, unsigned char key,
			   uint64_t now_ms, fplinux_multitap_emit_fn emit,
			   void *context)
{
	uint64_t elapsed =
		now_ms > input->compose_ms ? now_ms - input->compose_ms : 0;
	enum fplinux_multitap_result result = fplinux_multitap_press(
		&input->compose, key,
		elapsed > UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed, emit,
		context);

	if (result != FPLINUX_MULTITAP_BLOCKED)
		input->compose_ms = now_ms;
	return result;
}

static enum fplinux_multitap_result
emitted(enum fplinux_multitap_emit_result result)
{
	switch (result) {
	case FPLINUX_MULTITAP_EMIT_ACCEPTED:
		return FPLINUX_MULTITAP_COMMITTED;
	case FPLINUX_MULTITAP_EMIT_REJECTED:
		return FPLINUX_MULTITAP_REJECTED;
	case FPLINUX_MULTITAP_EMIT_BLOCKED:
		return FPLINUX_MULTITAP_BLOCKED;
	}
	return FPLINUX_MULTITAP_BLOCKED;
}

enum fplinux_multitap_result
fplinux_text_input_press(struct fplinux_text_input *input, unsigned char digit,
			 uint64_t now_ms, fplinux_multitap_emit_fn emit,
			 void *context)
{
	enum fplinux_multitap_result result;

	if (!fplinux_multitap_handles(digit))
		return FPLINUX_MULTITAP_IGNORED;
	if (input->numeric) {
		result = fplinux_text_input_commit(input, emit, context);
		if (result == FPLINUX_MULTITAP_BLOCKED)
			return result;
		return emit ? emitted(emit(context, digit)) :
			      FPLINUX_MULTITAP_BLOCKED;
	}
	result =
		fplinux_text_input_compose(input, digit, now_ms, emit, context);
	if (result != FPLINUX_MULTITAP_BLOCKED &&
	    fplinux_multitap_pending(&input->compose)) {
		input->held_digit = digit;
		input->held_ms = now_ms;
	}
	return result;
}

static enum fplinux_multitap_result hold_digit(struct fplinux_text_input *input,
					       uint64_t now_ms,
					       fplinux_multitap_emit_fn emit,
					       void *context)
{
	enum fplinux_multitap_result result;

	if (!input->held_digit ||
	    now_ms < input->held_ms + FPLINUX_TEXT_INPUT_HOLD_MS)
		return FPLINUX_MULTITAP_PENDING;
	result = fplinux_multitap_hold(&input->compose, emit, context);
	if (result != FPLINUX_MULTITAP_BLOCKED)
		input->held_digit = 0;
	return result;
}

enum fplinux_multitap_result
fplinux_text_input_release(struct fplinux_text_input *input,
			   unsigned char digit, uint64_t now_ms,
			   fplinux_multitap_emit_fn emit, void *context)
{
	enum fplinux_multitap_result result;

	if (input->held_digit != digit)
		return FPLINUX_MULTITAP_IGNORED;
	result = hold_digit(input, now_ms, emit, context);
	if (result != FPLINUX_MULTITAP_BLOCKED)
		input->held_digit = 0;
	return result;
}

enum fplinux_multitap_emit_result
fplinux_text_input_clear_press(struct fplinux_text_input *input,
			       uint64_t now_ms,
			       fplinux_text_input_erase_fn erase, void *context)
{
	enum fplinux_multitap_emit_result result = erase(context);

	if (result != FPLINUX_MULTITAP_EMIT_BLOCKED) {
		input->held_digit = 0;
		input->clearing = result == FPLINUX_MULTITAP_EMIT_ACCEPTED;
		input->clear_ms = now_ms + FPLINUX_TEXT_INPUT_HOLD_MS;
	}
	return result;
}

void fplinux_text_input_clear_release(struct fplinux_text_input *input)
{
	input->clearing = false;
}

void fplinux_text_input_cancel_held(struct fplinux_text_input *input)
{
	input->held_digit = 0;
	input->clearing = false;
	input->picker_open = false;
}

enum fplinux_multitap_result
fplinux_text_input_tick(struct fplinux_text_input *input, uint64_t now_ms,
			fplinux_multitap_emit_fn emit,
			fplinux_text_input_erase_fn erase, void *context)
{
	enum fplinux_multitap_result result =
		hold_digit(input, now_ms, emit, context);

	if (result == FPLINUX_MULTITAP_BLOCKED)
		return result;
	if (!input->held_digit && fplinux_multitap_pending(&input->compose) &&
	    now_ms >= input->compose_ms + FPLINUX_TEXT_INPUT_COMMIT_MS) {
		result = fplinux_text_input_commit(input, emit, context);
		if (result == FPLINUX_MULTITAP_BLOCKED)
			return result;
	}
	if (input->clearing && now_ms >= input->clear_ms) {
		enum fplinux_multitap_emit_result erased = erase(context);

		if (erased == FPLINUX_MULTITAP_EMIT_BLOCKED)
			return FPLINUX_MULTITAP_BLOCKED;
		input->clearing = erased == FPLINUX_MULTITAP_EMIT_ACCEPTED;
		input->clear_ms = now_ms + FPLINUX_TEXT_INPUT_REPEAT_MS;
	}
	return result;
}

bool fplinux_text_input_deadline(const struct fplinux_text_input *input,
				 uint64_t *deadline_ms)
{
	uint64_t deadline = UINT64_MAX;

	if (input->held_digit)
		deadline = input->held_ms + FPLINUX_TEXT_INPUT_HOLD_MS;
	else if (fplinux_multitap_pending(&input->compose))
		deadline = input->compose_ms + FPLINUX_TEXT_INPUT_COMMIT_MS;
	if (input->clearing && input->clear_ms < deadline)
		deadline = input->clear_ms;
	if (deadline == UINT64_MAX)
		return false;
	*deadline_ms = deadline;
	return true;
}

enum fplinux_multitap_result
fplinux_text_input_open_picker(struct fplinux_text_input *input,
			       fplinux_multitap_emit_fn emit, void *context)
{
	enum fplinux_multitap_result result =
		fplinux_text_input_commit(input, emit, context);

	if (result != FPLINUX_MULTITAP_BLOCKED) {
		input->picker_open = true;
		input->picker_index = 0;
	}
	return result;
}

void fplinux_text_input_close_picker(struct fplinux_text_input *input)
{
	input->picker_open = false;
}

void fplinux_text_input_set_picker_columns(struct fplinux_text_input *input,
					   unsigned int columns)
{
	input->picker_columns =
		columns && columns <= fplinux_text_input_symbol_count() ?
			columns :
			FPLINUX_TEXT_INPUT_PICKER_COLUMNS;
}

void fplinux_text_input_move_picker(struct fplinux_text_input *input,
				    enum fplinux_text_input_direction direction)
{
	unsigned int count = (unsigned int)fplinux_text_input_symbol_count();
	unsigned int columns = input->picker_columns;
	unsigned int index = input->picker_index;

	switch (direction) {
	case FPLINUX_TEXT_INPUT_LEFT:
		index = index ? index - 1 : count - 1;
		break;
	case FPLINUX_TEXT_INPUT_RIGHT:
		index = (index + 1) % count;
		break;
	case FPLINUX_TEXT_INPUT_UP:
		if (index >= columns) {
			index -= columns;
		} else {
			while (index + columns < count)
				index += columns;
		}
		break;
	case FPLINUX_TEXT_INPUT_DOWN:
		index = index + columns < count ? index + columns :
						  index % columns;
		break;
	}
	input->picker_index = index;
}

enum fplinux_multitap_emit_result
fplinux_text_input_select_symbol(struct fplinux_text_input *input,
				 fplinux_multitap_emit_fn emit, void *context)
{
	enum fplinux_multitap_emit_result result =
		emit(context, fplinux_text_input_symbol(input->picker_index));

	if (result != FPLINUX_MULTITAP_EMIT_BLOCKED)
		fplinux_text_input_close_picker(input);
	return result;
}

size_t fplinux_text_input_symbol_count(void)
{
	return sizeof(symbols) / sizeof(symbols[0]);
}

uint32_t fplinux_text_input_symbol(size_t index)
{
	return index < fplinux_text_input_symbol_count() ? symbols[index] : 0;
}

size_t fplinux_text_input_encode_utf8(uint32_t character,
				      char bytes[FPLINUX_TEXT_INPUT_UTF8_BYTES])
{
	size_t size;

	if (character > 0x10ffff ||
	    (character >= 0xd800 && character <= 0xdfff))
		return 0;
	if (character < 0x80) {
		bytes[0] = (char)character;
		size = 1;
	} else if (character < 0x800) {
		bytes[0] = (char)(0xc0 | character >> 6);
		bytes[1] = (char)(0x80 | (character & 0x3f));
		size = 2;
	} else if (character < 0x10000) {
		bytes[0] = (char)(0xe0 | character >> 12);
		bytes[1] = (char)(0x80 | ((character >> 6) & 0x3f));
		bytes[2] = (char)(0x80 | (character & 0x3f));
		size = 3;
	} else {
		bytes[0] = (char)(0xf0 | character >> 18);
		bytes[1] = (char)(0x80 | ((character >> 12) & 0x3f));
		bytes[2] = (char)(0x80 | ((character >> 6) & 0x3f));
		bytes[3] = (char)(0x80 | (character & 0x3f));
		size = 4;
	}
	bytes[size] = '\0';
	return size;
}

const char *
fplinux_text_input_symbol_label(size_t index,
				char label[FPLINUX_TEXT_INPUT_UTF8_BYTES])
{
	uint32_t character = fplinux_text_input_symbol(index);

	if (!character)
		return NULL;
	fplinux_text_input_encode_utf8(character, label);
	return label;
}
