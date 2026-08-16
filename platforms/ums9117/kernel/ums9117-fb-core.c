// SPDX-License-Identifier: GPL-2.0-only
/*
 * Shared UMS9117 framebuffer/LCDC lifecycle.
 *
 * The target wrapper provides a fixed, source-validated panel script.  DT
 * supplies only board wiring (MMIO resources, pin configuration, WLED levels
 * and DBI timings); it never supplies panel command bytes or detects a panel.
 */
#include <linux/bitops.h>
#include <linux/console.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/fb.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/jiffies.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/slab.h>

#include "ums9117-fb-internal.h"

#define LCDC_CTRL 0x000
#define LCDC_DISP_SIZE 0x004
#define LCDC_LCM_START 0x008
#define LCDC_LCM_SIZE 0x00c
#define LCDC_BG_COLOR 0x010
#define LCDC_IMG_CTRL 0x020
#define LCDC_IMG_Y_BASE 0x024
#define LCDC_IMG_SIZE_XY 0x02c
#define LCDC_IMG_PITCH 0x030
#define LCDC_IMG_DISP_XY 0x034
#define LCDC_CAP_CTRL 0x0e0
#define LCDC_CAP_BASE 0x0e4
#define LCDC_IRQ_EN 0x110
#define LCDC_IRQ_CLR 0x114
#define LCDC_IRQ_STATUS 0x118
#define LCDC_IRQ_RAW 0x11c
#define LCDC_DONE BIT(0)
#define LCDC_RUN BIT(3)
#define LCDC_RGB_MODE (7u << 5)

#define UMS9117_ADI_PHYS 0x40600000u
#define UMS9117_ANALOG_PHYS 0x40608000u
#define ADI_CONTROLLER_MIN_SIZE 0x228u

#define BLTC_CTRL 0x180
#define BLTC_CURRENT0 0x1b8
#define BLTC_CURRENT1 0x1bc
#define BLTC_CURRENT2 0x1c0
#define BLTC_CURRENT3 0x1c4
#define BLTC_WLED_PRESCALER 0x1c8
#define BLTC_WLED_DUTY 0x1cc
#define BLTC_PD_CTRL 0x1d8
#define BLTC_CURRENT_MASK GENMASK(5, 0)
#define BLTC_SW_PD BIT(0)
#define ANA_MODULE_EN0 0xc08
#define ANA_RTC_CLK_EN0 0xc10
#define ANA_LDO_PD_CTRL 0xdec
#define ANA_WLED_MODULE_EN BIT(9)
#define ANA_WLED_RTC_CLK_EN BIT(7)
#define ANA_WLED_LDO_PD (BIT(2) | BIT(0))

#define AON_PANEL_RESET_SET 0x160c
#define AON_PANEL_RESET_CLEAR 0x260c
#define AP_AHB_LCDC_GATE BIT(11)
#define AP_AHB_LCM_GATE BIT(12)
#define AP_AHB_LCDC_RESET BIT(1)

#define PANEL_DISPLAY_OFF 0x28
#define PANEL_DISPLAY_ON 0x29
#define PANEL_SLEEP_IN 0x10
#define PANEL_SLEEP_OUT 0x11
#define PANEL_WRITE_RAM 0x2c

#define FRAME_TIMEOUT_MS 250u
#define FRAME_TIMEOUT_US (FRAME_TIMEOUT_MS * 1000u)
#define WLED_DISABLE_ATTEMPTS 3u
#define WLED_DISABLE_RETRY_US 5000u

static DEFINE_MUTEX(ums9117_fb_lifetime_lock);
static struct fb_info *ums9117_fb_retired_info;

static u32 ums9117_fb_stride(const struct ums9117_fb *ufb)
{
	return ufb->profile->width * sizeof(u16);
}

static u32 ums9117_fb_size(const struct ums9117_fb *ufb)
{
	return ums9117_fb_stride(ufb) * ufb->profile->height;
}

static bool ums9117_fb_damage_pending(const struct ums9117_fb *ufb)
{
	return ufb->damage_seq != ufb->submitted_seq;
}

static bool ums9117_fb_can_refresh(const struct ums9117_fb *ufb)
{
	return !ufb->stopping && !ufb->in_flight &&
	       ufb->state == UMS9117_FB_ACTIVE;
}

static void ums9117_fb_stop_lcdc(struct ums9117_fb *ufb)
{
	writel(readl(ufb->lcdc + LCDC_IRQ_EN) & ~LCDC_DONE,
	       ufb->lcdc + LCDC_IRQ_EN);
	writel(readl(ufb->lcdc + LCDC_CTRL) & ~(BIT(0) | LCDC_RUN),
	       ufb->lcdc + LCDC_CTRL);
	writel(LCDC_DONE, ufb->lcdc + LCDC_IRQ_CLR);
	readl(ufb->lcdc + LCDC_IRQ_RAW);
}

static void ums9117_fb_set_wled_state(struct ums9117_fb *ufb, bool known,
				      bool on)
{
	unsigned long flags;

	spin_lock_irqsave(&ufb->lock, flags);
	ufb->wled_known = known;
	ufb->wled_on = known && on;
	spin_unlock_irqrestore(&ufb->lock, flags);
}

static int ums9117_fb_wled_set(struct ums9117_fb *ufb, bool on)
{
	static const u32 current_regs[UMS9117_FB_MAX_WLED_CHANNELS] = {
		BLTC_CURRENT0,
		BLTC_CURRENT1,
		BLTC_CURRENT2,
		BLTC_CURRENT3,
	};
	struct ums9117_adi_transaction transaction = {};
	u16 value;
	u16 active_mask = 0;
	unsigned int i;
	int ret;
	int end_ret;

	ret = ums9117_adi_begin(&transaction);
	if (ret)
		goto out;
	ret = ums9117_adi_update_bits(&transaction, ANA_MODULE_EN0,
				      ANA_WLED_MODULE_EN, ANA_WLED_MODULE_EN);
	if (!ret)
		ret = ums9117_adi_update_bits(&transaction, ANA_RTC_CLK_EN0,
					      ANA_WLED_RTC_CLK_EN,
					      ANA_WLED_RTC_CLK_EN);
	if (!ret)
		ret = ums9117_adi_update_bits(&transaction, ANA_LDO_PD_CTRL,
					      ANA_WLED_LDO_PD, 0);
	if (!ret && !on)
		ret = ums9117_adi_write(&transaction, BLTC_CTRL, 0);
	if (!ret && !on) {
		ret = ums9117_adi_read(&transaction, BLTC_PD_CTRL, &value);
		if (!ret)
			ret = ums9117_adi_write(&transaction, BLTC_PD_CTRL,
						value | BLTC_SW_PD);
	}
	if (!ret && on) {
		for (i = 0; !ret && i < ARRAY_SIZE(current_regs); i++) {
			if (ufb->wled_levels[i])
				active_mask |= 0xc << (i * 4);
			ret = ums9117_adi_read(&transaction, current_regs[i],
					       &value);
			if (!ret)
				ret = ums9117_adi_write(
					&transaction, current_regs[i],
					(value & ~BLTC_CURRENT_MASK) |
						ufb->wled_levels[i]);
		}
	}
	if (!ret && on)
		ret = ums9117_adi_update_bits(&transaction, BLTC_WLED_PRESCALER,
					      0xff, 0);
	if (!ret && on)
		ret = ums9117_adi_write(&transaction, BLTC_WLED_DUTY, 0);
	if (!ret && on) {
		ret = ums9117_adi_read(&transaction, BLTC_PD_CTRL, &value);
		if (!ret)
			ret = ums9117_adi_write(&transaction, BLTC_PD_CTRL,
						value & ~BLTC_SW_PD);
	}
	if (!ret && on)
		ret = ums9117_adi_write(&transaction, BLTC_CTRL, active_mask);
	end_ret = ums9117_adi_end(&transaction);
	if (!ret)
		ret = end_ret;
out:
	if (ret)
		ufb->stats.wled_errors++;
	ums9117_fb_set_wled_state(ufb, !ret, on);
	return ret;
}

