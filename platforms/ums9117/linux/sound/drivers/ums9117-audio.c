// SPDX-License-Identifier: GPL-2.0-only
#include <linux/err.h>
#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/mfd/syscon.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/slab.h>

#include "ums9117-audio.h"

#define UMS9117_AON_EB0 0x0000
#define UMS9117_AON_RST0 0x0008
#define UMS9117_AON_VBC_CTRL 0x0020
#define UMS9117_AON_SET 0x1000
#define UMS9117_AON_CLEAR 0x2000
#define UMS9117_AUDIO_GATES GENMASK(19, 17)
#define UMS9117_AUDIO_RESETS GENMASK(20, 18)
#define UMS9117_AUDIO_OWNERS (GENMASK(19, 18) | GENMASK(11, 10) | GENMASK(5, 4))

#define UMS9117_CLK_AUD 0x00
#define UMS9117_CLK_AUDIF 0x04
#define UMS9117_CLK_VBC 0x08
#define UMS9117_CLK_DA0 0x0c

#define UMS9117_IIS_MATRIX_INF2 GENMASK(3, 2)
#define UMS9117_IIS_MATRIX_INF3 GENMASK(5, 4)
#define UMS9117_IIS_MATRIX_AUDIO \
	(UMS9117_IIS_MATRIX_INF2 | UMS9117_IIS_MATRIX_INF3)

#define UMS9117_AUD_TOP 0x00
#define UMS9117_AUD_CLEAR 0x04
#define UMS9117_AUD_IIS 0x08
#define UMS9117_AUD_DAC 0x0c
#define UMS9117_AUD_SDM0 0x10
#define UMS9117_AUD_SDM1 0x14
#define UMS9117_AUD_ADC_SRC 0x18
#define UMS9117_AUD_STS0 0x20
#define UMS9117_AUD_SDM_DC_LOW 0x38
#define UMS9117_AUD_SDM_DC_HIGH 0x3c
#define UMS9117_AUD_ADC_FIFO_STATUS 0x44
#define UMS9117_AUD_INTERFACE_STATUS 0x4c
#define UMS9117_AUD_ADC_LEFT BIT(1)
#define UMS9117_AUD_DAC_CHANNELS (BIT(0) | BIT(2))
#define UMS9117_AUD_IIS_ADC_FIELDS \
	(BIT(15) | BIT(13) | GENMASK(10, 9) | BIT(5) | BIT(2))
#define UMS9117_AUD_IIS_DAC_FIELDS \
	(BIT(14) | BIT(12) | BIT(11) | GENMASK(8, 7) | BIT(3) | BIT(1))
#define UMS9117_AUD_IIS_DAC_24BIT 0x180
#define UMS9117_AUD_DAC_MUTE BIT(14)
#define UMS9117_AUD_DAC_24K 0x80b4
#define UMS9117_AUD_DAC_32K 0x80b3
#define UMS9117_AUD_DAC_48K 0x80b1
#define UMS9117_AUD_STS0_MUTE_MASK GENMASK(1, 0)
#define UMS9117_AUD_STS0_MUTED 2
#define UMS9117_AUD_MUTE_POLL_US 1000
#define UMS9117_AUD_MUTE_TIMEOUT_US 600000

