/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "fplinux-ui-text-model.h"

#include <errno.h>
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INITIAL_CAPACITY 64U
#define UTF8_MAX_BYTES FPLINUX_TEXT_INPUT_UTF8_BYTES
/* U+2022 BULLET, which masks a password character. */
#define BULLET "\xe2\x80\xa2"
#define BULLET_BYTES (sizeof(BULLET) - 1)

/*
 * The * cycle of a PHONE field, as Nokia phones number it: * + p (pause)
 * w (wait). The engine cycles digit keys, so the cycle is key 0's map.
 */
static const struct fplinux_multitap_language phone_star = {
	.keys = { "*+pw" },
};

static bool is_continuation(char byte)
{
	return ((unsigned char)byte & 0xc0) == 0x80;
}

static size_t count_characters(const char *text, size_t bytes)
{
	size_t count = 0;

	for (size_t offset = 0; offset < bytes; ++offset)
		if (!is_continuation(text[offset]))
			++count;
	return count;
}

static size_t previous_boundary(const char *text, size_t offset)
{
	if (!offset)
		return 0;
	do
		--offset;
	while (offset && is_continuation(text[offset]));
	return offset;
}

static size_t next_boundary(const char *text, size_t length, size_t offset)
{
	if (offset >= length)
		return length;
	do
		++offset;
	while (offset < length && is_continuation(text[offset]));
	return offset;
}

static bool fits(const struct fplinux_ui_text_model *model, size_t characters)
{
	return !model->limit || characters <= model->limit;
}

/* Both buffers hold bytes of text plus the terminating NUL. */
static bool reserve(struct fplinux_ui_text_model *model, size_t bytes)
{
	size_t capacity;
	char *buffer;

	if (bytes < model->capacity)
		return true;
	if (bytes >= SIZE_MAX / 2) {
		errno = ENOMEM;
		return false;
	}
	capacity = model->capacity * 2;
	if (capacity <= bytes)
		capacity = bytes + 1;
	buffer = realloc(model->text, capacity);
	if (!buffer)
		return false;
	model->text = buffer;
	buffer = realloc(model->display, capacity);
	if (!buffer)
		return false;
	model->display = buffer;
	model->capacity = capacity;
	return true;
}

/* Abc mode capitalizes at the start, after a newline or ". ", "! ", "? ". */
static bool starts_sentence(const struct fplinux_ui_text_model *model)
{
	size_t offset = model->cursor;
	bool spaced = false;
	char previous;

	while (offset && model->text[offset - 1] == ' ') {
		--offset;
		spaced = true;
	}
	if (!offset)
		return true;
	previous = model->text[offset - 1];
	if (previous == '\n')
		return true;
	return spaced &&
	       (previous == '.' || previous == '!' || previous == '?');
}

static uint32_t apply_case(const struct fplinux_ui_text_model *model,
			   uint32_t character)
{
	return fplinux_text_input_apply_case(&model->input, character,
					     starts_sentence(model));
}

static size_t pending_bytes(const struct fplinux_ui_text_model *model,
			    char *bytes)
{
	if (!fplinux_multitap_pending(&model->input.compose))
		return 0;
	return fplinux_text_input_encode_utf8(
		apply_case(model,
			   fplinux_multitap_candidate(&model->input.compose)),
		bytes);
}

static bool insert_bytes(struct fplinux_ui_text_model *model, const char *bytes,
			 size_t size)
{
	size_t added = count_characters(bytes, size);

	if (!fits(model, model->characters + added) ||
	    !reserve(model, model->length + size))
		return false;
	memmove(model->text + model->cursor + size, model->text + model->cursor,
		model->length - model->cursor + 1);
	memcpy(model->text + model->cursor, bytes, size);
	model->cursor += size;
	model->length += size;
	model->characters += added;
	model->dirty = true;
	return true;
}

static void delete_range(struct fplinux_ui_text_model *model, size_t start,
			 size_t end)
{
	model->characters -= count_characters(model->text + start, end - start);
	memmove(model->text + start, model->text + end,
		model->length - end + 1);
	model->length -= end - start;
	model->cursor = start;
	model->dirty = true;
}

