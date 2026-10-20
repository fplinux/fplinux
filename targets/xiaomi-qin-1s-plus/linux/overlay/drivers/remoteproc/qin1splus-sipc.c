// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (C) 2015 Spreadtrum Communications Inc. */
#include <linux/device.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/jiffies.h>
#include <linux/mailbox_client.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/sysfs.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

#include <linux/soc/sprd/qin1splus-sipc.h>
#include "qin1splus-sipc-layout.h"

#define QIN_SIPC_RESERVED_BASE 0x88000000
#define QIN_SIPC_RESERVED_BYTES 0x280000
#define QIN_SIPC_SMEM_OFFSET 0x180000
#define QIN_SIPC_RX_COUNT 64
#define QIN_SIPC_TX_COUNT 20
#define QIN_SIPC_TX_TIMEOUT_MS 5000

#define QIN_SMSG_OPEN 1
#define QIN_SMSG_CLOSE 2
#define QIN_SMSG_EVENT 4
#define QIN_SMSG_CMD 5
#define QIN_SMSG_DONE 6
#define QIN_SMSG_OPEN_MAGIC 0xbeee
#define QIN_SMSG_CLOSE_MAGIC 0xeddd
#define QIN_SMSG_INIT 1
#define QIN_SMSG_INIT_DONE 2

struct qin_smsg {
	u32 words[2];
};

struct qin_sipc_tx {
	struct qin_smsg message;
	unsigned long deadline;
	bool pending;
};

struct qin_sipc_channel {
	struct qin_sipc_owner owner;
	u32 open_count;
	u32 init_count;
	u32 event_count;
	u32 close_count;
	bool opened;
	bool init_queued;
	bool initialized_once;
};

struct qin_sipc {
	struct device *dev;
	void __iomem *ram;
	struct mbox_client client;
	struct mbox_chan *mbox;
	/* Serializes protocol processing with the power owner's transitions. */
	struct mutex lock;
	/* RX callback and TX completion run in IRQ context. */
	spinlock_t irq_lock;
	struct work_struct rx_work;
	struct delayed_work timeout_work;
	struct qin_smsg rx[QIN_SIPC_RX_COUNT];
	struct qin_sipc_tx tx[QIN_SIPC_TX_COUNT];
	struct qin_sipc_channel channels[QIN_SIPC_OWNER_COUNT];
	u32 rx_read;
	u32 rx_write;
	u32 rx_count;
	u32 dropped;
	u32 tx_count;
	u32 tx_done;
	u32 timeouts;
	struct qin_smsg last_rx;
	int fault;
	bool accepting;
	bool prepared;
	bool released;
	wait_queue_head_t bt_wait;
	atomic_t bt_events;
	u32 bt_session;
	int bt_error;
	bool bt_claimed;
	bool bt_used;
};

static void bt_wake(struct qin_sipc *sipc)
{
	atomic_inc(&sipc->bt_events);
	wake_up_all(&sipc->bt_wait);
}

/* Caller holds lock; no reset/reopen can lend a new stream to an old token. */
static void bt_revoke(struct qin_sipc *sipc, int error)
{
	/* Cancellation is benign only until a real protocol/stop fault arrives. */
	if (!sipc->bt_error || sipc->bt_error == -ECANCELED)
		sipc->bt_error = error;
	bt_wake(sipc);
}

static struct qin_smsg make_message(u8 channel, u8 type, u16 flag, u32 value)
{
	return (struct qin_smsg){
		.words = { channel | (u32)type << 8 | (u32)flag << 16, value },
	};
}

static void receive_message(struct mbox_client *client, void *data)
{
	struct qin_sipc *sipc = container_of(client, struct qin_sipc, client);
	unsigned long flags;
	bool queued = false;

	spin_lock_irqsave(&sipc->irq_lock, flags);
	/* Controller's stack-backed words cease to exist when this returns. */
	memcpy(&sipc->last_rx, data, sizeof(sipc->last_rx));
	sipc->rx_count++;
	if (sipc->accepting &&
	    sipc->rx_write - sipc->rx_read < QIN_SIPC_RX_COUNT) {
		sipc->rx[sipc->rx_write % QIN_SIPC_RX_COUNT] = sipc->last_rx;
		sipc->rx_write++;
		queued = true;
	} else {
		sipc->dropped++;
		if (sipc->accepting && !sipc->fault)
			sipc->fault = -ENOBUFS;
	}
	spin_unlock_irqrestore(&sipc->irq_lock, flags);
	bt_wake(sipc);
	if (queued)
		queue_work(system_wq, &sipc->rx_work);
}

