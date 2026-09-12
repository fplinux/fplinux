// SPDX-License-Identifier: GPL-2.0-only
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/mutex.h>
#include <linux/skbuff.h>
#include <linux/string.h>
#include <linux/workqueue.h>
#include <net/bluetooth/bluetooth.h>
#include <net/bluetooth/hci_core.h>

#include "cm4-h4.h"
#include "cm4-hci.h"
#include "cm4-mailbox.h"

#define UMS9117_HCI_IO_ROUNDS 8
#define UMS9117_HCI_RX_CHUNK 256
#define UMS9117_HCI_TX_BYTES ALIGN(HCI_MAX_FRAME_SIZE + 1, 8)
#define UMS9117_HCI_QUIESCE_TIMEOUT_NS (2ULL * NSEC_PER_SEC)

static struct {
	struct hci_dev *hdev;
	struct delayed_work work;
	struct sk_buff_head tx_queue;
	struct sk_buff *tx_skb;
	struct ums9117_h4_rx rx;
	u8 tx_data[UMS9117_HCI_TX_BYTES];
	size_t tx_length;
	size_t tx_sent;
	int error;
	bool running;
	bool opened;
	bool rx_discard;
	bool suspending;
	bool transport_suspended;
	bool core_suspended;
	bool pm_activity;
	bool checking_pm_commands;
	int pm_command_error;
} runtime;

/*
 * io_lock serializes worker, open/close/flush and transport suspend/resume.
 * tx_queue.lock protects enqueue together with running/opened/error, because
 * send cannot sleep. Lock order is io_lock followed by tx_queue.lock. Mailbox
 * calls run with local IRQs enabled and no spinlock held. Admission takes
 * hdev->lock before io_lock; never hold either across a synchronous HCI call.
 */
static DEFINE_MUTEX(io_lock);

static void release_tx(void)
{
	kfree_skb(runtime.tx_skb);
	runtime.tx_skb = NULL;
	runtime.tx_length = 0;
	runtime.tx_sent = 0;
}

static void purge_queued_tx(void)
{
	struct sk_buff *skb;

	while ((skb = __skb_dequeue(&runtime.tx_queue)))
		kfree_skb(skb);
}

static void fail_transport(int error)
{
	unsigned long flags;

	spin_lock_irqsave(&runtime.tx_queue.lock, flags);
	runtime.error = error;
	runtime.running = false;
	purge_queued_tx();
	spin_unlock_irqrestore(&runtime.tx_queue.lock, flags);
	release_tx();
	ums9117_h4_rx_reset(&runtime.rx);
	bt_dev_err(runtime.hdev, "CM4 transport stopped: %d", error);
}

static void observe_pm_reply(void)
{
	const struct ums9117_h4_rx *rx = &runtime.rx;

	if (!runtime.checking_pm_commands || rx->type != HCI_EVENT_PKT)
		return;
	/* Native HCI suspend/resume do not propagate every command's status. */
	if ((rx->data[0] == HCI_EV_CMD_COMPLETE && rx->length >= 6 &&
	     rx->data[5]) ||
	    (rx->data[0] == HCI_EV_CMD_STATUS && rx->length >= 6 &&
	     rx->data[2]))
		runtime.pm_command_error = -EREMOTEIO;
}

static int receive_bytes(const u8 *input, size_t length)
{
	struct hci_dev *hdev = runtime.hdev;
	struct sk_buff *skb;
	size_t i;
	int ret;

	for (i = 0; i < length; i++) {
		if (!runtime.rx.type)
			runtime.rx_discard = !runtime.opened;
		ret = ums9117_h4_rx_byte(&runtime.rx, input[i]);
		if (ret < 0)
			return ret;
		if (!ret)
			continue;
		observe_pm_reply();
		if (!runtime.rx_discard && runtime.opened) {
			skb = bt_skb_alloc(runtime.rx.length, GFP_KERNEL);
			if (!skb)
				return -ENOMEM;
			hci_skb_pkt_type(skb) = runtime.rx.type;
			skb_put_data(skb, runtime.rx.data, runtime.rx.length);
			/* hci_recv_frame consumes skb on success and failure. */
			ret = hci_recv_frame(hdev, skb);
			if (ret && ret != -ENXIO)
				return ret;
		}
		ums9117_h4_rx_reset(&runtime.rx);
	}
	return 0;
}

