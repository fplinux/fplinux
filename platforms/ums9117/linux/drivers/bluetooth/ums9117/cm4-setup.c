// SPDX-License-Identifier: GPL-2.0-only
#include <linux/errno.h>
#include <linux/firmware.h>
#include <linux/kernel.h>
#include <linux/string.h>

#include "cm4-setup.h"
#include "cm4-mailbox.h"

enum setup_step {
	UMS9117_CM4_SETUP_PSKEY,
	UMS9117_CM4_SETUP_RF,
	UMS9117_CM4_SETUP_ENABLE,
	UMS9117_CM4_SETUP_RESET,
	UMS9117_CM4_SETUP_COMPLETE,
};

static const u16 setup_opcode[] = { 0xfca0, 0xfca2, 0xfca1, 0x0c03 };

static struct {
	u8 config[8];
	u8 sprd[176];
	u8 rf[252];
	u8 version[UMS9117_CM4_VERSION_SIZE];
	u8 tx[256];
	u8 event[260];
	size_t tx_len, tx_sent, event_len, event_need;
	enum setup_step step;
	bool prepared, attached, awaiting;
	int error;
} setup;

const u8 *ums9117_cm4_setup_rx_prefix(size_t *bytes)
{
	*bytes = setup.event_len;
	return setup.event;
}

static int load_record(struct device *dev, const char *name, u8 *data,
		       size_t size)
{
	const struct firmware *fw;
	int ret;

	ret = request_firmware_direct(&fw, name, dev);
	if (ret)
		return ret;
	if (fw->size != size)
		ret = -EINVAL;
	else
		memcpy(data, fw->data, size);
	release_firmware(fw);
	return ret;
}

int ums9117_cm4_setup_prepare(struct device *dev, const char *const names[3],
			      const u8 version[UMS9117_CM4_VERSION_SIZE])
{
	int ret;

	memset(&setup, 0, sizeof(setup));
	memcpy(setup.version, version, sizeof(setup.version));
	ret = load_record(dev, names[0], setup.config, sizeof(setup.config));
	if (!ret)
		ret = load_record(dev, names[1], setup.sprd,
				  sizeof(setup.sprd));
	if (!ret)
		ret = load_record(dev, names[2], setup.rf, sizeof(setup.rf));
	setup.error = ret;
	setup.prepared = !ret;
	return ret;
}

static void prepare_command(void)
{
	u16 opcode = setup_opcode[setup.step];
	u8 plen = 0;

	memset(setup.tx, 0, sizeof(setup.tx));
	switch (setup.step) {
	case UMS9117_CM4_SETUP_PSKEY:
		plen = sizeof(setup.sprd);
		memcpy(setup.tx + 4, setup.sprd, plen);
		/* Stock AP replaces only device_addr; raw NV remains separate. */
		memcpy(setup.tx + 4 + 20, setup.config, 6);
		/* Fitted CM4 rejects Park commands and default link policy. */
		setup.tx[4 + 5] &= ~0x01;
		break;
	case UMS9117_CM4_SETUP_RF:
		plen = sizeof(setup.rf);
		memcpy(setup.tx + 4, setup.rf, plen);
		break;
	case UMS9117_CM4_SETUP_ENABLE:
		plen = 3;
		setup.tx[4] = 1;
		setup.tx[6] = 1;
		break;
	case UMS9117_CM4_SETUP_RESET:
	case UMS9117_CM4_SETUP_COMPLETE:
		break;
	}
	setup.tx[0] = 1;
	setup.tx[1] = opcode & 0xff;
	setup.tx[2] = opcode >> 8;
	setup.tx[3] = plen;
	setup.tx_len = ALIGN(4 + plen, 8);
	setup.tx_sent = 0;
}

static int consume_event(void)
{
	const u8 *event = setup.event;
	size_t len = setup.event_len;
	u16 opcode;

	if (event[1] == 0x0e) {
		if (len < 6)
			return -EPROTO;
		opcode = event[4] | (event[5] << 8);
	} else if (event[1] == 0x0f) {
		if (len != 7)
			return -EPROTO;
		opcode = event[5] | (event[6] << 8);
	} else {
		return 0;
	}
	if (!setup.awaiting || opcode != setup_opcode[setup.step])
		return 0;
	if (event[1] == 0x0f)
		/* Status cannot substitute for the required Command Complete. */
		return event[3] ? -EREMOTEIO : 0;
	if (event[3] != 1)
		return -EPROTO;
	if (setup.step == UMS9117_CM4_SETUP_ENABLE) {
		/* FCA1 returns mode_le16,state, with no status byte. */
		if (len != 9 || event[6] != 1 || event[7] || event[8] != 1)
			return -EPROTO;
	} else {
		if (len != (setup.step == UMS9117_CM4_SETUP_PSKEY ? 13 : 7))
			return -EPROTO;
		if (event[6])
			return -EREMOTEIO;
		if (setup.step == UMS9117_CM4_SETUP_PSKEY &&
		    memcmp(event + 7, setup.version, sizeof(setup.version)))
			return -EPROTO;
	}
	setup.step++;
	setup.awaiting = false;
	setup.tx_len = 0;
	setup.tx_sent = 0;
	return 0;
}

static int receive_events(void)
{
	u8 input[64];
	size_t received, i;
	int ret;

	ret = ums9117_cm4_mailbox_h4_read(input, sizeof(input), &received);
	if (ret)
		return ret;
	for (i = 0; i < received; i++) {
		if (!setup.event_len) {
			if (input[i] != 4)
				return -EPROTO;
			setup.event_need = 3;
		}
		setup.event[setup.event_len++] = input[i];
		if (setup.event_len == 3)
			setup.event_need = 3 + setup.event[2];
		if (setup.event_len != setup.event_need)
			continue;
		ret = consume_event();
		if (ret)
			return ret;
		setup.event_len = 0;
		setup.event_need = 0;
	}
	return 0;
}

int ums9117_cm4_setup_poll(void)
{
	size_t written;
	int ret;

	if (setup.error)
		return setup.error;
	if (!setup.prepared)
		return -EINVAL;
	if (setup.step == UMS9117_CM4_SETUP_COMPLETE)
		return 0;
	if (!setup.attached) {
		ret = ums9117_cm4_mailbox_h4_begin();
		if (ret == -EINPROGRESS)
			return ret;
		if (ret)
			goto fault;
		setup.attached = true;
	}
	if (!setup.tx_len)
		prepare_command();
	if (setup.tx_sent < setup.tx_len) {
		ret = ums9117_cm4_mailbox_h4_write(setup.tx + setup.tx_sent,
						   setup.tx_len - setup.tx_sent,
						   &written);
		if (ret)
			goto fault;
		setup.tx_sent += written;
		if (setup.tx_sent < setup.tx_len)
			return -EINPROGRESS;
		setup.awaiting = true;
	}
	ret = receive_events();
	if (ret)
		goto fault;
	return setup.step == UMS9117_CM4_SETUP_COMPLETE ? 0 : -EINPROGRESS;

fault:
	setup.error = ret;
	return ret;
}
