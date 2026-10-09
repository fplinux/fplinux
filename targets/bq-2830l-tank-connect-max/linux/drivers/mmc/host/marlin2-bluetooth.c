// SPDX-License-Identifier: GPL-2.0-only
#include <linux/completion.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/jiffies.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <net/bluetooth/bluetooth.h>
#include <net/bluetooth/hci_core.h>

#include "marlin2-bluetooth.h"
#include "marlin2-bluetooth-wire.h"
#include "marlin2-transport.h"

struct marlin2_bluetooth {
	struct hci_dev *hdev;
	struct marlin2_channel *channel;
	struct sk_buff_head tx_queue;
	struct work_struct tx_work;
	struct sk_buff *rx;
	struct completion setup_complete;
	spinlock_t setup_lock;
	u8 pskey[MARLIN2_BLUETOOTH_PSKEY_BYTES];
	u16 setup_opcode;
	u16 setup_mode;
	u8 setup_state;
	int setup_error;
	bool setup_pending;
	bool enabled;
	size_t rx_header_bytes;
	size_t rx_frame_bytes;
	int io_error;
};

static bool consume_setup_event(struct marlin2_bluetooth *bt, const u8 *event,
				size_t bytes)
{
	unsigned long flags;
	bool consumed = false;
	bool finished = false;
	int ret;

	spin_lock_irqsave(&bt->setup_lock, flags);
	if (!bt->setup_pending)
		goto unlock;
	ret = marlin2_bluetooth_setup_reply(bt->setup_opcode, bt->setup_mode,
					    bt->setup_state, event, bytes);
	if (ret == -EAGAIN)
		goto unlock;
	consumed = true;
	if (ret == -EINPROGRESS)
		goto unlock;
	bt->setup_error = ret;
	bt->setup_pending = false;
	finished = true;
unlock:
	spin_unlock_irqrestore(&bt->setup_lock, flags);
	if (finished)
		complete(&bt->setup_complete);
	return consumed;
}

static void cancel_setup(struct marlin2_bluetooth *bt, int error)
{
	unsigned long flags;
	bool pending;

	spin_lock_irqsave(&bt->setup_lock, flags);
	pending = bt->setup_pending;
	if (pending) {
		bt->setup_error = error;
		bt->setup_pending = false;
	}
	spin_unlock_irqrestore(&bt->setup_lock, flags);
	if (pending)
		complete(&bt->setup_complete);
}

static void receive(void *context, u8 subtype, const u8 *data, size_t bytes)
{
	struct marlin2_bluetooth *bt = context;
	size_t count;
	u8 type;
	int ret;

	if (subtype) {
		bt->hdev->stat.err_rx++;
		return;
	}
	while (bytes) {
		if (!bt->rx) {
			type = *data++;
			bytes--;
			switch (type) {
			case HCI_EVENT_PKT:
				bt->rx_header_bytes = 3;
				break;
			case HCI_ACLDATA_PKT:
				bt->rx_header_bytes = 5;
				break;
			case HCI_SCODATA_PKT:
				bt->rx_header_bytes = 4;
				break;
			default:
				bt->hdev->stat.err_rx++;
				return;
			}
			bt->rx = bt_skb_alloc(HCI_MAX_FRAME_SIZE + 1,
					      GFP_KERNEL);
			if (!bt->rx) {
				bt->hdev->stat.err_rx++;
				return;
			}
			*(u8 *)skb_put(bt->rx, 1) = type;
			bt->rx_frame_bytes = bt->rx_header_bytes;
		}
		count = min(bytes, bt->rx_frame_bytes - bt->rx->len);
		skb_put_data(bt->rx, data, count);
		data += count;
		bytes -= count;
		if (bt->rx->len < bt->rx_frame_bytes)
			continue;
		if (bt->rx_frame_bytes == bt->rx_header_bytes) {
			ret = marlin2_bluetooth_frame_length(
				bt->rx->data, bt->rx->len, &bt->rx_frame_bytes);
			if (ret ||
			    bt->rx_frame_bytes > HCI_MAX_FRAME_SIZE + 1) {
				bt->hdev->stat.err_rx++;
				kfree_skb(bt->rx);
				bt->rx = NULL;
				return;
			}
			if (bt->rx->len < bt->rx_frame_bytes)
				continue;
		}
		if (consume_setup_event(bt, bt->rx->data, bt->rx->len)) {
			kfree_skb(bt->rx);
			bt->rx = NULL;
			continue;
		}
		type = bt->rx->data[0];
		hci_skb_pkt_type(bt->rx) = type;
		skb_pull(bt->rx, 1);
		bt->hdev->stat.byte_rx += bt->rx->len;
		ret = hci_recv_frame(bt->hdev, bt->rx);
		if (ret)
			dev_err(&bt->hdev->dev,
				"Bluetooth core %s failed: type=%u error=%pe\n",
				__func__, type, ERR_PTR(ret));
		bt->rx = NULL;
	}
}

