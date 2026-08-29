// SPDX-License-Identifier: GPL-2.0-only
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/jiffies.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/pm_wakeirq.h>
#include <linux/pm_wakeup.h>
#include <linux/rtc.h>
#include <linux/soc/sprd/ums9117-adi.h>

#define SC2720_CHIP_ID_LOW 0xc00U
#define SC2720_CHIP_ID_HIGH 0xc04U
#define SC2720_MODULE_EN0 0xc08U
#define SC2720_RTC_CLK_EN0 0xc10U
#define SC2720_SOFT_RST0 0xc14U

#define SC2720_RTC_SECONDS 0x200U
#define SC2720_RTC_MINUTES 0x204U
#define SC2720_RTC_HOURS 0x208U
#define SC2720_RTC_DAYS 0x20cU
#define SC2720_RTC_ALARM_SECONDS 0x220U
#define SC2720_RTC_ALARM_MINUTES 0x224U
#define SC2720_RTC_ALARM_HOURS 0x228U
#define SC2720_RTC_ALARM_DAYS 0x22cU
#define SC2720_RTC_INT_EN 0x230U
#define SC2720_RTC_RSTS 0x234U
#define SC2720_RTC_CLR 0x238U
#define SC2720_RTC_MSK 0x23cU
#define SC2720_RTC_SPG_VALUE 0x250U
#define SC2720_RTC_SPG_UPD 0x254U

#define SC2720_EXPECTED_ID_LOW 0xa003U
#define SC2720_EXPECTED_ID_HIGH 0x2720U
#define SC2720_RTC_GATE BIT(1)
#define SC2720_RTC_SECONDS_MASK GENMASK(5, 0)
#define SC2720_RTC_MINUTES_MASK GENMASK(5, 0)
#define SC2720_RTC_HOURS_MASK GENMASK(4, 0)
#define SC2720_RTC_ALARM_EVENT BIT(4)
#define SC2720_RTC_SPG_ACK BIT(7)
#define SC2720_RTC_ALARM_ACK_MASK GENMASK(15, 12)
#define SC2720_RTC_RUNTIME_INT_MASK (GENMASK(4, 0) | GENMASK(15, 8))
#define SC2720_RTC_SPG_LOW_MASK GENMASK(7, 0)
#define SC2720_RTC_ALARM_UNLOCK 0xa5U

#define TA1618_RTC_EPOCH 315532800LL
#define TA1618_RTC_MAX_DAYS 43829U
#define TA1618_RTC_MAX_ATTEMPTS 3U
#define TA1618_RTC_ACK_TIMEOUT_MS 250U
#define TA1618_RTC_ACK_POLL_MIN_US 5000U
#define TA1618_RTC_ACK_POLL_MAX_US 10000U

struct ta1618_rtc_sample {
	u16 chip_id_low;
	u16 chip_id_high;
	u16 module_en0;
	u16 rtc_clk_en0;
	u16 soft_rst0;
	u16 seconds_before;
	u16 minutes;
	u16 hours;
	u16 days;
	u16 seconds_after;
};

struct ta1618_rtc_alarm_state {
	u16 int_en;
	u16 rsts;
	u16 msk;
	u16 spg_value;
	u16 spg_upd;
};

struct ta1618_rtc {
	struct device *dev;
	struct rtc_device *rtc;
	struct mutex lock;
	u16 initial_int_en;
	u8 initial_spg_low;
	int irq;
	bool irq_enabled;
	bool suspend_disabled;
	bool state_may_have_changed;
	bool spg_may_have_changed;
	bool armed;
	bool failed;
};

static void ta1618_rtc_record_error(int *ret, int step_ret)
{
	if (!*ret && step_ret)
		*ret = step_ret;
}

static int ta1618_rtc_adi_read(u32 offset, u16 *value)
{
	struct ums9117_adi_transaction transaction = {};
	int end_ret;
	int ret;

	ret = ums9117_adi_begin(&transaction);
	if (ret)
		return ret;
	ret = ums9117_adi_read(&transaction, offset, value);
	end_ret = ums9117_adi_end(&transaction);
	return ret ? ret : end_ret;
}