static int ums9117_fb_wled_off_bounded(struct ums9117_fb *ufb)
{
	unsigned int attempt;
	int ret = -EBUSY;

	for (attempt = 0; attempt < WLED_DISABLE_ATTEMPTS; attempt++) {
		ret = ums9117_fb_wled_set(ufb, false);
		if (ret != -EBUSY)
			break;
		usleep_range(WLED_DISABLE_RETRY_US,
			     WLED_DISABLE_RETRY_US + 1000);
	}
	return ret;
}

static int ums9117_fb_dcs_common(struct ums9117_fb *ufb, u8 command,
				 const u8 *data, size_t length, bool force)
{
	int ret;

	if (!force && READ_ONCE(ufb->transport_faulted))
		return -EIO;
	if (ufb->profile->transport == UMS9117_FB_TRANSPORT_SPI1_3WIRE)
		ret = ums9117_fb_spi_dcs(ufb, command, data, length);
	else
		ret = ums9117_fb_lcm_dcs(ufb, command, data, length);
	if (ret) {
		unsigned long flags;

		spin_lock_irqsave(&ufb->lock, flags);
		ufb->transport_faulted = true;
		ufb->last_dcs_command = command;
		ufb->stats.dcs_errors++;
		if (ret == -ETIMEDOUT)
			ufb->stats.dcs_timeouts++;
		spin_unlock_irqrestore(&ufb->lock, flags);
	}
	return ret;
}

static int ums9117_fb_dcs(struct ums9117_fb *ufb, u8 command, const u8 *data,
			  size_t length)
{
	return ums9117_fb_dcs_common(ufb, command, data, length, false);
}

/* Best-effort display-off must still reach the transport after a prior fault. */
static int ums9117_fb_dcs_force(struct ums9117_fb *ufb, u8 command)
{
	return ums9117_fb_dcs_common(ufb, command, NULL, 0, true);
}

static int ums9117_fb_fail_dark(struct ums9117_fb *ufb)
{
	unsigned long flags;
	int wled_ret;
	int dcs_ret;

	/* Do both bounded operations: either one may be the only one that works. */
	wled_ret = ums9117_fb_wled_off_bounded(ufb);
	dcs_ret = ums9117_fb_dcs_force(ufb, PANEL_DISPLAY_OFF);
	if (wled_ret && dcs_ret) {
		spin_lock_irqsave(&ufb->lock, flags);
		ufb->stats.fail_dark_failures++;
		spin_unlock_irqrestore(&ufb->lock, flags);
	}
	return wled_ret ? wled_ret : dcs_ret;
}

static int ums9117_fb_run_init(struct ums9117_fb *ufb)
{
	unsigned int i;
	int ret;

	for (i = 0; i < ufb->profile->init_count; i++) {
		const struct ums9117_fb_command *command =
			&ufb->profile->init[i];

		if (command->length > ARRAY_SIZE(command->data))
			return -EINVAL;
		if (command->command || command->length) {
			ret = ums9117_fb_dcs(ufb, command->command,
					     command->data, command->length);
			if (ret)
				return ret;
		}
		if (command->delay_ms)
			msleep(command->delay_ms);
	}
	return 0;
}

static int ums9117_fb_begin_transport_frame(struct ums9117_fb *ufb)
{
	if (ufb->profile->transport == UMS9117_FB_TRANSPORT_SPI1_3WIRE)
		return ums9117_fb_spi_begin_frame(ufb);
	return ums9117_fb_lcm_begin_frame(ufb);
}

static int ums9117_fb_start_frame(struct ums9117_fb *ufb, bool cold)
{
	unsigned long flags;
	u32 width = ufb->profile->width;
	u32 height = ufb->profile->height;
	u32 value;
	u64 submitted;
	int ret;

	spin_lock_irqsave(&ufb->lock, flags);
	if (ufb->stopping || ufb->in_flight || ufb->state == UMS9117_FB_ERROR ||
	    (!cold && ufb->state != UMS9117_FB_ACTIVE &&
	     ufb->state != UMS9117_FB_WAKING)) {
		spin_unlock_irqrestore(&ufb->lock, flags);
		return -ESHUTDOWN;
	}
	if (!cold && !ums9117_fb_damage_pending(ufb)) {
		spin_unlock_irqrestore(&ufb->lock, flags);
		return -ESHUTDOWN;
	}
	submitted = ufb->damage_seq;
	spin_unlock_irqrestore(&ufb->lock, flags);

	ret = ums9117_fb_begin_transport_frame(ufb);
	if (ret)
		return ret;
	memcpy_fromio(ufb->snapshot,
		      ufb->screen + ufb->shown * ums9117_fb_stride(ufb),
		      ums9117_fb_size(ufb));
	memcpy_toio(ufb->transfer, ufb->snapshot, ums9117_fb_size(ufb));
	/* Publish the frame pixels before the LCDC is programmed to fetch. */
	wmb();

	writel(readl(ufb->lcdc + LCDC_CTRL) | BIT(0), ufb->lcdc + LCDC_CTRL);
	writel(width | height << 16, ufb->lcdc + LCDC_DISP_SIZE);
	writel(0, ufb->lcdc + LCDC_LCM_START);
	writel(width | height << 16, ufb->lcdc + LCDC_LCM_SIZE);
	writel(0, ufb->lcdc + LCDC_BG_COLOR);
	value = readl(ufb->lcdc + LCDC_IMG_CTRL);
	value &= ~BIT(1);
	value = (value & ~(0xf << 4)) | (5 << 4);
	value = (value & ~(3 << 8)) | (2 << 8);
	value |= BIT(0);
	writel(value, ufb->lcdc + LCDC_IMG_CTRL);
	writel((u32)(ufb->transfer_phys >> 2), ufb->lcdc + LCDC_IMG_Y_BASE);
	writel(width | height << 16, ufb->lcdc + LCDC_IMG_SIZE_XY);
	writel(width, ufb->lcdc + LCDC_IMG_PITCH);
	writel(0, ufb->lcdc + LCDC_IMG_DISP_XY);
	value = readl(ufb->lcdc + LCDC_CAP_CTRL);
	value &= ~(3 << 6);
	value |= ufb->profile->transport == UMS9117_FB_TRANSPORT_LCM_DBI ?
			 2 << 6 :
			 0;
	value |= 0x20;
	writel(value, ufb->lcdc + LCDC_CAP_CTRL);
	writel((u32)(ufb->stream_phys >> 2), ufb->lcdc + LCDC_CAP_BASE);
	/* Complete the LCDC programming before arming and starting a frame. */
	wmb();
	reinit_completion(&ufb->frame_done);
	writel(LCDC_DONE, ufb->lcdc + LCDC_IRQ_CLR);
	if (ufb->profile->completion == UMS9117_FB_COMPLETION_IRQ)
		writel(LCDC_DONE, ufb->lcdc + LCDC_IRQ_EN);
	else
		writel(readl(ufb->lcdc + LCDC_IRQ_EN) & ~LCDC_DONE,
		       ufb->lcdc + LCDC_IRQ_EN);
	value = readl(ufb->lcdc + LCDC_CTRL);
	value &= ~LCDC_RGB_MODE;
	value &= ~ufb->profile->lcdc_ctrl_clear;
	value |= ufb->profile->lcdc_ctrl_set;

	spin_lock_irqsave(&ufb->lock, flags);
	if (ufb->stopping || ufb->in_flight) {
		spin_unlock_irqrestore(&ufb->lock, flags);
		ums9117_fb_stop_lcdc(ufb);
		return -ESHUTDOWN;
	}
	ufb->submitted_seq = submitted;
	ufb->generation++;
	ufb->frame_deadline = jiffies + msecs_to_jiffies(FRAME_TIMEOUT_MS);
	ufb->in_flight = true;
	ufb->stats.frames_started++;
	writel(value | LCDC_RUN, ufb->lcdc + LCDC_CTRL);
	spin_unlock_irqrestore(&ufb->lock, flags);
	return 0;
}

