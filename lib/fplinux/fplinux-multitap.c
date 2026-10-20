/* SPDX-License-Identifier: GPL-2.0-only */

#include "fplinux-multitap.h"

#include <string.h>
#include <wctype.h>

/* Language files listed per load; a code takes four bytes. */
#define LANGUAGE_LIST_COUNT 64U

/* Decodes one UTF-8 character; returns its bytes, or 0 when invalid. */
static size_t decode_utf8(const char *text, uint32_t *character)
{
	const unsigned char *bytes = (const unsigned char *)text;
	uint32_t value;
	size_t count;

	if (bytes[0] < 0x80) {
		*character = bytes[0];
		return 1;
	}
	if (bytes[0] >= 0xc2 && bytes[0] <= 0xdf) {
		value = bytes[0] & 0x1f;
		count = 2;
	} else if (bytes[0] >= 0xe0 && bytes[0] <= 0xef) {
		value = bytes[0] & 0x0f;
		count = 3;
	} else if (bytes[0] >= 0xf0 && bytes[0] <= 0xf4) {
		value = bytes[0] & 0x07;
		count = 4;
	} else {
		return 0;
	}
	for (size_t index = 1; index < count; ++index) {
		if ((bytes[index] & 0xc0) != 0x80)
			return 0;
		value = value << 6 | (bytes[index] & 0x3f);
	}
	if ((count == 3 && value < 0x800) || (count == 4 && value < 0x10000) ||
	    value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff))
		return 0;
	*character = value;
	return count;
}

/* The index-th character of a validated key map; 0 past its end. */
static uint32_t character_at(const char *characters, size_t index)
{
	while (*characters) {
		uint32_t character;
		size_t size = decode_utf8(characters, &character);

		if (!size)
			return 0;
		if (!index--)
			return character;
		characters += size;
	}
	return 0;
}

/* A key map is non-empty valid UTF-8 that contains the key's digit. */
static bool is_key_map_valid(const char *characters, unsigned char digit)
{
	bool has_digit = false;

	while (*characters) {
		uint32_t character;
		size_t size = decode_utf8(characters, &character);

		if (!size)
			return false;
		has_digit |= character == digit;
		characters += size;
	}
	return has_digit;
}

static bool add_language(struct fplinux_multitap_languages *languages,
			 const struct fplinux_language *language)
{
	struct fplinux_multitap_language *input;

	if (!language->input ||
	    languages->count == FPLINUX_MULTITAP_LANGUAGE_COUNT)
		return false;
	for (size_t key = 0; key < FPLINUX_LANGUAGE_KEYS; ++key)
		if (!is_key_map_valid(language->multitap[key],
				      (unsigned char)('0' + key)))
			return false;
	input = &languages->language[languages->count++];
	memcpy(input->code, language->code, sizeof(input->code));
	memcpy(input->name, language->name, sizeof(input->name));
	memcpy(input->indicator, language->indicator, sizeof(input->indicator));
	memcpy(input->keys, language->multitap, sizeof(input->keys));
	return true;
}

static void load_language(struct fplinux_multitap_languages *languages,
			  const char *directory, const char *code)
{
	struct fplinux_language language;

	if (fplinux_language_load(directory, code, &language))
		add_language(languages, &language);
}

void fplinux_multitap_load_languages(
	struct fplinux_multitap_languages *languages, const char *directory,
	const char *first)
{
	char codes[LANGUAGE_LIST_COUNT][FPLINUX_LANGUAGE_CODE_BYTES];
	size_t count = 0;

	memset(languages, 0, sizeof(*languages));
	if (directory)
		count = fplinux_language_list(directory, codes,
					      LANGUAGE_LIST_COUNT);
	if (count > LANGUAGE_LIST_COUNT)
		count = LANGUAGE_LIST_COUNT;
	for (size_t index = 0; first && index < count; ++index)
		if (!strcmp(codes[index], first))
			load_language(languages, directory, codes[index]);
	for (size_t index = 0; index < count; ++index)
		if (!first || strcmp(codes[index], first))
			load_language(languages, directory, codes[index]);
	if (!languages->count) {
		struct fplinux_language english;

		fplinux_language_english(&english);
		add_language(languages, &english);
	}
}

static const char *characters_for(const struct fplinux_multitap *state,
				  unsigned char key)
{
	if (!fplinux_multitap_handles(key) || !state->language)
		return NULL;
	return state->language->keys[key - '0'];
}

void fplinux_multitap_init(struct fplinux_multitap *state,
			   const struct fplinux_multitap_language *language,
			   uint32_t timeout_ms)
{
	state->key = '\0';
	state->index = 0;
	state->pending = false;
	state->advanced = false;
	state->timeout_ms = timeout_ms;
	state->language = language;
}

bool fplinux_multitap_handles(unsigned char key)
{
	return key >= '0' && key <= '9';
}

bool fplinux_multitap_pending(const struct fplinux_multitap *state)
{
	return state->pending;
}