static void complete_message(struct mbox_client *client, void *data, int status)
{
	struct qin_sipc *sipc = container_of(client, struct qin_sipc, client);
	struct qin_sipc_tx *tx =
		container_of(data, struct qin_sipc_tx, message);
	unsigned long flags;

	spin_lock_irqsave(&sipc->irq_lock, flags);
	tx->pending = false;
	sipc->tx_done++;
	if (status && !sipc->fault)
		sipc->fault = status;
	spin_unlock_irqrestore(&sipc->irq_lock, flags);
	bt_wake(sipc);
}

/* Caller holds lock. The mailbox retains the message until real TX-done. */
static int send_message(struct qin_sipc *sipc, struct qin_smsg message)
{
	struct qin_sipc_tx *tx = NULL;
	unsigned long flags;
	int i, ret;

	spin_lock_irqsave(&sipc->irq_lock, flags);
	if (sipc->fault) {
		ret = sipc->fault;
		goto unlock;
	}
	for (i = 0; i < QIN_SIPC_TX_COUNT; i++) {
		if (!sipc->tx[i].pending) {
			tx = &sipc->tx[i];
			break;
		}
	}
	if (!tx) {
		ret = -ENOBUFS;
		goto unlock;
	}
	tx->message = message;
	tx->pending = true;
	tx->deadline = jiffies + msecs_to_jiffies(QIN_SIPC_TX_TIMEOUT_MS);
	spin_unlock_irqrestore(&sipc->irq_lock, flags);
	ret = mbox_send_message(sipc->mbox, &tx->message);
	spin_lock_irqsave(&sipc->irq_lock, flags);
	if (ret < 0) {
		tx->pending = false;
		if (!sipc->fault)
			sipc->fault = ret;
	} else {
		sipc->tx_count++;
		ret = 0;
	}
unlock:
	spin_unlock_irqrestore(&sipc->irq_lock, flags);
	if (ret)
		bt_wake(sipc);
	return ret;
}

static struct qin_sipc_channel *find_channel(struct qin_sipc *sipc, u8 channel)
{
	int i;

	for (i = 0; i < QIN_SIPC_OWNER_COUNT; i++)
		if (sipc->channels[i].owner.channel == channel)
			return &sipc->channels[i];
	return NULL;
}