static void ums9117_fb_enter_error(struct ums9117_fb *ufb, int error)
{
	unsigned long flags;

	spin_lock_irqsave(&ufb->lock, flags);
	ufb->last_error_errno = error;
	ufb->state = UMS9117_FB_ERROR;
	ufb->in_flight = false;
	spin_unlock_irqrestore(&ufb->lock, flags);
	ums9117_fb_stop_lcdc(ufb);
	ums9117_fb_fail_dark(ufb);
}

/*
 * A successful completion, whether delivered by Nokia's IRQ or an INOI raw
 * status poll, follows exactly one lifecycle and releases the same waiters.
 */
static void ums9117_fb_frame_done(struct ums9117_fb *ufb, bool from_irq)
{
	unsigned long flags;
	bool wake_done;

	spin_lock_irqsave(&ufb->lock, flags);
	if (!ufb->in_flight) {
		ufb->stats.irq_spurious++;
		spin_unlock_irqrestore(&ufb->lock, flags);
		return;
	}
	ufb->in_flight = false;
	ufb->done_generation = ufb->generation;
	if (from_irq)
		ufb->stats.frames_done_irq++;
	else
		ufb->stats.frames_done_poll++;
	wake_done = !ufb->stopping && ufb->state == UMS9117_FB_WAKING;
	cancel_delayed_work(&ufb->timeout_work);
	complete(&ufb->frame_done);
	if (wake_done)
		schedule_work(&ufb->wake_work);
	else if (ums9117_fb_can_refresh(ufb) && ums9117_fb_damage_pending(ufb))
		schedule_work(&ufb->refresh_work);
	else if (ufb->profile->completion == UMS9117_FB_COMPLETION_IRQ)
		writel(readl(ufb->lcdc + LCDC_IRQ_EN) & ~LCDC_DONE,
		       ufb->lcdc + LCDC_IRQ_EN);
	spin_unlock_irqrestore(&ufb->lock, flags);
}

/*
 * fpdoom's INOI LCM path observes LCDC IRQ_RAW bit 0 as the transfer-done
 * status.  This is deliberately a finite status poll, not an IRQ19 claim.
 */
static int ums9117_fb_poll_frame_done(struct ums9117_fb *ufb)
{
	unsigned long flags;
	u32 raw;
	int ret;
	bool timed_out = false;

	ret = readl_poll_timeout(ufb->lcdc + LCDC_IRQ_RAW, raw, raw & LCDC_DONE,
				 1000, FRAME_TIMEOUT_US);
	if (!ret) {
		writel(LCDC_DONE, ufb->lcdc + LCDC_IRQ_CLR);
		readl(ufb->lcdc + LCDC_IRQ_RAW);
		ums9117_fb_frame_done(ufb, false);
		return 0;
	}

	spin_lock_irqsave(&ufb->lock, flags);
	if (ufb->in_flight && !ufb->stopping) {
		ufb->stats.frame_timeouts++;
		ufb->stats.last_error_irq_status =
			readl(ufb->lcdc + LCDC_IRQ_STATUS);
		ufb->stats.last_error_irq_raw = readl(ufb->lcdc + LCDC_IRQ_RAW);
		if (ufb->stats.last_error_irq_raw & LCDC_DONE)
			ufb->stats.irq_missed++;
		timed_out = true;
	}
	spin_unlock_irqrestore(&ufb->lock, flags);
	if (!timed_out)
		return 0;
	ums9117_fb_enter_error(ufb, -ETIMEDOUT);
	complete(&ufb->frame_done);
	return -ETIMEDOUT;
}

static void ums9117_fb_poll_work(struct work_struct *work)
{
	struct ums9117_fb *ufb =
		container_of(work, struct ums9117_fb, poll_work);

	ums9117_fb_poll_frame_done(ufb);
}

static void ums9117_fb_prepare_irq_frame(struct ums9117_fb *ufb)
{
	if (ufb->profile->completion == UMS9117_FB_COMPLETION_IRQ)
		cancel_delayed_work_sync(&ufb->timeout_work);
}

static void ums9117_fb_arm_irq_timeout(struct ums9117_fb *ufb)
{
	unsigned long flags;

	spin_lock_irqsave(&ufb->lock, flags);
	if (ufb->profile->completion == UMS9117_FB_COMPLETION_IRQ &&
	    ufb->in_flight && !ufb->stopping)
		schedule_delayed_work(&ufb->timeout_work,
				      msecs_to_jiffies(FRAME_TIMEOUT_MS));
	spin_unlock_irqrestore(&ufb->lock, flags);
}