#define UMS9117_VBC_CLEAR 0x00
#define UMS9117_VBC_ENABLE 0x18
#define UMS9117_VBC_FORMAT 0x38
#define UMS9117_VBC_IIS 0x3c
#define UMS9117_VBC_DAC_PATH 0x40
#define UMS9117_VBC_DAC_DG_CTRL 0x44
#define UMS9117_VBC_DAC_ST_CTL0 0x78
#define UMS9117_VBC_DAC_ST_CTL1 0x7c
#define UMS9117_VBC_ADC_PATH 0x80
#define UMS9117_VBC_ADC_SRC 0x90
#define UMS9117_VBC_INT_ENABLE 0xa0
#define UMS9117_VBC_DAC0_STATUS 0xbc
#define UMS9117_VBC_DAC1_STATUS 0xc0
#define UMS9117_VBC_CHANNELS 0xc8
#define UMS9117_VBC_ADC0_STATUS 0xd4
#define UMS9117_VBC_ADC1_STATUS 0xd8
#define UMS9117_VBC_DAC_LEVEL 0xe4
#define UMS9117_VBC_ADC_LEVEL 0xec
#define UMS9117_VBC_FM_MUTE 0xf8
#define UMS9117_VBC_TONE_CTRL 0x900
#define UMS9117_VBC_VT_SIN 0x918
#define UMS9117_VBC_VT_COS 0x91c
#define UMS9117_VBC_VT_LEVEL 0x920
#define UMS9117_VBC_VT_RAMP 0x924
#define UMS9117_VBC_VT_HOLD 0x928
#define UMS9117_VBC_TONE_ENABLE 0x92c
#define UMS9117_VBC_DAC0_DATA 0x1000
#define UMS9117_VBC_DAC1_DATA 0x1004
#define UMS9117_VBC_ADC1_DATA 0x1014
#define UMS9117_VBC_ADC1_CHANNEL BIT(5)
#define UMS9117_VBC_DAC_CHANNELS (BIT(0) | BIT(1))
#define UMS9117_VBC_FIFO_EMPTY BIT(18)
#define UMS9117_VBC_FIFO_FULL BIT(19)
#define UMS9117_VBC_FIFO_READ GENMASK(17, 9)
#define UMS9117_VBC_FIFO_WRITE GENMASK(8, 0)
#define UMS9117_VBC_DAC0_DG_GAIN GENMASK(6, 0)
#define UMS9117_VBC_DAC1_DG_GAIN GENMASK(13, 7)
#define UMS9117_VBC_DAC0_DG_ENABLE BIT(14)
#define UMS9117_VBC_DAC1_DG_ENABLE BIT(15)
#define UMS9117_VBC_DAC_DG_MASK GENMASK(15, 0)
#define UMS9117_VBC_ST_DG_GAIN GENMASK(10, 4)
#define UMS9117_VBC_ST_DG_ENABLE BIT(12)
/* The largest DG code is the strongest attenuation, about -77.5 dB. */
#define UMS9117_VBC_DAC_DG_MINIMUM 0x7f
#define UMS9117_VBC_IIS_ADC01_SELECT GENMASK(2, 0)
#define UMS9117_VBC_IIS_DAC_SELECT GENMASK(8, 6)
#define UMS9117_VBC_IIS_ADC01_FM 1
#define UMS9117_VBC_DAC_PATH_FM BIT(8)
#define UMS9117_VBC_DAC0_ADD_FM GENMASK(1, 0)
#define UMS9117_VBC_DAC1_ADD_FM GENMASK(3, 2)
#define UMS9117_VBC_ADC01_CHANNELS GENMASK(5, 4)
#define UMS9117_VBC_FM_MUTE_ENABLE GENMASK(14, 13)
#define UMS9117_VBC_FM_MUTE_UNMUTE BIT(16)
#define UMS9117_VBC_FM_MUTE_STEP 0x1e
#define UMS9117_VBC_FM_PREFILL_FRAMES 64U
#define UMS9117_VBC_VT_ON BIT(1)
#define UMS9117_VBC_VT_ENABLE BIT(2)
#define UMS9117_VBC_VT_LEVEL1 GENMASK(31, 16)
#define UMS9117_VBC_VT_LEVEL0 GENMASK(15, 0)
#define UMS9117_VBC_VT_RISE GENMASK(31, 16)
#define UMS9117_VBC_VT_FALL GENMASK(15, 0)

struct ums9117_audio {
	struct device *dev;
	struct regmap *aon;
	void __iomem *aud;
	void __iomem *vbc;
	void __iomem *clk;
	void __iomem *iis_matrix;
	u32 acquired_gates;
	u32 saved_iis_matrix;
	struct ums9117_audio_vibrate_tone vibrate_tone;
	u8 dac_left_gain;
	u8 dac_right_gain;
	unsigned int playback_rate;
	enum ums9117_audio_dac_rate dac_rate;
	bool dac_gain_configured;
	bool vibrate_tone_fitted;
	bool vibration;
	bool music_muted;
	bool prepared;
	bool playback_prepared;
	bool fm_prepared;
	bool capture_prepared;
};

static void update_bits(void __iomem *base, u32 reg, u32 mask, u32 value)
{
	writel((readl(base + reg) & ~mask) | (value & mask), base + reg);
}

static void clear_fifos(struct ums9117_audio *audio)
{
	/* Independent write-one strobes, not a read/modify/write register. */
	writel(BIT(0), audio->vbc + UMS9117_VBC_CLEAR);
	writel(BIT(1), audio->vbc + UMS9117_VBC_CLEAR);
}

