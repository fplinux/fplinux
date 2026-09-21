// SPDX-License-Identifier: GPL-2.0-only
/*
 * SDHCI platform glue for the UMS9117 SDIO0 removable microSD slot.
 *
 * The controller implements the SDHCI 4.10 register layout in v4 mode, but
 * its register interface is 32-bit only.  Board resources are owned by their
 * normal kernel providers; this driver owns only the SDHCI window and the
 * fitted polling-only card-detect window.
 */
#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/ktime.h>
#include <linux/mm.h>
#include <linux/mmc/core.h>
#include <linux/mmc/host.h>
#include <linux/mmc/mmc.h>
#include <linux/mmc/sd.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/sizes.h>
#include <linux/spinlock.h>

#include "sdhci-pltfm.h"

#define UMS9117_SDHCI_HOST_MMIO_BYTES SZ_256
#define UMS9117_SDHCI_BASE_CLOCK_HZ 195000000U
#define UMS9117_SDHCI_IDENT_CLOCK_HZ 399590U
#define UMS9117_SDHCI_LEGACY_CLOCK_HZ 24375000U
#define UMS9117_SDHCI_MAX_CLOCK_HZ 48750000U
#define UMS9117_SDHCI_CLOCK_STABLE_TIMEOUT_US 10000U
#define UMS9117_SDHCI_INTERRUPT_MASK 0x1b7f0003U
#define UMS9117_SDHCI_HW_RESET_CARD BIT(3)

#define UMS9117_SDHCI_MAX_SEGS 32U
#define UMS9117_SDHCI_MAX_REQUEST_BYTES SZ_128K
#define UMS9117_SDHCI_ADMA_TABLE_COUNT (UMS9117_SDHCI_MAX_SEGS * 2U + 1U)

#define UMS9117_SDHCI_CD_DATA 0x0
#define UMS9117_SDHCI_CD_MASK 0x4
#define UMS9117_SDHCI_CD_MMIO_BYTES 0x8
#define UMS9117_SDHCI_CD_BIT BIT(0)
#define UMS9117_SDHCI_CD_SETTLE_MIN_US 3000U
#define UMS9117_SDHCI_CD_SETTLE_MAX_US 4000U

#define UMS9117_SDHCI_CMD6_CHECK_ARG 0x00fffff0U
#define UMS9117_SDHCI_CMD6_SWITCH_ARG 0x80fffff1U

struct ums9117_sdhci_host {
	struct clk *sdio_clk;
	struct clk *enable_clk;
	struct reset_control *reset;
	void __iomem *card_detect;
	spinlock_t policy_lock;
	u16 transfer_mode;
	u32 deferred_clock_hz;
	u32 applied_clock_hz;
	u32 app_cmd_arg;
	bool raw_card_detect_owned;
	bool app_cmd_armed;
	bool active_width_acmd6;
	bool width_acmd6_clean;
	bool operational_clock_deferred;
	bool physical_width4;
	bool transition_failed;
};

static struct ums9117_sdhci_host *ums9117_sdhci_priv(struct sdhci_host *host)
{
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);

	return sdhci_pltfm_priv(pltfm_host);
}

static u32 ums9117_sdhci_readl(struct sdhci_host *host, int reg)
{
	return readl(host->ioaddr + reg);
}

static u16 ums9117_sdhci_readw(struct sdhci_host *host, int reg)
{
	struct ums9117_sdhci_host *ums_host = ums9117_sdhci_priv(host);
	u32 value;

	if (reg == SDHCI_TRANSFER_MODE)
		return ums_host->transfer_mode;
	value = readl(host->ioaddr + (reg & ~0x3));
	return value >> ((reg & 0x2) * 8);
}

static u8 ums9117_sdhci_readb(struct sdhci_host *host, int reg)
{
	u32 value = readl(host->ioaddr + (reg & ~0x3));

	return value >> ((reg & 0x3) * 8);
}

static void ums9117_sdhci_writel(struct sdhci_host *host, u32 value, int reg)
{
	if (reg == SDHCI_INT_ENABLE || reg == SDHCI_SIGNAL_ENABLE)
		value &= UMS9117_SDHCI_INTERRUPT_MASK;
	writel(value, host->ioaddr + reg);
}

static void ums9117_sdhci_writew(struct sdhci_host *host, u16 value, int reg)
{
	struct ums9117_sdhci_host *ums_host = ums9117_sdhci_priv(host);
	u32 shift;
	u32 mask;
	u32 old;

	if (reg == SDHCI_32BIT_BLK_CNT) {
		writel(value, host->ioaddr + reg);
		return;
	}
	if (reg == SDHCI_TRANSFER_MODE) {
		if (host->cmd && !host->cmd->data)
			value = 0;
		else if ((value & SDHCI_TRNS_AUTO_SEL) == SDHCI_TRNS_AUTO_SEL) {
			/* Use explicit Auto CMD12 for the SD memory-card path. */
			value = (value & ~SDHCI_TRNS_AUTO_SEL) |
				SDHCI_TRNS_AUTO_CMD12;
		}
		ums_host->transfer_mode = value;
		return;
	}
	if (reg == SDHCI_COMMAND) {
		if (host->cmd && !host->cmd->data)
			value |= SDHCI_CMD_SUB_CMD;
		writel((u32)value << 16 | ums_host->transfer_mode,
		       host->ioaddr + SDHCI_TRANSFER_MODE);
		return;
	}

	shift = (reg & 0x2) * 8;
	mask = 0xffffU << shift;
	old = readl(host->ioaddr + (reg & ~0x3));
	writel((old & ~mask) | (u32)value << shift,
	       host->ioaddr + (reg & ~0x3));
}

