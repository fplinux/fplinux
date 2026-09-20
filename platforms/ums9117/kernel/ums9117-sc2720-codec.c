// SPDX-License-Identifier: GPL-2.0-only
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/soc/sprd/ums9117-adi.h>

#include "ums9117-sc2720-codec.h"

#define SC2720_CHIP_ID_LOW 0x0c00U
#define SC2720_CHIP_ID_HIGH 0x0c04U
#define SC2720_MODULE_EN0 0x0c08U
#define SC2720_ARM_CLK_EN0 0x0c0cU
#define SC2720_SOFT_RST0 0x0c14U
#define SC2720_AUDIO_CTRL0 0x0e08U

#define SC2720_ANA_PMU0 0x0700U
#define SC2720_ANA_CLK0 0x0718U
#define SC2720_ANA_CDC1 0x0720U
#define SC2720_ANA_CDC2 0x0724U
#define SC2720_ANA_CDC3 0x0728U
#define SC2720_ANA_CDC4 0x072cU
#define SC2720_ANA_DCL0 0x073cU
#define SC2720_ANA_DCL4 0x074cU
#define SC2720_ANA_DCL6 0x0754U
#define SC2720_ANA_DCL7 0x0758U
#define SC2720_ANA_STS0 0x075cU
#define SC2720_ANA_STS2 0x0764U
#define SC2720_ANA_CLK1 0x077cU
#define SC2720_AUD_CFGA_LP_MODULE_CTRL 0x0830U
#define SC2720_AUD_CFGA_ANA_ET2 0x0834U
#define SC2720_AUD_CFGA_CLK_EN 0x0838U

#define SC2720_ID_LOW 0xa003U
#define SC2720_ID_HIGH 0x2720U

#define SC2720_AUD_MODULE_EN BIT(4)
#define SC2720_AUD_IF_CLOCKS (BIT(1) | BIT(0))
#define SC2720_AUD_RESETS (BIT(13) | BIT(12) | BIT(8))
#define SC2720_AUD_IF_TX_INVERT BIT(3)

#define SC2720_AUD_VB_EN BIT(15)
#define SC2720_AUD_VB_SLEEP_PD BIT(13)
#define SC2720_AUD_BG_EN BIT(12)
#define SC2720_AUD_BIAS_EN BIT(11)
#define SC2720_VBG_SEL_1P50_V BIT(6)
#define SC2720_VBG_TEMP_TUNE_MASK GENMASK(4, 3)
#define SC2720_VBG_CONFIG_MASK \
	(SC2720_VBG_SEL_1P50_V | SC2720_VBG_TEMP_TUNE_MASK)
#define SC2720_AUD_POWER                                                \
	(SC2720_AUD_VB_EN | SC2720_AUD_VB_SLEEP_PD | SC2720_AUD_BG_EN | \
	 SC2720_AUD_BIAS_EN)

#define SC2720_DIG_CLK_6P5M_EN BIT(14)
#define SC2720_ANA_CLK_EN BIT(12)
#define SC2720_AD_CLK_RST BIT(10)
#define SC2720_DA_CLK_EN BIT(9)
#define SC2720_DRV_CLK_EN BIT(8)
#define SC2720_PLAYBACK_CLOCKS                                           \
	(SC2720_DIG_CLK_6P5M_EN | SC2720_ANA_CLK_EN | SC2720_DA_CLK_EN | \
	 SC2720_DRV_CLK_EN)

#define SC2720_DCL_EN BIT(6)
#define SC2720_DCL_RST BIT(5)
#define SC2720_DRV_SOFT_EN BIT(1)
#define SC2720_DPOP_AUTO_RST BIT(0)
#define SC2720_DCL_CONTROL                                     \
	(SC2720_DCL_EN | SC2720_DCL_RST | SC2720_DRV_SOFT_EN | \
	 SC2720_DPOP_AUTO_RST)
#define SC2720_AUD_CLK_PN_MASK 0x00ffU
#define SC2720_AUD_CLK_PN_VALUE 0x000dU

#define SC2720_CLK_AUD_6P5M_EN BIT(4)
#define SC2720_CLK_AUD_HID_EN BIT(2)
#define SC2720_CLK_AUD_1K_EN BIT(1)
#define SC2720_CLK_AUD_32K_EN BIT(0)
#define SC2720_CODEC_CLOCKS                               \
	(SC2720_CLK_AUD_6P5M_EN | SC2720_CLK_AUD_HID_EN | \
	 SC2720_CLK_AUD_1K_EN | SC2720_CLK_AUD_32K_EN)
#define SC2720_DAC_EN_L BIT(2)
#define SC2720_DAC_EN_R BIT(4)
#define SC2720_DAC_ENABLE (SC2720_DAC_EN_L | SC2720_DAC_EN_R)
#define SC2720_DALR_MIX_MASK GENMASK(3, 2)

#define SC2720_ADVCMI_INT_SEL_MASK GENMASK(13, 12)
#define SC2720_DALR_OFFSET_MASK (BIT(5) | BIT(4) | BIT(3))
#define SC2720_DALR_OFFSET_2 BIT(4)
#define SC2720_DAS_EN BIT(13)
#define SC2720_DAC_EN_L_ANALOG BIT(12)
#define SC2720_DAC_EN_R_ANALOG BIT(11)
#define SC2720_HPL_DUMMY_LOOP BIT(10)
#define SC2720_HPL_DUMMY_LOOP_END BIT(9)
#define SC2720_HPR_DUMMY_LOOP BIT(8)
#define SC2720_HPR_DUMMY_LOOP_END BIT(7)
#define SC2720_HPL_EN BIT(4)
#define SC2720_HPR_EN BIT(3)
#define SC2720_HP_BUFFER_EN BIT(2)
#define SC2720_PA_EN BIT(0)
#define SC2720_CDC2_OUTPUT_MASK GENMASK(13, 0)
#define SC2720_DALR_OFFSET_EN BIT(12)
#define SC2720_DACL_TO_HPL BIT(9)
#define SC2720_DACR_TO_HPR BIT(8)
#define SC2720_DACL_TO_RCV BIT(7)
#define SC2720_DACS_TO_PA BIT(6)
#define SC2720_CDC3_OUTPUT_MIXERS                                       \
	(SC2720_DACL_TO_HPL | SC2720_DACR_TO_HPR | SC2720_DACL_TO_RCV | \
	 SC2720_DACS_TO_PA)