static void ums9117_fb_refresh_work(struct work_struct *work)
{
	struct ums9117_fb *ufb =
		container_of(work, struct ums9117_fb, refresh_work);
	unsigned long flags;
	bool refresh;
	int ret;

	spin_lock_irqsave(&ufb->lock, flags);
	refresh = ums9117_fb_can_refresh(ufb) && ums9117_fb_damage_pending(ufb);
	spin_unlock_irqrestore(&ufb->lock, flags);
	if (!refresh)
		return;
	ums9117_fb_prepare_irq_frame(ufb);
	mutex_lock(&ufb->panel_lock);
	ret = ums9117_fb_start_frame(ufb, false);
	mutex_unlock(&ufb->panel_lock);
	if (ret && ret != -ESHUTDOWN)
		ums9117_fb_enter_error(ufb, ret);
	else if (!ret && ufb->profile->completion == UMS9117_FB_COMPLETION_IRQ)
		ums9117_fb_arm_irq_timeout(ufb);
	else if (!ret)
		schedule_work(&ufb->poll_work);
}

static void ums9117_fb_wake_work(struct work_struct *work)
{
	struct ums9117_fb *ufb =
		container_of(work, struct ums9117_fb, wake_work);
	unsigned long flags;
	int ret;

	mutex_lock(&ufb->transition_lock);
	mutex_lock(&ufb->panel_lock);
	spin_lock_irqsave(&ufb->lock, flags);
	if (ufb->stopping || ufb->state != UMS9117_FB_WAKING ||
	    ufb->in_flight) {
		spin_unlock_irqrestore(&ufb->lock, flags);
		goto out;
	}
	spin_unlock_irqrestore(&ufb->lock, flags);
	ret = ums9117_fb_wled_set(ufb, true);
	if (ret) {
		ums9117_fb_enter_error(ufb, ret);
		goto out;
	}
	spin_lock_irqsave(&ufb->lock, flags);
	ufb->state = UMS9117_FB_ACTIVE;
	ufb->stats.wake_count++;
	if (ums9117_fb_damage_pending(ufb))
		schedule_work(&ufb->refresh_work);
	spin_unlock_irqrestore(&ufb->lock, flags);
out:
	mutex_unlock(&ufb->panel_lock);
	mutex_unlock(&ufb->transition_lock);
}

static irqreturn_t ums9117_fb_lcdc_irq(int irq, void *data)
{
	struct ums9117_fb *ufb = data;
	unsigned long flags;
	u32 status = readl(ufb->lcdc + LCDC_IRQ_STATUS);

	(void)irq;
	if (!(status & LCDC_DONE)) {
		spin_lock_irqsave(&ufb->lock, flags);
		ufb->stats.irq_spurious++;
		ufb->stats.last_error_irq_status = status;
		ufb->stats.last_error_irq_raw = readl(ufb->lcdc + LCDC_IRQ_RAW);
		spin_unlock_irqrestore(&ufb->lock, flags);
		return IRQ_NONE;
	}
	writel(LCDC_DONE, ufb->lcdc + LCDC_IRQ_CLR);
	readl(ufb->lcdc + LCDC_IRQ_RAW);
	ums9117_fb_frame_done(ufb, true);
	return IRQ_HANDLED;
}

static void ums9117_fb_timeout_work(struct work_struct *work)
{
	struct ums9117_fb *ufb = container_of(to_delayed_work(work),
					      struct ums9117_fb, timeout_work);
	unsigned long flags;
	bool timed_out = false;

	mutex_lock(&ufb->panel_lock);
	spin_lock_irqsave(&ufb->lock, flags);
	if (ufb->in_flight && !ufb->stopping &&
	    time_after_eq(jiffies, ufb->frame_deadline)) {
		ufb->stats.frame_timeouts++;
		ufb->stats.last_error_irq_status =
			readl(ufb->lcdc + LCDC_IRQ_STATUS);
		ufb->stats.last_error_irq_raw = readl(ufb->lcdc + LCDC_IRQ_RAW);
		if (ufb->stats.last_error_irq_raw & LCDC_DONE)
			ufb->stats.irq_missed++;
		timed_out = true;
	}
	spin_unlock_irqrestore(&ufb->lock, flags);
	if (timed_out) {
		ums9117_fb_enter_error(ufb, -ETIMEDOUT);
		complete(&ufb->frame_done);
	}
	mutex_unlock(&ufb->panel_lock);
}

static int ums9117_fb_quiesce(struct ums9117_fb *ufb)
{
	unsigned long flags;
	unsigned long remaining;
	bool in_flight;

	cancel_work_sync(&ufb->refresh_work);
	cancel_delayed_work_sync(&ufb->timeout_work);
	spin_lock_irqsave(&ufb->lock, flags);
	in_flight = ufb->in_flight;
	remaining = in_flight && time_before(jiffies, ufb->frame_deadline) ?
			    ufb->frame_deadline - jiffies :
			    0;
	spin_unlock_irqrestore(&ufb->lock, flags);
	if (in_flight &&
	    !wait_for_completion_timeout(&ufb->frame_done, remaining)) {
		cancel_work_sync(&ufb->poll_work);
		ums9117_fb_enter_error(ufb, -ETIMEDOUT);
		return -ETIMEDOUT;
	}
	cancel_work_sync(&ufb->poll_work);
	ums9117_fb_stop_lcdc(ufb);
	if (ufb->profile->completion == UMS9117_FB_COMPLETION_IRQ)
		synchronize_irq(ufb->irq);
	return 0;
}

static void ums9117_fb_mark_damage(struct ums9117_fb *ufb)
{
	unsigned long flags;

	spin_lock_irqsave(&ufb->lock, flags);
	ufb->damage_seq++;
	if (ums9117_fb_can_refresh(ufb))
		schedule_work(&ufb->refresh_work);
	spin_unlock_irqrestore(&ufb->lock, flags);
}

static int ums9117_fb_setcolreg(unsigned int regno, unsigned int red,
				unsigned int green, unsigned int blue,
				unsigned int transp, struct fb_info *info)
{
	struct ums9117_fb *ufb = info->par;

	(void)transp;
	if (regno >= ARRAY_SIZE(ufb->pseudo_palette))
		return -EINVAL;
	ufb->pseudo_palette[regno] = ((red >> 11) << 11) |
				     ((green >> 10) << 5) | (blue >> 11);
	return 0;
}

static void ums9117_fb_damage_range(struct fb_info *info, off_t offset,
				    size_t length)
{
	(void)offset;
	(void)length;
	ums9117_fb_mark_damage(info->par);
}

static void ums9117_fb_damage_area(struct fb_info *info, u32 x, u32 y,
				   u32 width, u32 height)
{
	(void)x;
	(void)y;
	(void)width;
	(void)height;
	ums9117_fb_mark_damage(info->par);
}

static int ums9117_fb_check_var(struct fb_var_screeninfo *var,
				struct fb_info *info)
{
	struct ums9117_fb *ufb = info->par;
	u32 width = ufb->profile->width;
	u32 height = ufb->profile->height;