static enum fplinux_multitap_emit_result emit_character(void *context,
							uint32_t character)
{
	struct fplinux_ui_text_model *model = context;
	char bytes[UTF8_MAX_BYTES];
	size_t size = fplinux_text_input_encode_utf8(
		apply_case(model, character), bytes);

	return insert_bytes(model, bytes, size) ?
		       FPLINUX_MULTITAP_EMIT_ACCEPTED :
		       FPLINUX_MULTITAP_EMIT_REJECTED;
}

static void commit_pending(struct fplinux_ui_text_model *model)
{
	model->phone_zero_held = false;
	fplinux_text_input_cancel_held(&model->input);
	if (!fplinux_multitap_pending(&model->input.compose))
		return;
	fplinux_text_input_commit(&model->input, emit_character, model);
	model->dirty = true;
}

static void cancel_pending(struct fplinux_ui_text_model *model)
{
	model->phone_zero_held = false;
	fplinux_text_input_cancel(&model->input);
	model->dirty = true;
}

static char *put_bullets(char *out, size_t count)
{
	for (size_t index = 0; index < count; ++index) {
		memcpy(out, BULLET, BULLET_BYTES);
		out += BULLET_BYTES;
	}
	return out;
}

/* Without memory for the dots a password shows nothing, never the text. */
static void refresh_masked(struct fplinux_ui_text_model *model,
			   const char *pending, size_t size)
{
	size_t before = count_characters(model->text, model->cursor);
	size_t after = model->characters - before;
	size_t bytes = model->characters * BULLET_BYTES + size + 1;
	char *out;

	if (bytes > model->masked_capacity) {
		char *buffer = model->characters < SIZE_MAX / 4 ?
				       realloc(model->masked, bytes) :
				       NULL;

		if (!buffer) {
			free(model->masked);
			model->masked = NULL;
			model->masked_capacity = 0;
			return;
		}
		model->masked = buffer;
		model->masked_capacity = bytes;
	}
	out = put_bullets(model->masked, before);
	memcpy(out, pending, size);
	out = put_bullets(out + size, after);
	*out = '\0';
}

/* A failed refresh drops the pending character rather than overflow. */
static void refresh_display(struct fplinux_ui_text_model *model)
{
	char bytes[UTF8_MAX_BYTES];
	size_t size = pending_bytes(model, bytes);

	if (size && !reserve(model, model->length + size)) {
		fplinux_text_input_cancel(&model->input);
		size = 0;
	}
	memcpy(model->display, model->text, model->cursor);
	memcpy(model->display + model->cursor, bytes, size);
	memcpy(model->display + model->cursor + size,
	       model->text + model->cursor, model->length - model->cursor + 1);
	if (model->password)
		refresh_masked(model, bytes, size);
}

static void finish(struct fplinux_ui_text_model *model)
{
	if (!model->dirty)
		return;
	refresh_display(model);
	model->dirty = false;
	++model->revision;
}

/* Returns false when the pending character does not fit. */
static bool press_multitap(struct fplinux_ui_text_model *model,
			   unsigned char key, uint64_t now_ms, bool hold)
{
	char bytes[UTF8_MAX_BYTES];
	size_t size;

	if (hold)
		fplinux_text_input_press(&model->input, key, now_ms,
					 emit_character, model);
	else
		fplinux_text_input_compose(&model->input, key, now_ms,
					   emit_character, model);
	model->dirty = true;
	size = pending_bytes(model, bytes);
	if (!fits(model, model->characters + 1) ||
	    !reserve(model, model->length + size)) {
		cancel_pending(model);
		return false;
	}
	return true;
}

static bool is_number(const struct fplinux_ui_text_model *model)
{
	return model->type == FPLINUX_UI_TEXT_MODEL_NUMERIC ||
	       model->type == FPLINUX_UI_TEXT_MODEL_DECIMAL;
}

/* Digits and the separator go after a leading minus, never before it. */
static void skip_minus(struct fplinux_ui_text_model *model)
{
	if (is_number(model) && !model->cursor && model->length &&
	    model->text[0] == '-')
		model->cursor = 1;
}