static int ta1618_rtc_adi_write(u32 offset, u16 value)
{
	struct ums9117_adi_transaction transaction = {};
	int end_ret;
	int ret;

	ret = ums9117_adi_begin(&transaction);
	if (ret)
		return ret;
	ret = ums9117_adi_write(&transaction, offset, value);
	end_ret = ums9117_adi_end(&transaction);
	return ret ? ret : end_ret;
}

static int ta1618_rtc_adi_update(u32 offset, u16 mask, u16 value)
{
	struct ums9117_adi_transaction transaction = {};
	int end_ret;
	int ret;

	ret = ums9117_adi_begin(&transaction);
	if (ret)
		return ret;
	ret = ums9117_adi_update_bits(&transaction, offset, mask, value);
	end_ret = ums9117_adi_end(&transaction);
	return ret ? ret : end_ret;
}

static int ta1618_rtc_adi_command(u32 offset, u16 value)
{
	struct ums9117_adi_transaction transaction = {};
	int end_ret;
	int ret;

	ret = ums9117_adi_begin(&transaction);
	if (ret)
		return ret;
	ret = ums9117_adi_write_final(&transaction, offset, value);
	end_ret = ums9117_adi_end(&transaction);
	return ret ? ret : end_ret;
}

static int ta1618_rtc_read_if_ok(struct ums9117_adi_transaction *transaction,
				 int ret, u32 offset, u16 *value)
{
	if (ret)
		return ret;

	return ums9117_adi_read(transaction, offset, value);
}

static int ta1618_rtc_validate_sample(const struct ta1618_rtc_sample *sample)
{
	if (sample->chip_id_low != SC2720_EXPECTED_ID_LOW ||
	    sample->chip_id_high != SC2720_EXPECTED_ID_HIGH)
		return -ENODEV;
	if (!(sample->module_en0 & SC2720_RTC_GATE) ||
	    !(sample->rtc_clk_en0 & SC2720_RTC_GATE) ||
	    (sample->soft_rst0 & SC2720_RTC_GATE))
		return -EHOSTDOWN;
	if (sample->seconds_before & ~SC2720_RTC_SECONDS_MASK ||
	    sample->seconds_after & ~SC2720_RTC_SECONDS_MASK ||
	    sample->minutes & ~SC2720_RTC_MINUTES_MASK ||
	    sample->hours & ~SC2720_RTC_HOURS_MASK)
		return -EPROTO;
	if (sample->seconds_before > 59 || sample->seconds_after > 59 ||
	    sample->minutes > 59 || sample->hours > 23 ||
	    sample->days > TA1618_RTC_MAX_DAYS)
		return -ERANGE;

	return 0;
}

static int ta1618_rtc_read_once(struct ta1618_rtc_sample *sample)
{
	struct ums9117_adi_transaction transaction = {};
	int end_ret;
	int ret;

	ret = ums9117_adi_begin(&transaction);
	if (ret)
		return ret;

	ret = ums9117_adi_read(&transaction, SC2720_CHIP_ID_LOW,
			       &sample->chip_id_low);
	ret = ta1618_rtc_read_if_ok(&transaction, ret, SC2720_CHIP_ID_HIGH,
				    &sample->chip_id_high);
	ret = ta1618_rtc_read_if_ok(&transaction, ret, SC2720_MODULE_EN0,
				    &sample->module_en0);
	ret = ta1618_rtc_read_if_ok(&transaction, ret, SC2720_RTC_CLK_EN0,
				    &sample->rtc_clk_en0);
	ret = ta1618_rtc_read_if_ok(&transaction, ret, SC2720_SOFT_RST0,
				    &sample->soft_rst0);
	ret = ta1618_rtc_read_if_ok(&transaction, ret, SC2720_RTC_SECONDS,
				    &sample->seconds_before);
	ret = ta1618_rtc_read_if_ok(&transaction, ret, SC2720_RTC_MINUTES,
				    &sample->minutes);
	ret = ta1618_rtc_read_if_ok(&transaction, ret, SC2720_RTC_HOURS,
				    &sample->hours);
	ret = ta1618_rtc_read_if_ok(&transaction, ret, SC2720_RTC_DAYS,
				    &sample->days);
	ret = ta1618_rtc_read_if_ok(&transaction, ret, SC2720_RTC_SECONDS,
				    &sample->seconds_after);

	end_ret = ums9117_adi_end(&transaction);
	if (ret)
		return ret;
	if (end_ret)
		return end_ret;

	return ta1618_rtc_validate_sample(sample);
}

