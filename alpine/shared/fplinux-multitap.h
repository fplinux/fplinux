/* SPDX-License-Identifier: GPL-2.0-only */
/* Portable numeric-keypad multi-tap composition state machine. */

#ifndef FPLINUX_MULTITAP_H
#define FPLINUX_MULTITAP_H

#include <stdbool.h>
#include <stdint.h>

#define FPLINUX_MULTITAP_TIMEOUT_MS 700U

struct fplinux_multitap {
	unsigned char key;
	unsigned char index;
	bool pending;
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
	void *context, unsigned char character);

void fplinux_multitap_init(struct fplinux_multitap *state);
bool fplinux_multitap_handles(unsigned char key);
bool fplinux_multitap_pending(const struct fplinux_multitap *state);
unsigned char
fplinux_multitap_pending_key(const struct fplinux_multitap *state);
unsigned char fplinux_multitap_candidate(const struct fplinux_multitap *state);
void fplinux_multitap_cancel(struct fplinux_multitap *state);

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

#endif