/* The minus counts toward the limit; the caret stays by its character. */
static void toggle_minus(struct fplinux_ui_text_model *model)
{
	size_t cursor = model->cursor;

	if (model->length && model->text[0] == '-') {
		delete_range(model, 0, 1);
		model->cursor = cursor ? cursor - 1 : 0;
		return;
	}
	model->cursor = 0;
	if (insert_bytes(model, "-", 1))
		++cursor;
	model->cursor = cursor;
}

static bool has_separator(const struct fplinux_ui_text_model *model)
{
	return memchr(model->text, model->separator, model->length) != NULL;
}

static bool type_separator(struct fplinux_ui_text_model *model)
{
	if (has_separator(model))
		return false;
	skip_minus(model);
	return insert_bytes(model, &model->separator, 1);
}

static void decimal_star(struct fplinux_ui_text_model *model, uint64_t now_ms)
{
	bool again =
		model->separator_typed &&
		model->separator_revision == model->revision &&
		now_ms < model->separator_ms + FPLINUX_TEXT_INPUT_COMMIT_MS &&
		model->cursor &&
		model->text[model->cursor - 1] == model->separator;

	model->separator_typed = false;
	if (again) {
		delete_range(model, model->cursor - 1, model->cursor);
		toggle_minus(model);
		return;
	}
	if (has_separator(model)) {
		toggle_minus(model);
		return;
	}
	if (!type_separator(model))
		return;
	model->separator_typed = true;
	/* The revision that finish() gives this change. */
	model->separator_revision = model->revision + 1;
	model->separator_ms = now_ms;
}

static void type_digit(struct fplinux_ui_text_model *model, unsigned int code,
		       unsigned char digit, uint64_t now_ms)
{
	commit_pending(model);
	skip_minus(model);
	if (!insert_bytes(model, (const char *)&digit, 1))
		return;
	if (model->type == FPLINUX_UI_TEXT_MODEL_PHONE &&
	    code == KEY_NUMERIC_0) {
		model->phone_zero_held = true;
		model->phone_zero_ms = now_ms;
	}
}

/* A held 0 in a PHONE field becomes the international prefix. */
static void hold_plus(struct fplinux_ui_text_model *model)
{
	if (!model->cursor || model->text[model->cursor - 1] != '0')
		return;
	model->text[model->cursor - 1] = '+';
	model->dirty = true;
}

static void hold_phone_zero(struct fplinux_ui_text_model *model,
			    uint64_t now_ms)
{
	if (!model->phone_zero_held ||
	    now_ms < model->phone_zero_ms + FPLINUX_TEXT_INPUT_HOLD_MS)
		return;
	model->phone_zero_held = false;
	hold_plus(model);
}

static void delete_backward(struct fplinux_ui_text_model *model)
{
	if (fplinux_multitap_pending(&model->input.compose)) {
		cancel_pending(model);
		return;
	}
	if (model->cursor)
		delete_range(model,
			     previous_boundary(model->text, model->cursor),
			     model->cursor);
}

static enum fplinux_multitap_emit_result erase_character(void *context)
{
	struct fplinux_ui_text_model *model = context;

	delete_backward(model);
	return model->cursor ? FPLINUX_MULTITAP_EMIT_ACCEPTED :
			       FPLINUX_MULTITAP_EMIT_REJECTED;
}

static void delete_forward(struct fplinux_ui_text_model *model)
{
	if (model->cursor < model->length)
		delete_range(model, model->cursor,
			     next_boundary(model->text, model->length,
					   model->cursor));
}

static void move_cursor(struct fplinux_ui_text_model *model, size_t cursor)
{
	if (cursor == model->cursor)
		return;
	model->cursor = cursor;
	model->dirty = true;
}

static void move_to_line_edge(struct fplinux_ui_text_model *model, bool end)
{
	size_t cursor = model->cursor;

	if (end) {
		while (cursor < model->length && model->text[cursor] != '\n')
			++cursor;
	} else {
		while (cursor && model->text[cursor - 1] != '\n')
			--cursor;
	}
	move_cursor(model, cursor);
}

static void close_picker(struct fplinux_ui_text_model *model, bool insert)
{
	if (insert)
		fplinux_text_input_select_symbol(&model->input, emit_character,
						 model);
	else
		fplinux_text_input_close_picker(&model->input);
	model->dirty = true;
}

