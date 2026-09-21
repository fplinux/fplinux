/* SPDX-License-Identifier: GPL-2.0-only */

#include "fplinux-multitap.h"

#include <string.h>

static const char *characters_for(unsigned char key)
{
	switch (key) {
	case '0':
		return " 0";
	case '1':
		return ".,!?@$/+-=%^_:;'*#1";
	case '2':
		return "abc2";
	case '3':
		return "def3";
	case '4':
		return "ghi4";
	case '5':
		return "jkl5";
	case '6':
		return "mno6";
	case '7':
		return "pqrs7";
	case '8':
		return "tuv8";
	case '9':
		return "wxyz9";
	default:
		return NULL;
	}
}

void fplinux_multitap_init(struct fplinux_multitap *state)
{
	state->key = '\0';
	state->index = 0;
	state->pending = false;
}

bool fplinux_multitap_handles(unsigned char key)
{
	return characters_for(key) != NULL;
}

bool fplinux_multitap_pending(const struct fplinux_multitap *state)
{
	return state->pending;
}

unsigned char fplinux_multitap_pending_key(const struct fplinux_multitap *state)
{
	return state->pending ? state->key : '\0';
}

unsigned char fplinux_multitap_candidate(const struct fplinux_multitap *state)
{
	const char *characters;

	if (!fplinux_multitap_pending(state))
		return '\0';
	characters = characters_for(state->key);
	if (!characters)
		return '\0';
	return (unsigned char)characters[state->index];
}

void fplinux_multitap_cancel(struct fplinux_multitap *state)
{
	fplinux_multitap_init(state);
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
	const char *characters;
	enum fplinux_multitap_result result = FPLINUX_MULTITAP_PENDING;

	characters = characters_for(key);
	if (!characters)
		return FPLINUX_MULTITAP_IGNORED;
	if (fplinux_multitap_pending(state) && state->key == key &&
	    elapsed_ms < FPLINUX_MULTITAP_TIMEOUT_MS) {
		state->index = (unsigned char)((state->index + 1U) %
					       strlen(characters));
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