	if (var->xres != width || var->yres != height ||
	    var->bits_per_pixel != 16 || var->xres_virtual != width ||
	    (var->yres_virtual != height && var->yres_virtual != 2 * height) ||
	    (var->yoffset != 0 && var->yoffset != height) ||
	    var->yoffset + height > var->yres_virtual)
		return -EINVAL;
	return 0;
}

static int ums9117_fb_pan_display(struct fb_var_screeninfo *var,
				  struct fb_info *info)
{
	struct ums9117_fb *ufb = info->par;
	unsigned long flags;

	if (var->yoffset != 0 && var->yoffset != ufb->profile->height)
		return -EINVAL;
	spin_lock_irqsave(&ufb->lock, flags);
	ufb->shown = var->yoffset;
	ufb->damage_seq++;
	if (ums9117_fb_can_refresh(ufb))
		schedule_work(&ufb->refresh_work);
	spin_unlock_irqrestore(&ufb->lock, flags);
	return 0;
}

static int ums9117_fb_blank(int blank, struct fb_info *info)
{
	struct ums9117_fb *ufb = info->par;
	unsigned long flags;
	bool do_blank = false;
	int ret = 0;

	mutex_lock(&ufb->transition_lock);
	if (blank == FB_BLANK_UNBLANK) {
		spin_lock_irqsave(&ufb->lock, flags);
		if (ufb->stopping)
			ret = -ENODEV;
		else if (ufb->state == UMS9117_FB_ERROR)
			ret = -EIO;
		else if (ufb->state == UMS9117_FB_BLANKED) {
			ufb->state = UMS9117_FB_WAKING;
			ufb->damage_seq++;
		}
		spin_unlock_irqrestore(&ufb->lock, flags);
		if (ret || ufb->state != UMS9117_FB_WAKING)
			goto out;
		ums9117_fb_prepare_irq_frame(ufb);
		mutex_lock(&ufb->panel_lock);
		ret = ums9117_fb_dcs(ufb, PANEL_SLEEP_OUT, NULL, 0);
		if (!ret)
			msleep(ufb->profile->sleep_out_ms);
		if (!ret)
			ret = ums9117_fb_dcs(ufb, PANEL_DISPLAY_ON, NULL, 0);
		if (!ret)
			ret = ums9117_fb_start_frame(ufb, false);
		mutex_unlock(&ufb->panel_lock);
		if (ret && ret != -ESHUTDOWN)
			ums9117_fb_enter_error(ufb, ret);
		else if (!ret &&
			 ufb->profile->completion == UMS9117_FB_COMPLETION_IRQ)
			ums9117_fb_arm_irq_timeout(ufb);
		else if (!ret)
			schedule_work(&ufb->poll_work);
		goto out;
	}
	spin_lock_irqsave(&ufb->lock, flags);
	if (ufb->stopping)
		ret = -ENODEV;
	else if (ufb->state == UMS9117_FB_ERROR)
		ret = -EIO;
	else if (ufb->state == UMS9117_FB_ACTIVE) {
		ufb->state = UMS9117_FB_BLANKING;
		ufb->stats.blank_count++;
		do_blank = true;
	}
	spin_unlock_irqrestore(&ufb->lock, flags);
	if (!do_blank)
		goto out;
	ret = ums9117_fb_quiesce(ufb);
	if (!ret) {
		mutex_lock(&ufb->panel_lock);
		ret = ums9117_fb_fail_dark(ufb);
		if (!ret)
			ret = ums9117_fb_dcs(ufb, PANEL_SLEEP_IN, NULL, 0);
		if (!ret)
			msleep(ufb->profile->sleep_in_ms);
		mutex_unlock(&ufb->panel_lock);
	}
	if (ret) {
		ums9117_fb_enter_error(ufb, ret);
	} else {
		spin_lock_irqsave(&ufb->lock, flags);
		ufb->state = UMS9117_FB_BLANKED;
		ufb->stats.blank_completed++;
		spin_unlock_irqrestore(&ufb->lock, flags);
	}
out:
	mutex_unlock(&ufb->transition_lock);
	return ret == -ESHUTDOWN ? -ENODEV : ret;
}

FB_GEN_DEFAULT_DEFERRED_IOMEM_OPS(ums9117, ums9117_fb_damage_range,
				  ums9117_fb_damage_area)

static void ums9117_fb_destroy(struct fb_info *info)
{
	struct ums9117_fb *ufb = info->par;

	fb_dealloc_cmap(&info->cmap);
	kvfree(ufb->snapshot);
	iounmap(ufb->screen);
	mutex_lock(&ums9117_fb_lifetime_lock);
	if (ums9117_fb_retired_info == info)
		ums9117_fb_retired_info = NULL;
	mutex_unlock(&ums9117_fb_lifetime_lock);
	framebuffer_release(info);
}

static int ums9117_fb_open(struct fb_info *info, int user)
{
	struct ums9117_fb *ufb = info->par;
	unsigned long flags;
	int ret;

	(void)user;
	spin_lock_irqsave(&ufb->lock, flags);
	ret = ufb->stopping ? -ENODEV : 0;
	spin_unlock_irqrestore(&ufb->lock, flags);
	return ret;
}

static const struct fb_ops ums9117_fb_ops = {
	.owner = THIS_MODULE,
	__FB_DEFAULT_DEFERRED_OPS_RDWR(ums9117),
	__FB_DEFAULT_DEFERRED_OPS_DRAW(ums9117),
	__FB_DEFAULT_IOMEM_OPS_MMAP,
	.fb_open = ums9117_fb_open,
	.fb_setcolreg = ums9117_fb_setcolreg,
	.fb_check_var = ums9117_fb_check_var,
	.fb_pan_display = ums9117_fb_pan_display,
	.fb_blank = ums9117_fb_blank,
	.fb_destroy = ums9117_fb_destroy,
};