static int ta1618_rtc_read_time(struct device *dev, struct rtc_time *time)
{
	struct ta1618_rtc_sample sample;
	s64 seconds;
	unsigned int attempt;
	int ret;

	(void)dev;
	for (attempt = 0; attempt < TA1618_RTC_MAX_ATTEMPTS; attempt++) {
		ret = ta1618_rtc_read_once(&sample);
		if (ret)
			return ret;
		if (sample.seconds_before != sample.seconds_after)
			continue;

		seconds = TA1618_RTC_EPOCH;
		seconds += (s64)sample.days * 86400;
		seconds += (s64)sample.hours * 3600;
		seconds += (s64)sample.minutes * 60;
		seconds += sample.seconds_before;
		rtc_time64_to_tm(seconds, time);
		return rtc_valid_tm(time);
	}

	return -EAGAIN;
}

static int ta1618_rtc_read_alarm_state(struct ta1618_rtc_alarm_state *state)
{
	int ret;

	ret = ta1618_rtc_adi_read(SC2720_RTC_INT_EN, &state->int_en);
	if (ret)
		return ret;
	ret = ta1618_rtc_adi_read(SC2720_RTC_RSTS, &state->rsts);
	if (ret)
		return ret;
	ret = ta1618_rtc_adi_read(SC2720_RTC_MSK, &state->msk);
	if (ret)
		return ret;
	ret = ta1618_rtc_adi_read(SC2720_RTC_SPG_VALUE, &state->spg_value);
	if (ret)
		return ret;
	return ta1618_rtc_adi_read(SC2720_RTC_SPG_UPD, &state->spg_upd);
}

static int ta1618_rtc_validate_alarm_baseline(struct ta1618_rtc *rtc,
					      bool initial)
{
	struct ta1618_rtc_alarm_state state;
	u16 pending_mask = SC2720_RTC_ALARM_EVENT | SC2720_RTC_ALARM_ACK_MASK |
			   SC2720_RTC_SPG_ACK;
	int ret;

	ret = ta1618_rtc_read_alarm_state(&state);
	if (ret)
		return ret;
	if ((state.int_en & SC2720_RTC_ALARM_EVENT) ||
	    (state.rsts & pending_mask) || (state.msk & pending_mask) ||
	    (state.spg_value & SC2720_RTC_SPG_LOW_MASK) ==
		    SC2720_RTC_ALARM_UNLOCK ||
	    (state.spg_upd & SC2720_RTC_SPG_LOW_MASK) ==
		    SC2720_RTC_ALARM_UNLOCK)
		return -EBUSY;
	if (initial) {
		rtc->initial_int_en = state.int_en;
		rtc->initial_spg_low = state.spg_value &
				       SC2720_RTC_SPG_LOW_MASK;
	} else if (state.int_en != rtc->initial_int_en ||
		   (state.spg_value & SC2720_RTC_SPG_LOW_MASK) !=
			   rtc->initial_spg_low) {
		return -EPROTO;
	}

	return 0;
}

static int ta1618_rtc_wait_status(u16 mask, u16 expected)
{
	unsigned long deadline =
		jiffies + msecs_to_jiffies(TA1618_RTC_ACK_TIMEOUT_MS);
	u16 status;
	int ret;

	do {
		ret = ta1618_rtc_adi_read(SC2720_RTC_RSTS, &status);
		if (ret)
			return ret;
		if ((status & mask) == expected)
			return 0;
		usleep_range(TA1618_RTC_ACK_POLL_MIN_US,
			     TA1618_RTC_ACK_POLL_MAX_US);
	} while (time_before(jiffies, deadline));

	return -ETIMEDOUT;
}

