/* SPDX-License-Identifier: GPL-2.0-only */
/* Portable numeric-keypad multi-tap composition state machine. */

#ifndef FPLINUX_MULTITAP_H
#define FPLINUX_MULTITAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fplinux-language.h"

/* Input languages offered at once; further installed ones are left out. */
#define FPLINUX_MULTITAP_LANGUAGE_COUNT 8U

/* An input language: its key map and the names shown for it. */
struct fplinux_multitap_language {
	char code[FPLINUX_LANGUAGE_CODE_BYTES];
	char name[FPLINUX_LANGUAGE_TEXT_BYTES];
	/* Mode indicators indexed by enum fplinux_language_case. */
	char indicator[3][FPLINUX_LANGUAGE_TEXT_BYTES];
	/* UTF-8 characters cycled by keys 0-9; each contains its own digit. */
	char keys[FPLINUX_LANGUAGE_KEYS][FPLINUX_LANGUAGE_TEXT_BYTES];
};

struct fplinux_multitap_languages {
	struct fplinux_multitap_language
		language[FPLINUX_MULTITAP_LANGUAGE_COUNT];
	/* At least one after fplinux_multitap_load_languages(). */
	size_t count;
};

struct fplinux_multitap {
	unsigned char key;
	unsigned char index;
	bool pending;
	/* The latest press stepped an existing cycle instead of starting one. */
	bool advanced;
	uint32_t timeout_ms;
	/* Borrowed; it must outlive its use by this state. */
	const struct fplinux_multitap_language *language;
};

enum fplinux_multitap_emit_result {
	FPLINUX_MULTITAP_EMIT_ACCEPTED,
	FPLINUX_MULTITAP_EMIT_BLOCKED,
	FPLINUX_MULTITAP_EMIT_REJECTED,
};

enum fplinux_multitap_result {
	FPLINUX_MULTITAP_IGNORED,
	FPLINUX_MULTITAP_PENDING,
	FPLINUX_MULTITAP_COMMITTED,
	FPLINUX_MULTITAP_BLOCKED,
	FPLINUX_MULTITAP_REJECTED,
};

typedef enum fplinux_multitap_emit_result (*fplinux_multitap_emit_fn)(
	void *context, uint32_t character);

/*
 * Reads the input languages (input=yes) from the language files in
 * directory: the language named first, when it is one, then the others by
 * code. A language with a key whose characters lack the key's own digit is
 * left out. Without any, or with a NULL directory, the built-in English
 * definition is the only language.
 */
void fplinux_multitap_load_languages(
	struct fplinux_multitap_languages *languages, const char *directory,
	const char *first);

/* The caller supplies the interval for cycling and committing a candidate. */
void fplinux_multitap_init(struct fplinux_multitap *state,
			   const struct fplinux_multitap_language *language,
			   uint32_t timeout_ms);
bool fplinux_multitap_handles(unsigned char key);
bool fplinux_multitap_pending(const struct fplinux_multitap *state);
uint32_t fplinux_multitap_candidate(const struct fplinux_multitap *state);
void fplinux_multitap_cancel(struct fplinux_multitap *state);
/* Commit or cancel pending input before selecting a different language. */
void fplinux_multitap_set_language(
	struct fplinux_multitap *state,
	const struct fplinux_multitap_language *language);
/*
 * Upper case of a letter: Latin and Cyrillic by table, other scripts through
 * the C library; characters without one are returned unchanged.
 */
uint32_t fplinux_multitap_uppercase(uint32_t character);

enum fplinux_multitap_result
fplinux_multitap_commit(struct fplinux_multitap *state,
			fplinux_multitap_emit_fn emit, void *context);
enum fplinux_multitap_result
fplinux_multitap_expire(struct fplinux_multitap *state, uint32_t elapsed_ms,
			fplinux_multitap_emit_fn emit, void *context);
enum fplinux_multitap_result
fplinux_multitap_press(struct fplinux_multitap *state, unsigned char key,
		       uint32_t elapsed_ms, fplinux_multitap_emit_fn emit,
		       void *context);
/*
 * A long press of the pending key: the press is replaced by the key's digit.
 * When that press stepped an existing cycle, the character shown before it is
 * committed first. A blocked digit stays pending as the candidate.
 */
enum fplinux_multitap_result
fplinux_multitap_hold(struct fplinux_multitap *state,
		      fplinux_multitap_emit_fn emit, void *context);

#endif
