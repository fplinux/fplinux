/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_SOC_SPRD_UMS9117_ADI_H
#define _LINUX_SOC_SPRD_UMS9117_ADI_H

#include <linux/types.h>

struct ums9117_adi_transaction {
	unsigned long irq_flags;
	bool active;
};

int ums9117_adi_begin(struct ums9117_adi_transaction *transaction);
int ums9117_adi_end(struct ums9117_adi_transaction *transaction);
int ums9117_adi_read(struct ums9117_adi_transaction *transaction, u32 offset,
		     u16 *value);
int ums9117_adi_write(struct ums9117_adi_transaction *transaction, u32 offset,
		      u16 value);
int ums9117_adi_update_bits(struct ums9117_adi_transaction *transaction,
			    u32 offset, u16 mask, u16 value);
int ums9117_adi_write_final(struct ums9117_adi_transaction *transaction,
			    u32 offset, u16 value);
bool ums9117_adi_is_poisoned(void);

#endif