static int ta1618_rtc_clear_status(u16 mask)
{
	int ret;

	ret = ta1618_rtc_adi_command(SC2720_RTC_CLR, mask);
	if (ret)
		return ret;
	return ta1618_rtc_wait_status(mask, 0);
}

static int ta1618_rtc_update_spg(struct ta1618_rtc *rtc, u8 low)
{
	u16 value;
	int ret;

	ret = ta1618_rtc_wait_status(SC2720_RTC_SPG_ACK, 0);
	if (ret)
		return ret;
	ret = ta1618_rtc_adi_read(SC2720_RTC_SPG_VALUE, &value);
	if (ret)
		return ret;
	value = (value & ~SC2720_RTC_SPG_LOW_MASK) | low;
	rtc->spg_may_have_changed = true;
	ret = ta1618_rtc_adi_write(SC2720_RTC_SPG_UPD, value);
	if (ret)
		return ret;
	ret = ta1618_rtc_wait_status(SC2720_RTC_SPG_ACK, SC2720_RTC_SPG_ACK);
	if (ret)
		return ret;
	ret = ta1618_rtc_clear_status(SC2720_RTC_SPG_ACK);
	if (ret)
		return ret;
	ret = ta1618_rtc_adi_read(SC2720_RTC_SPG_VALUE, &value);
	if (ret)
		return ret;
	return (value & SC2720_RTC_SPG_LOW_MASK) == low ? 0 : -EIO;
}

static int ta1618_rtc_alarm_fields(time64_t absolute, u16 *seconds,
				   u16 *minutes, u16 *hours, u16 *days)
{
	s64 day_count;
	s64 relative;
	int remainder;

	if (absolute < TA1618_RTC_EPOCH)
		return -ERANGE;
	relative = absolute - TA1618_RTC_EPOCH;
	day_count = div_s64_rem(relative, 86400, &remainder);
	if (day_count > TA1618_RTC_MAX_DAYS)
		return -ERANGE;
	*days = (u16)day_count;
	*hours = remainder / 3600;
	remainder -= *hours * 3600;
	*minutes = remainder / 60;
	*seconds = remainder - *minutes * 60;
	return 0;
}

static int ta1618_rtc_disarm_locked(struct ta1618_rtc *rtc)
{
	u16 safe_int_en = rtc->initial_int_en & ~SC2720_RTC_RUNTIME_INT_MASK;
	u16 pending_mask = SC2720_RTC_ALARM_EVENT | SC2720_RTC_ALARM_ACK_MASK |
			   SC2720_RTC_SPG_ACK;
	struct ta1618_rtc_alarm_state state;
	int read_ret;
	int ret = 0;
	int step_ret;

	if (!rtc->state_may_have_changed && !rtc->armed && !rtc->irq_enabled &&
	    !rtc->spg_may_have_changed)
		return 0;

	step_ret = ta1618_rtc_adi_write(SC2720_RTC_INT_EN, safe_int_en);
	ta1618_rtc_record_error(&ret, step_ret);
	if (rtc->irq_enabled) {
		disable_irq_nosync(rtc->irq);
		rtc->irq_enabled = false;
	}

	read_ret = ta1618_rtc_adi_read(SC2720_RTC_RSTS, &state.rsts);
	ta1618_rtc_record_error(&ret, read_ret);
	if (!read_ret && (state.rsts & SC2720_RTC_ALARM_EVENT)) {
		step_ret = ta1618_rtc_clear_status(SC2720_RTC_ALARM_EVENT);
		ta1618_rtc_record_error(&ret, step_ret);
	}
	if (!read_ret && (state.rsts & SC2720_RTC_ALARM_ACK_MASK)) {
		step_ret = ta1618_rtc_clear_status(state.rsts &
						   SC2720_RTC_ALARM_ACK_MASK);
		ta1618_rtc_record_error(&ret, step_ret);
	}
	if (!read_ret && (state.rsts & SC2720_RTC_SPG_ACK)) {
		step_ret = ta1618_rtc_clear_status(SC2720_RTC_SPG_ACK);
		ta1618_rtc_record_error(&ret, step_ret);
	}

	if (rtc->spg_may_have_changed) {
		step_ret = ta1618_rtc_update_spg(rtc, rtc->initial_spg_low);
		ta1618_rtc_record_error(&ret, step_ret);
	}
	step_ret = ta1618_rtc_adi_write(SC2720_RTC_INT_EN, rtc->initial_int_en);
	ta1618_rtc_record_error(&ret, step_ret);
	step_ret = ta1618_rtc_read_alarm_state(&state);
	ta1618_rtc_record_error(&ret, step_ret);
	if (!step_ret &&
	    (state.int_en != rtc->initial_int_en ||
	     (state.rsts & pending_mask) || (state.msk & pending_mask) ||
	     (state.spg_value & SC2720_RTC_SPG_LOW_MASK) !=
		     rtc->initial_spg_low ||
	     (state.spg_upd & SC2720_RTC_SPG_LOW_MASK) ==
		     SC2720_RTC_ALARM_UNLOCK))
		ta1618_rtc_record_error(&ret, -EIO);
	if (!ret) {
		rtc->state_may_have_changed = false;
		rtc->armed = false;
		rtc->spg_may_have_changed = false;
	}
	return ret;
}