uint32_t fplinux_multitap_candidate(const struct fplinux_multitap *state)
{
	const char *characters;

	if (!fplinux_multitap_pending(state))
		return '\0';
	characters = characters_for(state, state->key);
	if (!characters)
		return '\0';
	return character_at(characters, state->index);
}

void fplinux_multitap_cancel(struct fplinux_multitap *state)
{
	state->key = '\0';
	state->index = 0;
	state->pending = false;
	state->advanced = false;
}

void fplinux_multitap_set_language(
	struct fplinux_multitap *state,
	const struct fplinux_multitap_language *language)
{
	fplinux_multitap_cancel(state);
	state->language = language;
}

uint32_t fplinux_multitap_uppercase(uint32_t character)
{
	if (character >= 'a' && character <= 'z')
		return character - 'a' + 'A';
	if (character >= 0x430 && character <= 0x44f)
		return character - 0x20;
	if (character == 0x451)
		return 0x401;
	if (character < 0x80 || character > 0x10ffff)
		return character;
	return (uint32_t)towupper((wint_t)character);
}

static enum fplinux_multitap_result emit_pending(struct fplinux_multitap *state,
						 fplinux_multitap_emit_fn emit,
						 void *context)
{
	enum fplinux_multitap_emit_result result;

	if (!fplinux_multitap_pending(state))
		return FPLINUX_MULTITAP_PENDING;
	if (!emit)
		return FPLINUX_MULTITAP_BLOCKED;
	result = emit(context, fplinux_multitap_candidate(state));
	switch (result) {
	case FPLINUX_MULTITAP_EMIT_ACCEPTED:
		fplinux_multitap_cancel(state);
		return FPLINUX_MULTITAP_COMMITTED;
	case FPLINUX_MULTITAP_EMIT_BLOCKED:
		return FPLINUX_MULTITAP_BLOCKED;
	case FPLINUX_MULTITAP_EMIT_REJECTED:
		fplinux_multitap_cancel(state);
		return FPLINUX_MULTITAP_REJECTED;
	}
	return FPLINUX_MULTITAP_BLOCKED;
}

enum fplinux_multitap_result
fplinux_multitap_commit(struct fplinux_multitap *state,
			fplinux_multitap_emit_fn emit, void *context)
{
	return emit_pending(state, emit, context);
}

enum fplinux_multitap_result
fplinux_multitap_expire(struct fplinux_multitap *state, uint32_t elapsed_ms,
			fplinux_multitap_emit_fn emit, void *context)
{
	if (!fplinux_multitap_pending(state) || elapsed_ms < state->timeout_ms)
		return FPLINUX_MULTITAP_PENDING;
	return emit_pending(state, emit, context);
}

enum fplinux_multitap_result
fplinux_multitap_press(struct fplinux_multitap *state, unsigned char key,
		       uint32_t elapsed_ms, fplinux_multitap_emit_fn emit,
		       void *context)
{
	const char *characters = characters_for(state, key);
	enum fplinux_multitap_result result = FPLINUX_MULTITAP_PENDING;

	if (!characters || !*characters)
		return FPLINUX_MULTITAP_IGNORED;
	if (fplinux_multitap_pending(state) && state->key == key &&
	    elapsed_ms < state->timeout_ms) {
		++state->index;
		if (!character_at(characters, state->index))
			state->index = 0;
		state->advanced = true;
		return FPLINUX_MULTITAP_PENDING;
	}
	if (fplinux_multitap_pending(state)) {
		result = emit_pending(state, emit, context);
		if (result != FPLINUX_MULTITAP_COMMITTED)
			return result;
	}
	state->key = key;
	state->index = 0;
	state->pending = true;
	state->advanced = false;
	return result == FPLINUX_MULTITAP_COMMITTED ?
		       FPLINUX_MULTITAP_COMMITTED :
		       FPLINUX_MULTITAP_PENDING;
}

enum fplinux_multitap_result
fplinux_multitap_hold(struct fplinux_multitap *state,
		      fplinux_multitap_emit_fn emit, void *context)
{
	const char *characters = characters_for(state, state->key);
	unsigned char key = state->key;
	unsigned char digit = 0;
	uint32_t character;

	if (!fplinux_multitap_pending(state) || !characters)
		return FPLINUX_MULTITAP_IGNORED;
	/* Loaded maps contain the key's own digit; others cannot be held. */
	while ((character = character_at(characters, digit)) &&
	       character != key)
		++digit;
	if (!character)
		return FPLINUX_MULTITAP_IGNORED;
	if (state->advanced) {
		enum fplinux_multitap_result result;

		if (state->index) {
			--state->index;
		} else {
			while (character_at(characters, state->index + 1U))
				++state->index;
		}
		state->advanced = false;
		result = emit_pending(state, emit, context);
		if (result != FPLINUX_MULTITAP_COMMITTED)
			return result;
		state->key = key;
		state->pending = true;
	}
	state->index = digit;
	return emit_pending(state, emit, context);
}