#define SC2720_HP_GAIN_MASK GENMASK(7, 0)
#define SC2720_HP_GAIN_MUTE 0x00ffU
#define SC2720_HPL_GAIN_SHIFT 4
#define SC2720_HP_VOLUME_DEFAULT 1U

#define SC2720_CALDC_START BIT(15)
#define SC2720_CALDC_EN BIT(14)
#define SC2720_CALDC_ENO BIT(13)
#define SC2720_DCCAL_STS BIT(12)
#define SC2720_HP_DPOP_VALID BIT(10)
#define SC2720_DEPOP_CHARGE_START BIT(9)
#define SC2720_DEPOP_CHARGE_EN BIT(8)
#define SC2720_PLUGIN BIT(7)
#define SC2720_DEPOP_EN BIT(6)
#define SC2720_DEPOP_CHARGE_STS BIT(5)
#define SC2720_RCV_DPOP_VALID BIT(4)
#define SC2720_STS2_HARDWARE_CONTROLLED GENMASK(3, 1)
#define SC2720_STS2_RW_MASK 0xebceU
#define SC2720_STS2_START_MASK (SC2720_CALDC_START | SC2720_DEPOP_CHARGE_START)
#define SC2720_STS2_RESTORE_MASK \
	(SC2720_STS2_RW_MASK &   \
	 ~(SC2720_STS2_START_MASK | SC2720_STS2_HARDWARE_CONTROLLED))
#define SC2720_DCCAL_DONE (SC2720_DCCAL_STS | SC2720_HP_DPOP_VALID)
#define SC2720_DEPOP_CHARGE_DONE \
	(SC2720_DEPOP_CHARGE_STS | SC2720_RCV_DPOP_VALID)

#define SC2720_FAST_CHARGE_STEPS 12U
#define SC2720_DCCAL_ATTEMPTS 20U
#define SC2720_DEPOP_VALID_ATTEMPTS 20U
#define SC2720_DEPOP_CHARGE_ATTEMPTS 30U

enum ums9117_sc2720_codec_register {
	UMS9117_SC2720_CODEC_MODULE_EN0,
	UMS9117_SC2720_CODEC_SOFT_RST0,
	UMS9117_SC2720_CODEC_ARM_CLK_EN0,
	UMS9117_SC2720_CODEC_ANA_PMU0,
	UMS9117_SC2720_CODEC_ANA_CLK0,
	UMS9117_SC2720_CODEC_ANA_DCL0,
	UMS9117_SC2720_CODEC_ANA_CLK1,
	UMS9117_SC2720_CODEC_CFGA_CLK_EN,
	UMS9117_SC2720_CODEC_AUDIO_CTRL0,
	UMS9117_SC2720_CODEC_CFGA_LP_MODULE_CTRL,
	UMS9117_SC2720_CODEC_CFGA_ANA_ET2,
	UMS9117_SC2720_CODEC_ANA_CDC1,
	UMS9117_SC2720_CODEC_ANA_DCL4,
	UMS9117_SC2720_CODEC_ANA_DCL6,
	UMS9117_SC2720_CODEC_ANA_DCL7,
	UMS9117_SC2720_CODEC_ANA_STS0,
	UMS9117_SC2720_CODEC_ANA_STS2,
	UMS9117_SC2720_CODEC_ANA_CDC4,
	UMS9117_SC2720_CODEC_ANA_CDC3,
	UMS9117_SC2720_CODEC_ANA_CDC2,
	UMS9117_SC2720_CODEC_REGISTER_COUNT,
};

struct ums9117_sc2720_codec_register_desc {
	u32 offset;
	u16 mask;
};