static int ta1618_rtc_program_alarm_locked(struct ta1618_rtc *rtc,
					   struct rtc_time *time)
{
	struct rtc_time now;
	u16 safe_int_en = rtc->initial_int_en & ~SC2720_RTC_RUNTIME_INT_MASK;
	u16 seconds;
	u16 minutes;
	u16 hours;
	u16 days;
	time64_t alarm_time;
	int cleanup_ret;
	int ret;

	if (rtc->failed)
		return -EIO;
	if (rtc->armed || rtc->irq_enabled || rtc->spg_may_have_changed) {
		ret = ta1618_rtc_disarm_locked(rtc);
		if (ret)
			goto fail;
	}
	ret = ta1618_rtc_validate_alarm_baseline(rtc, false);
	if (ret)
		return ret;
	ret = ta1618_rtc_read_time(rtc->dev, &now);
	if (ret)
		return ret;
	alarm_time = rtc_tm_to_time64(time);
	if (alarm_time <= rtc_tm_to_time64(&now))
		return -ETIME;
	ret = ta1618_rtc_alarm_fields(alarm_time, &seconds, &minutes, &hours,
				      &days);
	if (ret)
		return ret;

	rtc->state_may_have_changed = true;
	ret = ta1618_rtc_adi_write(SC2720_RTC_INT_EN, safe_int_en);
	if (ret)
		goto fail;
	ret = ta1618_rtc_wait_status(SC2720_RTC_ALARM_ACK_MASK, 0);
	if (ret)
		goto fail;
	ret = ta1618_rtc_adi_write(SC2720_RTC_ALARM_SECONDS, seconds);
	if (ret)
		goto fail;
	ret = ta1618_rtc_adi_write(SC2720_RTC_ALARM_MINUTES, minutes);
	if (ret)
		goto fail;
	ret = ta1618_rtc_adi_write(SC2720_RTC_ALARM_HOURS, hours);
	if (ret)
		goto fail;
	ret = ta1618_rtc_adi_write(SC2720_RTC_ALARM_DAYS, days);
	if (ret)
		goto fail;
	ret = ta1618_rtc_wait_status(SC2720_RTC_ALARM_ACK_MASK,
				     SC2720_RTC_ALARM_ACK_MASK);
	if (ret)
		goto fail;
	ret = ta1618_rtc_clear_status(SC2720_RTC_ALARM_ACK_MASK);
	if (ret)
		goto fail;
	ret = ta1618_rtc_update_spg(rtc, SC2720_RTC_ALARM_UNLOCK);
	if (ret)
		goto fail;

