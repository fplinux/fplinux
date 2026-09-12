// SPDX-License-Identifier: GPL-2.0-only
/*
 * Link the production setup object to synthetic firmware and scripted mailbox
 * I/O. This checks command bytes and reply handling, not CM4, IRQs or radio.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <linux/errno.h>
#include <linux/firmware.h>

#include "cm4-mailbox.h"
#include "cm4-setup.h"

/* Synthetic records: the final two config bytes must not replace PSKEY data. */
static const u8 config[8] = {
	0xd1, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xaa, 0xbb,
};
static const u8 sprd[176] = { [0 ... 175] = 0x33 };
static const u8 rf[252] = { [0 ... 251] = 0x44 };

/* Independent H4 transcript: PSKEY, RF, enable, Reset, with ALIGN8 padding. */
/* Keep command fields and padding on separate lines. */
/* clang-format off */
static const u8 expected_wire[456] = {
	0x01, 0xa0, 0xfc, 0xb0,
	[4 ... 8] = 0x33,
	/* PSKEY feature byte 1 must not advertise unsupported Park mode. */
	[9] = 0x32,
	[10 ... 23] = 0x33,
	[24] = 0xd1, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6,
	[30 ... 179] = 0x33,
	[180] = 0, 0, 0, 0,
	[184] = 0x01, 0xa2, 0xfc, 0xfc,
	[188 ... 439] = 0x44,
	[440] = 0x01, 0xa1, 0xfc, 0x03, 0x01, 0, 0x01, 0,
	[448] = 0x01, 0x03, 0x0c, 0, 0, 0, 0, 0,
};
/* clang-format on */
static const u8 replies[4][13] = {
	{ 4, 0x0e, 0x0a, 1, 0xa0, 0xfc, 0, 0x42, 0x16, 5, 0x12, 0x15, 0x20 },
	{ 4, 0x0e, 4, 1, 0xa2, 0xfc, 0 },
	{ 4, 0x0e, 6, 1, 0xa1, 0xfc, 1, 0, 1 },
	{ 4, 0x0e, 4, 1, 3, 0x0c, 0 },
};
static const u8 inoi_pskey_reply[] = {
	4, 0x0e, 0x0a, 1, 0xa0, 0xfc, 0, 0x65, 0x10, 6, 9, 0x15, 0x20,
};
static const u8 nokia_version[] = { 0x42, 0x16, 5, 0x12, 0x15, 0x20 };
static const u8 inoi_version[] = { 0x65, 0x10, 6, 9, 0x15, 0x20 };
static const size_t reply_sizes[] = { 13, 7, 9, 7 };
static const size_t command_ends[] = { 184, 440, 448, 456 };

static struct firmware firmware;
static int missing_record, wrong_size_record, acquired, released;
static unsigned int begin_calls, write_calls, next_reply;
static size_t wire_len, rx_head, rx_tail, tx_chunk, rx_chunk;
static u8 rx_queue[4096];
static bool asynchronous, stall_writes, command_status, partial_final_event;
static bool inoi_reply;
static const u8 *selected_version;
static int corrupt_step, corrupt_byte, corrupt_value, transport_error;

static int prepare_setup(void)
{
	static const char *const names[] = {
		"example/address.bin",
		"example/pskey.bin",
		"example/calibration.bin",
	};

	return ums9117_cm4_setup_prepare(NULL, names, selected_version);
}

int request_firmware_direct(const struct firmware **out, const char *name,
			    struct device *device)
{
	int record;

	assert(!device);
	if (!strcmp(name, "example/address.bin")) {
		record = 1;
		firmware.data = config;
		firmware.size = sizeof(config);
	} else if (!strcmp(name, "example/pskey.bin")) {
		record = 2;
		firmware.data = sprd;
		firmware.size = sizeof(sprd);
	} else {
		assert(!strcmp(name, "example/calibration.bin"));
		record = 3;
		firmware.data = rf;
		firmware.size = sizeof(rf);
	}
	if (record == missing_record)
		return -ENOENT;
	if (record == wrong_size_record)
		firmware.size--;
	acquired++;
	*out = &firmware;
	return 0;
}

void release_firmware(const struct firmware *fw)
{
	assert(fw == &firmware);
	released++;
}

int ums9117_cm4_mailbox_h4_begin(void)
{
	return ++begin_calls < 3 ? -EINPROGRESS : 0;
}

static void queue_bytes(const u8 *bytes, size_t size)
{
	assert(rx_tail + size <= sizeof(rx_queue));
	memcpy(rx_queue + rx_tail, bytes, size);
	rx_tail += size;
}

static void queue_response(unsigned int step)
{
	static const u8 unknown_command[] = { 4, 0x0f, 4, 1, 1, 0xa0, 0xfc };
	static const u8 other_complete[] = { 4, 0x0e, 4, 1, 2, 0x10, 0 };
	/* Largest legal event parameters, fragmented by the mailbox read stub. */
	u8 unrelated[258] = { 4, 0xff, 255 };
	u8 reply[13];

	if (command_status) {
		queue_bytes(unknown_command, sizeof(unknown_command));
		return;
	}
	if (asynchronous) {
		queue_bytes(unrelated, sizeof(unrelated));
		queue_bytes(other_complete, sizeof(other_complete));
	}
	memcpy(reply, inoi_reply && !step ? inoi_pskey_reply : replies[step],
	       sizeof(reply));
	if ((int)step == corrupt_step)
		reply[corrupt_byte] = corrupt_value;
	queue_bytes(reply, reply_sizes[step]);
	if (asynchronous)
		queue_bytes(other_complete, sizeof(other_complete));
	if (partial_final_event && step == 3) {
		static const u8 prefix[] = { 4, 0xff, 2, 0xa5 };

		queue_bytes(prefix, sizeof(prefix));
	}
}

