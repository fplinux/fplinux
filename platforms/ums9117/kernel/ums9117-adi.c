// SPDX-License-Identifier: GPL-2.0-only
#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/spinlock.h>

#include <linux/soc/sprd/ums9117-adi.h>

#define UMS9117_AON_APB_PHYS 0x402e0000U
#define UMS9117_AON_APB_MMIO_BYTES 0x3000U
#define UMS9117_AON_APB_EB0 0x0000
#define UMS9117_AON_APB_EB0_SET 0x1000
#define UMS9117_AON_APB_EB0_CLEAR 0x2000
#define UMS9117_AON_APB_ADI_EB BIT(16)

#define UMS9117_ADI_PHYS 0x40600000U
#define UMS9117_ADI_SLAVE_PHYS 0x40608000U
#define UMS9117_ADI_MMIO_BYTES 0x1000U
#define UMS9117_ADI_SLAVE_MMIO_BYTES 0x1000U

#define UMS9117_ADI_VERSION 0x000
#define UMS9117_ADI_MST_CTL 0x004
#define UMS9117_ADI_MST_PRIL 0x008
#define UMS9117_ADI_MST_PRIH 0x00c
#define UMS9117_ADI_INT_RAW 0x014
#define UMS9117_ADI_INT_CLEAR 0x01c
#define UMS9117_ADI_GSSI_CTRL0 0x020
#define UMS9117_ADI_GSSI_CTRL1 0x024
#define UMS9117_ADI_RD_CMD 0x028
#define UMS9117_ADI_RD_DATA 0x02c
#define UMS9117_ADI_FIFO_STS 0x030
#define UMS9117_ADI_USER_LOCK 0x224
#define UMS9117_ADI_EXPECTED_VERSION 0x00000400U
#define UMS9117_ADI_EXPECTED_MST_CTL 0x00000000U
#define UMS9117_ADI_GSSI_CLOCK_ALL_ON BIT(30)
#define UMS9117_ADI_GSSI_2WIRE_MODE BIT(31)
#define UMS9117_ADI_RD_DATA_BUSY BIT(31)
#define UMS9117_ADI_RD_DATA_RETURNED_ADDRESS_MASK GENMASK(30, 16)
#define UMS9117_ADI_FIFO_STS_EMPTY BIT(10)
#define UMS9117_ADI_FIFO_STS_FULL BIT(11)
#define UMS9117_ADI_INT_ARM_FIFO_OVERFLOW BIT(3)
#define UMS9117_ADI_USER_LOCK_RELEASE 0x5348554cU
#define UMS9117_ADI_POLL_BUDGET_US 3000U

static void __iomem *adi_controller;
static void __iomem *analog_slave;
static DEFINE_RAW_SPINLOCK(adi_lock);
static bool adi_ready;
static bool adi_poisoned;

struct ums9117_adi_initial_state {
	u32 mst_pril;
	u32 mst_prih;
	u32 gssi_ctrl0;
	u32 gssi_ctrl1;
	bool gate_enabled;
};

static int ums9117_adi_validate(void)
{
	/* SC2720 transactions require the documented word-address master mode. */
	if (readl(adi_controller + UMS9117_ADI_VERSION) !=
		    UMS9117_ADI_EXPECTED_VERSION ||
	    readl(adi_controller + UMS9117_ADI_MST_CTL) !=
		    UMS9117_ADI_EXPECTED_MST_CTL)
		return -EPROTONOSUPPORT;
	return 0;
}

static void ums9117_adi_restore_initial_state(
	void __iomem *aon_apb, const struct ums9117_adi_initial_state *initial)
{
	writel(initial->mst_pril, adi_controller + UMS9117_ADI_MST_PRIL);
	writel(initial->mst_prih, adi_controller + UMS9117_ADI_MST_PRIH);
	writel(initial->gssi_ctrl1, adi_controller + UMS9117_ADI_GSSI_CTRL1);
	writel(initial->gssi_ctrl0, adi_controller + UMS9117_ADI_GSSI_CTRL0);
	/* Complete controller restoration before its posted-write flush. */
	wmb();
	readl(adi_controller + UMS9117_ADI_GSSI_CTRL0);

	if (!initial->gate_enabled) {
		writel(UMS9117_AON_APB_ADI_EB,
		       aon_apb + UMS9117_AON_APB_EB0_CLEAR);
		/* Complete the gate clear before confirming the disabled state. */
		wmb();
		readl(aon_apb + UMS9117_AON_APB_EB0);
	}
}