	enable_irq(rtc->irq);
	rtc->irq_enabled = true;
	ret = ta1618_rtc_adi_update(SC2720_RTC_INT_EN, SC2720_RTC_ALARM_EVENT,
				    SC2720_RTC_ALARM_EVENT);
	if (ret)
		goto fail;
	rtc->armed = true;
	return 0;

fail:
	cleanup_ret = ta1618_rtc_disarm_locked(rtc);
	rtc->failed = true;
	if (cleanup_ret)
		dev_crit(
			rtc->dev,
			"failed to restore RTC alarm state after programming error: %d\n",
			cleanup_ret);
	return ret;
}

static int ta1618_rtc_set_alarm(struct device *dev, struct rtc_wkalrm *alarm)
{
	struct ta1618_rtc *rtc = dev_get_drvdata(dev);
	int ret;

	mutex_lock(&rtc->lock);
	if (alarm->enabled) {
		ret = ta1618_rtc_program_alarm_locked(rtc, &alarm->time);
	} else {
		ret = ta1618_rtc_disarm_locked(rtc);
		if (ret)
			rtc->failed = true;
	}
	mutex_unlock(&rtc->lock);
	return ret;
}

static int ta1618_rtc_alarm_irq_enable(struct device *dev, unsigned int enabled)
{
	struct ta1618_rtc *rtc = dev_get_drvdata(dev);
	int ret;

	mutex_lock(&rtc->lock);
	if (!enabled) {
		ret = ta1618_rtc_disarm_locked(rtc);
		if (ret)
			rtc->failed = true;
	} else if (rtc->failed) {
		ret = -EIO;
	} else if (!rtc->armed || !rtc->irq_enabled) {
		ret = -EIO;
	} else {
		ret = 0;
	}
	mutex_unlock(&rtc->lock);
	return ret;
}

static irqreturn_t ta1618_rtc_alarm_thread(int irq, void *data)
{
	struct ta1618_rtc *rtc = data;
	struct ta1618_rtc_alarm_state state;
	u16 unexpected = SC2720_RTC_ALARM_ACK_MASK | SC2720_RTC_SPG_ACK;
	u16 active;
	int cleanup_ret;
	int ret;

	(void)irq;
	mutex_lock(&rtc->lock);
	if (rtc->failed || !rtc->armed) {
		ret = -EPROTO;
		goto fail;
	}
	ret = ta1618_rtc_read_alarm_state(&state);
	if (ret)
		goto fail;
	active = state.rsts & state.int_en;
	if (active != SC2720_RTC_ALARM_EVENT || (state.rsts & unexpected) ||
	    (state.msk & unexpected)) {
		ret = -EPROTO;
		goto fail;
	}
	ret = ta1618_rtc_disarm_locked(rtc);
	if (ret)
		goto fail;
	mutex_unlock(&rtc->lock);
	rtc_update_irq(rtc->rtc, 1, RTC_AF | RTC_IRQF);
	return IRQ_HANDLED;

fail:
	cleanup_ret = ta1618_rtc_disarm_locked(rtc);
	rtc->failed = true;
	mutex_unlock(&rtc->lock);
	dev_err(rtc->dev, "RTC alarm IRQ failed: %d (cleanup %d)\n", ret,
		cleanup_ret);
	return IRQ_HANDLED;
}

static const struct rtc_class_ops ta1618_rtc_ops = {
	.read_time = ta1618_rtc_read_time,
	.set_alarm = ta1618_rtc_set_alarm,
	.alarm_irq_enable = ta1618_rtc_alarm_irq_enable,
};

static int ta1618_rtc_suspend(struct device *dev)
{
	struct ta1618_rtc *rtc = dev_get_drvdata(dev);

	if (device_may_wakeup(dev) || rtc->suspend_disabled)
		return 0;

	/* Nested IRQs are not masked by suspend_device_irqs(). */
	disable_irq(rtc->irq);
	rtc->suspend_disabled = true;
	return 0;
}