static bool move_picker(struct fplinux_ui_text_model *model, unsigned int code)
{
	enum fplinux_text_input_direction direction;

	switch (code) {
	case KEY_LEFT:
		direction = FPLINUX_TEXT_INPUT_LEFT;
		break;
	case KEY_RIGHT:
		direction = FPLINUX_TEXT_INPUT_RIGHT;
		break;
	case KEY_UP:
		direction = FPLINUX_TEXT_INPUT_UP;
		break;
	case KEY_DOWN:
		direction = FPLINUX_TEXT_INPUT_DOWN;
		break;
	default:
		return false;
	}
	fplinux_text_input_move_picker(&model->input, direction);
	model->dirty = true;
	return true;
}

static enum fplinux_ui_text_model_result
picker_phone_key(struct fplinux_ui_text_model *model, unsigned int code)
{
	if (move_picker(model, code))
		return FPLINUX_UI_TEXT_MODEL_CONSUMED;
	if (code == KEY_OK)
		close_picker(model, true);
	else if (code == KEY_F14 || code == KEY_NUMERIC_STAR)
		close_picker(model, false);
	/* The picker is modal: other phone keys do nothing while it is open. */
	return FPLINUX_UI_TEXT_MODEL_CONSUMED;
}

static enum fplinux_ui_text_model_result
line_move(const struct fplinux_ui_text_model *model, bool up)
{
	if (!model->multiline)
		return FPLINUX_UI_TEXT_MODEL_IGNORED;
	return up ? FPLINUX_UI_TEXT_MODEL_LINE_UP :
		    FPLINUX_UI_TEXT_MODEL_LINE_DOWN;
}

static enum fplinux_ui_text_model_result
insert_newline(struct fplinux_ui_text_model *model)
{
	if (!fplinux_ui_text_model_takes_newline(model))
		return FPLINUX_UI_TEXT_MODEL_IGNORED;
	insert_bytes(model, "\n", 1);
	return FPLINUX_UI_TEXT_MODEL_CONSUMED;
}

static enum fplinux_ui_text_model_result
phone_key(struct fplinux_ui_text_model *model, unsigned int code,
	  uint64_t now_ms)
{
	bool had_pending;

	if (model->input.picker_open)
		return picker_phone_key(model, code);
	if (model->type == FPLINUX_UI_TEXT_MODEL_ANY &&
	    (code == KEY_NUMERIC_POUND || code == KEY_NUMERIC_STAR)) {
		fplinux_text_input_press(&model->input,
					 code == KEY_NUMERIC_POUND ? '#' : '*',
					 now_ms, emit_character, model);
		return FPLINUX_UI_TEXT_MODEL_CONSUMED;
	}
	if (code >= KEY_NUMERIC_0 && code <= KEY_NUMERIC_9) {
		unsigned char digit =
			(unsigned char)('0' + code - KEY_NUMERIC_0);

		if (!model->input.numeric)
			press_multitap(model, digit, now_ms, true);
		else
			type_digit(model, code, digit, now_ms);
		return FPLINUX_UI_TEXT_MODEL_CONSUMED;
	}
	if (code == KEY_NUMERIC_STAR &&
	    model->type == FPLINUX_UI_TEXT_MODEL_PHONE) {
		/* The * cycle steps a pending character instead of committing. */
		model->input.held_digit = 0;
		model->phone_zero_held = false;
		press_multitap(model, '0', now_ms, false);
		return FPLINUX_UI_TEXT_MODEL_CONSUMED;
	}
	if (code == KEY_F14) {
		fplinux_text_input_cancel_held(&model->input);
		model->phone_zero_held = false;
		if (fplinux_ui_text_model_is_empty(model))
			return FPLINUX_UI_TEXT_MODEL_IGNORED;
		/*
		 * Text after the caret keeps Clear from becoming Back, so the
		 * key is consumed at the start of the text even with nothing
		 * to delete.
		 */
		fplinux_text_input_clear_press(&model->input, now_ms,
					       erase_character, model);
		return FPLINUX_UI_TEXT_MODEL_CONSUMED;
	}
	had_pending = fplinux_multitap_pending(&model->input.compose);
	commit_pending(model);
	switch (code) {
	case KEY_NUMERIC_POUND:
		if (model->type != FPLINUX_UI_TEXT_MODEL_PHONE)
			return FPLINUX_UI_TEXT_MODEL_IGNORED;
		insert_bytes(model, "#", 1);
		return FPLINUX_UI_TEXT_MODEL_CONSUMED;
	case KEY_NUMERIC_STAR:
		if (model->type == FPLINUX_UI_TEXT_MODEL_NUMERIC)
			toggle_minus(model);
		else
			decimal_star(model, now_ms);
		return FPLINUX_UI_TEXT_MODEL_CONSUMED;
	case KEY_LEFT:
		move_cursor(model,
			    previous_boundary(model->text, model->cursor));
		return FPLINUX_UI_TEXT_MODEL_CONSUMED;
	case KEY_RIGHT:
		/* Right first only commits; the caret already follows it. */
		if (!had_pending)
			move_cursor(model,
				    next_boundary(model->text, model->length,
						  model->cursor));
		return FPLINUX_UI_TEXT_MODEL_CONSUMED;
	case KEY_UP:
	case KEY_DOWN:
		return line_move(model, code == KEY_UP);
	case KEY_OK:
		return insert_newline(model);
	default:
		return FPLINUX_UI_TEXT_MODEL_IGNORED;
	}
}