static int transmit_frame(size_t *written)
{
	struct hci_dev *hdev = runtime.hdev;
	struct sk_buff *skb;
	size_t frame_length;
	int ret;

	*written = 0;
	if (!runtime.tx_skb) {
		skb = skb_dequeue(&runtime.tx_queue);
		if (!skb)
			return 0;
		runtime.tx_skb = skb;
		runtime.tx_data[0] = hci_skb_pkt_type(skb);
		ret = skb_copy_bits(skb, 0, runtime.tx_data + 1, skb->len);
		if (ret)
			return ret;
		frame_length = skb->len + 1;
		runtime.tx_length = ALIGN(frame_length, 8);
		memset(runtime.tx_data + frame_length, 0,
		       runtime.tx_length - frame_length);
	}
	ret = ums9117_cm4_mailbox_h4_write(runtime.tx_data + runtime.tx_sent,
					   runtime.tx_length - runtime.tx_sent,
					   written);
	runtime.tx_sent += *written;
	hdev->stat.byte_tx += *written;
	if (ret || runtime.tx_sent != runtime.tx_length)
		return ret;
	switch (hci_skb_pkt_type(runtime.tx_skb)) {
	case HCI_COMMAND_PKT:
		hdev->stat.cmd_tx++;
		break;
	case HCI_ACLDATA_PKT:
		hdev->stat.acl_tx++;
		break;
	case HCI_SCODATA_PKT:
		hdev->stat.sco_tx++;
		break;
	}
	release_tx();
	return 0;
}

static void runtime_work(struct work_struct *work)
{
	u8 input[UMS9117_HCI_RX_CHUNK];
	unsigned long flags;
	size_t received;
	size_t written;
	unsigned int round;
	int ret;

	mutex_lock(&io_lock);
	if (!runtime.running || runtime.error || runtime.transport_suspended)
		goto out;
	for (round = 0; round < UMS9117_HCI_IO_ROUNDS; round++) {
		ret = ums9117_cm4_mailbox_poll();
		if (ret && ret != -EINPROGRESS)
			goto fault;
		ret = ums9117_cm4_mailbox_h4_read(input, sizeof(input),
						  &received);
		runtime.hdev->stat.byte_rx += received;
		if (!ret)
			ret = receive_bytes(input, received);
		if (ret) {
			runtime.hdev->stat.err_rx++;
			goto fault;
		}
		ret = transmit_frame(&written);
		if (ret) {
			runtime.hdev->stat.err_tx++;
			goto fault;
		}
		if (!received && !written)
			break;
	}
	/* Publish notifications caused by the last read/write before sleeping. */
	ret = ums9117_cm4_mailbox_poll();
	if (ret && ret != -EINPROGRESS)
		goto fault;
	/* Enqueue under the same lock as stopping, so cancellation cannot miss it. */
	spin_lock_irqsave(&runtime.tx_queue.lock, flags);
	if (runtime.running && !runtime.error && !runtime.transport_suspended)
		schedule_delayed_work(&runtime.work, 1);
	spin_unlock_irqrestore(&runtime.tx_queue.lock, flags);
	goto out;

fault:
	fail_transport(ret);
out:
	mutex_unlock(&io_lock);
}

static int runtime_open(struct hci_dev *hdev)
{
	unsigned long flags;
	int ret = 0;

	mutex_lock(&io_lock);
	spin_lock_irqsave(&runtime.tx_queue.lock, flags);
	if (runtime.error)
		ret = runtime.error;
	else if (!runtime.running)
		ret = -ESHUTDOWN;
	else if (runtime.suspending)
		ret = -EBUSY;
	else
		runtime.opened = true;
	spin_unlock_irqrestore(&runtime.tx_queue.lock, flags);
	mutex_unlock(&io_lock);
	return ret;
}