static void transport_error(void *context, int error)
{
	struct marlin2_bluetooth *bt = context;

	dev_err(&bt->hdev->dev, "Bluetooth transport receive failed: %pe\n",
		ERR_PTR(error));
	WRITE_ONCE(bt->io_error, error);
	cancel_setup(bt, error);
	bt->hdev->stat.err_rx++;
	kfree_skb(bt->rx);
	bt->rx = NULL;
}

static const struct marlin2_channel_ops channel_ops = {
	.receive = receive,
	.error = transport_error,
};

static void transmit_work(struct work_struct *work)
{
	struct marlin2_bluetooth *bt =
		container_of(work, struct marlin2_bluetooth, tx_work);
	enum marlin2_bluetooth_tx_path path;
	struct sk_buff *skb;
	size_t bytes;
	u8 *data;
	u8 type;
	int ret;

	while ((skb = skb_dequeue(&bt->tx_queue))) {
		ret = READ_ONCE(bt->io_error);
		if (ret)
			goto done;
		type = hci_skb_pkt_type(skb);
		data = kmalloc(skb->len + 8, GFP_KERNEL);
		if (!data) {
			ret = -ENOMEM;
			goto done;
		}
		ret = marlin2_bluetooth_tx_build(data, skb->len + 8, type,
						 skb->data, skb->len, &bytes,
						 &path);
		if (!ret) {
			if (path == MARLIN2_BLUETOOTH_PACKET)
				ret = marlin2_channel_send(bt->channel, 1, data,
							   bytes);
			else
				ret = marlin2_channel_write_bluetooth(
					bt->channel, data, bytes);
		}
		kfree(data);
		if (!ret) {
			bt->hdev->stat.byte_tx += skb->len;
			switch (type) {
			case HCI_COMMAND_PKT:
				bt->hdev->stat.cmd_tx++;
				break;
			case HCI_ACLDATA_PKT:
				bt->hdev->stat.acl_tx++;
				break;
			case HCI_SCODATA_PKT:
				bt->hdev->stat.sco_tx++;
				break;
			}
		}
done:
		if (ret) {
			bt->hdev->stat.err_tx++;
			dev_err(&bt->hdev->dev,
				"Bluetooth transmit failed: type=%u payload_bytes=%u error=%pe\n",
				hci_skb_pkt_type(skb), skb->len, ERR_PTR(ret));
		}
		kfree_skb(skb);
	}
}

static int open(struct hci_dev *hdev)
{
	struct marlin2_bluetooth *bt = hci_get_drvdata(hdev);
	int ret;

	WRITE_ONCE(bt->io_error, 0);
	ret = marlin2_channel_open(bt->channel);
	if (ret)
		dev_err(&hdev->dev, "Bluetooth transport %s failed: %pe\n",
			__func__, ERR_PTR(ret));
	return ret;
}

static int setup_command(struct marlin2_bluetooth *bt, u16 opcode,
			 const u8 *parameters, size_t parameter_bytes, u16 mode,
			 u8 state)
{
	u8 command[3 + MARLIN2_BLUETOOTH_PSKEY_BYTES];
	size_t wire_capacity = 8 + MARLIN2_BLUETOOTH_PSKEY_BYTES;
	u8 *wire;
	enum marlin2_bluetooth_tx_path path;
	unsigned long flags;
	unsigned long waited;
	size_t wire_bytes;
	int ret;

	command[0] = opcode;
	command[1] = opcode >> 8;
	command[2] = parameter_bytes;
	memcpy(command + 3, parameters, parameter_bytes);
	/* SDIO requires DMA-capable storage. */
	wire = kmalloc(wire_capacity, GFP_KERNEL);
	if (!wire)
		return -ENOMEM;
	ret = marlin2_bluetooth_tx_build(wire, wire_capacity, HCI_COMMAND_PKT,
					 command, 3 + parameter_bytes,
					 &wire_bytes, &path);
	if (ret) {
		kfree(wire);
		return ret;
	}
	reinit_completion(&bt->setup_complete);
	spin_lock_irqsave(&bt->setup_lock, flags);
	bt->setup_opcode = opcode;
	bt->setup_mode = mode;
	bt->setup_state = state;
	bt->setup_error = -ETIMEDOUT;
	bt->setup_pending = true;
	spin_unlock_irqrestore(&bt->setup_lock, flags);
	ret = marlin2_channel_write_bluetooth(bt->channel, wire, wire_bytes);
	kfree(wire);
	if (ret) {
		dev_err(&bt->hdev->dev,
			"Bluetooth setup transfer failed: opcode=0x%04x error=%pe\n",
			opcode, ERR_PTR(ret));
		cancel_setup(bt, ret);
		return ret;
	}
	/* This is the host HCI timeout, independent of SDIO framing. */
	waited = wait_for_completion_timeout(&bt->setup_complete,
					     HCI_CMD_TIMEOUT);
	if (!waited)
		cancel_setup(bt, -ETIMEDOUT);
	spin_lock_irqsave(&bt->setup_lock, flags);
	ret = bt->setup_error;
	spin_unlock_irqrestore(&bt->setup_lock, flags);
	if (ret)
		dev_err(&bt->hdev->dev,
			"Bluetooth setup reply failed: opcode=0x%04x completion=%lu io_error=%d error=%pe\n",
			opcode, waited, READ_ONCE(bt->io_error), ERR_PTR(ret));
	return ret;
}