static int ums9117_adi_initialize_transport(void __iomem *aon_apb)
{
	struct ums9117_adi_initial_state initial;
	u32 gssi_ctrl0;
	u32 gssi_ctrl1;
	int ret;

	initial.gate_enabled = readl(aon_apb + UMS9117_AON_APB_EB0) &
			       UMS9117_AON_APB_ADI_EB;
	writel(UMS9117_AON_APB_ADI_EB, aon_apb + UMS9117_AON_APB_EB0_SET);
	/* Make the ADI gate visible before touching the controller. */
	wmb();
	if (!(readl(aon_apb + UMS9117_AON_APB_EB0) & UMS9117_AON_APB_ADI_EB)) {
		ret = -EIO;
		goto restore_gate;
	}

	ret = ums9117_adi_validate();
	if (ret)
		goto restore_gate;

	initial.mst_pril = readl(adi_controller + UMS9117_ADI_MST_PRIL);
	initial.mst_prih = readl(adi_controller + UMS9117_ADI_MST_PRIH);
	initial.gssi_ctrl0 = readl(adi_controller + UMS9117_ADI_GSSI_CTRL0);
	initial.gssi_ctrl1 = readl(adi_controller + UMS9117_ADI_GSSI_CTRL1);

	writel(0, adi_controller + UMS9117_ADI_MST_PRIL);
	writel(0, adi_controller + UMS9117_ADI_MST_PRIH);
	writel(initial.gssi_ctrl1 | UMS9117_ADI_GSSI_2WIRE_MODE,
	       adi_controller + UMS9117_ADI_GSSI_CTRL1);
	writel(initial.gssi_ctrl0 & ~UMS9117_ADI_GSSI_CLOCK_ALL_ON,
	       adi_controller + UMS9117_ADI_GSSI_CTRL0);
	/* Complete transport programming before validating its state. */
	wmb();

	gssi_ctrl1 = readl(adi_controller + UMS9117_ADI_GSSI_CTRL1);
	gssi_ctrl0 = readl(adi_controller + UMS9117_ADI_GSSI_CTRL0);
	if (readl(adi_controller + UMS9117_ADI_MST_PRIL) ||
	    readl(adi_controller + UMS9117_ADI_MST_PRIH) ||
	    !(gssi_ctrl1 & UMS9117_ADI_GSSI_2WIRE_MODE) ||
	    (gssi_ctrl0 & UMS9117_ADI_GSSI_CLOCK_ALL_ON)) {
		ret = -EIO;
		goto restore;
	}

	return 0;

restore:
	ums9117_adi_restore_initial_state(aon_apb, &initial);
	return ret;

restore_gate:
	if (!initial.gate_enabled) {
		writel(UMS9117_AON_APB_ADI_EB,
		       aon_apb + UMS9117_AON_APB_EB0_CLEAR);
		/* Complete the gate clear before returning from failed setup. */
		wmb();
		readl(aon_apb + UMS9117_AON_APB_EB0);
	}
	return ret;
}

static int ums9117_adi_clear_overflow_locked(void)
{
	writel(UMS9117_ADI_INT_ARM_FIFO_OVERFLOW,
	       adi_controller + UMS9117_ADI_INT_CLEAR);
	return readl(adi_controller + UMS9117_ADI_INT_RAW) &
			       UMS9117_ADI_INT_ARM_FIFO_OVERFLOW ?
		       -EIO :
		       -EOVERFLOW;
}

static int ums9117_adi_wait_empty_locked(void)
{
	unsigned int waited;
	bool overflow = false;
	u32 raw;
	u32 status;

	for (waited = 0; waited < UMS9117_ADI_POLL_BUDGET_US; waited++) {
		raw = readl(adi_controller + UMS9117_ADI_INT_RAW);
		status = readl(adi_controller + UMS9117_ADI_FIFO_STS);
		overflow |= !!(raw & UMS9117_ADI_INT_ARM_FIFO_OVERFLOW);
		if (status & UMS9117_ADI_FIFO_STS_EMPTY)
			return overflow ? ums9117_adi_clear_overflow_locked() :
					  0;
		udelay(1);
	}
	return -ETIMEDOUT;
}