static void ums9117_sdhci_writeb(struct sdhci_host *host, u8 value, int reg)
{
	struct ums9117_sdhci_host *ums_host = ums9117_sdhci_priv(host);
	u32 shift = (reg & 0x3) * 8;
	u32 mask = 0xffU << shift;
	u32 old = readl(host->ioaddr + (reg & ~0x3));

	if (reg == SDHCI_SOFTWARE_RESET) {
		if (value &
		    (SDHCI_RESET_ALL | SDHCI_RESET_CMD | SDHCI_RESET_DATA))
			ums_host->transfer_mode = 0;
		/* Vendor bit 3 must stay set or the card remains in reset. */
		value |= (old >> shift) & UMS9117_SDHCI_HW_RESET_CARD;
	}
	writel((old & ~mask) | (u32)value << shift,
	       host->ioaddr + (reg & ~0x3));
}

static unsigned int ums9117_sdhci_get_max_clock(struct sdhci_host *host)
{
	struct ums9117_sdhci_host *ums_host = ums9117_sdhci_priv(host);

	return clk_get_rate(ums_host->sdio_clk);
}

static unsigned int ums9117_sdhci_get_min_clock(struct sdhci_host *host)
{
	return UMS9117_SDHCI_IDENT_CLOCK_HZ;
}

static unsigned int ums9117_sdhci_get_max_timeout_count(struct sdhci_host *host)
{
	return BIT(31);
}

static u16 ums9117_sdhci_divided_clock(struct sdhci_host *host,
				       unsigned int requested,
				       unsigned int *actual)
{
	unsigned int divisor;
	unsigned int encoded;

	if (host->max_clk <= requested)
		divisor = 1;
	else
		for (divisor = 2; divisor < SDHCI_MAX_DIV_SPEC_300;
		     divisor += 2)
			if (host->max_clk / divisor <= requested)
				break;

	*actual = host->max_clk / divisor;
	encoded = divisor >> 1;
	return (encoded & SDHCI_DIV_MASK) << SDHCI_DIVIDER_SHIFT |
	       ((encoded & SDHCI_DIV_HI_MASK) >> SDHCI_DIV_MASK_LEN)
		       << SDHCI_DIVIDER_HI_SHIFT;
}

static bool ums9117_sdhci_is_operational_clock(unsigned int clock)
{
	return clock >= UMS9117_SDHCI_LEGACY_CLOCK_HZ &&
	       clock <= UMS9117_SDHCI_MAX_CLOCK_HZ;
}

static unsigned int ums9117_sdhci_clock_profile(struct sdhci_host *host,
						unsigned int clock)
{
	if (host->mmc->ios.timing == MMC_TIMING_SD_HS &&
	    clock >= UMS9117_SDHCI_MAX_CLOCK_HZ)
		return UMS9117_SDHCI_MAX_CLOCK_HZ;
	return UMS9117_SDHCI_LEGACY_CLOCK_HZ;
}

static void ums9117_sdhci_apply_clock(struct sdhci_host *host,
				      unsigned int clock)
{
	ktime_t deadline;
	u16 control;

	host->mmc->actual_clock = 0;
	sdhci_writew(host, 0, SDHCI_CLOCK_CONTROL);
	if (!clock)
		return;

	control = ums9117_sdhci_divided_clock(host, clock,
					      &host->mmc->actual_clock);
	control |= SDHCI_CLOCK_INT_EN;
	sdhci_writew(host, control, SDHCI_CLOCK_CONTROL);
	deadline = ktime_add_us(ktime_get(),
				UMS9117_SDHCI_CLOCK_STABLE_TIMEOUT_US);
	do {
		control = sdhci_readw(host, SDHCI_CLOCK_CONTROL);
		if (control & SDHCI_CLOCK_INT_STABLE)
			break;
		udelay(10);
	} while (ktime_before(ktime_get(), deadline));

	if (!(control & SDHCI_CLOCK_INT_STABLE)) {
		dev_err(mmc_dev(host->mmc),
			"internal clock did not become stable\n");
		host->mmc->actual_clock = 0;
		sdhci_writew(host, 0, SDHCI_CLOCK_CONTROL);
		return;
	}

	control |= SDHCI_CLOCK_CARD_EN;
	sdhci_writew(host, control, SDHCI_CLOCK_CONTROL);
	if (sdhci_readw(host, SDHCI_CLOCK_CONTROL) != control) {
		dev_err(mmc_dev(host->mmc),
			"failed to enable the card clock\n");
		host->mmc->actual_clock = 0;
		sdhci_writew(host, 0, SDHCI_CLOCK_CONTROL);
		return;
	}
	host->timeout_clk = host->mmc->actual_clock / 1000U;
	host->mmc->max_busy_timeout =
		ums9117_sdhci_get_max_timeout_count(host) / host->timeout_clk;
}