static ssize_t audit_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	struct ums9117_fb *ufb = dev_get_drvdata(dev);
	struct ums9117_fb_stats stats;
	unsigned long flags;
	bool damage_pending;
	ssize_t len;

	(void)attr;
	if (!ufb)
		return -ENODEV;
	spin_lock_irqsave(&ufb->lock, flags);
	stats = ufb->stats;
	damage_pending = ums9117_fb_damage_pending(ufb);
	len = sysfs_emit(
		buf,
		"init_mode=cold-reset\n"
		"completion_mode=%s\n"
		"timeout_mode=finite-to-error\n"
		"damage_mode=full-frame-coalesced\n"
		"pan_mode=fb-pan-notify\n"
		"lifecycle_mode=wled+dcs-display+sleep\n"
		"profile=%s\n"
		"transport=%s\n"
		"panel_state=%u\n"
		"in_flight=%u\n"
		"damage_pending=%u\n"
		"shown_yoffset=%u\n"
		"wled_state=%s\n"
		"adi_poisoned=%u\n"
		"transport_faulted=%u\n"
		"frames_started=%llu\n"
		"frames_done_irq=%llu\n"
		"frames_done_poll=%llu\n"
		"frame_timeouts=%llu\n"
		"irq_spurious=%llu\n"
		"irq_missed=%llu\n"
		"blank_count=%llu\n"
		"blank_completed=%llu\n"
		"wake_count=%llu\n"
		"dcs_errors=%llu\n"
		"dcs_timeouts=%llu\n"
		"wled_errors=%llu\n"
		"fail_dark_failures=%llu\n"
		"last_error_errno=%d\n"
		"last_error_dcs_command=0x%02x\n"
		"last_error_irq_status=0x%08x\n"
		"last_error_irq_raw=0x%08x\n",
		ufb->profile->completion == UMS9117_FB_COMPLETION_IRQ ?
			"irq" :
			"poll-raw-done",
		ufb->profile->name,
		ufb->profile->transport == UMS9117_FB_TRANSPORT_SPI1_3WIRE ?
			"spi1-3wire" :
			"lcm-dbi",
		ufb->state, ufb->in_flight ? 1U : 0U, damage_pending ? 1U : 0U,
		ufb->shown,
		!ufb->wled_known ? "unknown" :
		ufb->wled_on	 ? "on" :
				   "off",
		ums9117_adi_is_poisoned() ? 1U : 0U,
		ufb->transport_faulted ? 1U : 0U, stats.frames_started,
		stats.frames_done_irq, stats.frames_done_poll,
		stats.frame_timeouts, stats.irq_spurious, stats.irq_missed,
		stats.blank_count, stats.blank_completed, stats.wake_count,
		stats.dcs_errors, stats.dcs_timeouts, stats.wled_errors,
		stats.fail_dark_failures, ufb->last_error_errno,
		ufb->last_dcs_command, stats.last_error_irq_status,
		stats.last_error_irq_raw);
	spin_unlock_irqrestore(&ufb->lock, flags);
	return len;
}
static DEVICE_ATTR_RO(audit);

static void __iomem *ums9117_fb_ioremap_shared(struct platform_device *pdev,
					       const char *name)
{
	struct resource *resource;

	resource = platform_get_resource_byname(pdev, IORESOURCE_MEM, name);
	if (!resource || resource_size(resource) < sizeof(u32))
		return IOMEM_ERR_PTR(-EINVAL);
	return devm_ioremap(&pdev->dev, resource->start,
			    resource_size(resource));
}

static int ums9117_fb_configure_pins(struct ums9117_fb *ufb,
				     struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct resource *mux_resource;
	struct resource *conf_resource;
	u32 *mux_values;
	u32 *mux_offsets;
	u32 *conf_values;
	u32 *conf_offsets;
	int mux_count;
	int conf_count;
	unsigned int i;

	mux_count = of_property_count_u32_elems(np, "sprd,pinmux-values");
	conf_count = of_property_count_u32_elems(np, "sprd,pinconf-values");
	if (mux_count <= 0 || conf_count <= 0 || mux_count != conf_count ||
	    mux_count !=
		    of_property_count_u32_elems(np, "sprd,pinmux-offsets") ||
	    conf_count !=
		    of_property_count_u32_elems(np, "sprd,pinconf-offsets"))
		return -EINVAL;
	mux_values = devm_kmalloc_array(&pdev->dev, mux_count,
					sizeof(*mux_values), GFP_KERNEL);
	mux_offsets = devm_kmalloc_array(&pdev->dev, mux_count,
					 sizeof(*mux_offsets), GFP_KERNEL);
	conf_values = devm_kmalloc_array(&pdev->dev, conf_count,
					 sizeof(*conf_values), GFP_KERNEL);
	conf_offsets = devm_kmalloc_array(&pdev->dev, conf_count,
					  sizeof(*conf_offsets), GFP_KERNEL);
	if (!mux_values || !mux_offsets || !conf_values || !conf_offsets ||
	    of_property_read_u32_array(np, "sprd,pinmux-values", mux_values,
				       mux_count) ||
	    of_property_read_u32_array(np, "sprd,pinmux-offsets", mux_offsets,
				       mux_count) ||
	    of_property_read_u32_array(np, "sprd,pinconf-values", conf_values,
				       conf_count) ||
	    of_property_read_u32_array(np, "sprd,pinconf-offsets", conf_offsets,
				       conf_count))
		return -EINVAL;
	ufb->pinmux = devm_platform_ioremap_resource_byname(pdev, "pinmux");
	if (IS_ERR(ufb->pinmux))
		return PTR_ERR(ufb->pinmux);
	ufb->pinconf = devm_platform_ioremap_resource_byname(pdev, "pinconf");
	if (IS_ERR(ufb->pinconf))
		return PTR_ERR(ufb->pinconf);
	mux_resource =
		platform_get_resource_byname(pdev, IORESOURCE_MEM, "pinmux");
	conf_resource =
		platform_get_resource_byname(pdev, IORESOURCE_MEM, "pinconf");
	if (!mux_resource || !conf_resource ||
	    resource_size(mux_resource) < sizeof(u32) ||
	    resource_size(conf_resource) < sizeof(u32))
		return -EINVAL;
	for (i = 0; i < mux_count; i++) {
		if (!IS_ALIGNED(mux_offsets[i], sizeof(u32)) ||
		    !IS_ALIGNED(conf_offsets[i], sizeof(u32)) ||
		    mux_offsets[i] >
			    resource_size(mux_resource) - sizeof(u32) ||
		    conf_offsets[i] >
			    resource_size(conf_resource) - sizeof(u32))
			return -EINVAL;
		writel(mux_values[i], ufb->pinmux + mux_offsets[i]);
		writel(conf_values[i], ufb->pinconf + conf_offsets[i]);
	}
	readl(ufb->pinconf + conf_offsets[conf_count - 1]);
	ufb->pinmux_count = mux_count;
	ufb->pinconf_count = conf_count;
	return 0;
}

static int ums9117_fb_reset_panel(struct ums9117_fb *ufb)
{
	int ret;

	ret = regmap_write(ufb->aon_apb, AON_PANEL_RESET_SET, BIT(0));
	if (ret)
		return ret;
	msleep(ufb->profile->reset_phase_ms);
	ret = regmap_write(ufb->aon_apb, AON_PANEL_RESET_CLEAR, BIT(0));
	if (ret)
		return ret;
	msleep(ufb->profile->reset_phase_ms);
	ret = regmap_write(ufb->aon_apb, AON_PANEL_RESET_SET, BIT(0));
	if (ret)
		return ret;
	msleep(ufb->profile->reset_phase_ms);
	msleep(ufb->profile->reset_release_ms);
	return 0;
}

