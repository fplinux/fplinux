/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef UMS9117_CM4_H4_H
#define UMS9117_CM4_H4_H

#include <linux/types.h>
#include <net/bluetooth/bluetooth.h>
#include <net/bluetooth/hci.h>

struct ums9117_h4_rx {
	u8 data[HCI_MAX_FRAME_SIZE];
	u16 length;
	u16 expected;
	u8 type;
	bool header_complete;
};

/* RX is unpadded. A positive return is one complete frame, excluding H4 type. */
int ums9117_h4_rx_byte(struct ums9117_h4_rx *rx, u8 byte);
void ums9117_h4_rx_reset(struct ums9117_h4_rx *rx);
/* header contains min(length, HCI_ACL_HDR_SIZE) bytes from the HCI packet. */
int ums9117_h4_tx_validate(u8 type, const u8 *header, size_t length);

#endif