static int ums9117_adi_wait_quiescent_locked(void)
{
	unsigned int waited;
	bool overflow = false;
	u32 data;
	u32 raw;
	u32 status;

	for (waited = 0; waited < UMS9117_ADI_POLL_BUDGET_US; waited++) {
		raw = readl(adi_controller + UMS9117_ADI_INT_RAW);
		status = readl(adi_controller + UMS9117_ADI_FIFO_STS);
		data = readl(adi_controller + UMS9117_ADI_RD_DATA);
		overflow |= !!(raw & UMS9117_ADI_INT_ARM_FIFO_OVERFLOW);
		if ((status & UMS9117_ADI_FIFO_STS_EMPTY) &&
		    !(data & UMS9117_ADI_RD_DATA_BUSY))
			return overflow ? ums9117_adi_clear_overflow_locked() :
					  0;
		udelay(1);
	}
	return -ETIMEDOUT;
}

static bool
ums9117_adi_transaction_valid(struct ums9117_adi_transaction *transaction)
{
	return transaction && transaction->active;
}

int ums9117_adi_begin(struct ums9117_adi_transaction *transaction)
{
	unsigned int waited;
	int ret;

	if (!transaction || transaction->active)
		return -EINVAL;
	raw_spin_lock_irqsave(&adi_lock, transaction->irq_flags);
	if (!adi_ready) {
		ret = -ENODEV;
		goto unlock;
	}
	if (adi_poisoned) {
		ret = -EIO;
		goto unlock;
	}

	for (waited = 0; waited < UMS9117_ADI_POLL_BUDGET_US; waited++) {
		if (!readl(adi_controller + UMS9117_ADI_USER_LOCK)) {
			ret = ums9117_adi_wait_quiescent_locked();
			if (!ret) {
				transaction->active = true;
				/* Ownership is transferred to ums9117_adi_end(). */
				__release(&adi_lock);
				return 0;
			}
			if (ret == -EOVERFLOW)
				writel(UMS9117_ADI_USER_LOCK_RELEASE,
				       adi_controller + UMS9117_ADI_USER_LOCK);
			else
				adi_poisoned = true;
			goto unlock;
		}
		udelay(1);
	}
	ret = -EBUSY;

unlock:
	raw_spin_unlock_irqrestore(&adi_lock, transaction->irq_flags);
	return ret;
}
EXPORT_SYMBOL_GPL(ums9117_adi_begin);

int ums9117_adi_end(struct ums9117_adi_transaction *transaction)
{
	int ret;

	if (!ums9117_adi_transaction_valid(transaction))
		return -EINVAL;
	/* Ownership was transferred by ums9117_adi_begin(). */
	__acquire(&adi_lock);
	ret = ums9117_adi_wait_quiescent_locked();
	if (!ret || ret == -EOVERFLOW)
		writel(UMS9117_ADI_USER_LOCK_RELEASE,
		       adi_controller + UMS9117_ADI_USER_LOCK);
	else
		adi_poisoned = true;
	transaction->active = false;
	raw_spin_unlock_irqrestore(&adi_lock, transaction->irq_flags);
	return ret;
}
EXPORT_SYMBOL_GPL(ums9117_adi_end);

int ums9117_adi_read(struct ums9117_adi_transaction *transaction, u32 offset,
		     u16 *value)
{
	unsigned int waited;
	u32 data;
	u32 returned;
	int ret;

	if (!ums9117_adi_transaction_valid(transaction) || !value ||
	    !IS_ALIGNED(offset, sizeof(u32)) ||
	    offset > UMS9117_ADI_SLAVE_MMIO_BYTES - sizeof(u32))
		return -EINVAL;
	ret = ums9117_adi_validate();
	if (ret)
		return ret;

	writel(offset, adi_controller + UMS9117_ADI_RD_CMD);
	for (waited = 0; waited < UMS9117_ADI_POLL_BUDGET_US; waited++) {
		data = readl(adi_controller + UMS9117_ADI_RD_DATA);
		if (!(data & UMS9117_ADI_RD_DATA_BUSY))
			break;
		udelay(1);
	}
	if (waited == UMS9117_ADI_POLL_BUDGET_US)
		return -ETIMEDOUT;
	returned = FIELD_GET(UMS9117_ADI_RD_DATA_RETURNED_ADDRESS_MASK, data);
	if (returned != offset >> 2)
		return -EIO;
	*value = data & 0xffffU;
	return 0;
}
EXPORT_SYMBOL_GPL(ums9117_adi_read);

