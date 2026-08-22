/* SPDX-License-Identifier: GPL-2.0-only */

#include <assert.h>
#include <stddef.h>
#include <string.h>

#include "fplinux-multitap.h"

struct output {
	char characters[64];
	size_t length;
};

static enum fplinux_multitap_emit_result emit_character(void *opaque,
							unsigned char character)
{
	struct output *output = opaque;

	assert(output->length < sizeof(output->characters));
	output->characters[output->length++] = (char)character;
	return FPLINUX_MULTITAP_EMIT_ACCEPTED;
}

static enum fplinux_multitap_emit_result
block_character(void *opaque, unsigned char character)
{
	(void)opaque;
	(void)character;
	return FPLINUX_MULTITAP_EMIT_BLOCKED;
}

static enum fplinux_multitap_emit_result
reject_character(void *opaque, unsigned char character)
{
	(void)opaque;
	(void)character;
	return FPLINUX_MULTITAP_EMIT_REJECTED;
}

static void assert_groups(void)
{
	static const struct {
		unsigned char key;
		const char *characters;
	} groups[] = {
		{ '0', " 0" },	 { '1', ".,!?@$/+-=%^_:;'*#1" },
		{ '2', "abc2" }, { '3', "def3" },
		{ '4', "ghi4" }, { '5', "jkl5" },
		{ '6', "mno6" }, { '7', "pqrs7" },
		{ '8', "tuv8" }, { '9', "wxyz9" },
	};
	struct fplinux_multitap state;
	size_t group;

	for (group = 0; group < sizeof(groups) / sizeof(groups[0]); ++group) {
		size_t index;

		fplinux_multitap_init(&state);
		assert(fplinux_multitap_handles(groups[group].key));
		for (index = 0; groups[group].characters[index] != '\0';
		     ++index) {
			assert(fplinux_multitap_press(&state, groups[group].key,
						      0, emit_character,
						      NULL) ==
			       FPLINUX_MULTITAP_PENDING);
			assert(fplinux_multitap_candidate(&state) ==
			       (unsigned char)groups[group].characters[index]);
		}
		assert(fplinux_multitap_press(&state, groups[group].key, 0,
					      emit_character, NULL) ==
		       FPLINUX_MULTITAP_PENDING);
		assert(fplinux_multitap_candidate(&state) ==
		       (unsigned char)groups[group].characters[0]);
	}
	assert(!fplinux_multitap_handles('*'));
	fplinux_multitap_init(&state);
	assert(fplinux_multitap_press(&state, '*', 0, emit_character, NULL) ==
	       FPLINUX_MULTITAP_IGNORED);
	assert(fplinux_multitap_candidate(&state) == '\0');
}

static void assert_timeout_boundary(void)
{
	struct fplinux_multitap state;
	struct output output = { { 0 }, 0 };

	fplinux_multitap_init(&state);
	assert(fplinux_multitap_press(&state, '2', 0, emit_character,
				      &output) == FPLINUX_MULTITAP_PENDING);
	assert(fplinux_multitap_expire(&state, 699, emit_character, &output) ==
	       FPLINUX_MULTITAP_PENDING);
	assert(output.length == 0);
	assert(fplinux_multitap_candidate(&state) == 'a');
	assert(fplinux_multitap_expire(&state, 700, emit_character, &output) ==
	       FPLINUX_MULTITAP_COMMITTED);
	assert(output.length == 1);
	assert(output.characters[0] == 'a');
	assert(fplinux_multitap_candidate(&state) == '\0');
}

static void assert_cycle_and_change_key_commit(void)
{
	struct fplinux_multitap state;
	struct output output = { { 0 }, 0 };

	fplinux_multitap_init(&state);
	assert(fplinux_multitap_press(&state, '2', 0, emit_character,
				      &output) == FPLINUX_MULTITAP_PENDING);
	assert(fplinux_multitap_press(&state, '2', 1, emit_character,
				      &output) == FPLINUX_MULTITAP_PENDING);
	assert(fplinux_multitap_candidate(&state) == 'b');
	assert(fplinux_multitap_press(&state, '3', 1, emit_character,
				      &output) == FPLINUX_MULTITAP_COMMITTED);
	assert(output.length == 1);
	assert(output.characters[0] == 'b');
	assert(fplinux_multitap_candidate(&state) == 'd');
	assert(fplinux_multitap_commit(&state, emit_character, &output) ==
	       FPLINUX_MULTITAP_COMMITTED);
	assert(output.length == 2);
	assert(memcmp(output.characters, "bd", output.length) == 0);
}

static void assert_cancel(void)
{
	struct fplinux_multitap state;

	fplinux_multitap_init(&state);
	assert(fplinux_multitap_press(&state, '9', 0, emit_character, NULL) ==
	       FPLINUX_MULTITAP_PENDING);
	fplinux_multitap_cancel(&state);
	assert(fplinux_multitap_candidate(&state) == '\0');
	assert(!fplinux_multitap_pending(&state));
}

static void assert_delivery_outcomes(void)
{
	struct fplinux_multitap state;

	fplinux_multitap_init(&state);
	assert(fplinux_multitap_press(&state, '2', 0, emit_character, NULL) ==
	       FPLINUX_MULTITAP_PENDING);
	assert(fplinux_multitap_press(&state, '3', 1, block_character, NULL) ==
	       FPLINUX_MULTITAP_BLOCKED);
	assert(fplinux_multitap_pending(&state));
	assert(fplinux_multitap_pending_key(&state) == '2');
	assert(fplinux_multitap_candidate(&state) == 'a');
	assert(fplinux_multitap_expire(&state, 700, reject_character, NULL) ==
	       FPLINUX_MULTITAP_REJECTED);
	assert(!fplinux_multitap_pending(&state));
	assert(fplinux_multitap_candidate(&state) == '\0');
}

int main(void)
{
	assert_groups();
	assert_timeout_boundary();
	assert_cycle_and_change_key_commit();
	assert_cancel();
	assert_delivery_outcomes();
	return 0;
}