static void apply_dac_gain(struct ums9117_audio *audio)
{
	u8 left_gain = audio->dac_left_gain;
	u8 right_gain = audio->dac_right_gain;
	u32 value;

	if (!audio->dac_gain_configured)
		return;
	if (audio->music_muted) {
		left_gain = UMS9117_VBC_DAC_DG_MINIMUM;
		right_gain = UMS9117_VBC_DAC_DG_MINIMUM;
	}
	/* The serial path maps DAC1 to left and DAC0 to right. */
	value = FIELD_PREP(UMS9117_VBC_DAC1_DG_GAIN, left_gain) |
		FIELD_PREP(UMS9117_VBC_DAC0_DG_GAIN, right_gain) |
		UMS9117_VBC_DAC1_DG_ENABLE | UMS9117_VBC_DAC0_DG_ENABLE;
	update_bits(audio->vbc, UMS9117_VBC_DAC_DG_CTRL,
		    UMS9117_VBC_DAC_DG_MASK, value);
	/*
	 * FM enters the DAC path after DAC DG and is attenuated by the side-tone
	 * gain, which has the same code scale but acts only while its enable bit
	 * is set. The side-tone output is not mixed into PCM playback, whose
	 * DAC path leaves the add-ST fields clear, so PCM is unaffected. Pair
	 * ST1 with DAC1 (left) and ST0 with DAC0 (right).
	 */
	update_bits(audio->vbc, UMS9117_VBC_DAC_ST_CTL1,
		    UMS9117_VBC_ST_DG_ENABLE | UMS9117_VBC_ST_DG_GAIN,
		    UMS9117_VBC_ST_DG_ENABLE |
			    FIELD_PREP(UMS9117_VBC_ST_DG_GAIN, left_gain));
	update_bits(audio->vbc, UMS9117_VBC_DAC_ST_CTL0,
		    UMS9117_VBC_ST_DG_ENABLE | UMS9117_VBC_ST_DG_GAIN,
		    UMS9117_VBC_ST_DG_ENABLE |
			    FIELD_PREP(UMS9117_VBC_ST_DG_GAIN, right_gain));
}

void ums9117_audio_set_dac_gain(struct ums9117_audio *audio, u8 left_gain,
				u8 right_gain)
{
	audio->dac_left_gain = left_gain;
	audio->dac_right_gain = right_gain;
	audio->dac_gain_configured = true;
	if (audio->playback_prepared)
		apply_dac_gain(audio);
}

static u32 fm_mute_control(const struct ums9117_audio *audio)
{
	u32 value = UMS9117_VBC_FM_MUTE_ENABLE | UMS9117_VBC_FM_MUTE_STEP;

	return audio->music_muted ? value : value | UMS9117_VBC_FM_MUTE_UNMUTE;
}

void ums9117_audio_set_music_mute(struct ums9117_audio *audio, bool mute)
{
	if (audio->music_muted == mute)
		return;
	audio->music_muted = mute;
	if (audio->playback_prepared)
		apply_dac_gain(audio);
	/* FM joins the DAC path after DG and ramps on its own mute control. */
	if (audio->fm_prepared)
		writel(fm_mute_control(audio),
		       audio->vbc + UMS9117_VBC_FM_MUTE);
}

void ums9117_audio_set_vibrate_tone(
	struct ums9117_audio *audio,
	const struct ums9117_audio_vibrate_tone *tone)
{
	audio->vibrate_tone = *tone;
	audio->vibrate_tone_fitted = true;
}

static void write_vibrate_tone_on(struct ums9117_audio *audio, bool on)
{
	update_bits(audio->vbc, UMS9117_VBC_TONE_CTRL, UMS9117_VBC_VT_ON,
		    on ? UMS9117_VBC_VT_ON : 0);
}

/*
 * The generator latches off when its hold time expires until the enable is
 * cycled, so each pulse re-arms it. The caller only does this while the tone
 * is off; the enable must not change during a tone.
 */
static void arm_vibrate_tone(struct ums9117_audio *audio)
{
	update_bits(audio->vbc, UMS9117_VBC_TONE_ENABLE, UMS9117_VBC_VT_ENABLE,
		    0);
	update_bits(audio->vbc, UMS9117_VBC_TONE_ENABLE, UMS9117_VBC_VT_ENABLE,
		    UMS9117_VBC_VT_ENABLE);
}

