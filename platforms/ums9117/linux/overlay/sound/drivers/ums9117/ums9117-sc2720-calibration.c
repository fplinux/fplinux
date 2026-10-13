// SPDX-License-Identifier: GPL-2.0-only
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/regmap.h>

#include "ums9117-sc2720-calibration.h"
#include "ums9117-sc2720-regs.h"

#define SC2720_CALDC_EN BIT(14)
#define SC2720_CALDC_ENO BIT(13)
#define SC2720_DCCAL_STS BIT(12)
#define SC2720_HP_DPOP_VALID BIT(10)
#define SC2720_DEPOP_CHARGE_EN BIT(8)
#define SC2720_PLUGIN BIT(7)
#define SC2720_DEPOP_EN BIT(6)
#define SC2720_DEPOP_CHARGE_STS BIT(5)
#define SC2720_RCV_DPOP_VALID BIT(4)
#define SC2720_DCCAL_DONE (SC2720_DCCAL_STS | SC2720_HP_DPOP_VALID)
#define SC2720_DEPOP_CHARGE_DONE \
	(SC2720_DEPOP_CHARGE_STS | SC2720_RCV_DPOP_VALID)

#define SC2720_FAST_CHARGE_STEPS 12U
#define SC2720_DCCAL_ATTEMPTS 20U
#define SC2720_DEPOP_VALID_ATTEMPTS 20U
#define SC2720_DEPOP_CHARGE_ATTEMPTS 30U

static int fast_charge(struct regmap *regmap)
{
	unsigned int i;
	int ret;

	ret = regmap_update_bits(regmap, SC2720_ANA_CDC2, SC2720_DAS_EN, 0);
	if (ret)
		return ret;
	ret = regmap_update_bits(regmap, SC2720_ANA_CDC2, SC2720_PA_EN, 0);
	if (ret)
		return ret;
	ret = regmap_write_bits(regmap, SC2720_ANA_STS2, SC2720_CALDC_ENO,
				SC2720_CALDC_ENO);
	if (ret)
		return ret;
	ret = regmap_update_bits(regmap, SC2720_ANA_STS0, BIT(0), BIT(0));
	if (ret)
		return ret;
	usleep_range(1000, 2000);
	ret = regmap_update_bits(regmap, SC2720_ANA_STS0, BIT(0), 0);
	if (ret)
		return ret;
	for (i = 0; i < SC2720_FAST_CHARGE_STEPS; i++)
		usleep_range(5000, 6000);
	return 0;
}

static int wait_dc_calibration(struct device *dev, struct regmap *regmap)
{
	unsigned int attempt;
	unsigned int value = 0;
	int ret;

	for (attempt = 0; attempt < SC2720_DCCAL_ATTEMPTS; attempt++) {
		ret = regmap_read(regmap, SC2720_ANA_STS2, &value);
		if (ret)
			return ret;
		if ((value & SC2720_DCCAL_DONE) == SC2720_DCCAL_DONE) {
			usleep_range(5000, 6000);
			ret = regmap_read(regmap, SC2720_ANA_STS2, &value);
			if (ret)
				return ret;
			if ((value & SC2720_DCCAL_DONE) == SC2720_DCCAL_DONE)
				return 0;
		}
		usleep_range(15000, 16000);
	}
	dev_err(dev, "headphone DC calibration timed out: ANA_STS2=%#06x\n",
		value);
	return -ETIMEDOUT;
}

static int wait_depop_valid(struct device *dev, struct regmap *regmap)
{
	unsigned int attempt;
	unsigned int value = 0;
	int ret;

	for (attempt = 0; attempt < SC2720_DEPOP_VALID_ATTEMPTS; attempt++) {
		ret = regmap_read(regmap, SC2720_ANA_STS2, &value);
		if (ret)
			return ret;
		if (value & SC2720_HP_DPOP_VALID)
			return 0;
		usleep_range(5000, 6000);
	}
	dev_err(dev, "headphone depop-valid status timed out: ANA_STS2=%#06x\n",
		value);
	return -ETIMEDOUT;
}

static int wait_depop_charge(struct device *dev, struct regmap *regmap)
{
	unsigned int attempt;
	unsigned int value = 0;
	int ret;

	for (attempt = 0; attempt < SC2720_DEPOP_CHARGE_ATTEMPTS; attempt++) {
		ret = regmap_read(regmap, SC2720_ANA_STS2, &value);
		if (ret)
			return ret;
		if ((value & SC2720_DEPOP_CHARGE_DONE) ==
		    SC2720_DEPOP_CHARGE_DONE)
			return 0;
		msleep(20);
	}
	dev_err(dev, "headphone depop charge timed out: ANA_STS2=%#06x\n",
		value);
	return -ETIMEDOUT;
}

int ums9117_sc2720_calibrate_headphones(struct device *dev,
					struct regmap *regmap)
{
	int restore_error = 0;
	unsigned int saved_cdc2;
	unsigned int saved_cdc3;
	int restore_ret;
	int ret;