static void ums9117_sdhci_set_clock(struct sdhci_host *host, unsigned int clock)
{
	struct ums9117_sdhci_host *ums_host = ums9117_sdhci_priv(host);
	unsigned long flags;
	unsigned int target = clock;
	bool physical_width4;

	if (ums9117_sdhci_is_operational_clock(clock))
		target = ums9117_sdhci_clock_profile(host, clock);

	spin_lock_irqsave(&ums_host->policy_lock, flags);
	physical_width4 = ums_host->physical_width4;
	if (clock && ums_host->transition_failed) {
		host->mmc->actual_clock = 0;
		spin_unlock_irqrestore(&ums_host->policy_lock, flags);
		return;
	}
	if (clock && ums9117_sdhci_is_operational_clock(clock) &&
	    !physical_width4) {
		/* SD core requests this before it sends the width-changing ACMD6. */
		ums_host->deferred_clock_hz = target;
		ums_host->operational_clock_deferred = true;
		spin_unlock_irqrestore(&ums_host->policy_lock, flags);
		/* set_ios may have gated CARD_EN before calling us again. */
		ums9117_sdhci_apply_clock(host, UMS9117_SDHCI_IDENT_CLOCK_HZ);
		spin_lock_irqsave(&ums_host->policy_lock, flags);
		ums_host->applied_clock_hz =
			host->mmc->actual_clock ? UMS9117_SDHCI_IDENT_CLOCK_HZ :
						  0;
		if (!host->mmc->actual_clock)
			ums_host->transition_failed = true;
		spin_unlock_irqrestore(&ums_host->policy_lock, flags);
		return;
	}
	if (!clock) {
		ums_host->deferred_clock_hz = 0;
		ums_host->applied_clock_hz = 0;
		ums_host->operational_clock_deferred = false;
	}
	spin_unlock_irqrestore(&ums_host->policy_lock, flags);

	ums9117_sdhci_apply_clock(host, target);

	spin_lock_irqsave(&ums_host->policy_lock, flags);
	ums_host->applied_clock_hz = host->mmc->actual_clock ? target : 0;
	if (physical_width4 && target == ums_host->deferred_clock_hz) {
		ums_host->deferred_clock_hz = 0;
		ums_host->operational_clock_deferred = false;
	}
	spin_unlock_irqrestore(&ums_host->policy_lock, flags);
}

static int ums9117_sdhci_wait_inhibit(struct sdhci_host *host)
{
	ktime_t deadline = ktime_add_us(ktime_get(),
					UMS9117_SDHCI_CLOCK_STABLE_TIMEOUT_US);
	u32 present;

	do {
		present = sdhci_readl(host, SDHCI_PRESENT_STATE);
		if (!(present & (SDHCI_CMD_INHIBIT | SDHCI_DATA_INHIBIT)))
			return 0;
		udelay(10);
	} while (ktime_before(ktime_get(), deadline));
	return -ETIMEDOUT;
}

static int ums9117_sdhci_wait_clock_control(struct sdhci_host *host,
					    u16 expected)
{
	ktime_t deadline = ktime_add_us(ktime_get(),
					UMS9117_SDHCI_CLOCK_STABLE_TIMEOUT_US);

	do {
		if (sdhci_readw(host, SDHCI_CLOCK_CONTROL) == expected)
			return 0;
		udelay(10);
	} while (ktime_before(ktime_get(), deadline));
	return -ETIMEDOUT;
}

static int ums9117_sdhci_transition_width4(struct sdhci_host *host,
					   unsigned int target_clock)
{
	u32 signal_enable;
	u16 clock_control;
	u16 expected_clock;
	u8 host_control;
	unsigned int actual_clock;
	bool signal_masked = false;
	bool width_set = false;
	int ret;

	ret = ums9117_sdhci_wait_inhibit(host);
	if (ret)
		return ret;
	clock_control = sdhci_readw(host, SDHCI_CLOCK_CONTROL);
	expected_clock = ums9117_sdhci_divided_clock(
		host, UMS9117_SDHCI_IDENT_CLOCK_HZ, &actual_clock);
	expected_clock |= SDHCI_CLOCK_INT_EN | SDHCI_CLOCK_INT_STABLE |
			  SDHCI_CLOCK_CARD_EN;
	host_control = sdhci_readb(host, SDHCI_HOST_CONTROL);
	if (clock_control != expected_clock ||
	    actual_clock != UMS9117_SDHCI_IDENT_CLOCK_HZ ||
	    host_control != SDHCI_CTRL_ADMA32 ||
	    host->mmc->actual_clock != UMS9117_SDHCI_IDENT_CLOCK_HZ)
		return -EPROTO;

	signal_enable = sdhci_readl(host, SDHCI_SIGNAL_ENABLE);
	sdhci_writel(host, 0, SDHCI_SIGNAL_ENABLE);
	signal_masked = true;
	if (sdhci_readl(host, SDHCI_SIGNAL_ENABLE)) {
		ret = -EIO;
		goto out_restore_signal;
	}

	clock_control &= ~SDHCI_CLOCK_CARD_EN;
	sdhci_writew(host, clock_control, SDHCI_CLOCK_CONTROL);
	ret = ums9117_sdhci_wait_clock_control(host, clock_control);
	if (ret)
		goto out_restore_signal;
	clock_control &= ~(SDHCI_CLOCK_INT_EN | SDHCI_CLOCK_INT_STABLE);
	sdhci_writew(host, clock_control, SDHCI_CLOCK_CONTROL);
	ret = ums9117_sdhci_wait_clock_control(host, clock_control);
	if (ret)
		goto out_restore_signal;

	sdhci_set_bus_width(host, MMC_BUS_WIDTH_4);
	host_control = sdhci_readb(host, SDHCI_HOST_CONTROL);
	if (host_control != (SDHCI_CTRL_ADMA32 | SDHCI_CTRL_4BITBUS)) {
		ret = -EIO;
		goto out_restore_signal;
	}
	width_set = true;
	ums9117_sdhci_apply_clock(host, target_clock);
	if (host->mmc->actual_clock != target_clock) {
		ret = -EIO;
		goto out_restore_signal;
	}
	ret = 0;

out_restore_signal:
	if (signal_masked)
		sdhci_writel(host, signal_enable, SDHCI_SIGNAL_ENABLE);
	if (ret)
		sdhci_writew(host, 0, SDHCI_CLOCK_CONTROL);
	if (ret && width_set)
		dev_err(mmc_dev(host->mmc),
			"4-bit transition failed after changing host width: %d\n",
			ret);
	return ret;
}