static bool is_printable(const char *text)
{
	if (!text || !text[0])
		return false;
	for (; *text; ++text)
		if ((unsigned char)*text < 0x20 || *text == 0x7f)
			return false;
	return true;
}

/* Keyboard text in a field of another type than ANY. */
static enum fplinux_ui_text_model_result
typed_text(struct fplinux_ui_text_model *model, const char *text)
{
	char character = text[1] ? '\0' : text[0];

	if (character >= '0' && character <= '9') {
		skip_minus(model);
		insert_bytes(model, text, 1);
	} else if (is_number(model) && character == '-') {
		toggle_minus(model);
	} else if (model->type == FPLINUX_UI_TEXT_MODEL_DECIMAL &&
		   (character == '.' || character == ',')) {
		type_separator(model);
	} else if (model->type == FPLINUX_UI_TEXT_MODEL_PHONE && character &&
		   strchr("+*#pPwW", character)) {
		character = character == 'P' ? 'p' :
			    character == 'W' ? 'w' :
					       character;
		insert_bytes(model, &character, 1);
	} else {
		return FPLINUX_UI_TEXT_MODEL_IGNORED;
	}
	return FPLINUX_UI_TEXT_MODEL_CONSUMED;
}

static enum fplinux_ui_text_model_result
keyboard_key(struct fplinux_ui_text_model *model,
	     const struct fplinux_ui_text_model_key *key)
{
	if (key->shortcut)
		return FPLINUX_UI_TEXT_MODEL_IGNORED;
	if (model->input.picker_open) {
		if (move_picker(model, key->code))
			return FPLINUX_UI_TEXT_MODEL_CONSUMED;
		if (key->code == KEY_ENTER || key->code == KEY_KPENTER ||
		    key->code == KEY_ESC) {
			close_picker(model, key->code != KEY_ESC);
			return FPLINUX_UI_TEXT_MODEL_CONSUMED;
		}
		close_picker(model, false);
	}
	fplinux_text_input_cancel_held(&model->input);
	if (key->code == KEY_BACKSPACE) {
		model->phone_zero_held = false;
		if (fplinux_ui_text_model_is_empty(model))
			return FPLINUX_UI_TEXT_MODEL_IGNORED;
		delete_backward(model);
		return FPLINUX_UI_TEXT_MODEL_CONSUMED;
	}
	commit_pending(model);
	switch (key->code) {
	case KEY_DELETE:
		delete_forward(model);
		return FPLINUX_UI_TEXT_MODEL_CONSUMED;
	case KEY_LEFT:
		move_cursor(model,
			    previous_boundary(model->text, model->cursor));
		return FPLINUX_UI_TEXT_MODEL_CONSUMED;
	case KEY_RIGHT:
		move_cursor(model, next_boundary(model->text, model->length,
						 model->cursor));
		return FPLINUX_UI_TEXT_MODEL_CONSUMED;
	case KEY_HOME:
	case KEY_END:
		move_to_line_edge(model, key->code == KEY_END);
		return FPLINUX_UI_TEXT_MODEL_CONSUMED;
	case KEY_UP:
	case KEY_DOWN:
		return line_move(model, key->code == KEY_UP);
	case KEY_ENTER:
	case KEY_KPENTER:
		return insert_newline(model);
	default:
		break;
	}
	if (!is_printable(key->text))
		return FPLINUX_UI_TEXT_MODEL_IGNORED;
	if (model->type != FPLINUX_UI_TEXT_MODEL_ANY)
		return typed_text(model, key->text);
	insert_bytes(model, key->text, strlen(key->text));
	return FPLINUX_UI_TEXT_MODEL_CONSUMED;
}

