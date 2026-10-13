// SPDX-License-Identifier: GPL-2.0-only
#include <linux/errno.h>
#include <linux/string.h>

#include "marlin2-fm-wire.h"

size_t marlin2_fm_command_build(u8 *command, size_t capacity, u8 subcommand,
				const u8 *payload, size_t payload_bytes)
{
	if (payload_bytes > 254 || capacity < 5 + payload_bytes ||
	    (!payload && payload_bytes))
		return 0;
	command[0] = 0x01;
	command[1] = 0x8c;
	command[2] = 0xfc;
	command[3] = payload_bytes + 1;
	command[4] = subcommand;
	if (payload_bytes)
		memcpy(command + 5, payload, payload_bytes);
	return 5 + payload_bytes;
}

void marlin2_fm_transaction_fail(struct marlin2_fm_transaction *transaction,
				 int error)
{
	transaction->error = error;
	transaction->pending = false;
	transaction->event_bytes = 0;
}

int marlin2_fm_transaction_begin(struct marlin2_fm_transaction *transaction,
				 size_t expected_bytes, bool seek)
{
	if (transaction->error)
		return transaction->error;
	if (transaction->pending || transaction->event_bytes)
		return -EBUSY;
	if (expected_bytes < 7 || expected_bytes > sizeof(transaction->reply) ||
	    (seek && expected_bytes != 9))
		return -EINVAL;
	transaction->reply_bytes = 0;
	transaction->expected_bytes = expected_bytes;
	transaction->acknowledged = false;
	transaction->pending = true;
	transaction->seek = seek;
	return 0;
}

static int complete_event(struct marlin2_fm_transaction *transaction)
{
	const u8 *event = transaction->event;
	size_t bytes = transaction->event_bytes;

	if (!transaction->pending)
		return 0;
	if (event[1] == 0x0e && bytes >= 7 && event[4] == 0x8c &&
	    event[5] == 0xfc) {
		if (transaction->seek && !event[6]) {
			if (bytes != 7 || transaction->acknowledged)
				return -EPROTO;
			transaction->acknowledged = true;
			return 0;
		}
		if (bytes != transaction->expected_bytes &&
		    !(bytes == 7 && event[6]))
			return -EPROTO;
	} else if (transaction->seek && event[1] == 0xff && bytes >= 4 &&
		   event[3] == 0x30) {
		if (!transaction->acknowledged || bytes != 9)
			return -EPROTO;
	} else {
		return 0;
	}
	memcpy(transaction->reply, event, bytes);
	transaction->reply_bytes = bytes;
	transaction->pending = false;
	return 0;
}

int marlin2_fm_event_receive(struct marlin2_fm_transaction *transaction,
			     const u8 *payload, size_t payload_bytes)
{
	size_t index;
	int ret;

	if (transaction->error)
		return transaction->error;
	for (index = 0; index < payload_bytes; index++) {
		if (!transaction->event_bytes && payload[index] != 0x04) {
			ret = -EPROTO;
			goto failed;
		}
		transaction->event[transaction->event_bytes++] = payload[index];
		if (transaction->event_bytes < 3 ||
		    transaction->event_bytes < 3U + transaction->event[2])
			continue;
		ret = complete_event(transaction);
		if (ret)
			goto failed;
		transaction->event_bytes = 0;
	}
	return 0;

failed:
	marlin2_fm_transaction_fail(transaction, ret);
	return ret;
}