static const struct ums9117_sc2720_codec_register_desc ums9117_sc2720_codec_registers[] = {
	[UMS9117_SC2720_CODEC_MODULE_EN0] = { SC2720_MODULE_EN0,
					      SC2720_AUD_MODULE_EN },
	[UMS9117_SC2720_CODEC_SOFT_RST0] = { SC2720_SOFT_RST0,
					     SC2720_AUD_RESETS },
	[UMS9117_SC2720_CODEC_ARM_CLK_EN0] = { SC2720_ARM_CLK_EN0,
					       SC2720_AUD_IF_CLOCKS },
	[UMS9117_SC2720_CODEC_ANA_PMU0] = { SC2720_ANA_PMU0,
					    SC2720_AUD_POWER |
						    SC2720_VBG_CONFIG_MASK },
	[UMS9117_SC2720_CODEC_ANA_CLK0] = { SC2720_ANA_CLK0,
					    SC2720_PLAYBACK_CLOCKS |
						    SC2720_AD_CLK_RST },
	[UMS9117_SC2720_CODEC_ANA_DCL0] = { SC2720_ANA_DCL0,
					    SC2720_DCL_CONTROL },
	[UMS9117_SC2720_CODEC_ANA_CLK1] = { SC2720_ANA_CLK1,
					    SC2720_AUD_CLK_PN_MASK },
	[UMS9117_SC2720_CODEC_CFGA_CLK_EN] = { SC2720_AUD_CFGA_CLK_EN,
					       SC2720_CODEC_CLOCKS },
	[UMS9117_SC2720_CODEC_AUDIO_CTRL0] = { SC2720_AUDIO_CTRL0,
					       SC2720_AUD_IF_TX_INVERT },
	[UMS9117_SC2720_CODEC_CFGA_LP_MODULE_CTRL] = { SC2720_AUD_CFGA_LP_MODULE_CTRL,
						       SC2720_DAC_ENABLE },
	[UMS9117_SC2720_CODEC_CFGA_ANA_ET2] = { SC2720_AUD_CFGA_ANA_ET2,
						SC2720_DALR_MIX_MASK },
	[UMS9117_SC2720_CODEC_ANA_CDC1] = { SC2720_ANA_CDC1,
					    SC2720_ADVCMI_INT_SEL_MASK |
						    SC2720_DALR_OFFSET_MASK },
	[UMS9117_SC2720_CODEC_ANA_DCL4] = { SC2720_ANA_DCL4, 0xffffU },
	[UMS9117_SC2720_CODEC_ANA_DCL6] = { SC2720_ANA_DCL6, 0x7ffeU },
	[UMS9117_SC2720_CODEC_ANA_DCL7] = { SC2720_ANA_DCL7, 0x7fffU },
	[UMS9117_SC2720_CODEC_ANA_STS0] = { SC2720_ANA_STS0, 0xffffU },
	[UMS9117_SC2720_CODEC_ANA_STS2] = { SC2720_ANA_STS2,
					    SC2720_STS2_RESTORE_MASK },
	[UMS9117_SC2720_CODEC_ANA_CDC4] = { SC2720_ANA_CDC4,
					    SC2720_HP_GAIN_MASK },
	[UMS9117_SC2720_CODEC_ANA_CDC3] = { SC2720_ANA_CDC3,
					    SC2720_DALR_OFFSET_EN |
						    SC2720_CDC3_OUTPUT_MIXERS },
	[UMS9117_SC2720_CODEC_ANA_CDC2] = { SC2720_ANA_CDC2,
					    SC2720_CDC2_OUTPUT_MASK },
};

/* Mute, -15, -12, -9, -6, -3 and 0 dB, in increasing volume order. */
static const u8 sc2720_hp_gain[] = { 0xf, 0x9, 0x8, 0x7, 0x6, 0x5, 0x4 };

struct ums9117_sc2720_codec {
	struct device *dev;
	u16 saved[UMS9117_SC2720_CODEC_REGISTER_COUNT];
	unsigned int volume_left;
	unsigned int volume_right;
	bool dirty;
	/* Successful DC/depop calibration survives ordinary playback stops. */
	bool prepared;
	bool enabled;
	bool xtal_held;
};

static int ums9117_sc2720_codec_sts2_update(u16 mask, u16 value)
{
	struct ums9117_adi_transaction transaction = {};
	u16 old_value;
	int end_ret;
	int ret;

	ret = ums9117_adi_begin(&transaction);
	if (ret)
		return ret;
	ret = ums9117_adi_read(&transaction, SC2720_ANA_STS2, &old_value);
	if (!ret)
		ret = ums9117_adi_write_final(&transaction, SC2720_ANA_STS2,
					      (old_value & ~mask) |
						      (value & mask));
	end_ret = ums9117_adi_end(&transaction);
	return ret ? ret : end_ret;
}

static int ums9117_sc2720_codec_sts2_restore(u16 saved)
{
	u16 value;
	int ret;

	ret = ums9117_sc2720_codec_sts2_update(SC2720_STS2_RESTORE_MASK, saved);
	if (ret)
		return ret;
	ret = ums9117_adi_read_once(SC2720_ANA_STS2, &value);
	if (ret)
		return ret;
	if ((value & SC2720_STS2_RESTORE_MASK) !=
	    (saved & SC2720_STS2_RESTORE_MASK))
		return -EIO;
	return 0;
}

static int ums9117_sc2720_codec_check_identity(void)
{
	u16 high;
	u16 low;
	int ret;

	ret = ums9117_adi_read_once(SC2720_CHIP_ID_LOW, &low);
	if (!ret)
		ret = ums9117_adi_read_once(SC2720_CHIP_ID_HIGH, &high);
	if (ret)
		return ret;
	if (low != SC2720_ID_LOW || high != SC2720_ID_HIGH)
		return -ENODEV;
	return 0;
}

static int ums9117_sc2720_codec_snapshot(struct ums9117_sc2720_codec *codec)
{
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(ums9117_sc2720_codec_registers); i++) {
		const struct ums9117_sc2720_codec_register_desc *reg =
			&ums9117_sc2720_codec_registers[i];

		ret = ums9117_adi_read_once(reg->offset, &codec->saved[i]);
		if (ret)
			return ret;
	}
	return 0;
}

static int
ums9117_sc2720_codec_validate_idle(const struct ums9117_sc2720_codec *codec)
{
	if (codec->saved[UMS9117_SC2720_CODEC_SOFT_RST0] & SC2720_AUD_RESETS)
		return -EBUSY;
	if (codec->saved[UMS9117_SC2720_CODEC_ANA_CDC2] &
	    SC2720_CDC2_OUTPUT_MASK)
		return -EBUSY;
	if (codec->saved[UMS9117_SC2720_CODEC_ANA_CDC3] &
	    SC2720_CDC3_OUTPUT_MIXERS)
		return -EBUSY;
	return 0;
}

