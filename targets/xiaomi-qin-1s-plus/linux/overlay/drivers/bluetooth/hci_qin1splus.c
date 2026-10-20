// SPDX-License-Identifier: GPL-2.0-only
#include <linux/firmware.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/remoteproc.h>
#include <linux/skbuff.h>
#include <linux/soc/sprd/qin1splus-sipc.h>
#include <linux/workqueue.h>

#include <net/bluetooth/bluetooth.h>
#include <net/bluetooth/hci_core.h>

#include "qin1splus-bt-protocol.h"

#define QIN_BT_PSKEY_FIRMWARE "qin1splus/bt-params.bin"
#define QIN_BT_COMMAND_TIMEOUT msecs_to_jiffies(5000)
#define QIN_BT_TX_QUEUE_LIMIT 64

struct qin_bt {
	struct device *dev;
	struct hci_dev *hdev;
	struct rproc *rproc;
	struct qin_sipc *sipc;
	struct task_struct *rx_thread;
	struct work_struct tx_work;
	struct sk_buff_head tx_queue;
	struct qin_bt_stream stream;
	/* Serializes non-sleeping send admission against close. */
	spinlock_t lock;
	atomic_t error;
	u32 session;
	bool running;
	bool power_owned;
	bool enable_echoed;
	bool teardown_clean;
};

static void stream_error(struct qin_bt *bt, int error)
{
	if (atomic_cmpxchg(&bt->error, 0, error))
		return;
	bt_dev_err(bt->hdev, "SIPC stream failed: %d type=%02x length=%u",
		   error, READ_ONCE(bt->stream.bad_type),
		   READ_ONCE(bt->stream.bad_length));
	qin1splus_sipc_bt_cancel(bt->sipc, bt->session);
	hci_cmd_sync_cancel_sync(bt->hdev, error);
}

static int receive_frame(void *context, u8 type, const u8 *data, size_t bytes)
{
	struct qin_bt *bt = context;
	struct sk_buff *skb;
	int ret;

	if (type == HCI_EVENT_PKT) {
		ret = qin_bt_validate_event(data, bytes);
		if (ret)
			return ret;
	}

	skb = bt_skb_alloc(bytes, GFP_KERNEL);
	if (!skb)
		return -ENOMEM;
	hci_skb_pkt_type(skb) = type;
	skb_put_data(skb, data, bytes);
	/* The core consumes skb on both success and error. */
	return hci_recv_frame(bt->hdev, skb);
}

static int receive_thread(void *data)
{
	struct qin_bt *bt = data;
	u8 bytes[256];
	int ret;

	while (!kthread_should_stop()) {
		ret = qin1splus_sipc_bt_read(bt->sipc, bt->session, bytes,
					     sizeof(bytes));
		if (kthread_should_stop() || !READ_ONCE(bt->running))
			break;
		if (ret == -ETIMEDOUT && !bt->stream.type)
			continue;
		if (ret < 0)
			goto failed;
		bt->hdev->stat.byte_rx += ret;
		ret = qin_bt_stream_receive(&bt->stream, bytes, ret,
					    receive_frame, bt);
		if (ret)
			goto failed;
	}
	return 0;
failed:
	bt->hdev->stat.err_rx++;
	stream_error(bt, ret);
	return ret;
}

