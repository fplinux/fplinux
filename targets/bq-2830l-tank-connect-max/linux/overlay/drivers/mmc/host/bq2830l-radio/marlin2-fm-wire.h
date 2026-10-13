/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_MARLIN2_FM_WIRE_H
#define FPLINUX_MARLIN2_FM_WIRE_H

#include <linux/types.h>

#define MARLIN2_FM_COMMAND_BYTES 259U
#define MARLIN2_FM_EVENT_BYTES 258U

struct marlin2_fm_transaction {
	u8 event[MARLIN2_FM_EVENT_BYTES];
	u8 reply[11];
	size_t event_bytes;
	size_t reply_bytes;
	size_t expected_bytes;
	bool pending;
	bool seek;
	bool acknowledged;
	int error;
};

size_t marlin2_fm_command_build(u8 *command, size_t capacity, u8 subcommand,
				const u8 *payload, size_t payload_bytes);
int marlin2_fm_transaction_begin(struct marlin2_fm_transaction *transaction,
				 size_t expected_bytes, bool seek);
int marlin2_fm_event_receive(struct marlin2_fm_transaction *transaction,
			     const u8 *payload, size_t payload_bytes);
void marlin2_fm_transaction_fail(struct marlin2_fm_transaction *transaction,
				 int error);

#endif