static int ums9117_sc2720_codec_close_headphones(void)
{
	int first_error = 0;
	int ret;

	ret = ums9117_adi_update_bits_once(SC2720_ANA_CDC4, SC2720_HP_GAIN_MASK,
					   SC2720_HP_GAIN_MUTE);
	ums9117_adi_record_first_error(&first_error, ret);
	ret = ums9117_adi_update_bits_once(SC2720_ANA_CDC3, SC2720_DACL_TO_HPL,
					   0);
	ums9117_adi_record_first_error(&first_error, ret);
	ret = ums9117_adi_update_bits_once(SC2720_ANA_CDC3, SC2720_DACR_TO_HPR,
					   0);
	ums9117_adi_record_first_error(&first_error, ret);
	ret = ums9117_adi_update_bits_once(SC2720_ANA_CDC2,
					   SC2720_HPL_DUMMY_LOOP_END, 0);
	ums9117_adi_record_first_error(&first_error, ret);
	ret = ums9117_adi_update_bits_once(SC2720_ANA_CDC2,
					   SC2720_HPR_DUMMY_LOOP_END, 0);
	ums9117_adi_record_first_error(&first_error, ret);
	ret = ums9117_adi_update_bits_once(SC2720_ANA_CDC2, SC2720_HPL_EN, 0);
	ums9117_adi_record_first_error(&first_error, ret);
	ret = ums9117_adi_update_bits_once(SC2720_ANA_CDC2, SC2720_HPR_EN, 0);
	ums9117_adi_record_first_error(&first_error, ret);
	ret = ums9117_adi_update_bits_once(SC2720_ANA_CDC2, SC2720_HP_BUFFER_EN,
					   0);
	ums9117_adi_record_first_error(&first_error, ret);
	ret = ums9117_adi_update_bits_once(SC2720_ANA_CDC2,
					   SC2720_DAC_EN_L_ANALOG, 0);
	ums9117_adi_record_first_error(&first_error, ret);
	ret = ums9117_adi_update_bits_once(SC2720_ANA_CDC2,
					   SC2720_DAC_EN_R_ANALOG, 0);
	ums9117_adi_record_first_error(&first_error, ret);
	return first_error;
}

static int ums9117_sc2720_codec_restore(struct ums9117_sc2720_codec *codec)
{
	int first_error = 0;
	unsigned int i;
	int ret;

	if (!codec->dirty)
		return codec->xtal_held ?
			       ums9117_adi_xtal_release(&codec->xtal_held) :
			       0;
	ret = ums9117_sc2720_codec_close_headphones();
	ums9117_adi_record_first_error(&first_error, ret);
	ret = ums9117_sc2720_codec_sts2_update(SC2720_CALDC_START, 0);
	ums9117_adi_record_first_error(&first_error, ret);
	ret = ums9117_sc2720_codec_sts2_update(SC2720_DEPOP_CHARGE_START, 0);
	ums9117_adi_record_first_error(&first_error, ret);

	for (i = ARRAY_SIZE(ums9117_sc2720_codec_registers); i-- > 0;) {
		const struct ums9117_sc2720_codec_register_desc *reg =
			&ums9117_sc2720_codec_registers[i];

		if (reg->offset == SC2720_ANA_STS2)
			ret = ums9117_sc2720_codec_sts2_restore(
				codec->saved[i]);
		else
			ret = ums9117_adi_update_bits_once(
				reg->offset, reg->mask, codec->saved[i]);
		ums9117_adi_record_first_error(&first_error, ret);
	}
	if (first_error)
		return first_error;

	codec->enabled = false;
	codec->prepared = false;
	codec->dirty = false;
	return ums9117_adi_xtal_release(&codec->xtal_held);
}

static int ums9117_sc2720_codec_pulse(u32 offset, u16 mask, u16 inactive)
{
	int ret;

	ret = ums9117_adi_update_bits_once(offset, mask, mask);
	if (ret)
		return ret;
	udelay(1);
	return ums9117_adi_update_bits_once(offset, mask, inactive);
}

