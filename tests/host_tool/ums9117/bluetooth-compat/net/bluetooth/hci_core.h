/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef BLUETOOTH_HOST_HCI_CORE_H
#define BLUETOOTH_HOST_HCI_CORE_H

#include <linux/skbuff.h>
#include <net/bluetooth/hci.h>

/* External HCI core model. Flags need distinct bits, not kernel ABI values. */
enum {
	HCI_UP,
	HCI_INIT,
	HCI_SETUP,
	HCI_CONFIG,
	HCI_USER_CHANNEL,
};
#define HCI_IPC 0
#define HCI_REQ_PEND 1
#define HCI_QUIRK_NO_SUSPEND_NOTIFIER 0

struct device {
	int unused;
};
struct hci_dev {
	struct mutex lock;
	struct sk_buff_head cmd_q;
	struct sk_buff_head rx_q;
	unsigned long flags;
	unsigned long quirk_flags;
	unsigned int connections;
	int bus;
	int req_status;
	int cmd_cnt;
	struct {
		size_t byte_tx, byte_rx, cmd_tx, acl_tx, sco_tx, err_rx, err_tx;
	} stat;
	int (*open)(struct hci_dev *hdev);
	int (*close)(struct hci_dev *hdev);
	int (*flush)(struct hci_dev *hdev);
	int (*send)(struct hci_dev *hdev, struct sk_buff *skb);
};

static inline bool test_bit(unsigned int bit, const unsigned long *flags)
{
	return !!(*flags & (1UL << bit));
}
#define atomic_read(value) (*(value))
#define hci_dev_test_flag(hdev, bit) test_bit(bit, &(hdev)->flags)
#define hci_set_quirk(hdev, bit) ((hdev)->quirk_flags |= 1UL << (bit))
#define SET_HCIDEV_DEV(hdev, dev) ((void)(dev))
#define bt_dev_err(hdev, format, ...) ((void)(hdev))
#define hci_dev_lock(hdev) mutex_lock(&(hdev)->lock)
#define hci_dev_unlock(hdev) mutex_unlock(&(hdev)->lock)
static inline unsigned int hci_conn_count(const struct hci_dev *hdev)
{
	return hdev->connections;
}
struct hci_dev *hci_alloc_dev(void);
void hci_free_dev(struct hci_dev *hdev);
int hci_register_dev(struct hci_dev *hdev);
void hci_unregister_dev(struct hci_dev *hdev);
int hci_recv_frame(struct hci_dev *hdev, struct sk_buff *skb);
int hci_suspend_dev(struct hci_dev *hdev);
int hci_resume_dev(struct hci_dev *hdev);

#endif
