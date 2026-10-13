/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_MARLIN2_SETUP_HOST_HCI_CORE_H
#define FPLINUX_MARLIN2_SETUP_HOST_HCI_CORE_H

#include <linux/skbuff.h>
#include <linux/workqueue.h>
#include <net/bluetooth/hci.h>

/* Only the external HCI core is replaced; setup and receive are production. */
#define HCI_SDIO 1
#define HCI_CMD_TIMEOUT 2000
#define HCI_QUIRK_NON_PERSISTENT_SETUP 0
enum { HCI_SETUP, HCI_UNCONFIGURED };
typedef struct {
	u8 b[6];
} bdaddr_t;
#define BDADDR_ANY (&(bdaddr_t){ { 0 } })
static inline int bacmp(const bdaddr_t *left, const bdaddr_t *right)
{
	return memcmp(left, right, sizeof(*left));
}
struct device {
	int unused;
};
struct hci_dev {
	struct device dev;
	struct work_struct power_on;
	bdaddr_t bdaddr;
	unsigned long flags;
	void *driver_data;
	int bus;
	struct {
		size_t byte_tx, byte_rx, cmd_tx, acl_tx, sco_tx, err_rx, err_tx;
	} stat;
	int (*open)(struct hci_dev *hdev);
	int (*close)(struct hci_dev *hdev);
	int (*flush)(struct hci_dev *hdev);
	int (*send)(struct hci_dev *hdev, struct sk_buff *skb);
	int (*setup)(struct hci_dev *hdev);
};
#define hci_dev_test_flag(hdev, bit) (!!((hdev)->flags & (1UL << (bit))))
#define hci_set_quirk(hdev, bit) ((void)(hdev), (void)(bit))
#define hci_set_drvdata(hdev, data) ((hdev)->driver_data = (data))
#define hci_get_drvdata(hdev) ((hdev)->driver_data)
#define SET_HCIDEV_DEV(hdev, parent) ((void)(hdev), (void)(parent))
static inline void dev_err(const struct device *dev, const char *format, ...)
{
	(void)dev;
	(void)format;
}
struct hci_dev *hci_alloc_dev(void);
void hci_free_dev(struct hci_dev *hdev);
int hci_register_dev(struct hci_dev *hdev);
void hci_unregister_dev(struct hci_dev *hdev);
int hci_recv_frame(struct hci_dev *hdev, struct sk_buff *skb);

#endif
