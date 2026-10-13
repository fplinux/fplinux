/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_MARLIN2_TRANSPORT_WIRE_H
#define FPLINUX_MARLIN2_TRANSPORT_WIRE_H

#include <linux/types.h>

struct marlin2_transport_message {
	const u8 *data;
	size_t length;
	u8 type;
	u8 subtype;
};

typedef int (*marlin2_transport_receive_fn)(
	void *context, const struct marlin2_transport_message *message);

struct marlin2_transport_aggregate {
	const u8 *packet;
	size_t data_bytes;
	size_t offset;
	u32 used_payload_bytes;
	u32 next_payload_bytes;
	u32 parsed_payload_bytes;
};

/* Initialize the envelope before its payload is filled in place. */
size_t marlin2_transport_record_prepare(u8 *packet, size_t capacity, u8 type,
					u8 subtype, size_t payload_bytes);
size_t marlin2_transport_record_build(u8 *packet, size_t capacity, u8 type,
				      u8 subtype, const u8 *payload,
				      size_t payload_bytes);
/* The aggregate borrows packet storage throughout iteration. */
int marlin2_transport_aggregate_init(
	struct marlin2_transport_aggregate *aggregate, const u8 *packet,
	size_t read_bytes);
/*
 * Skip dummy records; return 1 for a message, 0 at the end, or a negative errno.
 * puh is updated as soon as a header is read, including malformed records.
 */
int marlin2_transport_aggregate_next(
	struct marlin2_transport_aggregate *aggregate,
	struct marlin2_transport_message *message, u32 *puh);
int marlin2_transport_aggregate_decode(const u8 *packet, size_t read_bytes,
				       marlin2_transport_receive_fn receive,
				       void *context, u32 *next_payload_bytes);

#endif
