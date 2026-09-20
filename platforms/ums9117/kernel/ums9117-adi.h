/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_SOC_SPRD_UMS9117_ADI_H
#define _LINUX_SOC_SPRD_UMS9117_ADI_H

#include <linux/types.h>

struct ums9117_adi_transaction {
	unsigned long irq_flags;
	bool active;
};

static inline void ums9117_adi_record_first_error(int *first_error, int error)
{
	if (!*first_error && error)
		*first_error = error;
}

int ums9117_adi_begin(struct ums9117_adi_transaction *transaction);
int ums9117_adi_end(struct ums9117_adi_transaction *transaction);
int ums9117_adi_read(struct ums9117_adi_transaction *transaction, u32 offset,
		     u16 *value);
int ums9117_adi_read_once(u32 offset, u16 *value);
int ums9117_adi_write(struct ums9117_adi_transaction *transaction, u32 offset,
		      u16 value);
int ums9117_adi_write_once(u32 offset, u16 value);
int ums9117_adi_update_bits(struct ums9117_adi_transaction *transaction,
			    u32 offset, u16 mask, u16 value);
int ums9117_adi_update_bits_once(u32 offset, u16 mask, u16 value);
/*
 * Share the SC2720 26 MHz output request without clearing an inherited enable.
 * Each consumer keeps a zero-initialized lease until release and must not
 * modify it. These helpers open their own ADI transaction; do not call them
 * inside an active transaction. Repeated acquire/release calls are idempotent.
 * The lease records completed ownership changes even if transaction end fails;
 * callers must use its resulting value when unwinding an error.
 */
int ums9117_adi_xtal_acquire(bool *held);
int ums9117_adi_xtal_release(bool *held);
int ums9117_adi_write_final(struct ums9117_adi_transaction *transaction,
			    u32 offset, u16 value);
int ums9117_adi_write_final_once(u32 offset, u16 value);
bool ums9117_adi_is_poisoned(void);

#endif
