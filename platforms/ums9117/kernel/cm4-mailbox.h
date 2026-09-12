/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef UMS9117_CM4_MAILBOX_H
#define UMS9117_CM4_MAILBOX_H

#include <linux/io.h>
#include <linux/types.h>

struct platform_device;
struct regmap;

/* Resolve mappings and exclusive IRQs before changing controller power. */
int ums9117_cm4_mailbox_init(struct platform_device *pdev);
/* One cold boot, CM4 reset held, noncached 128-KiB SIPC mapping. */
int ums9117_cm4_mailbox_prepare(void __iomem *sipc, struct regmap *aon);
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
 * The stream owner has stopped new I/O. Suspend masks IRQs only after both
 * ring0 directions and notifications drain; -EBUSY leaves service unchanged.
 * Resume preserves the peer's descriptors, counters and any retained input.
 */
int ums9117_cm4_mailbox_suspend(void);
int ums9117_cm4_mailbox_resume(void);
/* Quiesce the stream first, then mask IRQs here before holding CM4 reset. */
void ums9117_cm4_mailbox_stop(void);

#endif
