/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef UMS9117_CM4_MAILBOX_H
#define UMS9117_CM4_MAILBOX_H

#include <linux/io.h>
#include <linux/types.h>

struct device;

/* Claim the native mailbox channel before changing controller power. */
int ums9117_cm4_mailbox_init(struct device *dev);
/* One cold boot, CM4 reset held, noncached 128-KiB SIPC mapping. */
int ums9117_cm4_mailbox_prepare(void __iomem *sipc);
/* Single IRQ-enabled caller: first the prologue, then the stream worker. */
int ums9117_cm4_mailbox_poll(void);
/* Wait for DONE delivery and CM4's initial tx_rd reset before any H4 TX. */
int ums9117_cm4_mailbox_h4_begin(void);
/* Transfer the attached stream to unlimited runtime notification service. */
int ums9117_cm4_mailbox_h4_continue(void);
/*
 * TX accepts fragments of ALIGN8-padded H4; RX has no padding.
 * Zero permits partial progress: retain the remainder and poll between calls.
 */
int ums9117_cm4_mailbox_h4_write(const u8 *data, size_t bytes, size_t *written);
int ums9117_cm4_mailbox_h4_read(u8 *data, size_t capacity, size_t *received);
/*
 * The stream owner has stopped new I/O. Suspend succeeds only after both ring0
 * directions and queued notifications drain; the native channel remains
 * claimed to service its no-suspend IRQ. Resume preserves peer state.
 */
int ums9117_cm4_mailbox_suspend(void);
int ums9117_cm4_mailbox_resume(void);
/* Quiesce the stream first, then hold CM4 reset after this returns. */
void ums9117_cm4_mailbox_stop(void);

#endif