static void ums9117_sdhci_set_bus_width(struct sdhci_host *host, int width)
{
	struct ums9117_sdhci_host *ums_host = ums9117_sdhci_priv(host);
	unsigned long flags;
	unsigned int target_clock;
	int ret;

	if (width == MMC_BUS_WIDTH_1) {
		sdhci_set_bus_width(host, width);
		spin_lock_irqsave(&ums_host->policy_lock, flags);
		ums_host->physical_width4 = false;
		spin_unlock_irqrestore(&ums_host->policy_lock, flags);
		return;
	}
	if (width != MMC_BUS_WIDTH_4) {
		dev_err(mmc_dev(host->mmc), "unsupported bus width %d\n",
			width);
		return;
	}

	spin_lock_irqsave(&ums_host->policy_lock, flags);
	if (ums_host->physical_width4) {
		spin_unlock_irqrestore(&ums_host->policy_lock, flags);
		if (sdhci_readb(host, SDHCI_HOST_CONTROL) ==
		    (SDHCI_CTRL_ADMA32 | SDHCI_CTRL_4BITBUS))
			return;
		spin_lock_irqsave(&ums_host->policy_lock, flags);
		ums_host->transition_failed = true;
		spin_unlock_irqrestore(&ums_host->policy_lock, flags);
		dev_err(mmc_dev(host->mmc),
			"host lost the established 4-bit bus width\n");
		return;
	}
	if (!ums_host->width_acmd6_clean ||
	    !ums_host->operational_clock_deferred ||
	    !ums_host->deferred_clock_hz) {
		ums_host->transition_failed = true;
		spin_unlock_irqrestore(&ums_host->policy_lock, flags);
		dev_err(mmc_dev(host->mmc),
			"4-bit transition lacks clean ACMD6 or deferred clock\n");
		return;
	}
	target_clock = ums_host->deferred_clock_hz;
	ums_host->transition_failed = true;
	spin_unlock_irqrestore(&ums_host->policy_lock, flags);

	ret = ums9117_sdhci_transition_width4(host, target_clock);

	spin_lock_irqsave(&ums_host->policy_lock, flags);
	ums_host->physical_width4 =
		!!(sdhci_readb(host, SDHCI_HOST_CONTROL) & SDHCI_CTRL_4BITBUS);
	if (!ret) {
		ums_host->transition_failed = false;
		ums_host->operational_clock_deferred = false;
		ums_host->deferred_clock_hz = 0;
		ums_host->applied_clock_hz = target_clock;
	} else {
		ums_host->applied_clock_hz = 0;
	}
	spin_unlock_irqrestore(&ums_host->policy_lock, flags);
	if (ret)
		dev_err(mmc_dev(host->mmc),
			"failed to apply 4-bit operational clock: %d\n", ret);
}

static void ums9117_sdhci_set_power(struct sdhci_host *host, unsigned char mode,
				    unsigned short vdd)
{
	struct ums9117_sdhci_host *ums_host = ums9117_sdhci_priv(host);
	struct mmc_host *mmc = host->mmc;
	unsigned long flags;
	int ret;

	switch (mode) {
	case MMC_POWER_OFF:
		ret = mmc_regulator_set_ocr(mmc, mmc->supply.vmmc, 0);
		mmc_regulator_disable_vqmmc(mmc);
		if (ret)
			dev_err(mmc_dev(mmc),
				"failed to disable the card supply: %d\n", ret);
		spin_lock_irqsave(&ums_host->policy_lock, flags);
		if (!ret) {
			ums_host->deferred_clock_hz = 0;
			ums_host->applied_clock_hz = 0;
			ums_host->app_cmd_arg = 0;
			ums_host->app_cmd_armed = false;
			ums_host->active_width_acmd6 = false;
			ums_host->width_acmd6_clean = false;
			ums_host->operational_clock_deferred = false;
			ums_host->physical_width4 = false;
			ums_host->transition_failed = false;
		} else {
			ums_host->transition_failed = true;
		}
		spin_unlock_irqrestore(&ums_host->policy_lock, flags);
		break;
	case MMC_POWER_UP:
		ret = mmc_regulator_set_ocr(mmc, mmc->supply.vmmc, vdd);
		if (!ret)
			ret = mmc_regulator_enable_vqmmc(mmc);
		if (ret) {
			mmc_regulator_disable_vqmmc(mmc);
			mmc_regulator_set_ocr(mmc, mmc->supply.vmmc, 0);
			dev_err(mmc_dev(mmc),
				"failed to enable the slot supplies: %d\n",
				ret);
		}
		break;
	case MMC_POWER_ON:
		break;
	}
}

