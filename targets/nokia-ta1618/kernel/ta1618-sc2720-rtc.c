// SPDX-License-Identifier: GPL-2.0-only
#include <linux/bitops.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
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

#define SC2720_EXPECTED_ID_LOW 0xa003U
#define SC2720_EXPECTED_ID_HIGH 0x2720U
#define SC2720_RTC_GATE BIT(1)
#define SC2720_RTC_SECONDS_MASK GENMASK(5, 0)
#define SC2720_RTC_MINUTES_MASK GENMASK(5, 0)
#define SC2720_RTC_HOURS_MASK GENMASK(4, 0)

#define TA1618_RTC_EPOCH 315532800LL
#define TA1618_RTC_MAX_DAYS 43829U
#define TA1618_RTC_MAX_ATTEMPTS 3U

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

static const struct rtc_class_ops ta1618_rtc_ops = {
	.read_time = ta1618_rtc_read_time,
};

static int ta1618_rtc_probe(struct platform_device *pdev)
{
	struct rtc_device *rtc;
	struct rtc_time time;
	int ret;

	rtc = devm_rtc_allocate_device(&pdev->dev);
	if (IS_ERR(rtc))
		return PTR_ERR(rtc);

	rtc->ops = &ta1618_rtc_ops;
	rtc->range_min = TA1618_RTC_EPOCH;
	rtc->range_max = 4102444799LL;
	clear_bit(RTC_FEATURE_ALARM, rtc->features);
	clear_bit(RTC_FEATURE_UPDATE_INTERRUPT, rtc->features);

	ret = rtc_read_time(rtc, &time);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "SC2720 RTC time unavailable\n");

	return devm_rtc_register_device(rtc);
}

static const struct of_device_id ta1618_rtc_of_match[] = {
	{ .compatible = "fplinux,ta1618-sc2720-rtc" },
	{},
};
MODULE_DEVICE_TABLE(of, ta1618_rtc_of_match);

static struct platform_driver ta1618_rtc_driver = {
	.probe = ta1618_rtc_probe,
	.driver = {
		.name = "ta1618-sc2720-rtc",
		.of_match_table = ta1618_rtc_of_match,
	},
};
module_platform_driver(ta1618_rtc_driver);

MODULE_DESCRIPTION("Nokia TA-1618 read-only SC2720 RTC");
MODULE_LICENSE("GPL");