static int ums9117_sc2720_codec_power_up(void)
{
	int ret;

	ret = ums9117_adi_update_bits_once(
		SC2720_MODULE_EN0, SC2720_AUD_MODULE_EN, SC2720_AUD_MODULE_EN);
	if (ret)
		return ret;
	ret = ums9117_sc2720_codec_pulse(SC2720_SOFT_RST0, SC2720_AUD_RESETS,
					 0);
	if (ret)
		return ret;
	ret = ums9117_adi_update_bits_once(
		SC2720_ARM_CLK_EN0, SC2720_AUD_IF_CLOCKS, SC2720_AUD_IF_CLOCKS);
	if (ret)
		return ret;
	ret = ums9117_adi_update_bits_once(SC2720_AUD_CFGA_CLK_EN,
					   SC2720_CLK_AUD_HID_EN,
					   SC2720_CLK_AUD_HID_EN);
	if (ret)
		return ret;
	/* Configure the soft driver before its VB supply is enabled. */
	ret = ums9117_adi_update_bits_once(SC2720_ANA_DCL0, SC2720_DRV_SOFT_EN,
					   SC2720_DRV_SOFT_EN);
	if (ret)
		return ret;
	ret = ums9117_adi_update_bits_once(SC2720_ANA_DCL0, SC2720_DCL_EN,
					   SC2720_DCL_EN);
	if (ret)
		return ret;
	ret = ums9117_adi_update_bits_once(
		SC2720_ANA_PMU0, SC2720_VBG_CONFIG_MASK,
		SC2720_VBG_SEL_1P50_V |
			FIELD_PREP(SC2720_VBG_TEMP_TUNE_MASK, 1));
	if (ret)
		return ret;
	ret = ums9117_adi_update_bits_once(SC2720_ANA_PMU0,
					   SC2720_AUD_VB_SLEEP_PD, 0);
	if (ret)
		return ret;
	ret = ums9117_adi_update_bits_once(SC2720_ANA_PMU0, SC2720_AUD_VB_EN,
					   SC2720_AUD_VB_EN);
	if (ret)
		return ret;
	usleep_range(1000, 2000);
	ret = ums9117_adi_update_bits_once(
		SC2720_ANA_CLK0, SC2720_DIG_CLK_6P5M_EN | SC2720_ANA_CLK_EN,
		SC2720_DIG_CLK_6P5M_EN | SC2720_ANA_CLK_EN);
	if (ret)
		return ret;
	ret = ums9117_sc2720_codec_pulse(SC2720_ANA_CLK0, SC2720_AD_CLK_RST, 0);
	if (ret)
		return ret;
	ret = ums9117_adi_update_bits_once(
		SC2720_ANA_CLK0, SC2720_DA_CLK_EN | SC2720_DRV_CLK_EN,
		SC2720_DA_CLK_EN | SC2720_DRV_CLK_EN);
	if (ret)
		return ret;
	ret = ums9117_sc2720_codec_pulse(SC2720_ANA_DCL0, SC2720_DCL_RST, 0);
	if (ret)
		return ret;
	ret = ums9117_sc2720_codec_pulse(SC2720_ANA_DCL0, SC2720_DPOP_AUTO_RST,
					 0);
	if (ret)
		return ret;
	ret = ums9117_adi_update_bits_once(SC2720_ANA_CLK1,
					   SC2720_AUD_CLK_PN_MASK,
					   SC2720_AUD_CLK_PN_VALUE);
	if (ret)
		return ret;
	ret = ums9117_adi_update_bits_once(SC2720_AUD_CFGA_CLK_EN,
					   SC2720_CODEC_CLOCKS,
					   SC2720_CODEC_CLOCKS);
	if (ret)
		return ret;
	ret = ums9117_adi_update_bits_once(
		SC2720_ANA_PMU0, SC2720_AUD_BG_EN | SC2720_AUD_BIAS_EN,
		SC2720_AUD_BG_EN | SC2720_AUD_BIAS_EN);
	if (ret)
		return ret;
	ret = ums9117_adi_update_bits_once(SC2720_ANA_PMU0,
					   SC2720_AUD_VB_SLEEP_PD,
					   SC2720_AUD_VB_SLEEP_PD);
	if (ret)
		return ret;
	usleep_range(5000, 6000);

	ret = ums9117_adi_update_bits_once(SC2720_AUDIO_CTRL0,
					   SC2720_AUD_IF_TX_INVERT,
					   SC2720_AUD_IF_TX_INVERT);
	if (ret)
		return ret;
	ret = ums9117_adi_update_bits_once(SC2720_AUD_CFGA_ANA_ET2,
					   SC2720_DALR_MIX_MASK, 0);
	if (ret)
		return ret;
	ret = ums9117_adi_update_bits_once(SC2720_ANA_CDC1,
					   SC2720_ADVCMI_INT_SEL_MASK, 0);
	if (ret)
		return ret;
	ret = ums9117_adi_update_bits_once(
		SC2720_ANA_CDC1, SC2720_DALR_OFFSET_MASK, SC2720_DALR_OFFSET_2);
	if (ret)
		return ret;
	ret = ums9117_adi_update_bits_once(
		SC2720_ANA_CDC3, SC2720_DALR_OFFSET_EN, SC2720_DALR_OFFSET_EN);
	if (ret)
		return ret;
	ret = ums9117_adi_update_bits_once(SC2720_AUD_CFGA_LP_MODULE_CTRL,
					   SC2720_DAC_ENABLE,
					   SC2720_DAC_ENABLE);
	if (ret)
		return ret;
	return 0;
}

static int ums9117_sc2720_codec_fast_charge(void)
{
	unsigned int i;
	int ret;

	ret = ums9117_adi_update_bits_once(SC2720_ANA_CDC2, SC2720_DAS_EN, 0);
	if (ret)
		return ret;
	ret = ums9117_adi_update_bits_once(SC2720_ANA_CDC2, SC2720_PA_EN, 0);
	if (ret)
		return ret;
	ret = ums9117_sc2720_codec_sts2_update(SC2720_CALDC_ENO,
					       SC2720_CALDC_ENO);
	if (ret)
		return ret;
	ret = ums9117_adi_update_bits_once(SC2720_ANA_STS0, BIT(0), BIT(0));
	if (ret)
		return ret;
	usleep_range(1000, 2000);
	ret = ums9117_adi_update_bits_once(SC2720_ANA_STS0, BIT(0), 0);
	if (ret)
		return ret;
	for (i = 0; i < SC2720_FAST_CHARGE_STEPS; i++)
		usleep_range(5000, 6000);
	return 0;
}

static int
ums9117_sc2720_codec_wait_dc_calibration(struct ums9117_sc2720_codec *codec)
{
	unsigned int attempt;
	u16 value = 0;
	int ret;

	for (attempt = 0; attempt < SC2720_DCCAL_ATTEMPTS; attempt++) {
		ret = ums9117_adi_read_once(SC2720_ANA_STS2, &value);
		if (ret)
			return ret;
		if ((value & SC2720_DCCAL_DONE) == SC2720_DCCAL_DONE) {
			usleep_range(5000, 6000);
			ret = ums9117_adi_read_once(SC2720_ANA_STS2, &value);
			if (ret)
				return ret;
			if ((value & SC2720_DCCAL_DONE) == SC2720_DCCAL_DONE)
				return 0;
		}
		usleep_range(15000, 16000);
	}
	dev_err(codec->dev,
		"headphone DC calibration timed out: ANA_STS2=%#06x\n", value);
	return -ETIMEDOUT;
}