static unsigned int ums9117_sdhci_get_ro(struct sdhci_host *host)
{
	return 0;
}

static void ums9117_sdhci_request_done(struct sdhci_host *host,
				       struct mmc_request *mrq);

static const struct sdhci_ops ums9117_sdhci_ops = {
	.read_l = ums9117_sdhci_readl,
	.read_w = ums9117_sdhci_readw,
	.read_b = ums9117_sdhci_readb,
	.write_l = ums9117_sdhci_writel,
	.write_w = ums9117_sdhci_writew,
	.write_b = ums9117_sdhci_writeb,
	.set_clock = ums9117_sdhci_set_clock,
	.set_power = ums9117_sdhci_set_power,
	.get_max_clock = ums9117_sdhci_get_max_clock,
	.get_min_clock = ums9117_sdhci_get_min_clock,
	.get_max_timeout_count = ums9117_sdhci_get_max_timeout_count,
	.set_bus_width = ums9117_sdhci_set_bus_width,
	.reset = sdhci_reset,
	.set_uhs_signaling = sdhci_set_uhs_signaling,
	.get_ro = ums9117_sdhci_get_ro,
	.request_done = ums9117_sdhci_request_done,
};

static const struct sdhci_pltfm_data ums9117_sdhci_pdata = {
	.ops = &ums9117_sdhci_ops,
	.quirks = SDHCI_QUIRK_BROKEN_CARD_DETECTION |
		  SDHCI_QUIRK_DATA_TIMEOUT_USES_SDCLK |
		  SDHCI_QUIRK_NO_ENDATTR_IN_NOPDESC |
		  SDHCI_QUIRK_MULTIBLOCK_READ_ACMD12 |
		  SDHCI_QUIRK_NO_HISPD_BIT | SDHCI_QUIRK_NO_LED,
	.quirks2 = SDHCI_QUIRK2_HOST_NO_CMD23 | SDHCI_QUIRK2_NO_1_8_V |
		   SDHCI_QUIRK2_BROKEN_64_BIT_DMA |
		   SDHCI_QUIRK2_USE_32BIT_BLK_CNT,
};

static bool ums9117_sdhci_forbidden_command(const struct mmc_command *command)
{
	switch (command->opcode) {
	case MMC_WRITE_DAT_UNTIL_STOP:
	case MMC_SET_BLOCK_COUNT:
	case MMC_PROGRAM_CID:
	case MMC_PROGRAM_CSD:
	case MMC_SET_WRITE_PROT:
	case MMC_CLR_WRITE_PROT:
	case SD_ERASE_WR_BLK_START:
	case SD_ERASE_WR_BLK_END:
	case MMC_ERASE_GROUP_START:
	case MMC_ERASE_GROUP_END:
	case MMC_ERASE:
	case MMC_LOCK_UNLOCK:
		return true;
	case SD_SWITCH:
		return command->data &&
		       command->arg != UMS9117_SDHCI_CMD6_CHECK_ARG &&
		       command->arg != UMS9117_SDHCI_CMD6_SWITCH_ARG;
	default:
		return false;
	}
}

static bool ums9117_sdhci_take_app_context(struct ums9117_sdhci_host *ums_host,
					   u32 *argument)
{
	unsigned long flags;
	bool armed;

	spin_lock_irqsave(&ums_host->policy_lock, flags);
	armed = ums_host->app_cmd_armed;
	*argument = ums_host->app_cmd_arg;
	ums_host->app_cmd_armed = false;
	ums_host->app_cmd_arg = 0;
	spin_unlock_irqrestore(&ums_host->policy_lock, flags);
	return armed;
}

static bool ums9117_sdhci_is_block_io(unsigned int opcode)
{
	return opcode == MMC_READ_SINGLE_BLOCK ||
	       opcode == MMC_READ_MULTIPLE_BLOCK || opcode == MMC_WRITE_BLOCK ||
	       opcode == MMC_WRITE_MULTIPLE_BLOCK;
}