/* Called while the DAC is muted, before the requested tone state is set. */
static void program_vibrate_tone(struct ums9117_audio *audio)
{
	const struct ums9117_audio_vibrate_tone *tone = &audio->vibrate_tone;

	if (!audio->vibrate_tone_fitted)
		return;
	write_vibrate_tone_on(audio, false);
	update_bits(audio->vbc, UMS9117_VBC_TONE_ENABLE, UMS9117_VBC_VT_ENABLE,
		    0);
	writel(tone->sin[audio->dac_rate], audio->vbc + UMS9117_VBC_VT_SIN);
	writel(tone->cos[audio->dac_rate], audio->vbc + UMS9117_VBC_VT_COS);
	writel(FIELD_PREP(UMS9117_VBC_VT_LEVEL0, tone->level[0]) |
		       FIELD_PREP(UMS9117_VBC_VT_LEVEL1, tone->level[1]),
	       audio->vbc + UMS9117_VBC_VT_LEVEL);
	writel(FIELD_PREP(UMS9117_VBC_VT_FALL, tone->fall) |
		       FIELD_PREP(UMS9117_VBC_VT_RISE, tone->rise),
	       audio->vbc + UMS9117_VBC_VT_RAMP);
	writel(tone->hold, audio->vbc + UMS9117_VBC_VT_HOLD);
	update_bits(audio->vbc, UMS9117_VBC_TONE_ENABLE, UMS9117_VBC_VT_ENABLE,
		    UMS9117_VBC_VT_ENABLE);
	write_vibrate_tone_on(audio, audio->vibration);
}

void ums9117_audio_set_vibration(struct ums9117_audio *audio, bool on)
{
	if (audio->vibration == on)
		return;
	audio->vibration = on;
	if (!audio->playback_prepared || !audio->vibrate_tone_fitted)
		return;
	if (on)
		arm_vibrate_tone(audio);
	write_vibrate_tone_on(audio, on);
}

static int fifo_frames(u32 status)
{
	unsigned int read_ptr = FIELD_GET(UMS9117_VBC_FIFO_READ, status);
	unsigned int write_ptr = FIELD_GET(UMS9117_VBC_FIFO_WRITE, status);

	if (read_ptr >= UMS9117_AUDIO_FIFO_FRAMES ||
	    write_ptr >= UMS9117_AUDIO_FIFO_FRAMES)
		return -EIO;
	if (status & UMS9117_VBC_FIFO_EMPTY)
		return 0;
	if (status & UMS9117_VBC_FIFO_FULL)
		return UMS9117_AUDIO_FIFO_FRAMES;
	return (write_ptr + UMS9117_AUDIO_FIFO_FRAMES - read_ptr) %
	       UMS9117_AUDIO_FIFO_FRAMES;
}

int ums9117_audio_queued(struct ums9117_audio *audio)
{
	int left = fifo_frames(readl(audio->vbc + UMS9117_VBC_DAC1_STATUS));
	int right = fifo_frames(readl(audio->vbc + UMS9117_VBC_DAC0_STATUS));

	if (left < 0 || right < 0)
		return -EIO;
	/* Sequential status reads can straddle a sample consumption edge. */
	return max(left, right);
}

void ums9117_audio_write(struct ums9117_audio *audio, u16 left, u16 right)
{
	/* PIO uses a sign-extended word for each signed 16-bit sample. */
	/* The SC2720 serial path maps DAC1 to left and DAC0 to right. */
	writel((s16)left, audio->vbc + UMS9117_VBC_DAC1_DATA);
	writel((s16)right, audio->vbc + UMS9117_VBC_DAC0_DATA);
}

int ums9117_audio_capture_available(struct ums9117_audio *audio)
{
	u32 status = readl(audio->vbc + UMS9117_VBC_ADC1_STATUS);

	/* Once full, continuity cannot be guaranteed even if no read was lost. */
	if (status & UMS9117_VBC_FIFO_FULL)
		return -EPIPE;
	return fifo_frames(status);
}

u16 ums9117_audio_read_capture(struct ums9117_audio *audio)
{
	return readl(audio->vbc + UMS9117_VBC_ADC1_DATA);
}

