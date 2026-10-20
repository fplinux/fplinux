/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef QIN1SPLUS_BT_PROTOCOL_H
#define QIN1SPLUS_BT_PROTOCOL_H

#include <linux/types.h>

#define QIN_BT_PSKEY_BYTES 218
#define QIN_BT_FRAME_BYTES 1028
#define QIN_BT_TX_BYTES 1040

struct qin_bt_stream {
	u8 frame[QIN_BT_FRAME_BYTES];
	size_t used;
	size_t needed;
	u8 type;
	u8 bad_type;
	u32 bad_length;
};

/* RX accepts event/ACL/SCO only; no inferred transport padding is skipped. */
int qin_bt_stream_receive(struct qin_bt_stream *stream, const u8 *data,
			  size_t bytes,
			  int (*receive)(void *, u8, const u8 *, size_t),
			  void *context);
/* Input excludes H4 type; output includes its command/ACL alignment tail. */
int qin_bt_pack(u8 type, const u8 *data, size_t bytes, u8 *output,
		size_t capacity);
int qin_bt_validate_pskey(const u8 *data, size_t bytes);
int qin_bt_validate_enable(const u8 *data, size_t bytes, bool enabled);
/* Preserve exact initialization CC framing before the core removes headers. */
int qin_bt_validate_event(const u8 *data, size_t bytes);

#endif