static int ums9117_sdhci_request_error(struct sdhci_host *host,
				       const struct mmc_request *mrq,
				       bool *width_acmd6)
{
	struct ums9117_sdhci_host *ums_host = ums9117_sdhci_priv(host);
	const struct mmc_command *command = mrq->cmd;
	unsigned long flags;
	u32 app_argument = 0;
	bool app_context = false;
	bool operational;
	bool transition_failed;

	*width_acmd6 = false;
	spin_lock_irqsave(&ums_host->policy_lock, flags);
	transition_failed = ums_host->transition_failed;
	operational = ums_host->physical_width4 &&
		      !ums_host->operational_clock_deferred &&
		      ums9117_sdhci_is_operational_clock(
			      ums_host->applied_clock_hz) &&
		      host->mmc->actual_clock == ums_host->applied_clock_hz;
	spin_unlock_irqrestore(&ums_host->policy_lock, flags);
	if (transition_failed)
		return -EIO;
	if (ums9117_sdhci_is_block_io(command->opcode) && !operational)
		return -EPROTO;

	if (command->opcode != MMC_APP_CMD)
		app_context =
			ums9117_sdhci_take_app_context(ums_host, &app_argument);
	if (mrq->sbc || ums9117_sdhci_forbidden_command(command))
		return -EOPNOTSUPP;
	if (command->opcode == SD_SWITCH && !command->data) {
		if (!app_context || !(app_argument & 0xffff0000U) ||
		    (app_argument & 0x0000ffffU) ||
		    command->arg != SD_BUS_WIDTH_4)
			return -EPROTO;
		*width_acmd6 = true;
	}
	if (command->data && (command->data->flags & MMC_DATA_WRITE) &&
	    command->opcode != MMC_WRITE_BLOCK &&
	    command->opcode != MMC_WRITE_MULTIPLE_BLOCK)
		return -EOPNOTSUPP;
	return 0;
}

static void ums9117_sdhci_reject_request(struct mmc_host *mmc,
					 struct mmc_request *mrq, int error)
{
	if (mrq->sbc)
		mrq->sbc->error = error;
	mrq->cmd->error = error;
	if (mrq->cmd->data) {
		mrq->cmd->data->error = error;
		mrq->cmd->data->bytes_xfered = 0;
	}
	dev_warn_ratelimited(
		mmc_dev(mmc),
		"request rejected before MMIO: opcode=%u error=%d\n",
		mrq->cmd->opcode, error);
	mmc_request_done(mmc, mrq);
}

static void ums9117_sdhci_request(struct mmc_host *mmc, struct mmc_request *mrq)
{
	struct sdhci_host *host = mmc_priv(mmc);
	struct ums9117_sdhci_host *ums_host = ums9117_sdhci_priv(host);
	unsigned long flags;
	bool width_acmd6;
	int error;

	error = ums9117_sdhci_request_error(host, mrq, &width_acmd6);
	if (error) {
		ums9117_sdhci_reject_request(mmc, mrq, error);
		return;
	}
	spin_lock_irqsave(&ums_host->policy_lock, flags);
	ums_host->active_width_acmd6 = width_acmd6;
	spin_unlock_irqrestore(&ums_host->policy_lock, flags);
	sdhci_request(mmc, mrq);
}

static void ums9117_sdhci_request_done(struct sdhci_host *host,
				       struct mmc_request *mrq)
{
	struct ums9117_sdhci_host *ums_host = ums9117_sdhci_priv(host);
	struct mmc_command *command = mrq->cmd;
	unsigned long flags;
	bool width_acmd6;
	bool fail_closed = false;

	spin_lock_irqsave(&ums_host->policy_lock, flags);
	width_acmd6 = ums_host->active_width_acmd6;
	ums_host->active_width_acmd6 = false;
	spin_unlock_irqrestore(&ums_host->policy_lock, flags);
	if ((command->opcode == MMC_APP_CMD || width_acmd6) &&
	    !command->error && R1_STATUS(command->resp[0]))
		command->error = -EIO;

	if (command->opcode == MMC_APP_CMD) {
		spin_lock_irqsave(&ums_host->policy_lock, flags);
		ums_host->app_cmd_armed = !command->error &&
					  (command->resp[0] & R1_APP_CMD);
		ums_host->app_cmd_arg = ums_host->app_cmd_armed ? command->arg :
								  0;
		spin_unlock_irqrestore(&ums_host->policy_lock, flags);
	}
	if (width_acmd6) {
		spin_lock_irqsave(&ums_host->policy_lock, flags);
		if (!command->error) {
			ums_host->width_acmd6_clean = true;
		} else {
			ums_host->width_acmd6_clean = false;
			ums_host->transition_failed = true;
			fail_closed = true;
		}
		spin_unlock_irqrestore(&ums_host->policy_lock, flags);
	}
	if (fail_closed) {
		sdhci_writel(host, 0, SDHCI_SIGNAL_ENABLE);
		ums9117_sdhci_apply_clock(host, 0);
	}
	mmc_request_done(host->mmc, mrq);
}

static int ums9117_sdhci_get_cd(struct mmc_host *mmc)
{
	struct sdhci_host *host = mmc_priv(mmc);
	struct ums9117_sdhci_host *ums_host = ums9117_sdhci_priv(host);

	if (host->flags & SDHCI_DEVICE_DEAD)
		return 0;
	if (!ums_host->raw_card_detect_owned ||
	    !(readl(ums_host->card_detect + UMS9117_SDHCI_CD_MASK) &
	      UMS9117_SDHCI_CD_BIT)) {
		dev_err_ratelimited(mmc_dev(mmc),
				    "card-detect ownership is unavailable\n");
		return 0;
	}
	return !(readl(ums_host->card_detect + UMS9117_SDHCI_CD_DATA) &
		 UMS9117_SDHCI_CD_BIT);
}

