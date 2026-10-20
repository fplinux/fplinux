// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <linux/errno.h>

#include <linux/soc/ums9117/fm-wire.h>

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

	assert(ums9117_fm_command_build(command, sizeof(command), 0x00, enabled,
					sizeof(enabled)) == 7);
	assert(!memcmp(command, enable, sizeof(enable)));
	assert(ums9117_fm_command_build(command, sizeof(command), 0x01,
					frequency, sizeof(frequency)) == 7);
	assert(!memcmp(command, tune, sizeof(tune)));
	assert(ums9117_fm_command_build(command, sizeof(command), 0x04, search,
					sizeof(search)) == 8);
	assert(!memcmp(command, seek, sizeof(seek)));
	assert(ums9117_fm_command_build(command, sizeof(command), 0x02,
					&audible, 1) == 6);
	assert(!memcmp(command, unmute, sizeof(unmute)));
	assert(!ums9117_fm_command_build(command, 6, 0x00, enabled, 2));
	assert(!ums9117_fm_command_build(command, sizeof(command), 0, oversized,
					 sizeof(oversized)));
	assert(!ums9117_fm_command_build(command, sizeof(command), 0, NULL, 1));
}

static void tune_completion_split_after_each_byte(void)
{
	const u8 reply[] = { 0x04, 0x0e, 0x08, 0x01, 0x8c, 0xfc,
			     0x00, 0x34, 0x12, 0x2e, 0x22 };
	u8 output[11];
	size_t split;

	for (split = 0; split <= sizeof(reply); split++) {
		struct ums9117_fm_wire wire = { 0 };

		assert(!ums9117_fm_wire_begin(&wire, output, sizeof(output), 11,
					      false));
		assert(!ums9117_fm_wire_receive(&wire, true, reply, split));
		assert(wire.complete == (split == sizeof(reply)));
		assert(!ums9117_fm_wire_receive(&wire, true, reply + split,
						sizeof(reply) - split));
		assert(wire.complete && wire.reply_bytes == sizeof(reply));
		assert(!memcmp(output, reply, sizeof(reply)));
	}
}

static void seek_ack_and_result_in_one_payload(void)
{
	const u8 events[] = { 0x04, 0xff, 0x03, 0x00, 0x13, 0x24, 0x04, 0x0e,
			      0x04, 0x01, 0x8c, 0xfc, 0x00, 0x04, 0x0e, 0x04,
			      0x01, 0x03, 0x0c, 0x00, 0x04, 0xff, 0x06, 0x30,
			      0x00, 0x51, 0x09, 0x74, 0x27 };
	const u8 expected[] = { 0x04, 0xff, 0x06, 0x30, 0x00,
				0x51, 0x09, 0x74, 0x27 };
	u8 output[9];
	size_t split;

	for (split = 0; split <= sizeof(events); split++) {
		struct ums9117_fm_wire wire = { 0 };

		assert(!ums9117_fm_wire_begin(&wire, output, sizeof(output), 9,
					      true));
		assert(!ums9117_fm_wire_receive(&wire, true, events, split));
		assert(wire.complete == (split == sizeof(events)));
		assert(!ums9117_fm_wire_receive(&wire, true, events + split,
						sizeof(events) - split));
		assert(wire.complete && wire.acknowledged);
		assert(wire.reply_bytes == sizeof(expected));
		assert(!memcmp(output, expected, sizeof(expected)));
	}
}

static void rds_does_not_complete_tune(void)
{
	const u8 rds[] = { 0x04, 0xff, 0x03, 0x00, 0x13, 0x24 };
	const u8 rejection[] = { 0x04, 0x0e, 0x04, 0x01, 0x8c, 0xfc, 0x02 };
	struct ums9117_fm_wire wire = { 0 };
	u8 output[11];

	assert(!ums9117_fm_wire_begin(&wire, output, sizeof(output), 11,
				      false));
	assert(!ums9117_fm_wire_receive(&wire, true, rds, sizeof(rds)));
	assert(!wire.complete);
	assert(!ums9117_fm_wire_receive(&wire, true, rejection,
					sizeof(rejection)));
	assert(wire.complete && wire.reply_bytes == sizeof(rejection));
	assert(!memcmp(output, rejection, sizeof(rejection)));
}

