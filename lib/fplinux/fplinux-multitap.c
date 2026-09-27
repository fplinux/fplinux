/* SPDX-License-Identifier: GPL-2.0-only */

#include "fplinux-multitap.h"

#include <stddef.h>
#include <uchar.h>

static const char32_t *characters_for(unsigned char key,
				      enum fplinux_multitap_language language)
{
	static const char32_t *const russian[] = {
		U"абвг2", U"деёжз3", U"ийкл4", U"мноп5",
		U"рсту6", U"фхцч7",  U"шщъы8", U"ьэюя9",
	};

	if (language == FPLINUX_MULTITAP_RUSSIAN && key >= '2' && key <= '9')
		return russian[key - '2'];
	switch (key) {
	case '0':
		return U" 0";
	case '1':
		return U".,!?@$/+-=%^_:;'*#1";
	case '2':
		return U"abc2";
	case '3':
		return U"def3";
	case '4':
		return U"ghi4";
	case '5':
		return U"jkl5";
	case '6':
		return U"mno6";
	case '7':
		return U"pqrs7";
	case '8':
		return U"tuv8";
	case '9':
		return U"wxyz9";
	default:
		return NULL;
	}
}

void fplinux_multitap_init(struct fplinux_multitap *state)
{
	state->key = '\0';
	state->index = 0;
	state->pending = false;
	state->language = FPLINUX_MULTITAP_ENGLISH;
}

bool fplinux_multitap_handles(unsigned char key)
{
	return characters_for(key, FPLINUX_MULTITAP_ENGLISH) != NULL;
}

bool fplinux_multitap_pending(const struct fplinux_multitap *state)
{
	return state->pending;
}

unsigned char fplinux_multitap_pending_key(const struct fplinux_multitap *state)
{
	return state->pending ? state->key : '\0';
}

uint32_t fplinux_multitap_candidate(const struct fplinux_multitap *state)
{
	const char32_t *characters;

	if (!fplinux_multitap_pending(state))
		return '\0';
	characters = characters_for(state->key, state->language);
	if (!characters)
		return '\0';
	return characters[state->index];
}

void fplinux_multitap_cancel(struct fplinux_multitap *state)
{
	state->key = '\0';
	state->index = 0;
	state->pending = false;
}

void fplinux_multitap_set_language(struct fplinux_multitap *state,
				   enum fplinux_multitap_language language)
{
	fplinux_multitap_cancel(state);
	state->language = language;
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
	if (!fplinux_multitap_pending(state) ||
	    elapsed_ms < FPLINUX_MULTITAP_TIMEOUT_MS)
		return FPLINUX_MULTITAP_PENDING;
	return emit_pending(state, emit, context);
}

enum fplinux_multitap_result
fplinux_multitap_press(struct fplinux_multitap *state, unsigned char key,
		       uint32_t elapsed_ms, fplinux_multitap_emit_fn emit,
		       void *context)
{
	const char32_t *characters;
	enum fplinux_multitap_result result = FPLINUX_MULTITAP_PENDING;

	characters = characters_for(key, state->language);
	if (!characters)
		return FPLINUX_MULTITAP_IGNORED;
	if (fplinux_multitap_pending(state) && state->key == key &&
	    elapsed_ms < FPLINUX_MULTITAP_TIMEOUT_MS) {
		++state->index;
		if (!characters[state->index])
			state->index = 0;
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
	return result == FPLINUX_MULTITAP_COMMITTED ?
		       FPLINUX_MULTITAP_COMMITTED :
		       FPLINUX_MULTITAP_PENDING;
}
