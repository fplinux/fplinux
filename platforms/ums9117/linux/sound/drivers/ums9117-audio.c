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

#define UMS9117_AUD_TOP 0x00
#define UMS9117_AUD_IIS 0x08
#define UMS9117_AUD_DAC 0x0c
#define UMS9117_AUD_SDM0 0x10
#define UMS9117_AUD_SDM1 0x14
#define UMS9117_AUD_STS0 0x20
#define UMS9117_AUD_SDM_DC_LOW 0x38
#define UMS9117_AUD_SDM_DC_HIGH 0x3c
#define UMS9117_AUD_DAC_CHANNELS (BIT(0) | BIT(2))
#define UMS9117_AUD_DAC_MUTE BIT(14)
#define UMS9117_AUD_DAC_24K 0x80b4
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
#define UMS9117_VBC_INT_ENABLE 0xa0
#define UMS9117_VBC_DAC0_STATUS 0xbc
#define UMS9117_VBC_DAC1_STATUS 0xc0
#define UMS9117_VBC_CHANNELS 0xc8
#define UMS9117_VBC_DAC_LEVEL 0xe4
#define UMS9117_VBC_DAC0_DATA 0x1000
#define UMS9117_VBC_DAC1_DATA 0x1004
#define UMS9117_VBC_FIFO_EMPTY BIT(18)
#define UMS9117_VBC_FIFO_FULL BIT(19)
#define UMS9117_VBC_FIFO_READ GENMASK(17, 9)
#define UMS9117_VBC_FIFO_WRITE GENMASK(8, 0)
#define UMS9117_VBC_DAC0_DG_GAIN GENMASK(6, 0)
#define UMS9117_VBC_DAC1_DG_GAIN GENMASK(13, 7)
#define UMS9117_VBC_DAC0_DG_ENABLE BIT(14)
#define UMS9117_VBC_DAC1_DG_ENABLE BIT(15)
#define UMS9117_VBC_DAC_DG_MASK GENMASK(15, 0)

struct ums9117_audio {
	struct device *dev;
	struct regmap *aon;
	void __iomem *aud;
	void __iomem *vbc;
	void __iomem *clk;
	u32 acquired_gates;
	u8 dac_left_gain;
	u8 dac_right_gain;
	bool dac_gain_configured;
	bool prepared;
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
	u32 value;

	if (!audio->dac_gain_configured)
		return;
	/* The serial path maps DAC1 to left and DAC0 to right. */
	value = FIELD_PREP(UMS9117_VBC_DAC1_DG_GAIN, audio->dac_left_gain) |
		FIELD_PREP(UMS9117_VBC_DAC0_DG_GAIN, audio->dac_right_gain) |
		UMS9117_VBC_DAC1_DG_ENABLE | UMS9117_VBC_DAC0_DG_ENABLE;
	update_bits(audio->vbc, UMS9117_VBC_DAC_DG_CTRL,
		    UMS9117_VBC_DAC_DG_MASK, value);
}

void ums9117_audio_set_dac_gain(struct ums9117_audio *audio, u8 left_gain,
				u8 right_gain)
{
	audio->dac_left_gain = left_gain;
	audio->dac_right_gain = right_gain;
	audio->dac_gain_configured = true;
	if (audio->prepared)
		apply_dac_gain(audio);
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

	if (!audio->prepared)
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
	writel(0, audio->vbc + UMS9117_VBC_ENABLE);
	clear_fifos(audio);
	writel(0, audio->vbc + UMS9117_VBC_CHANNELS);
	readl(audio->vbc + UMS9117_VBC_ENABLE);
}

void ums9117_audio_release(struct ums9117_audio *audio)
{
	int ret;

	ums9117_audio_stop(audio);
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

int ums9117_audio_prepare(struct ums9117_audio *audio, unsigned int rate)
{
	unsigned int value;
	u32 dac;
	int ret;

	switch (rate) {
	case 24000:
		dac = UMS9117_AUD_DAC_24K;
		break;
	case 48000:
		dac = UMS9117_AUD_DAC_48K;
		break;
	default:
		return -EINVAL;
	}
	ums9117_audio_release(audio);
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
	audio->prepared = true;
	writel(0, audio->aud + UMS9117_AUD_TOP);
	writel(0x100, audio->aud + UMS9117_AUD_SDM0);
	writel(0x8, audio->aud + UMS9117_AUD_SDM1);
	writel(0x9999, audio->aud + UMS9117_AUD_SDM_DC_LOW);
	writel(1, audio->aud + UMS9117_AUD_SDM_DC_HIGH);
	writel(0x180, audio->aud + UMS9117_AUD_IIS);
	writel(dac | UMS9117_AUD_DAC_MUTE, audio->aud + UMS9117_AUD_DAC);
	writel(0, audio->vbc + UMS9117_VBC_ENABLE);
	writel(0, audio->vbc + UMS9117_VBC_INT_ENABLE);
	writel(0, audio->vbc + UMS9117_VBC_FORMAT);
	writel(0, audio->vbc + UMS9117_VBC_IIS);
	writel(0, audio->vbc + UMS9117_VBC_DAC_PATH);
	apply_dac_gain(audio);
	writel(0xa0a0, audio->vbc + UMS9117_VBC_DAC_LEVEL);
	clear_fifos(audio);
	writel(3, audio->vbc + UMS9117_VBC_CHANNELS);
	/* No AP-DMA requests: the paired portals are fed by bounded PIO. */
	writel(3, audio->vbc + UMS9117_VBC_ENABLE);
	ret = ums9117_audio_queued(audio);
	if (!ret)
		return 0;
	if (ret > 0)
		ret = -EIO;

failed:
	ums9117_audio_release(audio);
	return ret;
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
	audio->aon = syscon_regmap_lookup_by_phandle(pdev->dev.of_node,
						     "fplinux,aon-apb");
	if (IS_ERR(audio->aon))
		return ERR_CAST(audio->aon);
	return audio;
}