static int dispatch_message(struct qin_sipc *sipc, struct qin_smsg message)
{
	u8 channel = message.words[0];
	u8 type = message.words[0] >> 8;
	u16 flag = message.words[0] >> 16;
	u32 value = message.words[1];
	struct qin_sipc_channel *ch = find_channel(sipc, channel);
	int ret;

	if (!ch)
		return -EOPNOTSUPP;
	switch (type) {
	case QIN_SMSG_OPEN:
		if (flag != QIN_SMSG_OPEN_MAGIC)
			return -EPROTO;
		ch->open_count++;
		if (ch->open_count > 1) {
			if (channel == 4 && sipc->bt_used)
				bt_revoke(sipc, -EPIPE);
			else if (ch->initialized_once)
				qin_sipc_recover_owner(sipc->ram, &ch->owner);
			ch->init_queued = false;
			ch->opened = true;
			return send_message(
				sipc, make_message(channel, QIN_SMSG_OPEN,
						   QIN_SMSG_OPEN_MAGIC, 0));
		}
		/* The initial AP OPEN already supplies the peer's OPEN reply. */
		ch->opened = true;
		return 0;
	case QIN_SMSG_CLOSE:
		if (!ch->opened || flag != QIN_SMSG_CLOSE_MAGIC)
			return -EPROTO;
		ch->close_count++;
		if (channel == 4 && sipc->bt_used)
			bt_revoke(sipc, -EPIPE);
		else if (!ch->owner.block)
			qin_sipc_recover_owner(sipc->ram, &ch->owner);
		ret = send_message(sipc, make_message(channel, QIN_SMSG_CLOSE,
						      QIN_SMSG_CLOSE_MAGIC, 0));
		if (ret)
			return ret;
		ch->opened = false;
		ch->init_queued = false;
		return 0;
	case QIN_SMSG_CMD:
		if (!ch->opened || flag != QIN_SMSG_INIT || ch->init_queued)
			return -EPROTO;
		ch->init_count++;
		/* Headers/descriptors must reach RAM before exposing the CP address. */
		wmb();
		ret = send_message(
			sipc,
			make_message(channel, QIN_SMSG_DONE, QIN_SMSG_INIT_DONE,
				     QIN_SIPC_CP_BASE + ch->owner.offset));
		if (!ret) {
			ch->init_queued = true;
			ch->initialized_once = true;
			bt_wake(sipc);
		}
		return ret;
	case QIN_SMSG_EVENT:
		if (!ch->init_queued || (flag != 1 && flag != 2) ||
		    (!ch->owner.block && value >= ch->owner.count))
			return -EPROTO;
		/* Endpoint data stays in the rings; no fictitious read/release ACK. */
		ch->event_count++;
		if (channel == 4)
			bt_wake(sipc);
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static void receive_work(struct work_struct *work)
{
	struct qin_sipc *sipc = container_of(work, struct qin_sipc, rx_work);
	struct qin_smsg message;
	unsigned long flags;
	bool present;
	int ret;

	mutex_lock(&sipc->lock);
	for (;;) {
		spin_lock_irqsave(&sipc->irq_lock, flags);
		present = sipc->accepting && sipc->rx_read != sipc->rx_write;
		if (present)
			message = sipc->rx[sipc->rx_read++ % QIN_SIPC_RX_COUNT];
		spin_unlock_irqrestore(&sipc->irq_lock, flags);
		if (!present)
			break;
		ret = dispatch_message(sipc, message);
		if (ret && (u8)message.words[0] == 4)
			bt_revoke(sipc, ret);
		if (ret)
			dev_warn_ratelimited(sipc->dev,
					     "unhandled RX %08x %08x: %d\n",
					     message.words[0], message.words[1],
					     ret);
	}
	mutex_unlock(&sipc->lock);
}

static void timeout_work(struct work_struct *work)
{
	struct qin_sipc *sipc = container_of(to_delayed_work(work),
					     struct qin_sipc, timeout_work);
	unsigned long flags;
	bool expired = false, again;
	int i;

	spin_lock_irqsave(&sipc->irq_lock, flags);
	again = sipc->accepting && sipc->released && !sipc->fault;
	if (again) {
		for (i = 0; i < QIN_SIPC_TX_COUNT; i++) {
			if (sipc->tx[i].pending &&
			    time_after_eq(jiffies, sipc->tx[i].deadline)) {
				sipc->fault = -ETIMEDOUT;
				sipc->timeouts++;
				expired = true;
				break;
			}
		}
	}
	spin_unlock_irqrestore(&sipc->irq_lock, flags);
	if (expired)
		bt_wake(sipc);
	if (expired)
		dev_err(sipc->dev,
			"mailbox delivery timed out; TX storage retained\n");
	else if (again)
		schedule_delayed_work(&sipc->timeout_work,
				      msecs_to_jiffies(100));
}

static void stop_locked(struct qin_sipc *sipc)
{
	unsigned long flags;

	spin_lock_irqsave(&sipc->irq_lock, flags);
	sipc->accepting = false;
	sipc->released = false;
	spin_unlock_irqrestore(&sipc->irq_lock, flags);
	if (sipc->mbox) {
		/* Controller shutdown masks and synchronizes both IRQ handlers. */
		mbox_free_channel(sipc->mbox);
		sipc->mbox = NULL;
	}
	sipc->prepared = false;
	bt_revoke(sipc, -ESHUTDOWN);
}

int qin1splus_sipc_prepare(struct qin_sipc *sipc)
{
	struct qin_sipc_owner owners[QIN_SIPC_OWNER_COUNT];
	unsigned long flags;
	int i, ret;

	mutex_lock(&sipc->lock);
	if (sipc->prepared) {
		ret = -EBUSY;
		goto unlock;
	}
	ret = qin_sipc_layout(owners);
	if (ret)
		goto unlock;
	/* prepare is called only after verified halt by the power owner. */
	sipc->bt_session++;
	sipc->bt_claimed = false;
	sipc->bt_used = false;
	sipc->bt_error = 0;
	for (i = 0; i < QIN_SIPC_OWNER_COUNT; i++) {
		memset(&sipc->channels[i], 0, sizeof(sipc->channels[i]));
		sipc->channels[i].owner = owners[i];
		qin_sipc_init_owner(sipc->ram, &owners[i]);
	}
	/* Publish every owner before any pre-power OPEN reaches the controller. */
	wmb();
	spin_lock_irqsave(&sipc->irq_lock, flags);
	memset(sipc->tx, 0, sizeof(sipc->tx));
	sipc->rx_read = sipc->rx_write;
	sipc->fault = 0;
	sipc->accepting = true;
	spin_unlock_irqrestore(&sipc->irq_lock, flags);
	sipc->mbox = mbox_request_channel(&sipc->client, 0);
	if (IS_ERR(sipc->mbox)) {
		ret = PTR_ERR(sipc->mbox);
		sipc->mbox = NULL;
		goto fail;
	}
	sipc->prepared = true;
	for (i = 0; i < QIN_SIPC_OWNER_COUNT; i++) {
		ret = send_message(sipc, make_message(owners[i].channel,
						      QIN_SMSG_OPEN,
						      QIN_SMSG_OPEN_MAGIC, 0));
		if (ret)
			goto fail;
	}
	ret = 0;
	goto unlock;
fail:
	stop_locked(sipc);
unlock:
	mutex_unlock(&sipc->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(qin1splus_sipc_prepare);

int qin1splus_sipc_released(struct qin_sipc *sipc)
{
	unsigned long flags;
	int i, ret = 0;

	mutex_lock(&sipc->lock);
	spin_lock_irqsave(&sipc->irq_lock, flags);
	if (!sipc->prepared || sipc->released)
		ret = -EINVAL;
	else if (sipc->fault)
		ret = sipc->fault;
	else {
		sipc->released = true;
		for (i = 0; i < QIN_SIPC_TX_COUNT; i++)
			if (sipc->tx[i].pending)
				sipc->tx[i].deadline =
					jiffies +
					msecs_to_jiffies(
						QIN_SIPC_TX_TIMEOUT_MS);
	}
	spin_unlock_irqrestore(&sipc->irq_lock, flags);
	if (!ret)
		schedule_delayed_work(&sipc->timeout_work,
				      msecs_to_jiffies(100));
	mutex_unlock(&sipc->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(qin1splus_sipc_released);

void qin1splus_sipc_halted(struct qin_sipc *sipc)
{
	mutex_lock(&sipc->lock);
	stop_locked(sipc);
	mutex_unlock(&sipc->lock);
	cancel_delayed_work_sync(&sipc->timeout_work);
	cancel_work_sync(&sipc->rx_work);
}
EXPORT_SYMBOL_GPL(qin1splus_sipc_halted);

static int bt_state_locked(struct qin_sipc *sipc, u32 session)
{
	if (session != sipc->bt_session || !sipc->bt_claimed)
		return -ESHUTDOWN;
	if (sipc->bt_error)
		return sipc->bt_error;
	if (READ_ONCE(sipc->fault))
		return READ_ONCE(sipc->fault);
	if (!sipc->prepared || !sipc->released)
		return -ESHUTDOWN;
	return 0;
}

int qin1splus_sipc_bt_claim(struct qin_sipc *sipc, u32 *session)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(5000);
	struct qin_sipc_channel *ch;
	int event, ret;
	long left;

	for (;;) {
		mutex_lock(&sipc->lock);
		event = atomic_read(&sipc->bt_events);
		ch = find_channel(sipc, 4);
		if (sipc->bt_used)
			ret = -ESTALE;
		else if (sipc->bt_error)
			ret = sipc->bt_error;
		else if (READ_ONCE(sipc->fault))
			ret = READ_ONCE(sipc->fault);
		else if (!sipc->prepared || !sipc->released)
			ret = -ESHUTDOWN;
		else if (ch->opened && ch->init_queued) {
			sipc->bt_claimed = true;
			sipc->bt_used = true;
			*session = sipc->bt_session;
			ret = 0;
		} else {
			ret = -EAGAIN;
		}
		mutex_unlock(&sipc->lock);
		if (ret != -EAGAIN)
			return ret;
		left = deadline - jiffies;
		if (left <= 0)
			return -ETIMEDOUT;
		left = wait_event_interruptible_timeout(
			sipc->bt_wait, atomic_read(&sipc->bt_events) != event,
			left);
		if (left <= 0)
			return left ? left : -ETIMEDOUT;
	}
}
EXPORT_SYMBOL_GPL(qin1splus_sipc_bt_claim);

void qin1splus_sipc_bt_cancel(struct qin_sipc *sipc, u32 session)
{
	mutex_lock(&sipc->lock);
	if (sipc->bt_session == session && sipc->bt_claimed)
		bt_revoke(sipc, -ECANCELED);
	mutex_unlock(&sipc->lock);
}
EXPORT_SYMBOL_GPL(qin1splus_sipc_bt_cancel);

/* Caller holds lock and irq_lock, so accepted control cannot cross release. */
static bool bt_control_pending(struct qin_sipc *sipc)
{
	u32 cursor;

	for (cursor = sipc->rx_read; cursor != sipc->rx_write; cursor++) {
		struct qin_smsg *message =
			&sipc->rx[cursor % QIN_SIPC_RX_COUNT];
		u8 type = message->words[0] >> 8;
		u16 flag = message->words[0] >> 16;

		if ((u8)message->words[0] != 4)
			continue;
		if (type != QIN_SMSG_EVENT || (flag != 1 && flag != 2) ||
		    message->words[1] >= 16)
			return true;
	}
	return false;
}

int qin1splus_sipc_bt_release(struct qin_sipc *sipc, u32 session, bool clean)
{
	struct qin_sipc_channel *ch;
	unsigned long flags;
	int ret = -ESTALE;

	/* Finish previously accepted OPEN/CLOSE/INIT before judging clean close. */
	flush_work(&sipc->rx_work);
	mutex_lock(&sipc->lock);
	if (sipc->bt_session != session || !sipc->bt_claimed)
		goto unlock;
	ch = find_channel(sipc, 4);
	spin_lock_irqsave(&sipc->irq_lock, flags);
	if (clean && !sipc->fault && !bt_control_pending(sipc) &&
	    sipc->prepared && sipc->released && ch->opened && ch->init_queued &&
	    (!sipc->bt_error || sipc->bt_error == -ECANCELED))
		ret = qin_sipc_bt_idle(sipc->ram, &ch->owner);
	sipc->bt_claimed = false;
	sipc->bt_session++;
	if (!ret) {
		/* Normal disabled/empty close leaves the shared CP running. */
		sipc->bt_used = false;
		sipc->bt_error = 0;
	} else {
		/* Unfinished bytes or errors require the owner's verified halt. */
		sipc->bt_error = ret;
	}
	spin_unlock_irqrestore(&sipc->irq_lock, flags);
	bt_wake(sipc);
unlock:
	mutex_unlock(&sipc->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(qin1splus_sipc_bt_release);

static int bt_transfer(struct qin_sipc *sipc, u32 session, void *data,
		       size_t bytes, bool write)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(5000);
	struct qin_sipc_channel *ch;
	bool notify;
	int event, ret, sent;
	long left;

	for (;;) {
		mutex_lock(&sipc->lock);
		event = atomic_read(&sipc->bt_events);
		ret = bt_state_locked(sipc, session);
		if (!ret) {
			ch = find_channel(sipc, 4);
			if (write)
				ret = qin_sipc_bt_write_ring(sipc->ram,
							     &ch->owner, data,
							     bytes, &notify);
			else
				ret = qin_sipc_bt_read_ring(sipc->ram,
							    &ch->owner, data,
							    bytes, &notify);
			if (ret > 0 && notify) {
				/* Publish the data counter before its mailbox notification. */
				wmb();
				sent = send_message(
					sipc, make_message(4, QIN_SMSG_EVENT,
							   write ? 1 : 2,
							   write ? 11 : 10));
				if (sent)
					ret = sent;
			}
			if (ret < 0 && ret != -EAGAIN)
				bt_revoke(sipc, ret);
		}
		mutex_unlock(&sipc->lock);
		if (ret != -EAGAIN)
			return ret;
		left = deadline - jiffies;
		if (left <= 0)
			return -ETIMEDOUT;
		/* RDPTR events cover full rings. A whole frame may need more
		 * space before the ring was full, so observe that progress too.
		 */
		left = wait_event_interruptible_timeout(
			sipc->bt_wait, atomic_read(&sipc->bt_events) != event,
			write ? min_t(long, left, msecs_to_jiffies(20)) : left);
		if (left < 0)
			return left;
	}
}

int qin1splus_sipc_bt_write(struct qin_sipc *sipc, u32 session,
			    const void *data, size_t bytes)
{
	return bt_transfer(sipc, session, (void *)data, bytes, true);
}
EXPORT_SYMBOL_GPL(qin1splus_sipc_bt_write);

int qin1splus_sipc_bt_read(struct qin_sipc *sipc, u32 session, void *data,
			   size_t capacity)
{
	return bt_transfer(sipc, session, data, capacity, false);
}
EXPORT_SYMBOL_GPL(qin1splus_sipc_bt_read);

static void put_supplier(void *data)
{
	put_device(data);
}

struct qin_sipc *devm_qin1splus_sipc_get(struct device *consumer)
{
	struct platform_device *supplier;
	struct device_node *node;
	struct qin_sipc *sipc;
	int ret;

	node = of_parse_phandle(consumer->of_node, "fplinux,sipc", 0);
	if (!node)
		return ERR_PTR(-EINVAL);
	supplier = of_find_device_by_node(node);
	of_node_put(node);
	if (!supplier)
		return ERR_PTR(-EPROBE_DEFER);
	sipc = platform_get_drvdata(supplier);
	if (!sipc || !device_is_bound(&supplier->dev)) {
		put_device(&supplier->dev);
		return ERR_PTR(-EPROBE_DEFER);
	}
	if (!device_link_add(consumer, &supplier->dev,
			     DL_FLAG_AUTOREMOVE_CONSUMER)) {
		put_device(&supplier->dev);
		return ERR_PTR(-ENOMEM);
	}
	ret = devm_add_action_or_reset(consumer, put_supplier, &supplier->dev);
	return ret ? ERR_PTR(ret) : sipc;
}
EXPORT_SYMBOL_GPL(devm_qin1splus_sipc_get);

static ssize_t status_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct qin_sipc *sipc = dev_get_drvdata(dev);
	unsigned long flags;
	ssize_t length;
	int i;

	mutex_lock(&sipc->lock);
	spin_lock_irqsave(&sipc->irq_lock, flags);
	length = sysfs_emit(
		buf,
		"prepared=%u released=%u fault=%d rx=%u drop=%u tx=%u done=%u timeout=%u last=%08x,%08x\n",
		sipc->prepared, sipc->released, sipc->fault, sipc->rx_count,
		sipc->dropped, sipc->tx_count, sipc->tx_done, sipc->timeouts,
		sipc->last_rx.words[0], sipc->last_rx.words[1]);
	spin_unlock_irqrestore(&sipc->irq_lock, flags);
	for (i = 0; i < QIN_SIPC_OWNER_COUNT; i++) {
		struct qin_sipc_channel *ch = &sipc->channels[i];

		length += sysfs_emit_at(
			buf, length,
			"ch=%u cp=%08x bytes=%x open=%u init=%u event=%u close=%u opened=%u init_queued=%u\n",
			ch->owner.channel, QIN_SIPC_CP_BASE + ch->owner.offset,
			ch->owner.bytes, ch->open_count, ch->init_count,
			ch->event_count, ch->close_count, ch->opened,
			ch->init_queued);
	}
	mutex_unlock(&sipc->lock);
	return length;
}
static DEVICE_ATTR_RO(status);

static ssize_t memory_read(struct file *file, struct kobject *kobj,
			   const struct bin_attribute *attr, char *buf,
			   loff_t offset, size_t count)
{
	struct qin_sipc *sipc = dev_get_drvdata(kobj_to_dev(kobj));
	int ret;

	if (offset < 0)
		return -EINVAL;
	if (offset >= QIN_SIPC_SMEM_BYTES)
		return 0;
	count = min_t(size_t, count, QIN_SIPC_SMEM_BYTES - offset);
	ret = mutex_lock_interruptible(&sipc->lock);
	if (ret)
		return ret;
	/* Diagnostic snapshot only; CP may still write while it is running. */
	memcpy_fromio(buf, sipc->ram + offset, count);
	mutex_unlock(&sipc->lock);
	return count;
}

static const struct bin_attribute bin_attr_memory = {
	.attr = { .name = "memory", .mode = 0400 },
	.size = QIN_SIPC_SMEM_BYTES,
	.read = memory_read,
};

static struct attribute *qin_sipc_attrs[] = {
	&dev_attr_status.attr,
	NULL,
};

static const struct bin_attribute *const qin_sipc_bin_attrs[] = {
	&bin_attr_memory,
	NULL,
};

static const struct attribute_group qin_sipc_group = {
	.attrs = qin_sipc_attrs,
	.bin_attrs = qin_sipc_bin_attrs,
};

static const struct attribute_group *qin_sipc_groups[] = {
	&qin_sipc_group,
	NULL,
};

static int qin_sipc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *mem;
	struct resource resource;
	struct qin_sipc *sipc;
	int ret;

	mem = of_parse_phandle(dev->of_node, "memory-region", 0);
	if (!mem)
		return -EINVAL;
	ret = of_address_to_resource(mem, 0, &resource);
	if (!ret && (!of_property_read_bool(mem, "no-map") ||
		     resource.start != QIN_SIPC_RESERVED_BASE ||
		     resource_size(&resource) != QIN_SIPC_RESERVED_BYTES))
		ret = -EINVAL;
	of_node_put(mem);
	if (ret)
		return dev_err_probe(dev, ret,
				     "invalid fixed WCN reservation\n");
	sipc = devm_kzalloc(dev, sizeof(*sipc), GFP_KERNEL);
	if (!sipc)
		return -ENOMEM;
	sipc->ram = devm_ioremap(dev, resource.start + QIN_SIPC_SMEM_OFFSET,
				 QIN_SIPC_SMEM_BYTES);
	if (!sipc->ram)
		return -ENOMEM;
	sipc->dev = dev;
	mutex_init(&sipc->lock);
	spin_lock_init(&sipc->irq_lock);
	init_waitqueue_head(&sipc->bt_wait);
	atomic_set(&sipc->bt_events, 0);
	INIT_WORK(&sipc->rx_work, receive_work);
	INIT_DELAYED_WORK(&sipc->timeout_work, timeout_work);
	sipc->client.dev = dev;
	sipc->client.rx_callback = receive_message;
	sipc->client.tx_done = complete_message;
	/* CP is powered off when the owner prepares its first OPEN messages. */
	sipc->client.tx_block = false;
	platform_set_drvdata(pdev, sipc);
	return 0;
}

static const struct of_device_id qin_sipc_match[] = {
	{ .compatible = "fplinux,qin1splus-sipc" },
	{}
};

static struct platform_driver qin_sipc_driver = {
	.probe = qin_sipc_probe,
	.driver = {
		.name = "qin1splus-sipc",
		.of_match_table = qin_sipc_match,
		.dev_groups = qin_sipc_groups,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(qin_sipc_driver);

MODULE_DESCRIPTION("Qin 1s+ fixed WCN shared-buffer transport");
MODULE_LICENSE("GPL");