void fplinux_ui_text_model_cancel_picker(struct fplinux_ui_text_model *model)
{
	if (!model->input.picker_open)
		return;
	close_picker(model, false);
	finish(model);
}

static bool take_consumed(struct fplinux_ui_text_model *model,
			  const struct fplinux_ui_text_model_key *key)
{
	unsigned char *byte = &model->consumed[key->keyboard][key->code / 8];
	unsigned char bit = (unsigned char)(1U << (key->code % 8));
	bool consumed = *byte & bit;

	*byte &= (unsigned char)~bit;
	return consumed;
}

static void release_key(struct fplinux_ui_text_model *model,
			const struct fplinux_ui_text_model_key *key,
			uint64_t now_ms)
{
	bool had_pending = fplinux_multitap_pending(&model->input.compose);

	if (key->keyboard)
		return;
	if (model->type == FPLINUX_UI_TEXT_MODEL_ANY &&
	    ((key->code == KEY_NUMERIC_POUND &&
	      model->input.held_control == '#') ||
	     (key->code == KEY_NUMERIC_STAR &&
	      model->input.held_control == '*'))) {
		fplinux_text_input_release(
			&model->input,
			key->code == KEY_NUMERIC_POUND ? '#' : '*', now_ms,
			emit_character, model);
		model->dirty = true;
	}
	if (key->code >= KEY_NUMERIC_0 && key->code <= KEY_NUMERIC_9)
		fplinux_text_input_release(
			&model->input,
			(unsigned char)('0' + key->code - KEY_NUMERIC_0),
			now_ms, emit_character, model);
	if (key->code == KEY_NUMERIC_0) {
		hold_phone_zero(model, now_ms);
		model->phone_zero_held = false;
	}
	if (key->code == KEY_F14)
		fplinux_text_input_clear_release(&model->input);
	if (had_pending != fplinux_multitap_pending(&model->input.compose))
		model->dirty = true;
}

bool fplinux_ui_text_model_init(struct fplinux_ui_text_model *model,
				bool multiline, size_t limit,
				const char *directory,
				const char *input_language)
{
	memset(model, 0, sizeof(*model));
	model->text = malloc(INITIAL_CAPACITY);
	model->display = malloc(INITIAL_CAPACITY);
	if (!model->text || !model->display) {
		fplinux_ui_text_model_cleanup(model);
		return false;
	}
	model->text[0] = '\0';
	model->display[0] = '\0';
	model->capacity = INITIAL_CAPACITY;
	model->separator = '.';
	model->limit = limit;
	model->multiline = multiline;
	model->focused = true;
	fplinux_text_input_init(&model->input, directory, input_language, true);
	return true;
}

void fplinux_ui_text_model_cleanup(struct fplinux_ui_text_model *model)
{
	free(model->text);
	free(model->display);
	free(model->masked);
	model->text = NULL;
	model->display = NULL;
	model->masked = NULL;
	model->capacity = 0;
	model->masked_capacity = 0;
}