static void ums9117_sdhci_release_raw_card_detect(void *data)
{
	struct ums9117_sdhci_host *ums_host = data;
	u32 mask;

	if (!ums_host->raw_card_detect_owned)
		return;
	mask = readl(ums_host->card_detect + UMS9117_SDHCI_CD_MASK);
	writel(mask & ~UMS9117_SDHCI_CD_BIT,
	       ums_host->card_detect + UMS9117_SDHCI_CD_MASK);
	ums_host->raw_card_detect_owned = false;
}

static int ums9117_sdhci_init_raw_card_detect(struct platform_device *pdev,
					      struct sdhci_host *host)
{
	struct ums9117_sdhci_host *ums_host = ums9117_sdhci_priv(host);
	struct resource *resource;
	u32 mask;
	u32 data;
	int ret;

	resource = platform_get_resource_byname(pdev, IORESOURCE_MEM,
						"card-detect");
	if (!resource || resource_size(resource) != UMS9117_SDHCI_CD_MMIO_BYTES)
		return dev_err_probe(
			&pdev->dev, -EINVAL,
			"card-detect must be an 8-byte resource\n");
	ums_host->card_detect = devm_ioremap_resource(&pdev->dev, resource);
	if (IS_ERR(ums_host->card_detect))
		return PTR_ERR(ums_host->card_detect);

	mask = readl(ums_host->card_detect + UMS9117_SDHCI_CD_MASK);
	if (mask ||
	    readl(ums_host->card_detect + UMS9117_SDHCI_CD_MASK) != mask)
		return dev_err_probe(&pdev->dev, -EBUSY,
				     "card-detect mask is already owned\n");
	writel(UMS9117_SDHCI_CD_BIT,
	       ums_host->card_detect + UMS9117_SDHCI_CD_MASK);
	if (readl(ums_host->card_detect + UMS9117_SDHCI_CD_MASK) !=
	    UMS9117_SDHCI_CD_BIT) {
		writel(0, ums_host->card_detect + UMS9117_SDHCI_CD_MASK);
		return dev_err_probe(&pdev->dev, -EIO,
				     "failed to enable card-detect input\n");
	}
	ums_host->raw_card_detect_owned = true;
	ret = devm_add_action_or_reset(
		&pdev->dev, ums9117_sdhci_release_raw_card_detect, ums_host);
	if (ret)
		return ret;

	usleep_range(UMS9117_SDHCI_CD_SETTLE_MIN_US,
		     UMS9117_SDHCI_CD_SETTLE_MAX_US);
	data = readl(ums_host->card_detect + UMS9117_SDHCI_CD_DATA);
	if (data & ~UMS9117_SDHCI_CD_BIT)
		return dev_err_probe(
			&pdev->dev, -EUCLEAN,
			"unexpected card-detect inputs are active\n");
	return 0;
}

static int ums9117_sdhci_enable_resources(struct platform_device *pdev,
					  struct sdhci_host *host)
{
	struct ums9117_sdhci_host *ums_host = ums9117_sdhci_priv(host);
	unsigned long rate;
	int ret;

	ums_host->enable_clk = devm_clk_get_enabled(&pdev->dev, "enable");
	if (IS_ERR(ums_host->enable_clk))
		return dev_err_probe(&pdev->dev, PTR_ERR(ums_host->enable_clk),
				     "failed to enable the SDIO0 gate\n");
	ums_host->reset = devm_reset_control_get_exclusive(&pdev->dev, "sdio");
	if (IS_ERR(ums_host->reset))
		return dev_err_probe(&pdev->dev, PTR_ERR(ums_host->reset),
				     "failed to get the SDIO0 reset\n");
	ret = reset_control_reset(ums_host->reset);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to reset SDIO0\n");
	ums_host->sdio_clk = devm_clk_get_enabled(&pdev->dev, "sdio");
	if (IS_ERR(ums_host->sdio_clk))
		return dev_err_probe(
			&pdev->dev, PTR_ERR(ums_host->sdio_clk),
			"failed to enable the SDIO source clock\n");
	rate = clk_get_rate(ums_host->sdio_clk);
	if (rate != UMS9117_SDHCI_BASE_CLOCK_HZ)
		return dev_err_probe(
			&pdev->dev, -EINVAL,
			"SDIO source clock must be %u Hz, got %lu Hz\n",
			UMS9117_SDHCI_BASE_CLOCK_HZ, rate);
	return 0;
}

static int ums9117_sdhci_get_supplies(struct sdhci_host *host)
{
	struct mmc_host *mmc = host->mmc;
	int ret;

	ret = mmc_regulator_get_supply(mmc);
	if (ret)
		return ret;
	if (IS_ERR(mmc->supply.vmmc) || IS_ERR(mmc->supply.vqmmc))
		return dev_err_probe(mmc_dev(mmc), -ENODEV,
				     "vmmc and vqmmc supplies are required\n");
	return 0;
}

static int ums9117_sdhci_validate_host_resource(struct platform_device *pdev)
{
	struct resource *resource;

	resource = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!resource ||
	    resource_size(resource) != UMS9117_SDHCI_HOST_MMIO_BYTES)
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "host must be a 0x%x-byte resource\n",
				     UMS9117_SDHCI_HOST_MMIO_BYTES);
	return 0;
}