int ums9117_cm4_mailbox_h4_write(const u8 *bytes, size_t size, size_t *written)
{
	size_t accepted = size < tx_chunk ? size : tx_chunk;

	write_calls++;
	if (transport_error)
		return transport_error;
	if (stall_writes && write_calls % 3 == 0)
		accepted = 0;
	assert(wire_len + accepted <= sizeof(expected_wire));
	assert(!memcmp(bytes, expected_wire + wire_len, accepted));
	wire_len += accepted;
	*written = accepted;
	if (next_reply < 4 && wire_len == command_ends[next_reply]) {
		queue_response(next_reply);
		next_reply++;
	}
	return 0;
}

int ums9117_cm4_mailbox_h4_read(u8 *bytes, size_t size, size_t *received)
{
	size_t available = rx_tail - rx_head;

	if (size > rx_chunk)
		size = rx_chunk;
	if (size > available)
		size = available;
	memcpy(bytes, rx_queue + rx_head, size);
	rx_head += size;
	*received = size;
	return 0;
}

static void reset_fixture(void)
{
	missing_record = wrong_size_record = acquired = released = 0;
	begin_calls = write_calls = next_reply = 0;
	wire_len = rx_head = rx_tail = 0;
	tx_chunk = 256;
	rx_chunk = 64;
	asynchronous = false;
	stall_writes = false;
	command_status = false;
	partial_final_event = false;
	inoi_reply = false;
	selected_version = nokia_version;
	corrupt_step = -1;
	transport_error = 0;
}

/* The caller, not the production polling routine, owns the setup deadline. */
static int run_until_terminal(void)
{
	unsigned int i;
	int ret;

	for (i = 0; i < 10000; i++) {
		ret = ums9117_cm4_setup_poll();
		if (ret != -EINPROGRESS)
			return ret;
	}
	return -ETIMEDOUT;
}

static void check_success(size_t tx, size_t rx, bool async)
{
	reset_fixture();
	tx_chunk = tx;
	rx_chunk = rx;
	asynchronous = async;
	stall_writes = true;
	assert(!prepare_setup());
	assert(acquired == 3 && released == 3);
	assert(!run_until_terminal());
	assert(wire_len == 456);
	assert(!ums9117_cm4_setup_poll());
	assert(wire_len == 456);
}

static void check_rejected_response(int step, int byte, int value, int expected)
{
	size_t before;

	reset_fixture();
	corrupt_step = step;
	corrupt_byte = byte;
	corrupt_value = value;
	assert(!prepare_setup());
	assert(run_until_terminal() == expected);
	before = wire_len;
	assert(ums9117_cm4_setup_poll() ==
	       (expected == -ETIMEDOUT ? -EINPROGRESS : expected));
	assert(before == wire_len);
}

static void check_partial_event_handoff(void)
{
	static const u8 expected[] = { 4, 0xff, 2, 0xa5 };
	const u8 *prefix;
	size_t bytes;

	reset_fixture();
	partial_final_event = true;
	assert(!prepare_setup());
	assert(!run_until_terminal());
	prefix = ums9117_cm4_setup_rx_prefix(&bytes);
	assert(bytes == sizeof(expected));
	assert(!memcmp(prefix, expected, bytes));
	assert(wire_len == 456);
}

static void check_firmware_admission(void)
{
	int record;

	for (record = 1; record <= 3; record++) {
		reset_fixture();
		missing_record = record;
		assert(prepare_setup() == -ENOENT);
		assert(acquired == released && !wire_len);
		assert(ums9117_cm4_setup_poll() == -ENOENT && !begin_calls);

		reset_fixture();
		wrong_size_record = record;
		assert(prepare_setup() == -EINVAL);
		assert(acquired == released && !wire_len);
		assert(ums9117_cm4_setup_poll() == -EINVAL && !begin_calls);
	}
}

static void check_revision_matching(void)
{
	reset_fixture();
	selected_version = inoi_version;
	inoi_reply = true;
	assert(!prepare_setup());
	assert(!run_until_terminal());
	assert(wire_len == 456);

	/* Neither fitted reply is accepted as the other selected revision. */
	reset_fixture();
	inoi_reply = true;
	assert(!prepare_setup());
	assert(run_until_terminal() == -EPROTO);
	assert(wire_len == 184);

	reset_fixture();
	selected_version = inoi_version;
	assert(!prepare_setup());
	assert(run_until_terminal() == -EPROTO);
	assert(wire_len == 184);
}

int main(void)
{
	check_firmware_admission();
	check_revision_matching();
	check_success(256, 64, false);
	check_success(7, 1, true);
	check_success(180, 2, true);
	check_success(256, 64, true);
	check_partial_event_handoff();
	check_rejected_response(0, 6, 1, -EREMOTEIO);
	check_rejected_response(1, 6, 1, -EREMOTEIO);
	check_rejected_response(3, 6, 1, -EREMOTEIO);
	check_rejected_response(0, 7, 0, -EPROTO);
	check_rejected_response(2, 8, 0, -EPROTO);
	check_rejected_response(0, 2, 9, -EPROTO);
	/* A different opcode never satisfies the outstanding command. */
	check_rejected_response(0, 4, 0x22, -ETIMEDOUT);

	reset_fixture();
	command_status = true;
	assert(!prepare_setup());
	assert(run_until_terminal() == -EREMOTEIO);

	reset_fixture();
	transport_error = -EIO;
	assert(!prepare_setup());
	assert(run_until_terminal() == -EIO);
	puts("PASS: setup wire bytes, firmware admission, reply matching and RX handoff");
	return 0;
}