static int ums9117_adi_write_locked(struct ums9117_adi_transaction *transaction,
				    u32 offset, u16 value)
{
	int ret;

	if (!ums9117_adi_transaction_valid(transaction) ||
	    !IS_ALIGNED(offset, sizeof(u32)) ||
	    offset > UMS9117_ADI_SLAVE_MMIO_BYTES - sizeof(u32))
		return -EINVAL;
	ret = ums9117_adi_wait_empty_locked();
	if (ret)
		return ret;
	if (readl(adi_controller + UMS9117_ADI_FIFO_STS) &
	    UMS9117_ADI_FIFO_STS_FULL)
		return -EBUSY;
	ret = ums9117_adi_validate();
	if (ret)
		return ret;

	writel(value, analog_slave + offset);
	/* Submit the analog write before polling its FIFO completion. */
	wmb();
	return ums9117_adi_wait_empty_locked();
}

int ums9117_adi_write(struct ums9117_adi_transaction *transaction, u32 offset,
		      u16 value)
{
	u16 readback;
	int ret;

	ret = ums9117_adi_write_locked(transaction, offset, value);
	if (ret)
		return ret;
	ret = ums9117_adi_read(transaction, offset, &readback);
	if (ret)
		return ret;
	return readback == value ? 0 : -EIO;
}
EXPORT_SYMBOL_GPL(ums9117_adi_write);

int ums9117_adi_update_bits(struct ums9117_adi_transaction *transaction,
			    u32 offset, u16 mask, u16 value)
{
	u16 old_value;
	int ret;

	ret = ums9117_adi_read(transaction, offset, &old_value);
	if (ret)
		return ret;
	return ums9117_adi_write(transaction, offset,
				 (old_value & ~mask) | (value & mask));
}
EXPORT_SYMBOL_GPL(ums9117_adi_update_bits);

int ums9117_adi_write_final(struct ums9117_adi_transaction *transaction,
			    u32 offset, u16 value)
{
	return ums9117_adi_write_locked(transaction, offset, value);
}
EXPORT_SYMBOL_GPL(ums9117_adi_write_final);

bool ums9117_adi_is_poisoned(void)
{
	return READ_ONCE(adi_poisoned);
}
EXPORT_SYMBOL_GPL(ums9117_adi_is_poisoned);

static int __init ums9117_adi_init(void)
{
	void __iomem *aon_apb;
	int ret;

	aon_apb = ioremap(UMS9117_AON_APB_PHYS, UMS9117_AON_APB_MMIO_BYTES);
	adi_controller = ioremap(UMS9117_ADI_PHYS, UMS9117_ADI_MMIO_BYTES);
	analog_slave =
		ioremap(UMS9117_ADI_SLAVE_PHYS, UMS9117_ADI_SLAVE_MMIO_BYTES);
	if (!aon_apb || !adi_controller || !analog_slave) {
		ret = -ENOMEM;
		goto unmap;
	}
	ret = ums9117_adi_initialize_transport(aon_apb);
	if (ret)
		goto unmap;
	iounmap(aon_apb);
	WRITE_ONCE(adi_ready, true);
	pr_info("UMS9117 ADI transport configured and ready\n");
	return 0;

unmap:
	if (aon_apb)
		iounmap(aon_apb);
	if (analog_slave) {
		iounmap(analog_slave);
		analog_slave = NULL;
	}
	if (adi_controller) {
		iounmap(adi_controller);
		adi_controller = NULL;
	}
	return ret;
}
arch_initcall(ums9117_adi_init);

MODULE_DESCRIPTION("UMS9117 analog-die interface transport");
MODULE_LICENSE("GPL");