enum fplinux_ui_text_model_result
fplinux_ui_text_model_key(struct fplinux_ui_text_model *model,
			  const struct fplinux_ui_text_model_key *key,
			  uint64_t now_ms)
{
	enum fplinux_ui_text_model_result result;

	if (!model->focused || key->code >= FPLINUX_UI_TEXT_MODEL_KEY_COUNT)
		return FPLINUX_UI_TEXT_MODEL_IGNORED;
	if (!key->pressed) {
		bool consumed = take_consumed(model, key);

		release_key(model, key, now_ms);
		finish(model);
		return consumed ? FPLINUX_UI_TEXT_MODEL_CONSUMED :
				  FPLINUX_UI_TEXT_MODEL_IGNORED;
	}
	if (model->read_only)
		return FPLINUX_UI_TEXT_MODEL_IGNORED;
	if (key->keyboard)
		result = keyboard_key(model, key);
	else
		result = phone_key(model, key->code, now_ms);
	if (result == FPLINUX_UI_TEXT_MODEL_CONSUMED)
		model->consumed[key->keyboard][key->code / 8] |=
			(unsigned char)(1U << (key->code % 8));
	finish(model);
	return result;
}

void fplinux_ui_text_model_tick(struct fplinux_ui_text_model *model,
				uint64_t now_ms)
{
	bool had_pending = fplinux_multitap_pending(&model->input.compose);
	const char *indicator = fplinux_text_input_indicator(&model->input);

	hold_phone_zero(model, now_ms);
	fplinux_text_input_tick(&model->input, now_ms, emit_character,
				erase_character, model);
	if (had_pending != fplinux_multitap_pending(&model->input.compose) ||
	    indicator != fplinux_text_input_indicator(&model->input))
		model->dirty = true;
	finish(model);
}

void fplinux_ui_text_model_cancel_held(struct fplinux_ui_text_model *model)
{
	if (model->input.picker_open)
		model->dirty = true;
	model->phone_zero_held = false;
	fplinux_text_input_cancel_held(&model->input);
	finish(model);
}

bool fplinux_ui_text_model_deadline(const struct fplinux_ui_text_model *model,
				    uint64_t *deadline_ms)
{
	uint64_t deadline;
	bool scheduled = fplinux_text_input_deadline(&model->input, &deadline);

	if (model->phone_zero_held &&
	    (!scheduled ||
	     model->phone_zero_ms + FPLINUX_TEXT_INPUT_HOLD_MS < deadline)) {
		deadline = model->phone_zero_ms + FPLINUX_TEXT_INPUT_HOLD_MS;
		scheduled = true;
	}
	if (scheduled)
		*deadline_ms = deadline;
	return scheduled;
}

/* Whether text suits the type; numbers may be unfinished, such as "-". */
static bool suits(enum fplinux_ui_text_model_type type, const char *text)
{
	bool separator = false;

	if (type == FPLINUX_UI_TEXT_MODEL_ANY)
		return true;
	if (type == FPLINUX_UI_TEXT_MODEL_PHONE)
		return strspn(text, "0123456789+*#pw") == strlen(text);
	if (*text == '-')
		++text;
	for (; *text; ++text) {
		if ((*text == '.' || *text == ',') &&
		    type == FPLINUX_UI_TEXT_MODEL_DECIMAL && !separator)
			separator = true;
		else if (*text < '0' || *text > '9')
			return false;
	}
	return true;
}

static void use_separator(struct fplinux_ui_text_model *model)
{
	char *separator;

	if (model->type != FPLINUX_UI_TEXT_MODEL_DECIMAL)
		return;
	separator = strpbrk(model->text, ".,");
	if (separator)
		*separator = model->separator;
}

bool fplinux_ui_text_model_set_text(struct fplinux_ui_text_model *model,
				    const char *text)
{
	size_t length = strlen(text);
	size_t characters = count_characters(text, length);

	if (!fits(model, characters)) {
		errno = E2BIG;
		return false;
	}
	if ((!model->multiline && strchr(text, '\n')) ||
	    !suits(model->type, text)) {
		errno = EINVAL;
		return false;
	}
	if (!reserve(model, length))
		return false;
	cancel_pending(model);
	model->input.clearing = false;
	memcpy(model->text, text, length + 1);
	use_separator(model);
	model->length = length;
	model->cursor = length;
	model->characters = characters;
	finish(model);
	return true;
}

