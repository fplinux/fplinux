/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef QIN1SPLUS_SIPC_H
#define QIN1SPLUS_SIPC_H

#include <linux/types.h>

struct device;
struct qin_sipc;

struct qin_sipc *devm_qin1splus_sipc_get(struct device *consumer);
/* Caller owns BTWF power/reset and has established that CP is stopped. */
int qin1splus_sipc_prepare(struct qin_sipc *sipc);
/* Call immediately before the first CPU-release operation. */
int qin1splus_sipc_released(struct qin_sipc *sipc);
/* Only after verified BTWF reset; never on an uncertain stop failure. */
void qin1splus_sipc_halted(struct qin_sipc *sipc);
/* One BT stream borrower. Calls copy bytes, never lend RAM. */
int qin1splus_sipc_bt_claim(struct qin_sipc *sipc, u32 *session);
/* Cancellation wakes readers/writers before the caller joins its workers. */
void qin1splus_sipc_bt_cancel(struct qin_sipc *sipc, u32 session);
/* After workers join; clean requires completed teardown and complete H4. */
int qin1splus_sipc_bt_release(struct qin_sipc *sipc, u32 session, bool clean);
int qin1splus_sipc_bt_write(struct qin_sipc *sipc, u32 session,
			    const void *data, size_t bytes);
int qin1splus_sipc_bt_read(struct qin_sipc *sipc, u32 session, void *data,
			   size_t capacity);

#endif