void ums9117_audio_report_capture(struct ums9117_audio *audio,
				  const char *reason)
{
	dev_err_ratelimited(
		audio->dev,
		"capture %s: AUDIF=%#x AUD-ADC=%#x ADC0=%#x ADC1=%#x\n", reason,
		readl(audio->aud + UMS9117_AUD_INTERFACE_STATUS),
		readl(audio->aud + UMS9117_AUD_ADC_FIFO_STATUS),
		readl(audio->vbc + UMS9117_VBC_ADC0_STATUS),
		readl(audio->vbc + UMS9117_VBC_ADC1_STATUS));
}

void ums9117_audio_start_capture(struct ums9117_audio *audio)
{
	/* Logical ADC-L uses ADC1; both receive FIFO enables are required. */
	update_bits(audio->vbc, UMS9117_VBC_CHANNELS,
		    UMS9117_VBC_ADC01_CHANNELS, UMS9117_VBC_ADC1_CHANNEL);
	update_bits(audio->vbc, UMS9117_VBC_ENABLE, UMS9117_VBC_ADC01_CHANNELS,
		    UMS9117_VBC_ADC01_CHANNELS);
	update_bits(audio->aud, UMS9117_AUD_TOP, UMS9117_AUD_ADC_LEFT,
		    UMS9117_AUD_ADC_LEFT);
	readl(audio->aud + UMS9117_AUD_TOP);
}

void ums9117_audio_stop_capture(struct ums9117_audio *audio)
{
	if (!audio->capture_prepared)
		return;
	/* The owner stops the SC2720 producer before the receiver. */
	update_bits(audio->aud, UMS9117_AUD_TOP, UMS9117_AUD_ADC_LEFT, 0);
	update_bits(audio->vbc, UMS9117_VBC_ENABLE, UMS9117_VBC_ADC01_CHANNELS,
		    0);
	writel(BIT(4), audio->vbc + UMS9117_VBC_CLEAR);
	writel(BIT(5), audio->vbc + UMS9117_VBC_CLEAR);
	update_bits(audio->vbc, UMS9117_VBC_CHANNELS,
		    UMS9117_VBC_ADC01_CHANNELS, 0);
	readl(audio->vbc + UMS9117_VBC_ENABLE);
}

void ums9117_audio_start(struct ums9117_audio *audio)
{
	update_bits(audio->aud, UMS9117_AUD_TOP, UMS9117_AUD_DAC_CHANNELS,
		    UMS9117_AUD_DAC_CHANNELS);
	update_bits(audio->aud, UMS9117_AUD_DAC, UMS9117_AUD_DAC_MUTE, 0);
	readl(audio->aud + UMS9117_AUD_DAC);
}

void ums9117_audio_stop(struct ums9117_audio *audio)
{
	u32 status;
	int ret;

	if (!audio->playback_prepared)
		return;
	update_bits(audio->aud, UMS9117_AUD_DAC, UMS9117_AUD_DAC_MUTE,
		    UMS9117_AUD_DAC_MUTE);
	/* The mute ramp needs the DAC lanes and clocks until it finishes. */
	if (readl(audio->aud + UMS9117_AUD_TOP) & UMS9117_AUD_DAC_CHANNELS) {
		ret = readl_poll_timeout(
			audio->aud + UMS9117_AUD_STS0, status,
			(status & UMS9117_AUD_STS0_MUTE_MASK) ==
				UMS9117_AUD_STS0_MUTED,
			UMS9117_AUD_MUTE_POLL_US, UMS9117_AUD_MUTE_TIMEOUT_US);
		if (ret)
			dev_warn(audio->dev,
				 "DAC mute did not complete: status=%#x\n",
				 status);
	}
	update_bits(audio->aud, UMS9117_AUD_TOP, UMS9117_AUD_DAC_CHANNELS, 0);
	if (audio->vibrate_tone_fitted) {
		write_vibrate_tone_on(audio, false);
		update_bits(audio->vbc, UMS9117_VBC_TONE_ENABLE,
			    UMS9117_VBC_VT_ENABLE, 0);
	}
	if (audio->fm_prepared) {
		update_bits(audio->vbc, UMS9117_VBC_FM_MUTE,
			    UMS9117_VBC_FM_MUTE_UNMUTE, 0);
		writel(0, audio->vbc + UMS9117_VBC_DAC_PATH);
	}
	update_bits(audio->vbc, UMS9117_VBC_ENABLE, UMS9117_VBC_DAC_CHANNELS,
		    0);
	clear_fifos(audio);
	update_bits(audio->vbc, UMS9117_VBC_CHANNELS, UMS9117_VBC_DAC_CHANNELS,
		    0);
	if (audio->fm_prepared) {
		update_bits(audio->vbc, UMS9117_VBC_IIS,
			    UMS9117_VBC_IIS_ADC01_SELECT, 0);
		update_bits(audio->vbc, UMS9117_VBC_CHANNELS,
			    UMS9117_VBC_ADC01_CHANNELS, 0);
		update_bits(audio->iis_matrix, 0, UMS9117_IIS_MATRIX_AUDIO,
			    audio->saved_iis_matrix);
		audio->fm_prepared = false;
	}
	readl(audio->vbc + UMS9117_VBC_ENABLE);
}