static void flush_tx(bool close)
{
	unsigned long flags;

	mutex_lock(&io_lock);
	spin_lock_irqsave(&runtime.tx_queue.lock, flags);
	if (close) {
		runtime.opened = false;
		/* Keep a partial RX boundary but never deliver it after reopening. */
		runtime.rx_discard = true;
	}
	purge_queued_tx();
	spin_unlock_irqrestore(&runtime.tx_queue.lock, flags);
	/* Published H4 bytes cannot be withdrawn while CM4 keeps running. */
	if (runtime.tx_skb && !runtime.tx_sent)
		release_tx();
	mutex_unlock(&io_lock);
}

static int runtime_close(struct hci_dev *hdev)
{
	flush_tx(true);
	return 0;
}

static int runtime_flush(struct hci_dev *hdev)
{
	flush_tx(false);
	return 0;
}

/* Link creation is not admitted between the empty-link check and HCI suspend. */
static bool creates_connection(const u8 *header)
{
	u16 opcode = header[0] | (header[1] << 8);

	switch (opcode) {
	case HCI_OP_CREATE_CONN:
	case HCI_OP_ADD_SCO:
	case HCI_OP_ACCEPT_CONN_REQ:
	case HCI_OP_SETUP_SYNC_CONN:
	case HCI_OP_ACCEPT_SYNC_CONN_REQ:
	case HCI_OP_ENHANCED_SETUP_SYNC_CONN:
	case HCI_OP_LE_CREATE_CONN:
	case HCI_OP_LE_EXT_CREATE_CONN:
	case HCI_OP_LE_PA_CREATE_SYNC:
	case HCI_OP_LE_CREATE_CIS:
	case HCI_OP_LE_ACCEPT_CIS:
	case HCI_OP_LE_CREATE_BIG:
	case HCI_OP_LE_BIG_CREATE_SYNC:
		return true;
	default:
		return false;
	}
}

static int runtime_send(struct hci_dev *hdev, struct sk_buff *skb)
{
	u8 header[HCI_ACL_HDR_SIZE];
	unsigned long flags;
	int ret;

	ret = skb_copy_bits(skb, 0, header,
			    min_t(size_t, skb->len, sizeof(header)));
	if (ret)
		return ret;
	ret = ums9117_h4_tx_validate(hci_skb_pkt_type(skb), header, skb->len);
	if (ret)
		return ret;
	spin_lock_irqsave(&runtime.tx_queue.lock, flags);
	if (runtime.error)
		ret = runtime.error;
	else if (!runtime.running || !runtime.opened)
		ret = -ESHUTDOWN;
	else if (runtime.transport_suspended)
		ret = -EHOSTDOWN;
	else if (runtime.suspending &&
		 (hci_skb_pkt_type(skb) != HCI_COMMAND_PKT ||
		  creates_connection(header))) {
		runtime.pm_activity = true;
		ret = -EBUSY;
	} else
		__skb_queue_tail(&runtime.tx_queue, skb);
	spin_unlock_irqrestore(&runtime.tx_queue.lock, flags);
	/* A negative return leaves skb ownership with the HCI core. */
	return ret;
}