static int ta1618_rtc_resume(struct device *dev)
{
	struct ta1618_rtc *rtc = dev_get_drvdata(dev);

	if (!rtc->suspend_disabled)
		return 0;

	rtc->suspend_disabled = false;
	enable_irq(rtc->irq);
	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(ta1618_rtc_pm_ops, ta1618_rtc_suspend,
				ta1618_rtc_resume);

static void ta1618_rtc_teardown(struct ta1618_rtc *rtc)
{
	int ret;

	mutex_lock(&rtc->lock);
	ret = ta1618_rtc_disarm_locked(rtc);
	rtc->failed = true;
	mutex_unlock(&rtc->lock);
	if (ret)
		dev_crit(
			rtc->dev,
			"failed to restore RTC alarm state during teardown: %d\n",
			ret);
}

static int ta1618_rtc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct ta1618_rtc *rtc;
	struct rtc_time time;
	int ret;

	rtc = devm_kzalloc(dev, sizeof(*rtc), GFP_KERNEL);
	if (!rtc)
		return -ENOMEM;
	rtc->dev = dev;
	mutex_init(&rtc->lock);

	rtc->rtc = devm_rtc_allocate_device(dev);
	if (IS_ERR(rtc->rtc))
		return PTR_ERR(rtc->rtc);
	rtc->rtc->ops = &ta1618_rtc_ops;
	rtc->rtc->range_min = TA1618_RTC_EPOCH;
	rtc->rtc->range_max = 4102444799LL;
	clear_bit(RTC_FEATURE_UPDATE_INTERRUPT, rtc->rtc->features);
	platform_set_drvdata(pdev, rtc);

	ret = rtc_read_time(rtc->rtc, &time);
	if (ret)
		return dev_err_probe(dev, ret, "SC2720 RTC time unavailable\n");
	ret = ta1618_rtc_validate_alarm_baseline(rtc, true);
	if (ret)
		return dev_err_probe(
			dev, ret, "SC2720 RTC alarm baseline is not usable\n");

	rtc->irq = platform_get_irq_byname(pdev, "alarm");
	if (rtc->irq < 0)
		return dev_err_probe(dev, rtc->irq,
				     "could not resolve RTC alarm IRQ\n");
	ret = devm_request_threaded_irq(dev, rtc->irq, NULL,
					ta1618_rtc_alarm_thread,
					IRQF_ONESHOT | IRQF_NO_AUTOEN,
					dev_name(dev), rtc);
	if (ret)
		return dev_err_probe(dev, ret,
				     "could not request RTC alarm IRQ\n");
	ret = devm_device_init_wakeup(dev);
	if (ret)
		return dev_err_probe(dev, ret,
				     "could not enable RTC wake capability\n");
	ret = devm_pm_set_wake_irq(dev, rtc->irq);
	if (ret)
		return dev_err_probe(dev, ret,
				     "could not register RTC alarm wake IRQ\n");

	return devm_rtc_register_device(rtc->rtc);
}

static void ta1618_rtc_remove(struct platform_device *pdev)
{
	ta1618_rtc_teardown(platform_get_drvdata(pdev));
}

static void ta1618_rtc_shutdown(struct platform_device *pdev)
{
	ta1618_rtc_teardown(platform_get_drvdata(pdev));
}

static const struct of_device_id ta1618_rtc_of_match[] = {
	{ .compatible = "fplinux,ta1618-sc2720-rtc" },
	{},
};
MODULE_DEVICE_TABLE(of, ta1618_rtc_of_match);

static struct platform_driver ta1618_rtc_driver = {
	.probe = ta1618_rtc_probe,
	.remove = ta1618_rtc_remove,
	.shutdown = ta1618_rtc_shutdown,
	.driver = {
		.name = "ta1618-sc2720-rtc",
		.of_match_table = ta1618_rtc_of_match,
		.pm = pm_sleep_ptr(&ta1618_rtc_pm_ops),
	},
};
module_platform_driver(ta1618_rtc_driver);

MODULE_DESCRIPTION("Nokia TA-1618 SC2720 RTC driver");
MODULE_LICENSE("GPL");
