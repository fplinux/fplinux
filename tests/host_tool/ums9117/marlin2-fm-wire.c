// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <linux/errno.h>

#include "marlin2-fm-wire.h"

static void command_bytes(void)
{
	const u8 enable[] = { 0x01, 0x8c, 0xfc, 0x03, 0x00, 0x00, 0x01 };
	const u8 tune[] = { 0x01, 0x8c, 0xfc, 0x03, 0x01, 0x2e, 0x22 };
	const u8 seek[] = { 0x01, 0x8c, 0xfc, 0x04, 0x04, 0x2e, 0x22, 0x00 };
	const u8 enabled[] = { 0x00, 0x01 };
	const u8 frequency[] = { 0x2e, 0x22 };
	const u8 search[] = { 0x2e, 0x22, 0x00 };
	const u8 unmute[] = { 0x01, 0x8c, 0xfc, 0x02, 0x02, 0x00 };
	const u8 audible = 0;
	u8 command[259];
	u8 oversized[255] = { 0 };

	assert(marlin2_fm_command_build(command, sizeof(command), 0x00, enabled,
					sizeof(enabled)) == 7);
	assert(!memcmp(command, enable, sizeof(enable)));
	assert(marlin2_fm_command_build(command, sizeof(command), 0x01,
					frequency, sizeof(frequency)) == 7);
	assert(!memcmp(command, tune, sizeof(tune)));
	assert(marlin2_fm_command_build(command, sizeof(command), 0x04, search,
					sizeof(search)) == 8);
	assert(!memcmp(command, seek, sizeof(seek)));
	assert(marlin2_fm_command_build(command, sizeof(command), 0x02,
					&audible, 1) == 6);
	assert(!memcmp(command, unmute, sizeof(unmute)));
	assert(!marlin2_fm_command_build(command, 6, 0x00, enabled, 2));
	assert(!marlin2_fm_command_build(command, sizeof(command), 0, oversized,
					 sizeof(oversized)));
	assert(!marlin2_fm_command_build(command, sizeof(command), 0, NULL, 1));
}

static void tune_completion_split_after_each_byte(void)
{
	const u8 reply[] = { 0x04, 0x0e, 0x08, 0x01, 0x8c, 0xfc,
			     0x00, 0x34, 0x12, 0x2e, 0x22 };
	struct marlin2_fm_transaction transaction = { 0 };
	size_t index;

	assert(!marlin2_fm_transaction_begin(&transaction, 11, false));
	for (index = 0; index < sizeof(reply); index++) {
		assert(!marlin2_fm_event_receive(&transaction, reply + index,
						 1));
		assert(transaction.pending == (index + 1 < sizeof(reply)));
	}
	assert(transaction.reply_bytes == sizeof(reply));
	assert(!memcmp(transaction.reply, reply, sizeof(reply)));
}

static void seek_ack_and_result_in_one_payload(void)
{
	const u8 events[] = { 0x04, 0x0e, 0x04, 0x01, 0x8c, 0xfc, 0x00, 0x04,
			      0xff, 0x06, 0x30, 0x00, 0x51, 0x09, 0x74, 0x27 };
	const u8 expected[] = { 0x04, 0xff, 0x06, 0x30, 0x00,
				0x51, 0x09, 0x74, 0x27 };
	struct marlin2_fm_transaction transaction = { 0 };

	assert(!marlin2_fm_transaction_begin(&transaction, 9, true));
	assert(!marlin2_fm_event_receive(&transaction, events, sizeof(events)));
	assert(!transaction.pending);
	assert(transaction.acknowledged);
	assert(transaction.reply_bytes == sizeof(expected));
	assert(!memcmp(transaction.reply, expected, sizeof(expected)));
}

static void rds_does_not_complete_tune(void)
{
	const u8 rds[] = { 0x04, 0xff, 0x03, 0x00, 0x13, 0x24 };
	const u8 rejection[] = { 0x04, 0x0e, 0x04, 0x01, 0x8c, 0xfc, 0x02 };
	struct marlin2_fm_transaction transaction = { 0 };

	assert(!marlin2_fm_transaction_begin(&transaction, 11, false));
	assert(!marlin2_fm_event_receive(&transaction, rds, sizeof(rds)));
	assert(transaction.pending);
	assert(!marlin2_fm_event_receive(&transaction, rejection,
					 sizeof(rejection)));
	assert(!transaction.pending);
	assert(transaction.reply_bytes == sizeof(rejection));
	assert(!memcmp(transaction.reply, rejection, sizeof(rejection)));
}

static void malformed_and_late_replies(void)
{
	const u8 premature[] = { 0x04, 0xff, 0x06, 0x30, 0x00,
				 0x51, 0x09, 0x74, 0x27 };
	const u8 ack[] = { 0x04, 0x0e, 0x04, 0x01, 0x8c, 0xfc, 0x00 };
	const u8 bad_type[] = { 0x01, 0x0e, 0x04, 0x01, 0x8c, 0xfc, 0x00 };
	struct marlin2_fm_transaction transaction = { 0 };

	assert(!marlin2_fm_transaction_begin(&transaction, 9, true));
	assert(marlin2_fm_event_receive(&transaction, premature,
					sizeof(premature)) == -EPROTO);
	assert(marlin2_fm_transaction_begin(&transaction, 7, false) == -EPROTO);
	assert(marlin2_fm_event_receive(&transaction, ack, sizeof(ack)) ==
	       -EPROTO);
	memset(&transaction, 0, sizeof(transaction));
	assert(!marlin2_fm_transaction_begin(&transaction, 7, false));
	assert(marlin2_fm_event_receive(&transaction, bad_type,
					sizeof(bad_type)) == -EPROTO);
	memset(&transaction, 0, sizeof(transaction));
	assert(!marlin2_fm_transaction_begin(&transaction, 7, false));
	marlin2_fm_transaction_fail(&transaction, -ETIMEDOUT);
	assert(!transaction.pending);
	assert(marlin2_fm_event_receive(&transaction, ack, sizeof(ack)) ==
	       -ETIMEDOUT);
}

int main(void)
{
	command_bytes();
	tune_completion_split_after_each_byte();
	seek_ack_and_result_in_one_payload();
	rds_does_not_complete_tune();
	malformed_and_late_replies();
	puts("Marlin2 FM wire host cases passed");
	return 0;
}