static int ums9117_fb_cold_init(struct ums9117_fb *ufb)
{
	unsigned long flags;
	int ret;

	memset_io(ufb->screen, 0, 2 * ums9117_fb_size(ufb));
	memset_io(ufb->transfer, 0, ums9117_fb_size(ufb));
	memset(ufb->snapshot, 0, ums9117_fb_size(ufb));
	/* Flush the cleared framebuffers before panel init scans them out. */
	wmb();
	ret = ums9117_fb_wled_off_bounded(ufb);
	if (ret)
		return ret;
	ret = ums9117_fb_reset_panel(ufb);
	if (!ret)
		ret = ums9117_fb_transport_post_reset(ufb);
	if (!ret)
		ret = ums9117_fb_run_init(ufb);
	if (!ret)
		ums9117_fb_prepare_irq_frame(ufb);
	if (!ret)
		ret = ums9117_fb_start_frame(ufb, true);
	if (!ret && ufb->profile->completion == UMS9117_FB_COMPLETION_POLL)
		ret = ums9117_fb_poll_frame_done(ufb);
	if (!ret && ufb->profile->completion == UMS9117_FB_COMPLETION_IRQ &&
	    !wait_for_completion_timeout(&ufb->frame_done,
					 msecs_to_jiffies(FRAME_TIMEOUT_MS)))
		ret = -ETIMEDOUT;
	if (!ret)
		ret = ums9117_fb_wled_set(ufb, true);
	if (ret) {
		ums9117_fb_enter_error(ufb, ret);
		return ret;
	}
	spin_lock_irqsave(&ufb->lock, flags);
	ufb->state = UMS9117_FB_ACTIVE;
	spin_unlock_irqrestore(&ufb->lock, flags);
	return 0;
}

static int ums9117_fb_map_common_resources(struct ums9117_fb *ufb,
					   struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct resource *fbres;
	struct resource *adires;
	struct resource *analogres;
	int ret;

	fbres = platform_get_resource_byname(pdev, IORESOURCE_MEM,
					     "framebuffer");
	if (!fbres || resource_size(fbres) < 3 * ums9117_fb_size(ufb))
		return -EINVAL;
	ufb->screen_phys = fbres->start;
	ufb->transfer_phys = ufb->screen_phys + 2 * ums9117_fb_size(ufb);
	ufb->screen = ioremap_wc(fbres->start, resource_size(fbres));
	if (!ufb->screen)
		return -ENOMEM;
	ufb->transfer = ufb->screen + 2 * ums9117_fb_size(ufb);
	ufb->lcdc = devm_platform_ioremap_resource_byname(pdev, "lcdc");
	if (IS_ERR(ufb->lcdc))
		return PTR_ERR(ufb->lcdc);
	ufb->ap_ahb_gate_set =
		ums9117_fb_ioremap_shared(pdev, "ap-ahb-gate-set");
	ufb->ap_ahb_reset_set =
		ums9117_fb_ioremap_shared(pdev, "ap-ahb-reset-set");
	ufb->ap_ahb_reset_clear =
		ums9117_fb_ioremap_shared(pdev, "ap-ahb-reset-clear");
	if (IS_ERR(ufb->ap_ahb_gate_set) || IS_ERR(ufb->ap_ahb_reset_set) ||
	    IS_ERR(ufb->ap_ahb_reset_clear))
		return -EINVAL;
	ufb->aon_apb =
		syscon_regmap_lookup_by_phandle(dev->of_node, "sprd,aon-apb");
	if (IS_ERR(ufb->aon_apb))
		return PTR_ERR(ufb->aon_apb);
	adires = platform_get_resource_byname(pdev, IORESOURCE_MEM,
					      "adi-controller");
	analogres = platform_get_resource_byname(pdev, IORESOURCE_MEM,
						 "analog-slave");
	if (!adires || !analogres || adires->start != UMS9117_ADI_PHYS ||
	    analogres->start != UMS9117_ANALOG_PHYS ||
	    resource_size(adires) < ADI_CONTROLLER_MIN_SIZE ||
	    resource_size(analogres) < ANA_LDO_PD_CTRL + sizeof(u32))
		return -EINVAL;
	ret = of_property_read_u32_array(dev->of_node,
					 "sprd,wled-current-levels",
					 ufb->wled_levels,
					 ARRAY_SIZE(ufb->wled_levels));
	if (ret)
		return ret;
	for (ret = 0; ret < ARRAY_SIZE(ufb->wled_levels); ret++)
		if (ufb->wled_levels[ret] > BLTC_CURRENT_MASK)
			return -EINVAL;
	return ums9117_fb_configure_pins(ufb, pdev);
}

int ums9117_fb_transport_init(struct ums9117_fb *ufb,
			      struct platform_device *pdev)
{
	if (ufb->profile->transport == UMS9117_FB_TRANSPORT_SPI1_3WIRE)
		return ums9117_fb_spi_init_transport(ufb, pdev);
	return ums9117_fb_lcm_init_transport(ufb, pdev);
}

int ums9117_fb_transport_enable(struct ums9117_fb *ufb)
{
	if (ufb->profile->transport == UMS9117_FB_TRANSPORT_SPI1_3WIRE)
		return ums9117_fb_spi_enable_transport(ufb);
	return ums9117_fb_lcm_enable_transport(ufb);
}

int ums9117_fb_transport_post_reset(struct ums9117_fb *ufb)
{
	if (ufb->profile->transport == UMS9117_FB_TRANSPORT_SPI1_3WIRE)
		return ums9117_fb_spi_post_reset(ufb);
	return ums9117_fb_lcm_post_reset(ufb);
}

static void ums9117_fb_retire(struct fb_info *info)
{
	mutex_lock(&ums9117_fb_lifetime_lock);
	ums9117_fb_retired_info = info;
	mutex_unlock(&ums9117_fb_lifetime_lock);
	unregister_framebuffer(info);
}