static int
ums9117_sc2720_codec_wait_depop_valid(struct ums9117_sc2720_codec *codec)
{
	unsigned int attempt;
	u16 value = 0;
	int ret;

	for (attempt = 0; attempt < SC2720_DEPOP_VALID_ATTEMPTS; attempt++) {
		ret = ums9117_adi_read_once(SC2720_ANA_STS2, &value);
		if (ret)
			return ret;
		if (value & SC2720_HP_DPOP_VALID)
			return 0;
		usleep_range(5000, 6000);
	}
	dev_err(codec->dev,
		"headphone depop-valid status timed out: ANA_STS2=%#06x\n",
		value);
	return -ETIMEDOUT;
}

static int
ums9117_sc2720_codec_wait_depop_charge(struct ums9117_sc2720_codec *codec)
{
	unsigned int attempt;
	u16 value = 0;
	int ret;

	for (attempt = 0; attempt < SC2720_DEPOP_CHARGE_ATTEMPTS; attempt++) {
		ret = ums9117_adi_read_once(SC2720_ANA_STS2, &value);
		if (ret)
			return ret;
		if ((value & SC2720_DEPOP_CHARGE_DONE) ==
		    SC2720_DEPOP_CHARGE_DONE)
			return 0;
		msleep(20);
	}
	dev_err(codec->dev,
		"headphone depop charge timed out: ANA_STS2=%#06x\n", value);
	return -ETIMEDOUT;
}

static int
ums9117_sc2720_codec_calibrate_headphones(struct ums9117_sc2720_codec *codec)
{
	int restore_error = 0;
	u16 saved_cdc2;
	u16 saved_cdc3;
	int restore_ret;
	int ret;

	ret = ums9117_adi_read_once(SC2720_ANA_CDC2, &saved_cdc2);
	if (ret)
		return ret;
	ret = ums9117_adi_read_once(SC2720_ANA_CDC3, &saved_cdc3);
	if (ret)
		return ret;
	ret = ums9117_adi_update_bits_once(SC2720_ANA_CDC4, SC2720_HP_GAIN_MASK,
					   SC2720_HP_GAIN_MUTE);
	if (ret)
		goto restore_routes;
	ret = ums9117_adi_update_bits_once(SC2720_ANA_PMU0, SC2720_AUD_BG_EN,
					   SC2720_AUD_BG_EN);
	if (ret)
		goto restore_routes;
	ret = ums9117_adi_update_bits_once(SC2720_ANA_PMU0, SC2720_AUD_BIAS_EN,
					   SC2720_AUD_BIAS_EN);
	if (ret)
		goto restore_routes;
	ret = ums9117_adi_write_once(SC2720_ANA_CDC2, 0);
	if (ret)
		goto restore_routes;
	ret = ums9117_adi_update_bits_once(SC2720_ANA_CDC3,
					   SC2720_DALR_OFFSET_EN, 0);
	if (ret)
		goto restore_routes;
	ret = ums9117_adi_update_bits_once(SC2720_ANA_CDC3, SC2720_DACL_TO_HPL,
					   0);
	if (ret)
		goto restore_routes;
	ret = ums9117_adi_update_bits_once(SC2720_ANA_CDC3, SC2720_DACR_TO_HPR,
					   0);
	if (ret)
		goto restore_routes;
	ret = ums9117_adi_update_bits_once(SC2720_ANA_CDC3, SC2720_DACL_TO_RCV,
					   0);
	if (ret)
		goto restore_routes;
	ret = ums9117_adi_write_final_once(SC2720_ANA_STS2, 0);
	if (ret)
		goto restore_routes;

	ret = ums9117_sc2720_codec_fast_charge();
	if (ret)
		goto restore_routes;
	usleep_range(5000, 6000);
	ret = ums9117_adi_update_bits_once(SC2720_ANA_CDC2, SC2720_HP_BUFFER_EN,
					   SC2720_HP_BUFFER_EN);
	if (ret)
		goto restore_routes;
	ret = ums9117_sc2720_codec_sts2_update(SC2720_CALDC_ENO,
					       SC2720_CALDC_ENO);
	if (ret)
		goto restore_routes;
	ret = ums9117_sc2720_codec_sts2_update(SC2720_CALDC_EN,
					       SC2720_CALDC_EN);
	if (ret)
		goto restore_routes;
	ret = ums9117_adi_write_once(SC2720_ANA_DCL4, 0xffffU);
	if (ret)
		goto restore_routes;
	ret = ums9117_adi_write_once(SC2720_ANA_DCL6, 0x4cd8U);
	if (ret)
		goto restore_routes;
	ret = ums9117_adi_write_once(SC2720_ANA_DCL7, 0x2e6cU);
	if (ret)
		goto restore_routes;
	ret = ums9117_adi_write_once(SC2720_ANA_STS0, 0xa820U);
	if (ret)
		goto restore_routes;
	usleep_range(2000, 3000);
	ret = ums9117_sc2720_codec_sts2_update(SC2720_CALDC_START, 0);
	if (ret)
		goto restore_routes;
	ret = ums9117_sc2720_codec_sts2_update(SC2720_CALDC_START,
					       SC2720_CALDC_START);
	if (ret)
		goto restore_routes;
	ret = ums9117_adi_write_final_once(
		SC2720_ANA_STS2,
		SC2720_CALDC_START | SC2720_CALDC_EN | SC2720_CALDC_ENO);
	if (ret)
		goto restore_routes;
	ret = ums9117_sc2720_codec_wait_dc_calibration(codec);
	if (ret)
		goto restore_routes;

	ret = ums9117_adi_write_once(SC2720_ANA_CDC2, 0);
	if (ret)
		goto restore_routes;
	ret = ums9117_sc2720_codec_wait_depop_valid(codec);
	if (ret)
		goto restore_routes;
	ret = ums9117_sc2720_codec_sts2_update(SC2720_DEPOP_CHARGE_EN,
					       SC2720_DEPOP_CHARGE_EN);
	if (ret)
		goto restore_routes;
	ret = ums9117_sc2720_codec_sts2_update(SC2720_PLUGIN, SC2720_PLUGIN);
	if (ret)
		goto restore_routes;
	ret = ums9117_sc2720_codec_sts2_update(SC2720_DEPOP_EN,
					       SC2720_DEPOP_EN);
	if (ret)
		goto restore_routes;
	usleep_range(2000, 3000);
	ret = ums9117_sc2720_codec_sts2_update(SC2720_DEPOP_CHARGE_START,
					       SC2720_DEPOP_CHARGE_START);
	if (ret)
		goto restore_routes;
	ret = ums9117_sc2720_codec_wait_depop_charge(codec);

restore_routes:
	restore_ret = ums9117_adi_write_once(SC2720_ANA_CDC2, saved_cdc2);
	ums9117_adi_record_first_error(&restore_error, restore_ret);
	restore_ret = ums9117_adi_write_once(SC2720_ANA_CDC3, saved_cdc3);
	ums9117_adi_record_first_error(&restore_error, restore_ret);
	if (!restore_error)
		return ret;
	if (ret) {
		dev_err(codec->dev,
			"cannot restore codec routes after headphone calibration error: %d\n",
			restore_error);
		return ret;
	}
	return restore_error;
}