bool fplinux_ui_text_model_set_type(struct fplinux_ui_text_model *model,
				    enum fplinux_ui_text_model_type type,
				    char separator)
{
	if ((unsigned int)type > FPLINUX_UI_TEXT_MODEL_PHONE ||
	    (type == FPLINUX_UI_TEXT_MODEL_DECIMAL && separator != '.' &&
	     separator != ',')) {
		errno = EINVAL;
		return false;
	}
	commit_pending(model);
	if (!suits(type, model->text)) {
		finish(model);
		errno = EINVAL;
		return false;
	}
	model->type = type;
	model->separator = separator;
	fplinux_text_input_reset_mode(&model->input,
				      type != FPLINUX_UI_TEXT_MODEL_ANY);
	model->input.picker_open = false;
	model->separator_typed = false;
	use_separator(model);
	fplinux_multitap_set_language(
		&model->input.compose,
		type == FPLINUX_UI_TEXT_MODEL_PHONE ?
			&phone_star :
			&model->input.languages.language[0]);
	model->dirty = true;
	finish(model);
	return true;
}

void fplinux_ui_text_model_set_password(struct fplinux_ui_text_model *model,
					bool password)
{
	if (model->password == password)
		return;
	model->password = password;
	fplinux_text_input_set_sentence_case(&model->input, !password);
	model->dirty = true;
	finish(model);
}

void fplinux_ui_text_model_set_focused(struct fplinux_ui_text_model *model,
				       bool focused)
{
	if (model->focused == focused)
		return;
	if (!focused) {
		commit_pending(model);
		model->input.picker_open = false;
		model->input.clearing = false;
		memset(model->consumed, 0, sizeof(model->consumed));
	}
	model->focused = focused;
	model->dirty = true;
	finish(model);
}

void fplinux_ui_text_model_set_picker_columns(
	struct fplinux_ui_text_model *model, unsigned int columns)
{
	fplinux_text_input_set_picker_columns(&model->input, columns);
}

void fplinux_ui_text_model_set_read_only(struct fplinux_ui_text_model *model,
					 bool read_only)
{
	if (model->read_only == read_only)
		return;
	if (read_only) {
		commit_pending(model);
		model->input.picker_open = false;
		model->input.clearing = false;
		model->separator_typed = false;
	}
	model->read_only = read_only;
	model->dirty = true;
	finish(model);
}

void fplinux_ui_text_model_set_caret(struct fplinux_ui_text_model *model,
				     size_t index)
{
	size_t cursor = 0;

	commit_pending(model);
	while (index && cursor < model->length) {
		cursor = next_boundary(model->text, model->length, cursor);
		--index;
	}
	move_cursor(model, cursor);
	finish(model);
}

bool fplinux_ui_text_model_is_empty(const struct fplinux_ui_text_model *model)
{
	return !model->length &&
	       !fplinux_multitap_pending(&model->input.compose);
}

bool fplinux_ui_text_model_takes_newline(
	const struct fplinux_ui_text_model *model)
{
	return model->multiline && model->type == FPLINUX_UI_TEXT_MODEL_ANY &&
	       !model->read_only;
}

const char *
fplinux_ui_text_model_view(const struct fplinux_ui_text_model *model)
{
	if (!model->password)
		return model->display;
	return model->masked ? model->masked : "";
}

size_t fplinux_ui_text_model_caret(const struct fplinux_ui_text_model *model)
{
	return count_characters(model->text, model->cursor) +
	       (fplinux_multitap_pending(&model->input.compose) ? 1 : 0);
}

bool fplinux_ui_text_model_pending(const struct fplinux_ui_text_model *model,
				   size_t *index)
{
	if (!fplinux_multitap_pending(&model->input.compose))
		return false;
	*index = count_characters(model->text, model->cursor);
	return true;
}

const char *
fplinux_ui_text_model_indicator(const struct fplinux_ui_text_model *model)
{
	return fplinux_text_input_indicator(&model->input);
}

void fplinux_ui_text_model_status(const struct fplinux_ui_text_model *model,
				  char *status, size_t size)
{
	const char *indicator = fplinux_ui_text_model_indicator(model);

	if (model->read_only) {
		if (size)
			status[0] = '\0';
		return;
	}
	if (!model->limit) {
		snprintf(status, size, "%s", indicator);
		return;
	}
	snprintf(status, size, "%s  %zu/%zu", indicator,
		 model->characters +
			 (fplinux_multitap_pending(&model->input.compose) ? 1 :
									    0),
		 model->limit);
}