static void ums9117_sdhci_apply_limits(struct sdhci_host *host)
{
	struct mmc_host *mmc = host->mmc;

	mmc->caps &= MMC_CAP_4_BIT_DATA | MMC_CAP_SD_HIGHSPEED;
	mmc->caps |= MMC_CAP_NEEDS_POLL;
	mmc->caps2 = MMC_CAP2_NO_SDIO | MMC_CAP2_NO_MMC |
		     MMC_CAP2_NO_WRITE_PROTECT;
	mmc->f_min = UMS9117_SDHCI_IDENT_CLOCK_HZ;
	mmc->f_max = min(mmc->f_max, UMS9117_SDHCI_MAX_CLOCK_HZ);
	mmc->max_segs = UMS9117_SDHCI_MAX_SEGS;
	mmc->max_seg_size = PAGE_SIZE;
	mmc->max_req_size = UMS9117_SDHCI_MAX_REQUEST_BYTES;
	mmc->max_blk_size = min(mmc->max_blk_size, 512U);
	mmc->max_blk_count = UMS9117_SDHCI_MAX_REQUEST_BYTES / 512U;
	mmc->retune_period = 0;
	host->tuning_count = 0;
	host->tuning_mode = SDHCI_TUNING_MODE_1;
}

static int ums9117_sdhci_probe(struct platform_device *pdev)
{
	struct sdhci_pltfm_host *pltfm_host;
	struct ums9117_sdhci_host *ums_host;
	struct sdhci_host *host;
	u16 version;
	int ret;

	ret = ums9117_sdhci_validate_host_resource(pdev);
	if (ret)
		return ret;
	host = sdhci_pltfm_init(pdev, &ums9117_sdhci_pdata, sizeof(*ums_host));
	if (IS_ERR(host))
		return PTR_ERR(host);
	pltfm_host = sdhci_priv(host);
	ums_host = sdhci_pltfm_priv(pltfm_host);
	spin_lock_init(&ums_host->policy_lock);

	sdhci_get_property(pdev);
	ret = mmc_of_parse(host->mmc);
	if (ret)
		return ret;
	ret = ums9117_sdhci_get_supplies(host);
	if (ret)
		return ret;
	ret = ums9117_sdhci_init_raw_card_detect(pdev, host);
	if (ret)
		return ret;
	ret = ums9117_sdhci_enable_resources(pdev, host);
	if (ret)
		return ret;
	pltfm_host->clk = ums_host->sdio_clk;

	version = sdhci_readw(host, SDHCI_HOST_VERSION);
	if ((version & SDHCI_SPEC_VER_MASK) != SDHCI_SPEC_410)
		return dev_err_probe(
			&pdev->dev, -EPROTONOSUPPORT,
			"expected SDHCI 4.10, got version 0x%04x\n", version);
	if (!(sdhci_readb(host, SDHCI_SOFTWARE_RESET) &
	      UMS9117_SDHCI_HW_RESET_CARD))
		return dev_err_probe(
			&pdev->dev, -EPROTONOSUPPORT,
			"SDIO card hardware-reset state is not ready\n");
	ums_host->transfer_mode = 0;
	sdhci_enable_v4_mode(host);
	host->sdma_boundary = 0;
	host->adma_table_cnt = UMS9117_SDHCI_ADMA_TABLE_COUNT;
	host->mmc->caps2 |= MMC_CAP2_NO_SDIO | MMC_CAP2_NO_MMC |
			    MMC_CAP2_NO_WRITE_PROTECT;
	host->mmc_host_ops.request = ums9117_sdhci_request;
	host->mmc_host_ops.get_cd = ums9117_sdhci_get_cd;

	ret = sdhci_setup_host(host);
	if (ret)
		return ret;
	if (host->version != SDHCI_SPEC_410 ||
	    host->max_clk != UMS9117_SDHCI_BASE_CLOCK_HZ ||
	    !(host->flags & SDHCI_USE_ADMA) ||
	    host->flags & SDHCI_USE_64_BIT_DMA ||
	    host->mmc->max_blk_size < 512U) {
		ret = -EPROTONOSUPPORT;
		dev_err(&pdev->dev,
			"SDHCI 4.10, 195 MHz and 32-bit ADMA2 are required\n");
		goto out_cleanup;
	}
	dma_set_max_seg_size(&pdev->dev, PAGE_SIZE);
	ums9117_sdhci_apply_limits(host);

	ret = __sdhci_add_host(host);
	if (ret)
		goto out_cleanup;
	return 0;

out_cleanup:
	sdhci_cleanup_host(host);
	return ret;
}

static void ums9117_sdhci_remove(struct platform_device *pdev)
{
	struct sdhci_host *host = platform_get_drvdata(pdev);
	bool dead;

	dead = sdhci_readl(host, SDHCI_INT_STATUS) == U32_MAX;
	sdhci_remove_host(host, dead);
}

static const struct of_device_id ums9117_sdhci_of_match[] = {
	{ .compatible = "sprd,ums9117-sdhci" },
	{}
};
MODULE_DEVICE_TABLE(of, ums9117_sdhci_of_match);

static struct platform_driver ums9117_sdhci_driver = {
	.probe = ums9117_sdhci_probe,
	.remove = ums9117_sdhci_remove,
	.driver = {
		.name = "sdhci-ums9117",
		.of_match_table = ums9117_sdhci_of_match,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(ums9117_sdhci_driver);

MODULE_DESCRIPTION("Unisoc UMS9117 SDHCI platform driver");
MODULE_LICENSE("GPL");