static void transmit_work(struct work_struct *work)
{
	struct qin_bt *bt = container_of(work, struct qin_bt, tx_work);
	struct sk_buff *skb;
	u8 bytes[QIN_BT_TX_BYTES];
	int count, ret;

	while ((skb = skb_dequeue(&bt->tx_queue))) {
		if (!READ_ONCE(bt->running) || atomic_read(&bt->error)) {
			kfree_skb(skb);
			continue;
		}
		count = qin_bt_pack(hci_skb_pkt_type(skb), skb->data, skb->len,
				    bytes, sizeof(bytes));
		ret = count;
		if (count > 0)
			ret = qin1splus_sipc_bt_write(bt->sipc, bt->session,
						      bytes, count);
		if (ret < 0) {
			bt->hdev->stat.err_tx++;
			stream_error(bt, ret);
		} else {
			bt->hdev->stat.byte_tx += ret;
			switch (hci_skb_pkt_type(skb)) {
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
		kfree_skb(skb);
	}
}

static int qin_bt_send(struct hci_dev *hdev, struct sk_buff *skb)
{
	struct qin_bt *bt = hci_get_drvdata(hdev);
	unsigned long flags;
	u8 type = hci_skb_pkt_type(skb);
	int ret;

	if (type != HCI_COMMAND_PKT && type != HCI_ACLDATA_PKT &&
	    type != HCI_SCODATA_PKT)
		return -EILSEQ;
	if (skb->len > QIN_BT_FRAME_BYTES || skb_is_nonlinear(skb))
		return -EMSGSIZE;
	spin_lock_irqsave(&bt->lock, flags);
	ret = atomic_read(&bt->error);
	if (!ret && !bt->running)
		ret = -ESHUTDOWN;
	if (!ret && skb_queue_len(&bt->tx_queue) >= QIN_BT_TX_QUEUE_LIMIT)
		ret = -ENOBUFS;
	if (!ret) {
		/* Success transfers ownership. On failure the HCI core frees skb. */
		skb_queue_tail(&bt->tx_queue, skb);
		queue_work(system_wq, &bt->tx_work);
	}
	spin_unlock_irqrestore(&bt->lock, flags);
	return ret;
}

static int release_power(struct qin_bt *bt)
{
	int ret;

	if (!bt->power_owned)
		return 0;
	ret = rproc_shutdown(bt->rproc);
	if (ret) {
		bt_dev_err(bt->hdev,
			   "BTWF stop failed: %d; power reference retained",
			   ret);
		return ret;
	}
	bt->power_owned = false;
	return 0;
}

static int qin_bt_open(struct hci_dev *hdev)
{
	struct qin_bt *bt = hci_get_drvdata(hdev);
	int ret, stop_ret;

	/* Finish a failed close before acquiring any new power reference. */
	ret = release_power(bt);
	if (ret)
		goto failed;
	ret = rproc_boot(bt->rproc);
	if (ret)
		goto failed;
	bt->power_owned = true;
	ret = qin1splus_sipc_bt_claim(bt->sipc, &bt->session);
	if (ret)
		goto stop;
	memset(&bt->stream, 0, sizeof(bt->stream));
	atomic_set(&bt->error, 0);
	bt->enable_echoed = false;
	bt->teardown_clean = true;
	WRITE_ONCE(bt->running, true);
	bt->rx_thread = kthread_run(receive_thread, bt, "qin-bt-rx");
	if (IS_ERR(bt->rx_thread)) {
		ret = PTR_ERR(bt->rx_thread);
		bt->rx_thread = NULL;
		WRITE_ONCE(bt->running, false);
		qin1splus_sipc_bt_cancel(bt->sipc, bt->session);
		qin1splus_sipc_bt_release(bt->sipc, bt->session, true);
		goto stop;
	}
	return 0;
stop:
	stop_ret = release_power(bt);
	if (stop_ret)
		ret = stop_ret;
failed:
	bt_dev_err(hdev, "open failed: %d", ret);
	return ret;
}

static int qin_bt_flush(struct hci_dev *hdev)
{
	struct qin_bt *bt = hci_get_drvdata(hdev);

	cancel_work_sync(&bt->tx_work);
	skb_queue_purge(&bt->tx_queue);
	return 0;
}

static int qin_bt_close(struct hci_dev *hdev)
{
	struct qin_bt *bt = hci_get_drvdata(hdev);
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&bt->lock, flags);
	bt->running = false;
	spin_unlock_irqrestore(&bt->lock, flags);
	qin1splus_sipc_bt_cancel(bt->sipc, bt->session);
	qin_bt_flush(hdev);
	if (bt->rx_thread) {
		kthread_stop(bt->rx_thread);
		bt->rx_thread = NULL;
	}
	ret = qin1splus_sipc_bt_release(bt->sipc, bt->session,
					bt->teardown_clean &&
						!atomic_read(&bt->error) &&
						!bt->stream.type);
	if (ret)
		bt_dev_warn(
			hdev,
			"stream needs a verified BTWF halt before reuse: %d",
			ret);
	memset(&bt->stream, 0, sizeof(bt->stream));
	bt->enable_echoed = false;
	/* Core ignores close's return; release_power also logs retained ownership. */
	return release_power(bt);
}

static int set_enabled(struct qin_bt *bt, bool enabled)
{
	u8 parameters[] = { 0, 0, enabled };
	struct sk_buff *response;
	int ret;

	response = __hci_cmd_sync(bt->hdev, 0xfca1, sizeof(parameters),
				  parameters, QIN_BT_COMMAND_TIMEOUT);
	if (IS_ERR(response))
		return PTR_ERR(response);
	ret = qin_bt_validate_enable(response->data, response->len, enabled);
	kfree_skb(response);
	if (!ret)
		bt->enable_echoed = enabled;
	if (!ret && !enabled)
		bt->teardown_clean = true;
	return ret;
}

static int qin_bt_setup(struct hci_dev *hdev)
{
	struct qin_bt *bt = hci_get_drvdata(hdev);
	const struct firmware *parameters;
	struct sk_buff *response;
	int ret;

	ret = request_firmware(&parameters, QIN_BT_PSKEY_FIRMWARE, bt->dev);
	if (ret)
		return ret;
	if (parameters->size != QIN_BT_PSKEY_BYTES) {
		ret = -EINVAL;
		goto release;
	}
	bt->teardown_clean = false;
	response = __hci_cmd_sync(hdev, 0xfca0, parameters->size,
				  parameters->data, QIN_BT_COMMAND_TIMEOUT);
	if (IS_ERR(response)) {
		ret = PTR_ERR(response);
		goto release;
	}
	ret = qin_bt_validate_pskey(response->data, response->len);
	kfree_skb(response);
release:
	release_firmware(parameters);
	if (ret) {
		/* setup failures do not get the core's shutdown callback. */
		if (!atomic_read(&bt->error) && !bt->teardown_clean)
			set_enabled(bt, false);
		return ret;
	}
	/* The echo is a protocol receipt; native core init proves Reset/version. */
	ret = set_enabled(bt, true);
	if (ret && !atomic_read(&bt->error))
		set_enabled(bt, false);
	return ret;
}

static int qin_bt_shutdown(struct hci_dev *hdev)
{
	struct qin_bt *bt = hci_get_drvdata(hdev);

	if (!bt->enable_echoed || atomic_read(&bt->error))
		return 0;
	return set_enabled(bt, false);
}

static int qin_bt_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *node;
	struct qin_bt *bt;
	u32 phandle;
	int ret;

	bt = devm_kzalloc(dev, sizeof(*bt), GFP_KERNEL);
	if (!bt)
		return -ENOMEM;
	bt->dev = dev;
	bt->sipc = devm_qin1splus_sipc_get(dev);
	if (IS_ERR(bt->sipc))
		return dev_err_probe(dev, PTR_ERR(bt->sipc),
				     "SIPC unavailable\n");
	node = of_parse_phandle(dev->of_node, "remoteproc", 0);
	if (!node)
		return -EINVAL;
	phandle = node->phandle;
	of_node_put(node);
	bt->rproc = rproc_get_by_phandle(phandle);
	if (!bt->rproc)
		return -EPROBE_DEFER;
	if (!device_link_add(dev, bt->rproc->dev.parent,
			     DL_FLAG_AUTOREMOVE_CONSUMER)) {
		ret = -ENOMEM;
		goto put_rproc;
	}
	bt->hdev = hci_alloc_dev();
	if (!bt->hdev) {
		ret = -ENOMEM;
		goto put_rproc;
	}
	spin_lock_init(&bt->lock);
	atomic_set(&bt->error, 0);
	skb_queue_head_init(&bt->tx_queue);
	INIT_WORK(&bt->tx_work, transmit_work);
	bt->hdev->bus = HCI_IPC;
	bt->hdev->open = qin_bt_open;
	bt->hdev->close = qin_bt_close;
	bt->hdev->flush = qin_bt_flush;
	bt->hdev->send = qin_bt_send;
	bt->hdev->setup = qin_bt_setup;
	bt->hdev->shutdown = qin_bt_shutdown;
	hci_set_quirk(bt->hdev, HCI_QUIRK_NON_PERSISTENT_SETUP);
	hci_set_drvdata(bt->hdev, bt);
	SET_HCIDEV_DEV(bt->hdev, dev);
	platform_set_drvdata(pdev, bt);
	/* Registration schedules native power-on/setup, including rproc_boot. */
	ret = hci_register_dev(bt->hdev);
	if (ret < 0)
		goto free_hdev;
	return 0;
free_hdev:
	hci_free_dev(bt->hdev);
put_rproc:
	rproc_put(bt->rproc);
	return ret;
}

static void qin_bt_platform_shutdown(struct platform_device *pdev)
{
	struct qin_bt *bt = platform_get_drvdata(pdev);

	/* Unregister closes the transport even when controller setup failed. */
	hci_unregister_dev(bt->hdev);
	if (bt->power_owned) {
		dev_err(bt->dev,
			"shutdown retains an uncertain BTWF power reference\n");
		return;
	}
	hci_free_dev(bt->hdev);
	rproc_put(bt->rproc);
}

static const struct of_device_id qin_bt_match[] = {
	{ .compatible = "fplinux,qin1splus-bluetooth" },
	{}
};

/* Fixed built-in DT device: no module unload, remove or manual bind interface. */
static struct platform_driver qin_bt_driver = {
	.probe = qin_bt_probe,
	.shutdown = qin_bt_platform_shutdown,
	.driver = {
		.name = "qin1splus-bluetooth",
		.of_match_table = qin_bt_match,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(qin_bt_driver);

MODULE_FIRMWARE(QIN_BT_PSKEY_FIRMWARE);
MODULE_DESCRIPTION("Qin 1s+ Bluetooth shared-buffer HCI endpoint");
MODULE_LICENSE("GPL");