	ret = regmap_read(regmap, SC2720_ANA_CDC2, &saved_cdc2);
	if (ret)
		return ret;
	ret = regmap_read(regmap, SC2720_ANA_CDC3, &saved_cdc3);
	if (ret)
		return ret;
	ret = regmap_update_bits(regmap, SC2720_ANA_CDC4, SC2720_HP_GAIN_MASK,
				 SC2720_HP_GAIN_MUTE);
	if (ret)
		goto restore_routes;
	ret = regmap_update_bits(regmap, SC2720_ANA_PMU0, SC2720_AUD_BG_EN,
				 SC2720_AUD_BG_EN);
	if (ret)
		goto restore_routes;
	ret = regmap_update_bits(regmap, SC2720_ANA_PMU0, SC2720_AUD_BIAS_EN,
				 SC2720_AUD_BIAS_EN);
	if (ret)
		goto restore_routes;
	ret = regmap_update_bits(regmap, SC2720_ANA_CDC2,
				 SC2720_CDC2_OUTPUT_MASK, 0);
	if (ret)
		goto restore_routes;
	ret = regmap_update_bits(regmap, SC2720_ANA_CDC3, SC2720_DALR_OFFSET_EN,
				 0);
	if (ret)
		goto restore_routes;
	ret = regmap_update_bits(regmap, SC2720_ANA_CDC3, SC2720_DACL_TO_HPL,
				 0);
	if (ret)
		goto restore_routes;
	ret = regmap_update_bits(regmap, SC2720_ANA_CDC3, SC2720_DACR_TO_HPR,
				 0);
	if (ret)
		goto restore_routes;
	ret = regmap_update_bits(regmap, SC2720_ANA_CDC3, SC2720_DACL_TO_RCV,
				 0);
	if (ret)
		goto restore_routes;
	ret = regmap_write(regmap, SC2720_ANA_STS2, 0);
	if (ret)
		goto restore_routes;

	ret = fast_charge(regmap);
	if (ret)
		goto restore_routes;
	usleep_range(5000, 6000);
	ret = regmap_update_bits(regmap, SC2720_ANA_CDC2, SC2720_HP_BUFFER_EN,
				 SC2720_HP_BUFFER_EN);
	if (ret)
		goto restore_routes;
	ret = regmap_write_bits(regmap, SC2720_ANA_STS2, SC2720_CALDC_ENO,
				SC2720_CALDC_ENO);
	if (ret)
		goto restore_routes;
	ret = regmap_write_bits(regmap, SC2720_ANA_STS2, SC2720_CALDC_EN,
				SC2720_CALDC_EN);
	if (ret)
		goto restore_routes;
	ret = regmap_write(regmap, SC2720_ANA_DCL4, 0xffffU);
	if (ret)
		goto restore_routes;
	ret = regmap_write(regmap, SC2720_ANA_DCL6, 0x4cd8U);
	if (ret)
		goto restore_routes;
	ret = regmap_write(regmap, SC2720_ANA_DCL7, 0x2e6cU);
	if (ret)
		goto restore_routes;
	ret = regmap_write(regmap, SC2720_ANA_STS0, 0xa820U);
	if (ret)
		goto restore_routes;
	usleep_range(2000, 3000);
	ret = regmap_write_bits(regmap, SC2720_ANA_STS2, SC2720_CALDC_START, 0);
	if (ret)
		goto restore_routes;
	ret = regmap_write_bits(regmap, SC2720_ANA_STS2, SC2720_CALDC_START,
				SC2720_CALDC_START);
	if (ret)
		goto restore_routes;
	ret = regmap_write(regmap, SC2720_ANA_STS2,
			   SC2720_CALDC_START | SC2720_CALDC_EN |
				   SC2720_CALDC_ENO);
	if (ret)
		goto restore_routes;
	ret = wait_dc_calibration(dev, regmap);
	if (ret)
		goto restore_routes;

	ret = regmap_update_bits(regmap, SC2720_ANA_CDC2,
				 SC2720_CDC2_OUTPUT_MASK, 0);
	if (ret)
		goto restore_routes;
	ret = wait_depop_valid(dev, regmap);
	if (ret)
		goto restore_routes;
	ret = regmap_write_bits(regmap, SC2720_ANA_STS2, SC2720_DEPOP_CHARGE_EN,
				SC2720_DEPOP_CHARGE_EN);
	if (ret)
		goto restore_routes;
	ret = regmap_write_bits(regmap, SC2720_ANA_STS2, SC2720_PLUGIN,
				SC2720_PLUGIN);
	if (ret)
		goto restore_routes;
	ret = regmap_write_bits(regmap, SC2720_ANA_STS2, SC2720_DEPOP_EN,
				SC2720_DEPOP_EN);
	if (ret)
		goto restore_routes;
	usleep_range(2000, 3000);
	ret = regmap_write_bits(regmap, SC2720_ANA_STS2,
				SC2720_DEPOP_CHARGE_START,
				SC2720_DEPOP_CHARGE_START);
	if (ret)
		goto restore_routes;
	ret = wait_depop_charge(dev, regmap);

restore_routes:
	restore_ret = regmap_update_bits(regmap, SC2720_ANA_CDC2,
					 SC2720_CDC2_OUTPUT_MASK, saved_cdc2);
	restore_error = restore_error ?: restore_ret;
	restore_ret = regmap_update_bits(
		regmap, SC2720_ANA_CDC3,
		SC2720_DALR_OFFSET_EN | SC2720_CDC3_OUTPUT_MIXERS, saved_cdc3);
	restore_error = restore_error ?: restore_ret;
	if (!restore_error)
		return ret;
	if (ret) {
		dev_err(dev,
			"cannot restore codec routes after headphone calibration error: %pe\n",
			ERR_PTR(restore_error));
		return ret;
	}
	return restore_error;
}