int ums9117_hci_runtime_register(struct device *dev, const u8 *rx_prefix,
				 size_t prefix_bytes)
{
	struct hci_dev *hdev;
	int ret;

	if (!rx_prefix && prefix_bytes)
		return -EINVAL;
	mutex_lock(&io_lock);
	if (runtime.hdev) {
		mutex_unlock(&io_lock);
		return -EALREADY;
	}
	memset(&runtime, 0, sizeof(runtime));
	skb_queue_head_init(&runtime.tx_queue);
	INIT_DELAYED_WORK(&runtime.work, runtime_work);
	hdev = hci_alloc_dev();
	if (!hdev) {
		runtime.error = -ENOMEM;
		mutex_unlock(&io_lock);
		return -ENOMEM;
	}
	runtime.hdev = hdev;
	/* The prologue owns these byte counters; preserve its partial RX boundary. */
	ret = receive_bytes(rx_prefix, prefix_bytes);
	if (ret) {
		runtime.error = ret;
		runtime.hdev = NULL;
		mutex_unlock(&io_lock);
		hci_free_dev(hdev);
		return ret;
	}
	runtime.running = true;
	hdev->bus = HCI_IPC;
	SET_HCIDEV_DEV(hdev, dev);
	hdev->open = runtime_open;
	hdev->close = runtime_close;
	hdev->flush = runtime_flush;
	hdev->send = runtime_send;
	/* The platform notifier propagates refusal before any link is torn down. */
	hci_set_quirk(hdev, HCI_QUIRK_NO_SUSPEND_NOTIFIER);
	mutex_unlock(&io_lock);

	/* hci_register_dev queues initial power-on and may call open immediately. */
	ret = hci_register_dev(hdev);
	if (ret < 0) {
		mutex_lock(&io_lock);
		runtime.running = false;
		runtime.error = ret;
		runtime.hdev = NULL;
		mutex_unlock(&io_lock);
		cancel_delayed_work_sync(&runtime.work);
		hci_free_dev(hdev);
		return ret;
	}
	schedule_delayed_work(&runtime.work, 0);
	return 0;
}

static bool hci_busy(struct hci_dev *hdev)
{
	return hci_conn_count(hdev) || hci_dev_test_flag(hdev, HCI_SETUP) ||
	       hci_dev_test_flag(hdev, HCI_CONFIG) ||
	       hci_dev_test_flag(hdev, HCI_USER_CHANNEL) ||
	       test_bit(HCI_INIT, &hdev->flags) ||
	       hdev->req_status == HCI_REQ_PEND ||
	       (test_bit(HCI_UP, &hdev->flags) &&
		!atomic_read(&hdev->cmd_cnt)) ||
	       !skb_queue_empty(&hdev->cmd_q) || !skb_queue_empty(&hdev->rx_q);
}

int ums9117_hci_suspend_prepare(void)
{
	struct hci_dev *hdev = runtime.hdev;
	unsigned long flags;
	int ret = 0;

	if (!hdev)
		return 0;
	/* Existing links are refused before send admission changes. */
	hci_dev_lock(hdev);
	mutex_lock(&io_lock);
	spin_lock_irqsave(&runtime.tx_queue.lock, flags);
	if (runtime.error)
		ret = runtime.error;
	else if (!runtime.running)
		ret = -ESHUTDOWN;
	else if (runtime.suspending || hci_busy(hdev) || runtime.tx_skb ||
		 runtime.rx.type || !skb_queue_empty(&runtime.tx_queue))
		ret = -EBUSY;
	else {
		runtime.suspending = true;
		runtime.pm_activity = false;
		runtime.pm_command_error = 0;
		runtime.checking_pm_commands = true;
	}
	spin_unlock_irqrestore(&runtime.tx_queue.lock, flags);
	mutex_unlock(&io_lock);
	hci_dev_unlock(hdev);
	if (ret)
		return ret;

	ret = hci_suspend_dev(hdev);
	mutex_lock(&io_lock);
	runtime.core_suspended = true;
	runtime.checking_pm_commands = false;
	spin_lock_irqsave(&runtime.tx_queue.lock, flags);
	if (!ret)
		ret = runtime.error	       ? runtime.error :
		      runtime.pm_command_error ? runtime.pm_command_error :
		      runtime.pm_activity      ? -EBUSY :
						 0;
	spin_unlock_irqrestore(&runtime.tx_queue.lock, flags);
	mutex_unlock(&io_lock);
	if (ret)
		/* The failing notifier is not included in PM's rollback chain. */
		ums9117_hci_post_suspend();
	return ret;
}