static int ums9117_sc2720_codec_apply_volume(unsigned int left,
					     unsigned int right)
{
	u16 gain = (sc2720_hp_gain[left] << SC2720_HPL_GAIN_SHIFT) |
		   sc2720_hp_gain[right];

	return ums9117_adi_update_bits_once(SC2720_ANA_CDC4,
					    SC2720_HP_GAIN_MASK, gain);
}

void ums9117_sc2720_codec_get_volume(struct ums9117_sc2720_codec *codec,
				     unsigned int *left, unsigned int *right)
{
	*left = codec->volume_left;
	*right = codec->volume_right;
}

int ums9117_sc2720_codec_set_volume(struct ums9117_sc2720_codec *codec,
				    unsigned int left, unsigned int right)
{
	int ret;

	if (left == codec->volume_left && right == codec->volume_right)
		return 0;
	if (codec->enabled) {
		ret = ums9117_sc2720_codec_apply_volume(left, right);
		if (ret)
			return ret;
	}
	codec->volume_left = left;
	codec->volume_right = right;
	return 1;
}

static int
ums9117_sc2720_codec_open_headphones(struct ums9117_sc2720_codec *codec)
{
	int ret;

	msleep(80);
	ret = ums9117_adi_update_bits_once(SC2720_ANA_CDC2, SC2720_HPL_EN,
					   SC2720_HPL_EN);
	if (ret)
		return ret;
	ret = ums9117_adi_update_bits_once(SC2720_ANA_CDC2, SC2720_HPR_EN,
					   SC2720_HPR_EN);
	if (ret)
		return ret;
	ret = ums9117_adi_update_bits_once(SC2720_ANA_CDC2, SC2720_HP_BUFFER_EN,
					   SC2720_HP_BUFFER_EN);
	if (ret)
		return ret;
	ret = ums9117_adi_update_bits_once(
		SC2720_ANA_CDC2, SC2720_HPL_DUMMY_LOOP, SC2720_HPL_DUMMY_LOOP);
	if (ret)
		return ret;
	ret = ums9117_adi_update_bits_once(
		SC2720_ANA_CDC2, SC2720_HPR_DUMMY_LOOP, SC2720_HPR_DUMMY_LOOP);
	if (ret)
		return ret;
	ret = ums9117_adi_update_bits_once(SC2720_ANA_CDC2,
					   SC2720_HPL_DUMMY_LOOP_END, 0);
	if (ret)
		return ret;
	ret = ums9117_adi_update_bits_once(SC2720_ANA_CDC2,
					   SC2720_HPR_DUMMY_LOOP_END, 0);
	if (ret)
		return ret;
	usleep_range(1000, 2000);
	ret = ums9117_adi_update_bits_once(SC2720_ANA_CDC2,
					   SC2720_HPL_DUMMY_LOOP, 0);
	if (ret)
		return ret;
	ret = ums9117_adi_update_bits_once(SC2720_ANA_CDC2,
					   SC2720_HPR_DUMMY_LOOP, 0);
	if (ret)
		return ret;
	ret = ums9117_adi_update_bits_once(SC2720_ANA_CDC2,
					   SC2720_HPL_DUMMY_LOOP_END,
					   SC2720_HPL_DUMMY_LOOP_END);
	if (ret)
		return ret;
	ret = ums9117_adi_update_bits_once(SC2720_ANA_CDC2,
					   SC2720_HPR_DUMMY_LOOP_END,
					   SC2720_HPR_DUMMY_LOOP_END);
	if (ret)
		return ret;
	ret = ums9117_adi_update_bits_once(SC2720_ANA_CDC2,
					   SC2720_DAC_EN_L_ANALOG,
					   SC2720_DAC_EN_L_ANALOG);
	if (ret)
		return ret;
	ret = ums9117_adi_update_bits_once(SC2720_ANA_CDC2,
					   SC2720_DAC_EN_R_ANALOG,
					   SC2720_DAC_EN_R_ANALOG);
	if (ret)
		return ret;
	ret = ums9117_adi_update_bits_once(SC2720_ANA_CDC3, SC2720_DACL_TO_HPL,
					   SC2720_DACL_TO_HPL);
	if (ret)
		return ret;
	ret = ums9117_adi_update_bits_once(SC2720_ANA_CDC3, SC2720_DACR_TO_HPR,
					   SC2720_DACR_TO_HPR);
	if (ret)
		return ret;
	return ums9117_sc2720_codec_apply_volume(codec->volume_left,
						 codec->volume_right);
}

