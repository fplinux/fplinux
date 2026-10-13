/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef UMS9117_CM4_MAILBOX_H
#define UMS9117_CM4_MAILBOX_H

#include <linux/io.h>
#include <linux/types.h>

struct device;

/* Claim the native mailbox channel before changing controller power. */
int ums9117_cm4_mailbox_init(struct device *dev);
/*
 * CM4 reset held, noncached 128-KiB SIPC mapping. A repeated prepare requires
 * a successful stop; it reacquires the native channel and starts fresh rings.
 */
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
 * Ring1 has no H4 TX padding. Write publishes the entire command once, or
 * returns -EAGAIN without publication. The same stream worker owns both rings;
 * it must allow only one FM command in flight and frame the RX event stream.
 */
int ums9117_cm4_mailbox_fm_write(const u8 *data, size_t bytes);
int ums9117_cm4_mailbox_fm_read(u8 *data, size_t capacity, size_t *received);
/*
 * The stream owner has stopped new I/O and refuses suspend while FM may be
 * enabled. Both rings and queued notifications must drain. CM4 and shared
 * memory remain retained. The native channel stays claimed to service its
 * no-suspend IRQ. Resume preserves peer state.
 */
int ums9117_cm4_mailbox_suspend(void);
int ums9117_cm4_mailbox_resume(void);
/*
 * Quiesce the stream first. Drain the last TX with IRQs enabled while CM4 is
 * live, then hold CM4 reset after this returns. A drain error blocks restart.
 */
int ums9117_cm4_mailbox_stop(void);

#endif