int ums9117_fb_probe(struct platform_device *pdev,
		     const struct ums9117_fb_profile *profile)
{
	struct device *dev = &pdev->dev;
	struct fb_info *info;
	struct ums9117_fb *ufb;
	int ret;

	if (!profile || !profile->name || !profile->width || !profile->height ||
	    !profile->init || !profile->init_count ||
	    profile->completion > UMS9117_FB_COMPLETION_POLL)
		return -EINVAL;
	mutex_lock(&ums9117_fb_lifetime_lock);
	ret = ums9117_fb_retired_info ? -EBUSY : 0;
	mutex_unlock(&ums9117_fb_lifetime_lock);
	if (ret)
		return dev_err_probe(dev, ret,
				     "previous framebuffer is still in use\n");
	info = framebuffer_alloc(sizeof(*ufb), dev);
	if (!info)
		return -ENOMEM;
	ufb = info->par;
	ufb->info = info;
	ufb->profile = profile;
	ufb->state = UMS9117_FB_COLD_INIT;
	spin_lock_init(&ufb->lock);
	mutex_init(&ufb->transition_lock);
	mutex_init(&ufb->panel_lock);
	init_completion(&ufb->frame_done);
	INIT_WORK(&ufb->refresh_work, ums9117_fb_refresh_work);
	INIT_WORK(&ufb->wake_work, ums9117_fb_wake_work);
	INIT_WORK(&ufb->poll_work, ums9117_fb_poll_work);
	INIT_DELAYED_WORK(&ufb->timeout_work, ums9117_fb_timeout_work);
	ufb->snapshot = kvmalloc(ums9117_fb_size(ufb), GFP_KERNEL);
	if (!ufb->snapshot) {
		ret = -ENOMEM;
		goto release;
	}
	ret = ums9117_fb_map_common_resources(ufb, pdev);
	if (ret)
		goto release;
	ret = ums9117_fb_transport_init(ufb, pdev);
	if (ret)
		goto release;
	ufb->irq = -1;
	if (profile->completion == UMS9117_FB_COMPLETION_IRQ) {
		ufb->irq = platform_get_irq_optional(pdev, 0);
		if (ufb->irq < 0) {
			ret = dev_err_probe(dev, ufb->irq,
					    "LCDC_DONE IRQ is required\n");
			goto release;
		}
	}
	writel(AP_AHB_LCDC_GATE | AP_AHB_LCM_GATE, ufb->ap_ahb_gate_set);
	usleep_range(1000, 2000);
	ret = ums9117_fb_transport_enable(ufb);
	if (ret)
		goto release;
	writel(AP_AHB_LCDC_RESET, ufb->ap_ahb_reset_set);
	usleep_range(10000, 11000);
	writel(AP_AHB_LCDC_RESET, ufb->ap_ahb_reset_clear);
	ums9117_fb_stop_lcdc(ufb);

	strscpy(info->fix.id, profile->name, sizeof(info->fix.id));
	info->fix.type = FB_TYPE_PACKED_PIXELS;
	info->fix.visual = FB_VISUAL_TRUECOLOR;
	info->fix.accel = FB_ACCEL_NONE;
	info->fix.ypanstep = profile->height;
	info->fix.smem_start = ufb->screen_phys;
	info->fix.smem_len = 2 * ums9117_fb_size(ufb);
	info->fix.line_length = ums9117_fb_stride(ufb);
	info->var.xres = profile->width;
	info->var.yres = profile->height;
	info->var.xres_virtual = profile->width;
	info->var.yres_virtual = 2 * profile->height;
	info->var.bits_per_pixel = 16;
	info->var.red.offset = 11;
	info->var.red.length = 5;
	info->var.green.offset = 5;
	info->var.green.length = 6;
	info->var.blue.length = 5;
	info->var.activate = FB_ACTIVATE_NOW;
	info->var.vmode = FB_VMODE_NONINTERLACED;
	info->fbops = &ums9117_fb_ops;
	info->screen_base = ufb->screen;
	info->screen_size = 2 * ums9117_fb_size(ufb);
	info->pseudo_palette = ufb->pseudo_palette;
	ret = fb_alloc_cmap(&info->cmap, 16, 0);
	if (ret)
		goto release;
	if (profile->completion == UMS9117_FB_COMPLETION_IRQ) {
		ret = devm_request_irq(dev, ufb->irq, ums9117_fb_lcdc_irq, 0,
				       dev_name(dev), ufb);
		if (ret)
			goto cmap;
	}
	platform_set_drvdata(pdev, ufb);
	ret = ums9117_fb_cold_init(ufb);
	if (ret)
		goto stop;
	ret = register_framebuffer(info);
	if (ret)
		goto stop;
	/* fbcon may have drawn while registering; submit that first visible state. */
	ums9117_fb_mark_damage(ufb);
	ret = device_create_file(dev, &dev_attr_audit);
	if (ret)
		goto unregister;
	ufb->audit_file_created = true;
	dev_info(dev, "%s framebuffer registered as fb%d\n", profile->name,
		 info->node);
	return 0;
unregister:
	ums9117_fb_shutdown(pdev);
	platform_set_drvdata(pdev, NULL);
	console_lock();
	ums9117_fb_retire(info);
	console_unlock();
	return ret;
stop:
	ums9117_fb_enter_error(ufb, ret);
	if (profile->completion == UMS9117_FB_COMPLETION_IRQ)
		devm_free_irq(dev, ufb->irq, ufb);
cmap:
	fb_dealloc_cmap(&info->cmap);
release:
	if (ufb->screen)
		iounmap(ufb->screen);
	kvfree(ufb->snapshot);
	framebuffer_release(info);
	return ret;
}
EXPORT_SYMBOL_GPL(ums9117_fb_probe);

void ums9117_fb_remove(struct platform_device *pdev)
{
	struct ums9117_fb *ufb = platform_get_drvdata(pdev);
	struct fb_info *info;
	unsigned long flags;
	int cleanup_ret;
	int quiesce_ret;

	if (!ufb)
		return;
	info = ufb->info;
	mutex_lock(&ufb->transition_lock);
	spin_lock_irqsave(&ufb->lock, flags);
	ufb->stopping = true;
	spin_unlock_irqrestore(&ufb->lock, flags);
	mutex_unlock(&ufb->transition_lock);
	if (ufb->audit_file_created) {
		device_remove_file(&pdev->dev, &dev_attr_audit);
		ufb->audit_file_created = false;
	}
	cancel_work_sync(&ufb->wake_work);
	console_lock();
	mutex_lock(&ufb->transition_lock);
	quiesce_ret = ums9117_fb_quiesce(ufb);
	cleanup_ret = 0;
	mutex_lock(&ufb->panel_lock);
	cleanup_ret = ums9117_fb_fail_dark(ufb);
	mutex_unlock(&ufb->panel_lock);
	mutex_unlock(&ufb->transition_lock);
	if (quiesce_ret)
		dev_err(&pdev->dev,
			"could not quiesce display during remove: %d\n",
			quiesce_ret);
	if (cleanup_ret)
		dev_err(&pdev->dev,
			"could not fail display dark during remove: %d\n",
			cleanup_ret);
	if (ufb->profile->completion == UMS9117_FB_COMPLETION_IRQ)
		devm_free_irq(&pdev->dev, ufb->irq, ufb);
	platform_set_drvdata(pdev, NULL);
	ums9117_fb_retire(info);
	console_unlock();
}
EXPORT_SYMBOL_GPL(ums9117_fb_remove);

void ums9117_fb_shutdown(struct platform_device *pdev)
{
	struct ums9117_fb *ufb = platform_get_drvdata(pdev);
	unsigned long flags;

	if (!ufb)
		return;
	mutex_lock(&ufb->transition_lock);
	spin_lock_irqsave(&ufb->lock, flags);
	ufb->stopping = true;
	spin_unlock_irqrestore(&ufb->lock, flags);
	mutex_unlock(&ufb->transition_lock);
	cancel_work_sync(&ufb->wake_work);
	mutex_lock(&ufb->transition_lock);
	ums9117_fb_quiesce(ufb);
	mutex_lock(&ufb->panel_lock);
	ums9117_fb_fail_dark(ufb);
	mutex_unlock(&ufb->panel_lock);
	mutex_unlock(&ufb->transition_lock);
}
EXPORT_SYMBOL_GPL(ums9117_fb_shutdown);

MODULE_DESCRIPTION("UMS9117 shared fbdev/LCDC core");
MODULE_LICENSE("GPL");
