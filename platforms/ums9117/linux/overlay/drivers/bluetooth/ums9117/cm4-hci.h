/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef UMS9117_CM4_HCI_H
#define UMS9117_CM4_HCI_H

#include <linux/types.h>

struct device;

/* Ring1 uses the existing worker. Replies include the H4 event header. */
int ums9117_hci_fm_command(u8 subcommand, const u8 *payload,
			   size_t payload_bytes, u8 *reply, size_t reply_bytes,
			   bool seek);
/* Hold CM4 and veto system sleep until a confirmed disable. */
int ums9117_hci_fm_hold(void);
void ums9117_hci_fm_release(void);
void ums9117_hci_fm_quarantine(int error);

/*
 * Sleepable, externally serialized lifecycle. The caller has completed the
 * vendor prologue and handed the attached mailbox stream to this component.
 * It owns firmware, the live CM4, and the idle policy, including retained
 * system sleep. Register allocates HCI/FM devices once and returns zero or
 * a negative errno; registration is not completion of HCI core initialization.
 * rx_prefix contains already-read prologue RX bytes, starting with an H4 type.
 * A partial frame is seeded before registration and discarded when completed;
 * bytes that follow it remain ordinary runtime input. NULL/zero is allowed.
 */
int ums9117_hci_runtime_register(struct device *dev, const u8 *rx_prefix,
				 size_t prefix_bytes);
/* Call without the platform owner lock: core close can release a CM4 user. */
void ums9117_hci_runtime_unregister(void);

/*
 * Start follows a fresh firmware prologue and preserves the HCI/FM devices.
 * Stop ends all mailbox I/O before physical reset; the caller serializes both
 * transitions and stops only after releasing the last user or during teardown.
 * Never call stop from this component's worker.
 */
int ums9117_hci_transport_start(const u8 *rx_prefix, size_t prefix_bytes);
void ums9117_hci_transport_stop(void);

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
