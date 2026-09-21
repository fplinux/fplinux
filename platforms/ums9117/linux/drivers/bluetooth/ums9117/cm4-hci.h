/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef UMS9117_CM4_HCI_H
#define UMS9117_CM4_HCI_H

#include <linux/types.h>

struct device;

/*
 * Sleepable, externally serialized lifecycle. The caller has completed the
 * vendor prologue and handed the attached mailbox stream to this component.
 * It owns firmware, the live CM4, and the idle policy, including retained
 * system sleep. Register returns zero or
 * a negative errno; registration is not completion of HCI core initialization.
 * rx_prefix contains already-read prologue RX bytes, starting with an H4 type.
 * A partial frame is seeded before registration and discarded when completed;
 * bytes that follow it remain ordinary runtime input. NULL/zero is allowed.
 */
int ums9117_hci_runtime_register(struct device *dev, const u8 *rx_prefix,
				 size_t prefix_bytes);
/* Never call from this component's worker. No mailbox I/O survives return. */
void ums9117_hci_runtime_unregister(void);

/*
 * Prepare runs before the process freezer and refuses active links or setup.
 * Suspend/resume quiesce only transport I/O; post_suspend restores HCI and
 * reopens admission after either wake or an aborted system suspend.
 */
int ums9117_hci_suspend_prepare(void);
int ums9117_hci_suspend(void);
int ums9117_hci_resume(void);
int ums9117_hci_post_suspend(void);

#endif