static void release_clocks(struct ums9117_audio *audio)
{
	int ret;

	audio->prepared = false;
	if (!audio->acquired_gates)
		return;
	ret = regmap_write(audio->aon, UMS9117_AON_EB0 + UMS9117_AON_CLEAR,
			   audio->acquired_gates);
	if (ret)
		dev_err(audio->dev, "cannot release audio clocks: %pe\n",
			ERR_PTR(ret));
	else
		audio->acquired_gates = 0;
}

void ums9117_audio_release(struct ums9117_audio *audio)
{
	if (!audio->playback_prepared)
		return;
	ums9117_audio_stop(audio);
	audio->playback_prepared = false;
	if (!audio->capture_prepared)
		release_clocks(audio);
}

void ums9117_audio_release_capture(struct ums9117_audio *audio)
{
	if (!audio->capture_prepared)
		return;
	ums9117_audio_stop_capture(audio);
	audio->capture_prepared = false;
	if (!audio->playback_prepared)
		release_clocks(audio);
}

static int check_clocks(struct ums9117_audio *audio)
{
	unsigned int value;
	int ret;

	ret = regmap_read(audio->aon, UMS9117_AON_RST0, &value);
	if (ret)
		return ret;
	if (value & UMS9117_AUDIO_RESETS)
		return -EBUSY;
	ret = regmap_read(audio->aon, UMS9117_AON_VBC_CTRL, &value);
	if (ret)
		return ret;
	if (value & UMS9117_AUDIO_OWNERS)
		return -EBUSY;
	if ((readl(audio->clk + UMS9117_CLK_AUD) & BIT(0)) ||
	    (readl(audio->clk + UMS9117_CLK_AUDIF) & GENMASK(1, 0)) ||
	    (readl(audio->clk + UMS9117_CLK_VBC) & BIT(0)) ||
	    (readl(audio->clk + UMS9117_CLK_DA0) & BIT(16)))
		return -EBUSY;
	return 0;
}

static int pulse_reset(struct ums9117_audio *audio, u32 mask)
{
	int ret;

	ret = regmap_write(audio->aon, UMS9117_AON_RST0 + UMS9117_AON_SET,
			   mask);
	if (ret)
		return ret;
	udelay(1);
	return regmap_write(audio->aon, UMS9117_AON_RST0 + UMS9117_AON_CLEAR,
			    mask);
}

static int prepare_clocks(struct ums9117_audio *audio)
{
	unsigned int value;
	int ret;

	if (audio->prepared)
		return 0;
	ret = check_clocks(audio);
	if (ret)
		return dev_err_probe(audio->dev, ret,
				     "audio clocks or ownership unavailable\n");
	ret = regmap_read(audio->aon, UMS9117_AON_EB0, &value);
	if (ret)
		return ret;
	audio->acquired_gates = UMS9117_AUDIO_GATES & ~value;
	ret = regmap_write(audio->aon, UMS9117_AON_EB0 + UMS9117_AON_SET,
			   audio->acquired_gates);
	if (ret)
		goto failed;
	ret = pulse_reset(audio, BIT(20));
	if (ret)
		goto failed;
	/* VBC reset leaves EQ6, EQ4 and ALC disabled; profiles supply gain only. */
	ret = pulse_reset(audio, GENMASK(19, 18));
	if (ret)
		goto failed;
	usleep_range(5000, 6000);
	writel(0, audio->aud + UMS9117_AUD_TOP);
	writel(0, audio->vbc + UMS9117_VBC_ENABLE);
	writel(0, audio->vbc + UMS9117_VBC_INT_ENABLE);
	writel(0, audio->vbc + UMS9117_VBC_FORMAT);
	writel(0, audio->vbc + UMS9117_VBC_IIS);
	writel(0, audio->vbc + UMS9117_VBC_CHANNELS);
	audio->prepared = true;
	return 0;

failed:
	release_clocks(audio);
	return ret;
}

