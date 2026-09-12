// SPDX-License-Identifier: GPL-2.0-only
#include <linux/errno.h>
#include <net/bluetooth/bluetooth.h>

#include "cm4-h4.h"

void ums9117_h4_rx_reset(struct ums9117_h4_rx *rx)
{
	rx->length = 0;
	rx->expected = 0;
	rx->type = 0;
	rx->header_complete = false;
}

int ums9117_h4_rx_byte(struct ums9117_h4_rx *rx, u8 byte)
{
	u16 payload;
	u16 limit;

	if (!rx->type) {
		switch (byte) {
		case HCI_EVENT_PKT:
			rx->expected = HCI_EVENT_HDR_SIZE;
			break;
		case HCI_ACLDATA_PKT:
			rx->expected = HCI_ACL_HDR_SIZE;
			break;
		case HCI_SCODATA_PKT:
			rx->expected = HCI_SCO_HDR_SIZE;
			break;
		default:
			return -EILSEQ;
		}
		rx->type = byte;
		return 0;
	}
	if (rx->length >= rx->expected)
		return -EPROTO;
	rx->data[rx->length++] = byte;
	if (rx->length != rx->expected)
		return 0;
	if (rx->header_complete)
		return 1;

	switch (rx->type) {
	case HCI_EVENT_PKT:
		payload = rx->data[1];
		limit = HCI_MAX_EVENT_SIZE;
		break;
	case HCI_ACLDATA_PKT:
		payload = rx->data[2] | (rx->data[3] << 8);
		limit = HCI_MAX_FRAME_SIZE;
		break;
	case HCI_SCODATA_PKT:
		payload = rx->data[2];
		limit = HCI_MAX_SCO_SIZE;
		break;
	default:
		return -EILSEQ;
	}
	if (payload > limit - rx->length)
		return -EMSGSIZE;
	rx->expected += payload;
	rx->header_complete = true;
	return !payload;
}

int ums9117_h4_tx_validate(u8 type, const u8 *header, size_t length)
{
	size_t header_size;
	size_t payload;
	size_t limit;

	switch (type) {
	case HCI_COMMAND_PKT:
		header_size = HCI_COMMAND_HDR_SIZE;
		limit = HCI_COMMAND_HDR_SIZE + 255;
		break;
	case HCI_ACLDATA_PKT:
		header_size = HCI_ACL_HDR_SIZE;
		limit = HCI_MAX_FRAME_SIZE;
		break;
	case HCI_SCODATA_PKT:
		header_size = HCI_SCO_HDR_SIZE;
		limit = HCI_MAX_SCO_SIZE;
		break;
	default:
		return -EILSEQ;
	}
	if (length < header_size || length > limit)
		return -EMSGSIZE;
	payload = header[2];
	if (type == HCI_ACLDATA_PKT)
		payload |= header[3] << 8;
	return length == header_size + payload ? 0 : -EPROTO;
}