int ums9117_sc2720_codec_prepare(struct ums9117_sc2720_codec *codec)
{
	int restore_ret;
	int ret;

	if (codec->prepared)
		return 0;
	if (codec->dirty || codec->xtal_held) {
		ret = ums9117_sc2720_codec_restore(codec);
		if (ret)
			return ret;
	}
	if (ums9117_adi_is_poisoned())
		return -EIO;
	ret = ums9117_sc2720_codec_check_identity();
	if (ret)
		return ret;
	ret = ums9117_sc2720_codec_snapshot(codec);
	if (ret)
		return ret;
	ret = ums9117_sc2720_codec_validate_idle(codec);
	if (ret)
		return ret;

	/* The analog audio clocks also require the shared 26 MHz output. */
	ret = ums9117_adi_xtal_acquire(&codec->xtal_held);
	if (ret)
		goto release_xtal;
	codec->dirty = true;
	ret = ums9117_sc2720_codec_power_up();
	if (!ret)
		ret = ums9117_sc2720_codec_calibrate_headphones(codec);
	if (ret) {
		restore_ret = ums9117_sc2720_codec_restore(codec);
		if (restore_ret)
			dev_err(codec->dev,
				"cannot restore codec after prepare error: %d\n",
				restore_ret);
		return ret;
	}
	codec->prepared = true;
	return 0;

release_xtal:
	if (codec->xtal_held) {
		restore_ret = ums9117_adi_xtal_release(&codec->xtal_held);
		if (restore_ret)
			dev_err(codec->dev,
				"cannot release audio clock after prepare error: %d\n",
				restore_ret);
	}
	return ret;
}

int ums9117_sc2720_codec_enable(struct ums9117_sc2720_codec *codec)
{
	int close_ret;
	int ret;

	if (codec->enabled)
		return 0;
	if (!codec->prepared)
		return -EINVAL;

	/* Reopen only the playback pieces; the calibrated base stays powered. */
	ret = ums9117_adi_update_bits_once(
		SC2720_ANA_PMU0, SC2720_AUD_BG_EN | SC2720_AUD_VB_SLEEP_PD,
		SC2720_AUD_BG_EN | SC2720_AUD_VB_SLEEP_PD);
	if (ret)
		goto failed;
	usleep_range(5000, 6000);
	ret = ums9117_adi_update_bits_once(
		SC2720_ANA_CLK0, SC2720_DA_CLK_EN | SC2720_DRV_CLK_EN,
		SC2720_DA_CLK_EN | SC2720_DRV_CLK_EN);
	if (ret)
		goto failed;
	ret = ums9117_adi_update_bits_once(SC2720_AUD_CFGA_LP_MODULE_CTRL,
					   SC2720_DAC_ENABLE,
					   SC2720_DAC_ENABLE);
	if (ret)
		goto failed;
	ret = ums9117_adi_update_bits_once(
		SC2720_ANA_CDC3, SC2720_DALR_OFFSET_EN, SC2720_DALR_OFFSET_EN);
	if (ret)
		goto failed;
	ret = ums9117_sc2720_codec_open_headphones(codec);
	if (ret)
		goto failed;
	codec->enabled = true;
	return 0;

failed:
	close_ret = ums9117_sc2720_codec_close_headphones();
	if (close_ret)
		dev_err(codec->dev,
			"cannot close headphones after enable error: %d\n",
			close_ret);
	return ret;
}

int ums9117_sc2720_codec_stop(struct ums9117_sc2720_codec *codec)
{
	int ret;

	if (!codec->prepared)
		return 0;

	ret = ums9117_sc2720_codec_close_headphones();
	if (ret)
		return ret;
	ret = ums9117_adi_update_bits_once(SC2720_AUD_CFGA_LP_MODULE_CTRL,
					   SC2720_DAC_ENABLE, 0);
	if (ret)
		return ret;
	ret = ums9117_adi_update_bits_once(SC2720_ANA_CDC3,
					   SC2720_DALR_OFFSET_EN, 0);
	if (ret)
		return ret;
	ret = ums9117_adi_update_bits_once(
		SC2720_ANA_CLK0, SC2720_DA_CLK_EN | SC2720_DRV_CLK_EN, 0);
	if (ret)
		return ret;
	ret = ums9117_adi_update_bits_once(
		SC2720_ANA_PMU0, SC2720_AUD_BG_EN | SC2720_AUD_VB_SLEEP_PD, 0);
	if (ret)
		return ret;
	codec->enabled = false;
	return 0;
}

int ums9117_sc2720_codec_disable(struct ums9117_sc2720_codec *codec)
{
	return ums9117_sc2720_codec_restore(codec);
}

static void ums9117_sc2720_codec_release(void *data)
{
	struct ums9117_sc2720_codec *codec = data;
	int ret;

	ret = ums9117_sc2720_codec_disable(codec);
	if (ret)
		dev_err(codec->dev, "cannot restore codec during removal: %d\n",
			ret);
}

struct ums9117_sc2720_codec *ums9117_sc2720_codec_create(struct device *dev)
{
	struct ums9117_sc2720_codec *codec;
	int ret;

	codec = devm_kzalloc(dev, sizeof(*codec), GFP_KERNEL);
	if (!codec)
		return ERR_PTR(-ENOMEM);
	codec->dev = dev;
	codec->volume_left = SC2720_HP_VOLUME_DEFAULT;
	codec->volume_right = SC2720_HP_VOLUME_DEFAULT;
	ret = devm_add_action_or_reset(dev, ums9117_sc2720_codec_release,
				       codec);
	if (ret)
		return ERR_PTR(ret);
	return codec;
}