int ums9117_audio_prepare_capture(struct ums9117_audio *audio)
{
	int ret;

	if (audio->fm_prepared)
		return -EBUSY;
	if (audio->playback_prepared && audio->playback_rate != 48000)
		return -EINVAL;
	if (audio->capture_prepared)
		return 0;
	ret = prepare_clocks(audio);
	if (ret)
		return ret;
	audio->capture_prepared = true;
	update_bits(audio->aud, UMS9117_AUD_TOP, UMS9117_AUD_ADC_LEFT, 0);
	update_bits(audio->aud, UMS9117_AUD_IIS, UMS9117_AUD_IIS_ADC_FIELDS, 0);
	/* ADC sample rate is N * 4000 Hz. */
	writel(12, audio->aud + UMS9117_AUD_ADC_SRC);
	writel(BIT(0), audio->aud + UMS9117_AUD_CLEAR);
	update_bits(audio->vbc, UMS9117_VBC_ENABLE, UMS9117_VBC_ADC01_CHANNELS,
		    0);
	update_bits(audio->vbc, UMS9117_VBC_IIS, UMS9117_VBC_IIS_ADC01_SELECT,
		    0);
	writel(0, audio->vbc + UMS9117_VBC_ADC_PATH);
	writel(0, audio->vbc + UMS9117_VBC_ADC_SRC);
	update_bits(audio->vbc, UMS9117_VBC_CHANNELS,
		    UMS9117_VBC_ADC01_CHANNELS, 0);
	writel(0xa0a0, audio->vbc + UMS9117_VBC_ADC_LEVEL);
	writel(BIT(4), audio->vbc + UMS9117_VBC_CLEAR);
	writel(BIT(5), audio->vbc + UMS9117_VBC_CLEAR);
	ret = ums9117_audio_capture_available(audio);
	if (!ret)
		return 0;
	ums9117_audio_release_capture(audio);
	return ret < 0 ? ret : -EIO;
}

int ums9117_audio_prepare(struct ums9117_audio *audio, unsigned int rate)
{
	enum ums9117_audio_dac_rate dac_rate;
	u32 dac;
	int ret;

	switch (rate) {
	case 24000:
		dac = UMS9117_AUD_DAC_24K;
		dac_rate = UMS9117_AUDIO_DAC_24K;
		break;
	case 32000:
		dac = UMS9117_AUD_DAC_32K;
		dac_rate = UMS9117_AUDIO_DAC_32K;
		break;
	case 48000:
		dac = UMS9117_AUD_DAC_48K;
		dac_rate = UMS9117_AUDIO_DAC_48K;
		break;
	default:
		return -EINVAL;
	}
	if (audio->fm_prepared)
		return -EBUSY;
	if (audio->capture_prepared && rate != 48000)
		return -EINVAL;
	if (audio->playback_prepared)
		return audio->playback_rate == rate ? 0 : -EBUSY;
	ret = prepare_clocks(audio);
	if (ret)
		return ret;
	audio->playback_prepared = true;
	audio->playback_rate = rate;
	audio->dac_rate = dac_rate;
	update_bits(audio->aud, UMS9117_AUD_TOP, UMS9117_AUD_DAC_CHANNELS, 0);
	writel(0x100, audio->aud + UMS9117_AUD_SDM0);
	writel(0x8, audio->aud + UMS9117_AUD_SDM1);
	writel(0x9999, audio->aud + UMS9117_AUD_SDM_DC_LOW);
	writel(1, audio->aud + UMS9117_AUD_SDM_DC_HIGH);
	update_bits(audio->aud, UMS9117_AUD_IIS, UMS9117_AUD_IIS_DAC_FIELDS,
		    UMS9117_AUD_IIS_DAC_24BIT);
	writel(dac | UMS9117_AUD_DAC_MUTE, audio->aud + UMS9117_AUD_DAC);
	update_bits(audio->vbc, UMS9117_VBC_ENABLE, UMS9117_VBC_DAC_CHANNELS,
		    0);
	update_bits(audio->vbc, UMS9117_VBC_IIS, UMS9117_VBC_IIS_DAC_SELECT, 0);
	writel(0, audio->vbc + UMS9117_VBC_DAC_PATH);
	apply_dac_gain(audio);
	/* A capture session keeps the clocks, so VBC may not have been reset. */
	program_vibrate_tone(audio);
	writel(0xa0a0, audio->vbc + UMS9117_VBC_DAC_LEVEL);
	clear_fifos(audio);
	update_bits(audio->vbc, UMS9117_VBC_CHANNELS, UMS9117_VBC_DAC_CHANNELS,
		    UMS9117_VBC_DAC_CHANNELS);
	/* No AP-DMA requests: the paired portals are fed by bounded PIO. */
	update_bits(audio->vbc, UMS9117_VBC_ENABLE, UMS9117_VBC_DAC_CHANNELS,
		    UMS9117_VBC_DAC_CHANNELS);
	ret = ums9117_audio_queued(audio);
	if (!ret)
		return 0;
	if (ret > 0)
		ret = -EIO;

	ums9117_audio_release(audio);
	return ret;
}

