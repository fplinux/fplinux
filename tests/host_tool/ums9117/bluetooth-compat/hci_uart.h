/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef BLUETOOTH_HOST_HCI_UART_H
#define BLUETOOTH_HOST_HCI_UART_H

/* Minimal external HCI UART API used by the transport adapter host fake. */
#include <linux/skbuff.h>
#include <linux/types.h>
#include <net/bluetooth/hci.h>
#include <net/bluetooth/hci_core.h>

struct hci_uart {
	struct hci_dev *hdev;
	u8 alignment;
	u8 padding;
};

struct h4_recv_pkt {
	u8 type;
	u8 hlen;
	u8 loff;
	u8 lsize;
	u16 maxlen;
	int (*recv)(struct hci_dev *hdev, struct sk_buff *skb);
};

#define H4_RECV_ACL                                                   \
	.type = HCI_ACLDATA_PKT, .hlen = HCI_ACL_HDR_SIZE, .loff = 2, \
	.lsize = 2, .maxlen = HCI_MAX_FRAME_SIZE

#define H4_RECV_SCO                                                   \
	.type = HCI_SCODATA_PKT, .hlen = HCI_SCO_HDR_SIZE, .loff = 2, \
	.lsize = 1, .maxlen = HCI_MAX_SCO_SIZE

#define H4_RECV_EVENT                                                 \
	.type = HCI_EVENT_PKT, .hlen = HCI_EVENT_HDR_SIZE, .loff = 1, \
	.lsize = 1, .maxlen = HCI_MAX_EVENT_SIZE

struct sk_buff *h4_recv_buf(struct hci_uart *hu, struct sk_buff *skb,
			    const unsigned char *buffer, int count,
			    const struct h4_recv_pkt *pkts, int pkts_count);

#endif