int ums9117_hci_suspend(void)
{
	struct hci_dev *hdev = runtime.hdev;
	u64 deadline = ktime_get_ns() + UMS9117_HCI_QUIESCE_TIMEOUT_NS;
	unsigned long flags;
	int ret;

	if (!hdev)
		return 0;
	do {
		hci_dev_lock(hdev);
		mutex_lock(&io_lock);
		spin_lock_irqsave(&runtime.tx_queue.lock, flags);
		if (runtime.error)
			ret = runtime.error;
		else if (!runtime.suspending || !runtime.running)
			ret = -ESHUTDOWN;
		else if (runtime.pm_activity || hci_busy(hdev) ||
			 runtime.tx_skb || runtime.rx.type ||
			 !skb_queue_empty(&runtime.tx_queue))
			ret = -EBUSY;
		else {
			/* Prevent send/rearm while the mailbox emptiness is tested. */
			runtime.transport_suspended = true;
			ret = 0;
		}
		spin_unlock_irqrestore(&runtime.tx_queue.lock, flags);
		if (!ret) {
			ret = ums9117_cm4_mailbox_suspend();
			if (ret) {
				spin_lock_irqsave(&runtime.tx_queue.lock,
						  flags);
				runtime.transport_suspended = false;
				spin_unlock_irqrestore(&runtime.tx_queue.lock,
						       flags);
			}
		}
		mutex_unlock(&io_lock);
		hci_dev_unlock(hdev);
		if (!ret) {
			cancel_delayed_work_sync(&runtime.work);
			return 0;
		}
		if (ret != -EBUSY)
			return ret;
		usleep_range(1000, 2000);
	} while (ktime_get_ns() < deadline);
	return -EBUSY;
}

int ums9117_hci_resume(void)
{
	unsigned long flags;
	int ret = 0;

	mutex_lock(&io_lock);
	if (!runtime.hdev || !runtime.transport_suspended)
		goto out;
	ret = ums9117_cm4_mailbox_resume();
	if (ret) {
		fail_transport(ret);
		goto out;
	}
	spin_lock_irqsave(&runtime.tx_queue.lock, flags);
	runtime.transport_suspended = false;
	if (runtime.running && !runtime.error)
		schedule_delayed_work(&runtime.work, 0);
	spin_unlock_irqrestore(&runtime.tx_queue.lock, flags);
out:
	mutex_unlock(&io_lock);
	return ret;
}

int ums9117_hci_post_suspend(void)
{
	struct hci_dev *hdev = runtime.hdev;
	unsigned long flags;
	int ret;

	if (!hdev)
		return 0;
	/* Also covers an aborted device suspend before the normal resume phase. */
	ret = ums9117_hci_resume();
	if (!ret && runtime.core_suspended) {
		mutex_lock(&io_lock);
		runtime.pm_command_error = 0;
		runtime.checking_pm_commands = true;
		mutex_unlock(&io_lock);
		ret = hci_resume_dev(hdev);
	}
	mutex_lock(&io_lock);
	runtime.checking_pm_commands = false;
	spin_lock_irqsave(&runtime.tx_queue.lock, flags);
	runtime.core_suspended = false;
	runtime.suspending = false;
	runtime.pm_activity = false;
	if (!ret)
		ret = runtime.error ? runtime.error : runtime.pm_command_error;
	spin_unlock_irqrestore(&runtime.tx_queue.lock, flags);
	mutex_unlock(&io_lock);
	return ret;
}

void ums9117_hci_runtime_unregister(void)
{
	struct hci_dev *hdev;
	unsigned long flags;

	mutex_lock(&io_lock);
	hdev = runtime.hdev;
	if (!hdev) {
		mutex_unlock(&io_lock);
		return;
	}
	spin_lock_irqsave(&runtime.tx_queue.lock, flags);
	runtime.running = false;
	runtime.opened = false;
	purge_queued_tx();
	spin_unlock_irqrestore(&runtime.tx_queue.lock, flags);
	mutex_unlock(&io_lock);
	cancel_delayed_work_sync(&runtime.work);
	/* Core close/flush take io_lock, so unregister must run without it. */
	hci_unregister_dev(hdev);
	mutex_lock(&io_lock);
	release_tx();
	ums9117_h4_rx_reset(&runtime.rx);
	runtime.hdev = NULL;
	mutex_unlock(&io_lock);
	hci_free_dev(hdev);
}