int ums9117_audio_prepare_fm(struct ums9117_audio *audio)
{
	unsigned int i;
	int ret;

	if (audio->capture_prepared || audio->playback_prepared)
		return -EBUSY;
	ret = ums9117_audio_prepare(audio, 32000);
	if (ret)
		return ret;
	/* INF2 carries AUD/VBC IIS0; INF3 carries the internal FM IIS1. */
	audio->saved_iis_matrix = readl(audio->iis_matrix);
	update_bits(audio->iis_matrix, 0, UMS9117_IIS_MATRIX_AUDIO, 0);
	update_bits(audio->vbc, UMS9117_VBC_IIS, UMS9117_VBC_IIS_ADC01_SELECT,
		    FIELD_PREP(UMS9117_VBC_IIS_ADC01_SELECT,
			       UMS9117_VBC_IIS_ADC01_FM));
	update_bits(audio->vbc, UMS9117_VBC_CHANNELS,
		    UMS9117_VBC_ADC01_CHANNELS, UMS9117_VBC_ADC01_CHANNELS);
	writel(fm_mute_control(audio), audio->vbc + UMS9117_VBC_FM_MUTE);
	writel(UMS9117_VBC_DAC_PATH_FM |
		       FIELD_PREP(UMS9117_VBC_DAC0_ADD_FM, 1) |
		       FIELD_PREP(UMS9117_VBC_DAC1_ADD_FM, 1),
	       audio->vbc + UMS9117_VBC_DAC_PATH);
	/* FM clocks its own stream; the DA FIFO only needs a zero trigger. */
	for (i = 0; i < UMS9117_VBC_FM_PREFILL_FRAMES; i++)
		ums9117_audio_write(audio, 0, 0);
	audio->fm_prepared = true;
	return 0;
}

struct ums9117_audio *ums9117_audio_create(struct platform_device *pdev)
{
	struct ums9117_audio *audio;

	audio = devm_kzalloc(&pdev->dev, sizeof(*audio), GFP_KERNEL);
	if (!audio)
		return ERR_PTR(-ENOMEM);
	audio->dev = &pdev->dev;
	audio->aud = devm_platform_ioremap_resource_byname(pdev, "aud");
	if (IS_ERR(audio->aud))
		return ERR_CAST(audio->aud);
	audio->vbc = devm_platform_ioremap_resource_byname(pdev, "vbc");
	if (IS_ERR(audio->vbc))
		return ERR_CAST(audio->vbc);
	audio->clk = devm_platform_ioremap_resource_byname(pdev, "clk1");
	if (IS_ERR(audio->clk))
		return ERR_CAST(audio->clk);
	audio->iis_matrix =
		devm_platform_ioremap_resource_byname(pdev, "iis-matrix");
	if (IS_ERR(audio->iis_matrix))
		return ERR_CAST(audio->iis_matrix);
	audio->aon = syscon_regmap_lookup_by_phandle(pdev->dev.of_node,
						     "fplinux,aon-apb");
	if (IS_ERR(audio->aon))
		return ERR_CAST(audio->aon);
	return audio;
}