static void malformed_seek_order_and_reply_lengths(void)
{
	const u8 premature[] = { 0x04, 0xff, 0x06, 0x30, 0x00,
				 0x51, 0x09, 0x74, 0x27 };
	const u8 ack[] = { 0x04, 0x0e, 0x04, 0x01, 0x8c, 0xfc, 0x00 };
	const u8 bad_type[] = { 0x01, 0x0e, 0x04, 0x01, 0x8c, 0xfc, 0x00 };
	const u8 short_success[] = { 0x04, 0x0e, 0x05, 0x01,
				     0x8c, 0xfc, 0x00, 0x01 };
	struct ums9117_fm_wire wire = { 0 };
	u8 output[11];

	assert(!ums9117_fm_wire_begin(&wire, output, sizeof(output), 9, true));
	assert(ums9117_fm_wire_receive(&wire, true, premature,
				       sizeof(premature)) == -EPROTO);
	memset(&wire, 0, sizeof(wire));
	assert(!ums9117_fm_wire_begin(&wire, output, sizeof(output), 9, true));
	assert(!ums9117_fm_wire_receive(&wire, true, ack, sizeof(ack)));
	assert(wire.acknowledged && !wire.complete);
	assert(ums9117_fm_wire_receive(&wire, true, ack, sizeof(ack)) ==
	       -EPROTO);
	memset(&wire, 0, sizeof(wire));
	assert(!ums9117_fm_wire_begin(&wire, output, sizeof(output), 7, false));
	assert(ums9117_fm_wire_receive(&wire, true, bad_type,
				       sizeof(bad_type)) == -EPROTO);
	memset(&wire, 0, sizeof(wire));
	assert(!ums9117_fm_wire_begin(&wire, output, sizeof(output), 11,
				      false));
	assert(ums9117_fm_wire_receive(&wire, true, short_success,
				       sizeof(short_success)) == -EPROTO);
}

static void reply_capacity_belongs_to_the_consumer(void)
{
	const u8 clock[] = { 0x04, 0x0e, 0x0e, 0x01, 0x8c, 0xfc,
			     0x00, 0x00, 0x94, 0x41, 0x00, 0x00,
			     0x80, 0x01, 0x00, 0x00, 0x03 };
	struct ums9117_fm_wire wire = { 0 };
	u8 marlin_reply[11];
	u8 cm4_reply[258];
	u8 oversized_reply[259];
	size_t index;

	assert(ums9117_fm_wire_begin(&wire, marlin_reply, sizeof(marlin_reply),
				     17, false) == -EINVAL);
	assert(!ums9117_fm_wire_begin(&wire, cm4_reply, sizeof(cm4_reply), 258,
				      false));
	assert(ums9117_fm_wire_begin(&wire, oversized_reply,
				     sizeof(oversized_reply), 259,
				     false) == -EINVAL);
	assert(!ums9117_fm_wire_begin(&wire, cm4_reply, sizeof(cm4_reply), 17,
				      false));
	for (index = 0; index < sizeof(clock); index++) {
		assert(!ums9117_fm_wire_receive(&wire, true, clock + index, 1));
		assert(wire.complete == (index + 1 == sizeof(clock)));
	}
	assert(wire.reply_bytes == sizeof(clock));
	assert(!memcmp(cm4_reply, clock, sizeof(clock)));
}

static void unadmitted_and_completed_replies_are_ignored(void)
{
	const u8 stale[] = { 0x04, 0x0e, 0x04, 0x01, 0x8c, 0xfc, 0x02 };
	const u8 current[] = { 0x04, 0x0e, 0x04, 0x01, 0x8c, 0xfc, 0x00 };
	struct ums9117_fm_wire wire = { 0 };
	u8 output[7];

	assert(!ums9117_fm_wire_begin(&wire, output, sizeof(output), 7, false));
	assert(!ums9117_fm_wire_receive(&wire, false, stale, 4));
	assert(wire.event_bytes == 4 && !wire.complete);
	assert(!ums9117_fm_wire_receive(&wire, false, stale + 4, 3));
	assert(!wire.event_bytes && !wire.complete && !wire.reply_bytes);
	assert(!ums9117_fm_wire_receive(&wire, true, current, sizeof(current)));
	assert(wire.complete && wire.reply_bytes == 7);
	assert(!ums9117_fm_wire_receive(&wire, true, stale, sizeof(stale)));
	assert(!memcmp(output, current, sizeof(current)));
}

int main(void)
{
	command_bytes();
	tune_completion_split_after_each_byte();
	seek_ack_and_result_in_one_payload();
	rds_does_not_complete_tune();
	malformed_seek_order_and_reply_lengths();
	reply_capacity_belongs_to_the_consumer();
	unadmitted_and_completed_replies_are_ignored();
	puts("UMS9117 FM wire host cases passed");
	return 0;
}