static int setup(struct hci_dev *hdev)
{
	struct marlin2_bluetooth *bt = hci_get_drvdata(hdev);
	const u8 enable[] = { 1, 0, 1 };
	int ret;

	ret = setup_command(bt, MARLIN2_BLUETOOTH_WRITE_PSKEY, bt->pskey,
			    sizeof(bt->pskey), 0, 0);
	if (ret)
		return ret;
	ret = setup_command(bt, MARLIN2_BLUETOOTH_MODULE_ENABLE, enable,
			    sizeof(enable), 1, 1);
	if (!ret)
		bt->enabled = true;
	return ret;
}

static int flush(struct hci_dev *hdev)
{
	struct marlin2_bluetooth *bt = hci_get_drvdata(hdev);

	cancel_work_sync(&bt->tx_work);
	skb_queue_purge(&bt->tx_queue);
	return 0;
}

static int close(struct hci_dev *hdev)
{
	struct marlin2_bluetooth *bt = hci_get_drvdata(hdev);
	const u8 disable[] = { 0, 0, 0 };
	int ret = 0;

	cancel_setup(bt, -ECANCELED);
	flush(hdev);
	if (bt->enabled && !READ_ONCE(bt->io_error))
		ret = setup_command(bt, MARLIN2_BLUETOOTH_MODULE_ENABLE,
				    disable, sizeof(disable), 0, 0);
	bt->enabled = false;
	marlin2_channel_close(bt->channel);
	kfree_skb(bt->rx);
	bt->rx = NULL;
	return ret;
}

static int send(struct hci_dev *hdev, struct sk_buff *skb)
{
	struct marlin2_bluetooth *bt = hci_get_drvdata(hdev);
	int ret = READ_ONCE(bt->io_error);

	if (ret)
		return ret;
	switch (hci_skb_pkt_type(skb)) {
	case HCI_COMMAND_PKT:
	case HCI_ACLDATA_PKT:
	case HCI_SCODATA_PKT:
		break;
	default:
		return -EILSEQ;
	}
	skb_queue_tail(&bt->tx_queue, skb);
	schedule_work(&bt->tx_work);
	return 0;
}

struct marlin2_bluetooth *
marlin2_bluetooth_register(struct device *dev,
			   struct marlin2_transport *transport,
			   const u8 pskey[MARLIN2_BLUETOOTH_PSKEY_BYTES])
{
	struct marlin2_bluetooth *bt;
	int ret;

	bt = kzalloc_obj(*bt, GFP_KERNEL);
	if (!bt)
		return ERR_PTR(-ENOMEM);
	memcpy(bt->pskey, pskey, sizeof(bt->pskey));
	bt->hdev = hci_alloc_dev();
	if (!bt->hdev) {
		ret = -ENOMEM;
		goto free_bt;
	}
	skb_queue_head_init(&bt->tx_queue);
	INIT_WORK(&bt->tx_work, transmit_work);
	init_completion(&bt->setup_complete);
	spin_lock_init(&bt->setup_lock);
	bt->channel = marlin2_channel_register(
		transport, MARLIN2_CLIENT_BLUETOOTH, &channel_ops, bt);
	if (IS_ERR(bt->channel)) {
		ret = PTR_ERR(bt->channel);
		goto free_hdev;
	}
	bt->hdev->bus = HCI_SDIO;
	bt->hdev->open = open;
	bt->hdev->close = close;
	bt->hdev->flush = flush;
	bt->hdev->send = send;
	bt->hdev->setup = setup;
	hci_set_quirk(bt->hdev, HCI_QUIRK_NON_PERSISTENT_SETUP);
	hci_set_drvdata(bt->hdev, bt);
	SET_HCIDEV_DEV(bt->hdev, dev);
	ret = hci_register_dev(bt->hdev);
	if (ret < 0) {
		dev_err(dev, "Bluetooth registration failed: %pe\n",
			ERR_PTR(ret));
		goto unregister_channel;
	}
	return bt;

unregister_channel:
	marlin2_channel_unregister(bt->channel);
free_hdev:
	hci_free_dev(bt->hdev);
free_bt:
	kfree(bt);
	return ERR_PTR(ret);
}

void marlin2_bluetooth_unregister(struct marlin2_bluetooth *bt)
{
	cancel_setup(bt, -ECANCELED);
	hci_unregister_dev(bt->hdev);
	flush(bt->hdev);
	marlin2_channel_unregister(bt->channel);
	kfree_skb(bt->rx);
	hci_free_dev(bt->hdev);
	kfree(bt);
}

int marlin2_bluetooth_wait_ready(struct marlin2_bluetooth *bt)
{
	flush_work(&bt->hdev->power_on);
	if (hci_dev_test_flag(bt->hdev, HCI_SETUP))
		return -EIO;
	if (hci_dev_test_flag(bt->hdev, HCI_UNCONFIGURED))
		return -EKEYREJECTED;
	if (!bacmp(&bt->hdev->bdaddr, BDADDR_ANY))
		return -EADDRNOTAVAIL;
	return 0;
}
