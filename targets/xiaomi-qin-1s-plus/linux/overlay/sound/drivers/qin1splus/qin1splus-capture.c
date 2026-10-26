// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2015 SpreadTrum Ltd.
 * Copyright (C) 2016 SpreadTrum Ltd.
 * Copyright (C) 2015 Spreadtrum Communications Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */
#include <linux/bitops.h>
#include <linux/aw87339.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/hrtimer.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/regmap.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/soc/sprd/vbc-fifo.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/wait.h>

#include <sound/core.h>
#include <sound/control.h>
#include <sound/initval.h>
#include <sound/pcm.h>

#include "qin-capture-fifo.h"
#include "qin-audio-profile.h"
#include "qin-hp.h"
#include "qin-playback.h"

#define QIN_AUDIO_NAME "SC9820E PCM"
#define QIN_CAPTURE_BUFFER_BYTES (256U * 1024U)
#define QIN_CAPTURE_PERIOD_NS NSEC_PER_MSEC
#define QIN_CAPTURE_STALL_MS 100U
#define QIN_CAPTURE_SETTLE_MS 1000U
#define QIN_CAPTURE_SAVED_FIELDS 64U

#define SC9820E_AON_EB0 0x00
#define SC9820E_AON_RESET0 0x08
#define SC9820E_AON_VBC_CTRL 0x20
#define SC9820E_AON_AUDIF BIT(17)
#define SC9820E_AON_AUD BIT(18)
#define SC9820E_AON_VBC BIT(19)
#define SC9820E_AON_AUDIF_RESET BIT(18)
#define SC9820E_AON_AUD_RESET BIT(19)
#define SC9820E_AON_VBC_RESET BIT(20)
#define SC9820E_AON_AUDIF_AUTO BIT(20)
#define SC9820E_PMU_XTLBUF1 0x90
#define SC9820E_G6_SINE_CTRL 0x08
#define SC9820E_G6_SINE_ENABLE BIT(10)
#define SC9820E_XTLBUF_STABLE BIT(11)

#define SC2721_MODULE_ENABLE 0xc08
#define SC2721_CLOCK_ENABLE 0xc0c
#define SC2721_RESET 0xc14
#define SC2721_XTL_ENABLE 0xe9c
#define SC2721_AUDIO_CTRL0 0xebc
#define SC2721_AUDIO_MODULE BIT(4)
#define SC2721_AUDIO_RESET (BIT(13) | BIT(12) | BIT(8))
#define SC2721_PMU0 0x700
#define SC2721_PMU1 0x704
#define SC2721_PMU2 0x708
#define SC2721_CLOCK0 0x718
#define SC2721_CDC0 0x71c
#define SC2721_CDC1 0x720
#define SC2721_CDC3 0x728
#define SC2721_INTERFACE 0x830
#define SC2721_CFGA_CLOCK 0x838
#define SC2721_CFGA_RESET 0x83c
#define SC2721_HP_DCL5 0x750
#define SC2721_HP_STS2 0x764
#define SC2721_ADL_INTERFACE BIT(3)
#define SC2721_ADL_RESET BIT(6)
#define SC2721_AD_CLOCK_RESET BIT(10)
#define SC2721_AD_CLOCK_ENABLE BIT(11)
#define SC2721_ADL_GAIN_MASK GENMASK(11, 9)

#define SC9820E_AUD_TOP 0x00
#define SC9820E_AUD_IIS 0x08
#define SC9820E_AUD_DAC 0x0c
#define SC9820E_AUD_SDM 0x10
#define SC9820E_AUD_ADC 0x18
#define SC9820E_AUD_ADC1_IIS 0x34
#define SC9820E_AUD_DAC_DC_LOW 0x38
#define SC9820E_AUD_DAC_DC_HIGH 0x3c
#define SC9820E_AUD_ADCL BIT(1)
#define SC9820E_AUD_DACS (BIT(0) | BIT(2))

#define SC9820E_VBC_CLEAR 0x00
#define SC9820E_VBC_IIS_CLOCK 0x14
#define SC9820E_VBC_ENABLE 0x18
#define SC9820E_VBC_FORMAT 0x38
#define SC9820E_VBC_IIS 0x3c
#define SC9820E_VBC_DAC_PATH 0x40
#define SC9820E_VBC_DAC_GAIN 0x44
#define SC9820E_VBC_DAC_PROCESSING 0x48
#define SC9820E_VBC_ADC_PATH 0x80
#define SC9820E_VBC_ADC_GAIN 0x84
#define SC9820E_VBC_ADC_EQ 0x8c
#define SC9820E_VBC_ADC_SRC 0x90
#define SC9820E_VBC_DAC_SRC 0x94
#define SC9820E_VBC_DAC_MIXER 0x98
#define SC9820E_VBC_IRQ_ENABLE 0xa0
#define SC9820E_VBC_DAC_NOISE 0xb8
#define SC9820E_VBC_DAC0_STATUS 0xbc
#define SC9820E_VBC_DAC1_STATUS 0xc0
#define SC9820E_VBC_CHANNELS 0xc8
#define SC9820E_VBC_ADC0_STATUS 0xd4
#define SC9820E_VBC_ADC1_STATUS 0xd8
#define SC9820E_VBC_ADC01_LEVEL 0xec
#define SC9820E_VBC_DAC_TONE_ENABLE 0x92c
#define SC9820E_VBC_DAC0_DATA 0x1000
#define SC9820E_VBC_DAC1_DATA 0x1004
#define SC9820E_VBC_ADC0_DATA 0x1010
#define SC9820E_VBC_DAC01 (BIT(0) | BIT(1))
#define SC9820E_VBC_DAC01_DMA (BIT(8) | BIT(9))
#define SC9820E_VBC_ADC0 BIT(4)
#define SC9820E_VBC_ADC01 (BIT(4) | BIT(5))
#define SC9820E_VBC_ADC01_DMA (BIT(12) | BIT(13))

enum qin_capture_bank {
	QIN_CAPTURE_PMIC,
	QIN_CAPTURE_AON,
	QIN_CAPTURE_PMU,
	QIN_CAPTURE_G6,
	QIN_CAPTURE_AUD,
	QIN_CAPTURE_VBC,
};

struct qin_capture_saved {
	enum qin_capture_bank bank;
	unsigned int reg;
	u32 mask;
	u32 value;
};

struct qin_capture {
	struct device *dev;
	struct regmap *pmic;
	struct regmap *aon;
	struct regmap *pmu;
	struct regmap *g6;
	void __iomem *aud;
	void __iomem *vbc;
	void __iomem *stable;
	struct snd_card *card;
	struct snd_pcm *pcm;
	struct task_struct *thread;
	wait_queue_head_t thread_wait;
	struct hrtimer timer;
	/* PCM stream lock precedes this mutex; the timer never takes it. */
	struct mutex lock;
	struct qin_capture_saved saved[QIN_CAPTURE_SAVED_FIELDS];
	unsigned int saved_count;
	bool pmic_reset_done;
	bool prepared;
	bool producer_active;
	bool receiver_active;
	bool playback_fifo_active;
	bool playback_dac_active;
	bool selftest_done;
	bool removing;
	bool suspended;
	/* Hard timer context owns copying; process context disables IRQs here. */
	spinlock_t fifo_lock;
	struct snd_pcm_substream *substream;
	snd_pcm_uframes_t hw_pos;
	snd_pcm_uframes_t capture_ptr;
	snd_pcm_uframes_t period_frames;
	unsigned long last_progress;
	u64 captured_frames;
	u32 last_fifo_status;
	u32 playback_status[2];
	struct qin_playback playback;
	struct qin_hp hp;
	struct qin_audio_profile profile;
	struct qin_audio_processing_state processing;
	struct aw87339 *amplifier;
	unsigned int volume;
	bool amplifier_pending;
	int codec_irq;
	unsigned int fifo_wraps;
	bool running;
	bool period_pending;
	bool drained_pending;
	bool xrun_pending;
	const char *xrun_reason;
};

/* One bounded, intentionally discontinuous FIFO qualification before PCM. */
static bool fifo_selftest;
module_param(fifo_selftest, bool, 0444);
MODULE_PARM_DESC(fifo_selftest,
		 "Run one bounded ADC0 fill/drain check at prepare");

static const struct snd_pcm_hardware qin_capture_hardware = {
	.info = SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_BLOCK_TRANSFER |
		SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_MMAP_VALID |
		SNDRV_PCM_INFO_FIFO_IN_FRAMES | SNDRV_PCM_INFO_NO_REWINDS,
	.formats = SNDRV_PCM_FMTBIT_S16_LE,
	.rates = SNDRV_PCM_RATE_48000,
	.rate_min = 48000,
	.rate_max = 48000,
	.channels_min = 1,
	.channels_max = 1,
	.buffer_bytes_max = QIN_CAPTURE_BUFFER_BYTES,
	.period_bytes_min = 384,
	.period_bytes_max = 32768,
	.periods_min = 2,
	.periods_max = QIN_CAPTURE_BUFFER_BYTES / 384,
	.fifo_size = SPRD_VBC_FIFO_FRAMES,
};

static const struct snd_pcm_hardware qin_playback_hardware = {
	.info = SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_BLOCK_TRANSFER |
		SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_MMAP_VALID |
		SNDRV_PCM_INFO_FIFO_IN_FRAMES | SNDRV_PCM_INFO_NO_REWINDS,
	.formats = SNDRV_PCM_FMTBIT_S16_LE,
	.rates = SNDRV_PCM_RATE_44100 | SNDRV_PCM_RATE_48000,
	.rate_min = 44100,
	.rate_max = 48000,
	.channels_min = 2,
	.channels_max = 2,
	.buffer_bytes_max = QIN_CAPTURE_BUFFER_BYTES,
	.period_bytes_min = 768,
	.period_bytes_max = 32768,
	.periods_min = 2,
	.periods_max = QIN_CAPTURE_BUFFER_BYTES / 768,
	.fifo_size = SPRD_VBC_FIFO_FRAMES,
};

static struct regmap *capture_regmap(struct qin_capture *capture,
				     enum qin_capture_bank bank)
{
	switch (bank) {
	case QIN_CAPTURE_PMIC:
		return capture->pmic;
	case QIN_CAPTURE_AON:
		return capture->aon;
	case QIN_CAPTURE_PMU:
		return capture->pmu;
	case QIN_CAPTURE_G6:
		return capture->g6;
	default:
		return NULL;
	}
}

static void __iomem *capture_mmio(struct qin_capture *capture,
				  enum qin_capture_bank bank)
{
	return bank == QIN_CAPTURE_AUD ? capture->aud : capture->vbc;
}

static int capture_read(struct qin_capture *capture, enum qin_capture_bank bank,
			unsigned int reg, u32 *value)
{
	struct regmap *map = capture_regmap(capture, bank);

	if (map)
		return regmap_read(map, reg, value);
	*value = readl(capture_mmio(capture, bank) + reg);
	return 0;
}

static int capture_update(struct qin_capture *capture,
			  enum qin_capture_bank bank, unsigned int reg,
			  u32 mask, u32 value)
{
	struct regmap *map = capture_regmap(capture, bank);
	u32 readback;
	int ret;

	if (map) {
		if (bank == QIN_CAPTURE_PMIC &&
		    (reg == SC2721_HP_STS2 || reg == SC2721_HP_DCL5)) {
			u32 command;

			ret = regmap_read(map, reg, &readback);
			if (!ret)
				ret = qin_hp_merge_controls(reg, readback, mask,
							    value, &command);
			if (!ret)
				ret = regmap_write(map, reg, command);
		} else {
			ret = regmap_update_bits(map, reg, mask, value);
		}
		if (ret) {
			dev_err(capture->dev,
				"update bank %u register %#x mask %#x value %#x failed: %d\n",
				bank, reg, mask, value, ret);
			return ret;
		}
	} else {
		void __iomem *address = capture_mmio(capture, bank) + reg;

		readback = readl(address);
		writel((readback & ~mask) | (value & mask), address);
	}
	ret = capture_read(capture, bank, reg, &readback);
	if (ret) {
		dev_err(capture->dev,
			"read bank %u register %#x after update failed: %d\n",
			bank, reg, ret);
		return ret;
	}
	if ((readback & mask) != (value & mask)) {
		dev_err(capture->dev,
			"readback bank %u register %#x mask %#x requested %#x got %#x\n",
			bank, reg, mask, value, readback);
		return -EIO;
	}
	return 0;
}

/* Record before writing: an ADI error need not mean that no write arrived. */
static int save_field(struct qin_capture *capture, enum qin_capture_bank bank,
		      unsigned int reg, u32 mask)
{
	struct qin_capture_saved *saved;
	unsigned int i;
	u32 previous;
	int ret;

	for (i = 0; i < capture->saved_count; i++) {
		saved = &capture->saved[i];
		if (saved->bank == bank && saved->reg == reg &&
		    saved->mask == mask)
			return 0;
	}
	if (capture->saved_count == ARRAY_SIZE(capture->saved))
		return -EOVERFLOW;
	ret = capture_read(capture, bank, reg, &previous);
	if (ret)
		return ret;
	saved = &capture->saved[capture->saved_count++];
	saved->bank = bank;
	saved->reg = reg;
	saved->mask = mask;
	saved->value = previous & mask;
	return 0;
}

static int save_update(struct qin_capture *capture, enum qin_capture_bank bank,
		       unsigned int reg, u32 mask, u32 value)
{
	int ret = save_field(capture, bank, reg, mask);

	return ret ? ret : capture_update(capture, bank, reg, mask, value);
}

static int hp_read(void *context, unsigned int reg, u32 *value)
{
	struct qin_capture *capture = context;

	return regmap_read(capture->pmic, reg, value);
}

static int hp_save(void *context, unsigned int reg, u32 mask)
{
	return save_field(context, QIN_CAPTURE_PMIC, reg, mask);
}

static int hp_update(void *context, unsigned int reg, u32 mask, u32 value)
{
	return capture_update(context, QIN_CAPTURE_PMIC, reg, mask, value);
}

static int hp_command(void *context, unsigned int reg, u32 mask, u32 value)
{
	struct qin_capture *capture = context;
	u32 previous;
	u32 command;
	int ret;

	/* Emit controls only; status from the mixed register is never replayed. */
	ret = regmap_read(capture->pmic, reg, &previous);
	if (ret)
		return ret;
	ret = qin_hp_merge_controls(reg, previous, mask, value, &command);
	return ret ? ret : regmap_write(capture->pmic, reg, command);
}

static int hp_write(void *context, unsigned int reg, u32 value)
{
	struct qin_capture *capture = context;

	return regmap_write(capture->pmic, reg, value);
}

static int hp_pulse(void *context, unsigned int reg, u32 mask,
		    unsigned int delay_us)
{
	struct qin_capture *capture = context;
	int ret;

	ret = regmap_write_bits(capture->pmic, reg, mask, mask);
	if (ret)
		return ret;
	udelay(delay_us);
	return regmap_write_bits(capture->pmic, reg, mask, 0);
}

static void hp_wait(void *context, unsigned int delay_ms)
{
	(void)context;
	msleep(delay_ms);
}

static void hp_irq(void *context, bool enable)
{
	struct qin_capture *capture = context;

	if (enable)
		enable_irq(capture->codec_irq);
	else
		disable_irq_nosync(capture->codec_irq);
}

static struct qin_hp_io hp_io(struct qin_capture *capture)
{
	return (struct qin_hp_io){
		.context = capture,
		.read = hp_read,
		.save = hp_save,
		.update = hp_update,
		.command = hp_command,
		.write = hp_write,
		.pulse = hp_pulse,
		.wait_ms = hp_wait,
		.irq = hp_irq,
	};
}

static int capture_reset(struct qin_capture *capture,
			 enum qin_capture_bank bank, unsigned int reg, u32 mask,
			 unsigned int delay_us)
{
	int ret;

	ret = capture_update(capture, bank, reg, mask, mask);
	if (ret)
		return ret;
	if (delay_us)
		udelay(delay_us);
	return capture_update(capture, bank, reg, mask, 0);
}

static int reset_cfga(struct qin_capture *capture)
{
	int ret;

	/* The pulse sequence does not require an asserted readback. */
	ret = regmap_write_bits(capture->pmic, SC2721_CFGA_RESET, 0x33, 0x33);
	if (ret)
		return ret;
	udelay(10);
	return regmap_write_bits(capture->pmic, SC2721_CFGA_RESET, 0x33, 0);
}

static int restore_fields(struct qin_capture *capture)
{
	struct qin_capture_saved *saved;
	int ret;

	while (capture->saved_count) {
		saved = &capture->saved[capture->saved_count - 1];
		/* VBC must lose its gate before the last XTLBUF request is released. */
		if (saved->bank == QIN_CAPTURE_PMU &&
		    capture->saved[0].bank == QIN_CAPTURE_AON &&
		    capture->saved[0].reg == SC9820E_AON_EB0 &&
		    capture->saved[0].mask == SC9820E_AON_VBC) {
			ret = capture_update(capture, QIN_CAPTURE_AON,
					     SC9820E_AON_EB0, SC9820E_AON_VBC,
					     capture->saved[0].value);
			if (ret)
				return ret;
			capture->saved_count--;
			memmove(&capture->saved[0], &capture->saved[1],
				capture->saved_count * sizeof(*saved));
			continue;
		}
		if (saved->bank == QIN_CAPTURE_PMIC &&
		    saved->reg == SC2721_CLOCK0 &&
		    saved->mask == SC2721_AD_CLOCK_ENABLE && !saved->value) {
			ret = capture_update(capture, QIN_CAPTURE_PMIC,
					     SC2721_CLOCK0,
					     SC2721_AD_CLOCK_ENABLE, 0);
			if (ret)
				return ret;
			ret = capture_update(capture, QIN_CAPTURE_PMIC,
					     SC2721_CLOCK0,
					     SC2721_AD_CLOCK_RESET,
					     SC2721_AD_CLOCK_RESET);
			if (ret)
				return ret;
		}
		if (saved->bank == QIN_CAPTURE_PMIC &&
		    saved->reg == SC2721_CFGA_CLOCK) {
			ret = reset_cfga(capture);
			if (ret)
				return ret;
		}
		/* Discharge an acquired MICBIAS after releasing its enable. */
		if (saved->bank == QIN_CAPTURE_PMIC &&
		    saved->reg == SC2721_PMU0 && saved->mask == 0x0404 &&
		    !(saved->value & BIT(10)))
			saved->value = BIT(2);
		ret = capture_update(capture, saved->bank, saved->reg,
				     saved->mask, saved->value);
		if (ret) {
			dev_err(capture->dev,
				"restore bank %u register %#x mask %#x failed: %d\n",
				saved->bank, saved->reg, saved->mask, ret);
			return ret;
		}
		capture->saved_count--;
	}
	return 0;
}

static void stop_copying(struct qin_capture *capture)
{
	unsigned long flags;

	spin_lock_irqsave(&capture->fifo_lock, flags);
	capture->running = false;
	capture->period_pending = false;
	capture->drained_pending = false;
	capture->xrun_pending = false;
	spin_unlock_irqrestore(&capture->fifo_lock, flags);
	hrtimer_cancel(&capture->timer);
}

static int stop_producer(struct qin_capture *capture)
{
	int ret;

	if (!capture->producer_active)
		return 0;
	ret = capture_update(capture, QIN_CAPTURE_AUD, SC9820E_AUD_TOP,
			     SC9820E_AUD_ADCL, 0);
	if (ret)
		return ret;
	ret = capture_update(capture, QIN_CAPTURE_PMIC, SC2721_INTERFACE,
			     SC2721_ADL_INTERFACE, 0);
	if (ret)
		return ret;
	capture->producer_active = false;
	return 0;
}

static int stop_receiver(struct qin_capture *capture)
{
	u32 cleared;
	int ret;

	if (!capture->receiver_active)
		return 0;
	ret = capture_update(capture, QIN_CAPTURE_VBC, SC9820E_VBC_ENABLE,
			     SC9820E_VBC_ADC01 | SC9820E_VBC_ADC01_DMA, 0);
	if (ret)
		return ret;
	/* Clear the asynchronous FIFO before disabling its data-path clock. */
	writel(SC9820E_VBC_ADC0, capture->vbc + SC9820E_VBC_CLEAR);
	cleared = readl(capture->vbc + SC9820E_VBC_ADC0_STATUS);
	ret = capture_update(capture, QIN_CAPTURE_VBC, SC9820E_VBC_CHANNELS,
			     SC9820E_VBC_ADC01, 0);
	if (ret)
		return ret;
	if (fifo_selftest && !capture->selftest_done)
		dev_info(capture->dev, "ADC0 clear before clock stop: %#x\n",
			 cleared);
	capture->receiver_active = false;
	return 0;
}

static int stop_playback(struct qin_capture *capture)
{
	int ret;

	if (capture->playback_dac_active) {
		ret = capture_update(capture, QIN_CAPTURE_AUD, SC9820E_AUD_TOP,
				     SC9820E_AUD_DACS, 0);
		if (ret)
			return ret;
		capture->playback_dac_active = false;
	}
	ret = qin_audio_processing_restore(capture->dev, capture->vbc,
					   &capture->processing);
	if (ret)
		return ret;
	if (!capture->playback_fifo_active)
		return 0;
	ret = capture_update(capture, QIN_CAPTURE_VBC, SC9820E_VBC_ENABLE,
			     SC9820E_VBC_DAC01 | SC9820E_VBC_DAC01_DMA, 0);
	if (ret)
		return ret;
	writel(BIT(0), capture->vbc + SC9820E_VBC_CLEAR);
	writel(BIT(1), capture->vbc + SC9820E_VBC_CLEAR);
	capture->playback_status[0] =
		readl(capture->vbc + SC9820E_VBC_DAC0_STATUS);
	capture->playback_status[1] =
		readl(capture->vbc + SC9820E_VBC_DAC1_STATUS);
	if (!(capture->playback_status[0] & SPRD_VBC_FIFO_EMPTY) ||
	    !(capture->playback_status[1] & SPRD_VBC_FIFO_EMPTY) ||
	    qin_playback_queued(capture->playback_status[0],
				capture->playback_status[1]))
		return -EIO;
	ret = capture_update(capture, QIN_CAPTURE_VBC, SC9820E_VBC_CHANNELS,
			     SC9820E_VBC_DAC01, 0);
	if (!ret)
		capture->playback_fifo_active = false;
	return ret;
}

static int stop_amplifier(struct qin_capture *capture)
{
	int ret;

	if (!capture->amplifier_pending)
		return 0;
	ret = aw87339_stop(capture->amplifier);
	if (!ret)
		capture->amplifier_pending = false;
	return ret;
}

static int quiet_speaker(struct qin_capture *capture)
{
	struct qin_hp_io io = hp_io(capture);
	int ret = qin_hp_mute(&capture->hp, &io);
	int amplifier_ret = stop_amplifier(capture);

	if (amplifier_ret)
		dev_err(capture->dev, "amplifier stop failed: %d\n",
			amplifier_ret);
	return ret ? ret : amplifier_ret;
}

static int shutdown_capture(struct qin_capture *capture)
{
	struct qin_hp_io io = hp_io(capture);
	int hp_ret;
	int ret;

	stop_copying(capture);
	capture->prepared = false;
	ret = quiet_speaker(capture);
	hp_ret = qin_hp_stop(&capture->hp, &io);
	if (!ret)
		ret = hp_ret;
	if (ret) {
		int digital_ret = stop_playback(capture);

		if (digital_ret)
			dev_err(capture->dev,
				"digital stop after analog failure failed: %d\n",
				digital_ret);
		goto failed;
	}
	/* Failure leaves the receiver and clocks available for another stop. */
	ret = stop_producer(capture);
	if (ret)
		goto failed;
	ret = stop_receiver(capture);
	if (ret)
		goto failed;
	ret = stop_playback(capture);
	if (ret)
		goto failed;
	ret = restore_fields(capture);
	if (!ret)
		return 0;
failed:
	dev_err(capture->dev,
		"PCM cleanup incomplete: %d (%u fields retained)\n", ret,
		capture->saved_count);
	return ret;
}

static int prepare_clocks(struct qin_capture *capture)
{
	u32 eb0;
	u32 value;
	int ret;

	ret = regmap_read(capture->aon, SC9820E_AON_EB0, &eb0);
	if (ret)
		return ret;
	ret = save_update(capture, QIN_CAPTURE_AON, SC9820E_AON_EB0,
			  SC9820E_AON_VBC, SC9820E_AON_VBC);
	if (ret)
		return ret;
	ret = save_update(capture, QIN_CAPTURE_PMU, SC9820E_PMU_XTLBUF1, BIT(1),
			  BIT(1));
	if (ret)
		return ret;
	ret = save_update(capture, QIN_CAPTURE_PMU, SC9820E_PMU_XTLBUF1, BIT(0),
			  BIT(0));
	if (ret)
		return ret;
	/* The first observation is also delayed by the required 200 us. */
	usleep_range(200, 250);
	ret = readl_poll_timeout(capture->stable, value,
				 value & SC9820E_XTLBUF_STABLE, 200, 4000);
	if (ret)
		return ret;
	if (!(eb0 & SC9820E_AON_VBC)) {
		ret = capture_reset(capture, QIN_CAPTURE_AON,
				    SC9820E_AON_RESET0, SC9820E_AON_VBC_RESET,
				    10);
		if (ret)
			return ret;
	}
	return 0;
}

static int prepare_digital(struct qin_capture *capture)
{
	int ret;

	ret = save_update(capture, QIN_CAPTURE_AON, SC9820E_AON_EB0,
			  SC9820E_AON_AUD, SC9820E_AON_AUD);
	if (ret)
		return ret;
	ret = save_update(capture, QIN_CAPTURE_AON, SC9820E_AON_EB0,
			  SC9820E_AON_AUDIF, SC9820E_AON_AUDIF);
	if (ret)
		return ret;
	ret = save_update(capture, QIN_CAPTURE_AON, SC9820E_AON_VBC_CTRL,
			  SC9820E_AON_AUDIF_AUTO, 0);
	if (ret)
		return ret;
	/* The sine-driver request remains asserted after the stream stops. */
	ret = capture_update(capture, QIN_CAPTURE_G6, SC9820E_G6_SINE_CTRL,
			     SC9820E_G6_SINE_ENABLE, SC9820E_G6_SINE_ENABLE);
	if (ret)
		return ret;
	ret = capture_update(capture, QIN_CAPTURE_AON, SC9820E_AON_RESET0,
			     SC9820E_AON_AUD_RESET, SC9820E_AON_AUD_RESET);
	if (ret)
		return ret;
	ret = capture_update(capture, QIN_CAPTURE_AON, SC9820E_AON_RESET0,
			     SC9820E_AON_AUDIF_RESET, SC9820E_AON_AUDIF_RESET);
	if (ret)
		return ret;
	usleep_range(10, 20);
	ret = capture_update(capture, QIN_CAPTURE_AON, SC9820E_AON_RESET0,
			     SC9820E_AON_AUD_RESET, 0);
	if (ret)
		return ret;
	return capture_update(capture, QIN_CAPTURE_AON, SC9820E_AON_RESET0,
			      SC9820E_AON_AUDIF_RESET, 0);
}

static int prepare_analog_clocks(struct qin_capture *capture)
{
	int ret;

	ret = save_update(capture, QIN_CAPTURE_PMIC, SC2721_CFGA_CLOCK, 0x13,
			  0x13);
	if (ret)
		return ret;
	ret = reset_cfga(capture);
	if (ret)
		return ret;
	ret = save_update(capture, QIN_CAPTURE_PMIC, SC2721_CLOCK_ENABLE, 3, 3);
	if (ret)
		return ret;
	ret = save_update(capture, QIN_CAPTURE_PMIC, SC2721_AUDIO_CTRL0, BIT(1),
			  BIT(1));
	if (ret)
		return ret;
	ret = capture_update(capture, QIN_CAPTURE_PMIC, SC2721_XTL_ENABLE,
			     BIT(8), BIT(8));
	if (ret)
		return ret;
	return save_update(capture, QIN_CAPTURE_PMIC, SC2721_CLOCK0, 0x5000,
			   0x5000);
}

static int prepare_hp_power(struct qin_capture *capture)
{
	int ret;

	ret = save_update(capture, QIN_CAPTURE_PMIC, SC2721_MODULE_ENABLE,
			  SC2721_AUDIO_MODULE, SC2721_AUDIO_MODULE);
	if (ret)
		return ret;
	if (!capture->pmic_reset_done) {
		ret = capture_reset(capture, QIN_CAPTURE_PMIC, SC2721_RESET,
				    SC2721_AUDIO_RESET, 10);
		if (ret)
			return ret;
		capture->pmic_reset_done = true;
	}
	ret = save_update(capture, QIN_CAPTURE_PMIC, SC2721_PMU0, 0x8000,
			  0x8000);
	if (ret)
		return ret;
	ret = save_update(capture, QIN_CAPTURE_PMIC, SC2721_PMU0, 0x1058,
			  0x1048);
	if (ret)
		return ret;
	ret = save_update(capture, QIN_CAPTURE_PMIC, SC2721_PMU0, 0x0800,
			  0x0800);
	if (ret)
		return ret;
	ret = save_update(capture, QIN_CAPTURE_PMIC, SC2721_PMU0, 0x2000,
			  0x2000);
	if (ret)
		return ret;
	return prepare_analog_clocks(capture);
}

static int prepare_codec(struct qin_capture *capture)
{
	u32 selectors;
	int ret;

	ret = save_update(capture, QIN_CAPTURE_PMIC, SC2721_MODULE_ENABLE,
			  SC2721_AUDIO_MODULE, SC2721_AUDIO_MODULE);
	if (ret)
		return ret;
	if (!capture->pmic_reset_done) {
		ret = capture_reset(capture, QIN_CAPTURE_PMIC, SC2721_RESET,
				    SC2721_AUDIO_RESET, 10);
		if (ret)
			return ret;
		capture->pmic_reset_done = true;
	}
	ret = regmap_read(capture->pmic, SC2721_PMU1, &selectors);
	if (ret)
		return ret;
	dev_info(capture->dev,
		 "retained MICBIAS selector %u VCOM selector %u\n",
		 selectors & 7, (selectors >> 6) & 0x1f);
	ret = save_update(capture, QIN_CAPTURE_PMIC, SC2721_PMU0, 0x8000,
			  0x8000);
	if (ret)
		return ret;
	ret = save_update(capture, QIN_CAPTURE_PMIC, SC2721_PMU0, 0x1058,
			  0x1048);
	if (ret)
		return ret;
	ret = save_update(capture, QIN_CAPTURE_PMIC, SC2721_PMU0, 0x0800,
			  0x0800);
	if (ret)
		return ret;
	ret = prepare_digital(capture);
	if (ret)
		return ret;
	ret = save_update(capture, QIN_CAPTURE_AUD, SC9820E_AUD_ADC, 0xf, 12);
	if (ret)
		return ret;
	ret = save_update(capture, QIN_CAPTURE_AUD, SC9820E_AUD_IIS, BIT(2),
			  BIT(2));
	if (ret)
		return ret;
	ret = save_update(capture, QIN_CAPTURE_AUD, SC9820E_AUD_ADC1_IIS,
			  BIT(0), BIT(0));
	if (ret)
		return ret;
	ret = save_update(capture, QIN_CAPTURE_PMIC, SC2721_PMU0, 0x2000,
			  0x2000);
	if (ret)
		return ret;
	ret = prepare_analog_clocks(capture);
	if (ret)
		return ret;
	ret = save_update(capture, QIN_CAPTURE_PMIC, SC2721_PMU2, 0x1e0, 0xa0);
	if (ret)
		return ret;
	ret = save_update(capture, QIN_CAPTURE_PMIC, SC2721_CLOCK0, 0xc, 8);
	if (ret)
		return ret;
	ret = save_update(capture, QIN_CAPTURE_PMIC, SC2721_CLOCK0,
			  SC2721_AD_CLOCK_ENABLE, SC2721_AD_CLOCK_ENABLE);
	if (ret)
		return ret;
	ret = capture_reset(capture, QIN_CAPTURE_PMIC, SC2721_CLOCK0,
			    SC2721_AD_CLOCK_RESET, 0);
	if (ret)
		return ret;
	ret = save_update(capture, QIN_CAPTURE_PMIC, SC2721_CDC0, 0xc000,
			  0xc000);
	if (ret)
		return ret;
	ret = save_update(capture, QIN_CAPTURE_PMIC, SC2721_PMU0, 0x0404,
			  0x0400);
	if (ret)
		return ret;
	ret = save_update(capture, QIN_CAPTURE_PMIC, SC2721_CDC0, 0x2000,
			  0x2000);
	if (ret)
		return ret;
	ret = save_update(capture, QIN_CAPTURE_PMIC, SC2721_CDC3, 0xa, 8);
	if (ret)
		return ret;
	return save_update(capture, QIN_CAPTURE_PMIC, SC2721_CDC0, 0x80, 0x80);
}

static int prepare_receiver(struct qin_capture *capture)
{
	u32 status;
	int ret;

	ret = save_update(capture, QIN_CAPTURE_VBC, SC9820E_VBC_ENABLE,
			  SC9820E_VBC_ADC01 | SC9820E_VBC_ADC01_DMA, 0);
	if (ret)
		return ret;
	ret = save_update(capture, QIN_CAPTURE_VBC, SC9820E_VBC_CHANNELS,
			  SC9820E_VBC_ADC01, 0);
	if (ret)
		return ret;
	ret = save_update(capture, QIN_CAPTURE_VBC, SC9820E_VBC_IRQ_ENABLE,
			  BIT(0), 0);
	if (ret)
		return ret;
	ret = save_update(capture, QIN_CAPTURE_VBC, SC9820E_VBC_FORMAT, 0x30,
			  0);
	if (ret)
		return ret;
	ret = save_update(capture, QIN_CAPTURE_VBC, SC9820E_VBC_IIS,
			  BIT(22) | BIT(18) | BIT(13) | BIT(11) | BIT(9) | 7,
			  0);
	if (ret)
		return ret;
	ret = save_update(capture, QIN_CAPTURE_VBC, SC9820E_VBC_ADC_PATH,
			  BIT(8) | 3, 0);
	if (ret)
		return ret;
	ret = save_update(capture, QIN_CAPTURE_VBC, SC9820E_VBC_ADC_GAIN,
			  BIT(15) | BIT(7), 0);
	if (ret)
		return ret;
	ret = save_update(capture, QIN_CAPTURE_VBC, SC9820E_VBC_ADC_EQ, BIT(0),
			  0);
	if (ret)
		return ret;
	ret = save_update(capture, QIN_CAPTURE_VBC, SC9820E_VBC_ADC_SRC, BIT(0),
			  0);
	if (ret)
		return ret;
	ret = save_update(capture, QIN_CAPTURE_VBC, SC9820E_VBC_ADC01_LEVEL,
			  0x3ffff, 0x1e0f0);
	if (ret)
		return ret;
	writel(SC9820E_VBC_ADC0, capture->vbc + SC9820E_VBC_CLEAR);
	status = readl(capture->vbc + SC9820E_VBC_ADC0_STATUS);
	if (!(status & SPRD_VBC_FIFO_EMPTY) ||
	    sprd_vbc_fifo_capture_frames(status)) {
		dev_err(capture->dev, "ADC0 not empty after clear: %#x\n",
			status);
		return -EIO;
	}
	return 0;
}

static int start_receiver(struct qin_capture *capture)
{
	int ret;

	capture->receiver_active = true;
	ret = capture_update(capture, QIN_CAPTURE_VBC, SC9820E_VBC_CHANNELS,
			     SC9820E_VBC_ADC01, SC9820E_VBC_ADC0);
	if (ret)
		return ret;
	return capture_update(capture, QIN_CAPTURE_VBC, SC9820E_VBC_ENABLE,
			      SC9820E_VBC_ADC01 | SC9820E_VBC_ADC01_DMA,
			      SC9820E_VBC_ADC0);
}

static int start_producer(struct qin_capture *capture)
{
	int ret;

	capture->producer_active = true;
	ret = save_update(capture, QIN_CAPTURE_PMIC, SC2721_INTERFACE,
			  SC2721_ADL_INTERFACE, SC2721_ADL_INTERFACE);
	if (ret)
		return ret;
	ret = capture_reset(capture, QIN_CAPTURE_PMIC, SC2721_CDC0,
			    SC2721_ADL_RESET, 0);
	if (ret)
		return ret;
	ret = save_update(capture, QIN_CAPTURE_PMIC, SC2721_CDC1,
			  SC2721_ADL_GAIN_MASK,
			  (u32)capture->profile.mainmic_gain << 9);
	if (ret)
		return ret;
	return save_update(capture, QIN_CAPTURE_AUD, SC9820E_AUD_TOP,
			   SC9820E_AUD_ADCL, SC9820E_AUD_ADCL);
}

static void report_capture_hardware(struct qin_capture *capture,
				    const char *reason)
{
	u32 interface;
	int ret;

	dev_err(capture->dev,
		"capture %s: ADC0=%#x ADC1=%#x enable=%#x channels=%#x format=%#x IIS=%#x path=%#x SRC=%#x\n",
		reason, readl(capture->vbc + SC9820E_VBC_ADC0_STATUS),
		readl(capture->vbc + SC9820E_VBC_ADC1_STATUS),
		readl(capture->vbc + SC9820E_VBC_ENABLE),
		readl(capture->vbc + SC9820E_VBC_CHANNELS),
		readl(capture->vbc + SC9820E_VBC_FORMAT),
		readl(capture->vbc + SC9820E_VBC_IIS),
		readl(capture->vbc + SC9820E_VBC_ADC_PATH),
		readl(capture->vbc + SC9820E_VBC_ADC_SRC));
	dev_err(capture->dev, "producer top=%#x ADC=%#x IIS=%#x\n",
		readl(capture->aud + SC9820E_AUD_TOP),
		readl(capture->aud + SC9820E_AUD_ADC),
		readl(capture->aud + SC9820E_AUD_IIS));
	ret = regmap_read(capture->pmic, SC2721_INTERFACE, &interface);
	if (ret)
		dev_err(capture->dev, "cannot read analog interface: %d\n",
			ret);
	else
		dev_err(capture->dev, "analog interface=%#x\n", interface);
}

static int run_fifo_selftest(struct qin_capture *capture)
{
	ktime_t deadline;
	unsigned int reads = 0;
	unsigned int previous;
	bool wrapped = false;
	u32 before;
	u32 after;
	int ret;

	ret = start_receiver(capture);
	if (ret)
		return ret;
	ret = readl_poll_timeout(capture->vbc + SC9820E_VBC_ADC0_STATUS, before,
				 before & SPRD_VBC_FIFO_FULL, 200, 20000);
	if (ret)
		goto stop;
	ret = stop_producer(capture);
	if (ret)
		return ret;
	deadline = ktime_add_ms(ktime_get(), 20);
	while (reads < 512 && ktime_before(ktime_get(), deadline)) {
		before = readl(capture->vbc + SC9820E_VBC_ADC0_STATUS);
		if (before & SPRD_VBC_FIFO_EMPTY)
			break;
		previous = sprd_vbc_fifo_read(before);
		if (previous >= SPRD_VBC_FIFO_FRAMES ||
		    sprd_vbc_fifo_write(before) >= SPRD_VBC_FIFO_FRAMES) {
			ret = -EIO;
			goto stop;
		}
		readl(capture->vbc + SC9820E_VBC_ADC0_DATA);
		after = readl(capture->vbc + SC9820E_VBC_ADC0_STATUS);
		reads++;
		if (sprd_vbc_fifo_read(after) !=
		    (previous + 1) % SPRD_VBC_FIFO_FRAMES) {
			ret = -EIO;
			goto stop;
		}
		if (previous == 319)
			wrapped = true;
	}
	if (reads != SPRD_VBC_FIFO_FRAMES || !wrapped ||
	    !(before & SPRD_VBC_FIFO_EMPTY) ||
	    sprd_vbc_fifo_read(before) != sprd_vbc_fifo_write(before))
		ret = -EIO;
stop:
	dev_info(capture->dev,
		 "ADC0 fill/drain: result=%d reads=%u wrap=%u status=%#x\n",
		 ret, reads, wrapped, before);
	if (ret)
		report_capture_hardware(capture, "fill/drain failed");
	/* The caller performs the full unwind if either stop fails. */
	if (stop_producer(capture))
		return ret ? ret : -EIO;
	if (stop_receiver(capture))
		return ret ? ret : -EIO;
	return ret;
}

static int prepare_hardware(struct qin_capture *capture)
{
	int cleanup_ret;
	int ret;

	ret = shutdown_capture(capture);
	if (ret)
		return ret;
	ret = prepare_clocks(capture);
	if (ret)
		goto failed;
	ret = prepare_receiver(capture);
	if (ret)
		goto failed;
	ret = prepare_codec(capture);
	if (ret)
		goto failed;
	ret = start_producer(capture);
	if (ret)
		goto failed;
	if (fifo_selftest && !capture->selftest_done) {
		ret = run_fifo_selftest(capture);
		if (ret)
			goto failed;
		capture->selftest_done = true;
		ret = start_producer(capture);
		if (ret)
			goto failed;
	}
	/* Settle the microphone and ADC before the receive FIFO admits samples. */
	msleep(QIN_CAPTURE_SETTLE_MS);
	capture->prepared = true;
	return 0;
failed:
	dev_err(capture->dev, "capture preparation failed: %d\n", ret);
	cleanup_ret = shutdown_capture(capture);
	if (cleanup_ret)
		dev_err(capture->dev, "preparation cleanup failed: %d\n",
			cleanup_ret);
	return ret;
}

static int prepare_playback(struct qin_capture *capture, unsigned int rate)
{
	struct qin_hp_io io = hp_io(capture);
	bool processed = rate == capture->profile.processing_rate;
	u32 gain_mask = GENMASK(17, 14);
	u32 gain = 0;
	u32 processing_mask = GENMASK(11, 9);
	int ret, cleanup_ret;

	if (processed) {
		gain_mask |= GENMASK(13, 0);
		gain = capture->profile.digital_gain |
		       ((u32)capture->profile.digital_gain << 7);
		if (capture->profile.digital_gain_enabled)
			gain |= BIT(14) | BIT(15);
		processing_mask |= BIT(13) | 0xff;
	}
	ret = shutdown_capture(capture);
	if (ret)
		return ret;
	ret = prepare_clocks(capture);
	if (ret)
		goto failed;
	ret = prepare_digital(capture);
	if (ret)
		goto failed;
	/* This PIO path does not acquire or rewrite the interrupt controls. */
	if (readl(capture->vbc + SC9820E_VBC_IRQ_ENABLE) ||
	    (readl(capture->vbc + SC9820E_VBC_IIS) & GENMASK(8, 6))) {
		ret = -EBUSY;
		goto failed;
	}
	ret = save_update(capture, QIN_CAPTURE_AUD, SC9820E_AUD_TOP,
			  SC9820E_AUD_DACS, 0);
	if (ret)
		goto failed;
	ret = save_update(capture, QIN_CAPTURE_AUD, SC9820E_AUD_DAC,
			  BIT(15) | 0xf, processed ? 2 : 1);
	if (ret)
		goto failed;
	ret = save_update(capture, QIN_CAPTURE_AUD, SC9820E_AUD_DAC_DC_LOW,
			  0xffff, 0x9999);
	if (ret)
		goto failed;
	ret = save_update(capture, QIN_CAPTURE_AUD, SC9820E_AUD_DAC_DC_HIGH,
			  0xff, 1);
	if (ret)
		goto failed;
	ret = save_update(capture, QIN_CAPTURE_AUD, SC9820E_AUD_SDM, 0xffff, 0);
	if (ret)
		goto failed;
	ret = save_update(capture, QIN_CAPTURE_AUD, SC9820E_AUD_IIS, BIT(1), 0);
	if (ret)
		goto failed;
	ret = save_update(capture, QIN_CAPTURE_VBC, SC9820E_VBC_ENABLE,
			  SC9820E_VBC_DAC01 | SC9820E_VBC_DAC01_DMA, 0);
	if (ret)
		goto failed;
	ret = save_update(capture, QIN_CAPTURE_VBC, SC9820E_VBC_CHANNELS,
			  SC9820E_VBC_DAC01, 0);
	if (ret)
		goto failed;
	ret = save_update(capture, QIN_CAPTURE_VBC, SC9820E_VBC_FORMAT, 3, 0);
	if (ret)
		goto failed;
	ret = save_update(capture, QIN_CAPTURE_VBC, SC9820E_VBC_IIS,
			  BIT(21) | BIT(20) | BIT(14) | BIT(12) | BIT(10) |
				  GENMASK(8, 6),
			  BIT(14));
	if (ret)
		goto failed;
	ret = save_update(capture, QIN_CAPTURE_VBC, SC9820E_VBC_IIS_CLOCK,
			  BIT(10), 0);
	if (ret)
		goto failed;
	ret = save_update(capture, QIN_CAPTURE_VBC, SC9820E_VBC_DAC_PATH,
			  GENMASK(8, 0), 0);
	if (ret)
		goto failed;
	ret = save_update(capture, QIN_CAPTURE_VBC, SC9820E_VBC_DAC_GAIN,
			  gain_mask, gain);
	if (ret)
		goto failed;
	ret = save_update(capture, QIN_CAPTURE_VBC, SC9820E_VBC_DAC_PROCESSING,
			  processing_mask, 0);
	if (ret)
		goto failed;
	ret = save_update(capture, QIN_CAPTURE_VBC, SC9820E_VBC_DAC_SRC, BIT(0),
			  0);
	if (ret)
		goto failed;
	ret = save_update(capture, QIN_CAPTURE_VBC, SC9820E_VBC_DAC_NOISE,
			  BIT(0), 0);
	if (ret)
		goto failed;
	ret = save_update(capture, QIN_CAPTURE_VBC, SC9820E_VBC_DAC_TONE_ENABLE,
			  GENMASK(2, 0), 0);
	if (ret)
		goto failed;
	ret = save_update(capture, QIN_CAPTURE_VBC, SC9820E_VBC_CHANNELS,
			  SC9820E_VBC_DAC01, SC9820E_VBC_DAC01);
	if (ret)
		goto failed;
	/* The FIFO stop obligation requires the confirmed data-path clocks. */
	capture->playback_fifo_active = true;
	writel(BIT(0), capture->vbc + SC9820E_VBC_CLEAR);
	writel(BIT(1), capture->vbc + SC9820E_VBC_CLEAR);
	capture->playback_status[0] =
		readl(capture->vbc + SC9820E_VBC_DAC0_STATUS);
	capture->playback_status[1] =
		readl(capture->vbc + SC9820E_VBC_DAC1_STATUS);
	if (!(capture->playback_status[0] & SPRD_VBC_FIFO_EMPTY) ||
	    !(capture->playback_status[1] & SPRD_VBC_FIFO_EMPTY) ||
	    qin_playback_queued(capture->playback_status[0],
				capture->playback_status[1])) {
		ret = -EIO;
		goto failed;
	}
	ret = prepare_hp_power(capture);
	if (ret)
		goto failed;
	ret = qin_hp_prepare(&capture->hp, &io);
	if (ret)
		goto failed;
	if (processed) {
		ret = capture_update(capture, QIN_CAPTURE_PMIC, SC2721_PMU2,
				     0x18, (u32)capture->profile.dac_gain << 3);
		if (ret)
			goto failed;
		ret = qin_audio_processing_prepare(capture->dev, capture->vbc,
						   &capture->profile,
						   &capture->processing);
		if (ret)
			goto failed;
	}
	/* Keep both stereo channels audible through the single rear speaker. */
	ret = save_update(capture, QIN_CAPTURE_VBC, SC9820E_VBC_DAC_MIXER, 0xff,
			  0x12);
	if (ret)
		goto failed;
	dev_info(capture->dev,
		 "HP ready with both gains muted; saved fields=%u\n",
		 capture->saved_count);
	capture->prepared = true;
	return 0;
failed:
	dev_err(capture->dev, "playback preparation failed: %d\n", ret);
	cleanup_ret = shutdown_capture(capture);
	if (cleanup_ret)
		dev_err(capture->dev, "preparation cleanup failed: %d\n",
			cleanup_ret);
	return ret;
}

static void playback_write_frame(void *context, u32 left, u32 right)
{
	struct qin_capture *capture = context;

	writel(left, capture->vbc + SC9820E_VBC_DAC0_DATA);
	writel(right, capture->vbc + SC9820E_VBC_DAC1_DATA);
}

static struct qin_playback_buffer
playback_buffer(struct snd_pcm_runtime *runtime)
{
	return (struct qin_playback_buffer){
		.samples = runtime->dma_area,
		.application = READ_ONCE(runtime->control->appl_ptr),
		.boundary = runtime->boundary,
		.frames = runtime->buffer_size,
		.period = runtime->period_size,
		.draining = READ_ONCE(runtime->state) ==
			    SNDRV_PCM_STATE_DRAINING,
	};
}

static void playback_service(struct qin_capture *capture,
			     struct snd_pcm_runtime *runtime)
{
	struct qin_playback_buffer buffer = playback_buffer(runtime);
	struct qin_playback_step step;
	int ret;

	capture->playback_status[0] =
		readl(capture->vbc + SC9820E_VBC_DAC0_STATUS);
	capture->playback_status[1] =
		readl(capture->vbc + SC9820E_VBC_DAC1_STATUS);
	ret = qin_playback_service(&capture->playback, &buffer,
				   capture->playback_status[0],
				   capture->playback_status[1],
				   playback_write_frame, capture, &step);
	if (ret < 0) {
		capture->xrun_reason = step.fault;
		capture->xrun_pending = true;
	} else if (ret == QIN_PLAYBACK_DRAINED) {
		capture->drained_pending = true;
	} else {
		if (step.advanced) {
			capture->last_progress = jiffies;
		} else if (time_after(jiffies,
				      capture->last_progress +
					      msecs_to_jiffies(
						      QIN_CAPTURE_STALL_MS))) {
			capture->xrun_reason = "FIFO stalled";
			capture->xrun_pending = true;
		}
		if (ret == QIN_PLAYBACK_PERIOD)
			capture->period_pending = true;
	}
}

static void capture_service(struct qin_capture *capture,
			    struct snd_pcm_runtime *runtime)
{
	__le16 *samples = (__le16 *)runtime->dma_area;
	snd_pcm_uframes_t application = READ_ONCE(runtime->control->appl_ptr);
	unsigned long free_frames;
	unsigned int read_ptr;
	u32 status;
	int available;
	int i;

	if (qin_capture_ring_free(capture->capture_ptr, application,
				  runtime->boundary, runtime->buffer_size,
				  &free_frames)) {
		capture->xrun_reason = "application pointer invalid";
		goto xrun;
	}
	status = readl(capture->vbc + SC9820E_VBC_ADC0_STATUS);
	available = sprd_vbc_fifo_capture_frames(status);
	capture->last_fifo_status = status;
	if (available < 0) {
		capture->xrun_reason = available == -EPIPE ?
					       "FIFO overrun" :
					       "FIFO state invalid";
		goto xrun;
	}
	if (available > free_frames) {
		capture->xrun_reason = "ALSA ring overrun";
		goto xrun;
	}
	read_ptr = sprd_vbc_fifo_read(status);
	/* The one occupancy snapshot bounds IRQ work and never chases the writer. */
	for (i = 0; i < available; i++) {
		u16 sample = readl(capture->vbc + SC9820E_VBC_ADC0_DATA);

		samples[capture->hw_pos] = cpu_to_le16(sample);
		if (++capture->hw_pos == runtime->buffer_size)
			capture->hw_pos = 0;
		if (++capture->capture_ptr == runtime->boundary)
			capture->capture_ptr = 0;
	}
	if (read_ptr + available >= SPRD_VBC_FIFO_FRAMES)
		capture->fifo_wraps++;
	capture->captured_frames += available;
	if (available) {
		capture->last_progress = jiffies;
	} else if (time_after(jiffies,
			      capture->last_progress +
				      msecs_to_jiffies(QIN_CAPTURE_STALL_MS))) {
		capture->xrun_reason = "FIFO stalled";
		goto xrun;
	}
	capture->period_frames += available;
	if (capture->period_frames >= runtime->period_size) {
		capture->period_frames %= runtime->period_size;
		capture->period_pending = true;
	}
	return;
xrun:
	capture->xrun_pending = true;
}

static enum hrtimer_restart capture_timer(struct hrtimer *timer)
{
	struct qin_capture *capture =
		container_of(timer, struct qin_capture, timer);
	bool restart;
	bool notify;

	spin_lock(&capture->fifo_lock);
	if (capture->running && capture->substream && !capture->xrun_pending &&
	    !capture->drained_pending) {
		if (capture->substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
			playback_service(capture, capture->substream->runtime);
		else
			capture_service(capture, capture->substream->runtime);
	}
	notify = capture->period_pending || capture->xrun_pending ||
		 capture->drained_pending;
	restart = capture->running && !capture->xrun_pending;
	spin_unlock(&capture->fifo_lock);
	if (notify)
		wake_up(&capture->thread_wait);
	if (!restart)
		return HRTIMER_NORESTART;
	hrtimer_forward_now(timer, ns_to_ktime(QIN_CAPTURE_PERIOD_NS));
	return HRTIMER_RESTART;
}

static void notify_capture(struct qin_capture *capture)
{
	struct snd_pcm_substream *substream;
	unsigned long flags;
	const char *reason = NULL;
	bool period = false;
	bool drained = false;
	bool xrun = false;

	spin_lock_irqsave(&capture->fifo_lock, flags);
	substream = capture->substream;
	spin_unlock_irqrestore(&capture->fifo_lock, flags);
	if (!substream)
		return;
	snd_pcm_stream_lock(substream);
	spin_lock_irqsave(&capture->fifo_lock, flags);
	if (capture->substream != substream || !substream->runtime ||
	    !capture->running) {
		spin_unlock_irqrestore(&capture->fifo_lock, flags);
		snd_pcm_stream_unlock(substream);
		return;
	}
	period = capture->period_pending;
	drained = capture->drained_pending;
	xrun = capture->xrun_pending;
	reason = capture->xrun_reason;
	capture->period_pending = false;
	capture->drained_pending = false;
	spin_unlock_irqrestore(&capture->fifo_lock, flags);
	if (xrun) {
		if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
			dev_err(capture->dev,
				"playback %s: DAC0=%#x DAC1=%#x top=%#x rate=%#x\n",
				reason, capture->playback_status[0],
				capture->playback_status[1],
				readl(capture->aud + SC9820E_AUD_TOP),
				readl(capture->aud + SC9820E_AUD_DAC));
		} else {
			dev_err(capture->dev, "sampled ADC0 status=%#x\n",
				capture->last_fifo_status);
			report_capture_hardware(capture, reason);
		}
		snd_pcm_stop(substream, SNDRV_PCM_STATE_XRUN);
	} else if (period || drained) {
		/* Account the final short period before ending a natural drain. */
		snd_pcm_period_elapsed_under_stream_lock(substream);
		if (drained &&
		    substream->runtime->state == SNDRV_PCM_STATE_DRAINING)
			snd_pcm_drain_done(substream);
	}
	snd_pcm_stream_unlock(substream);
}

static int capture_thread(void *data)
{
	struct qin_capture *capture = data;

	while (!kthread_should_stop()) {
		wait_event_interruptible(
			capture->thread_wait,
			kthread_should_stop() ||
				READ_ONCE(capture->period_pending) ||
				READ_ONCE(capture->drained_pending) ||
				READ_ONCE(capture->xrun_pending));
		if (kthread_should_stop())
			break;
		notify_capture(capture);
	}
	return 0;
}

static int capture_open(struct snd_pcm_substream *substream)
{
	struct qin_capture *capture = snd_pcm_substream_chip(substream);
	unsigned long flags;
	int ret = 0;

	mutex_lock(&capture->lock);
	if (capture->removing) {
		ret = -ENODEV;
	} else if (capture->suspended) {
		ret = -ESTRPIPE;
	} else if (capture->substream) {
		ret = -EBUSY;
	} else {
		substream->runtime->hw =
			substream->stream == SNDRV_PCM_STREAM_PLAYBACK ?
				qin_playback_hardware :
				qin_capture_hardware;
		spin_lock_irqsave(&capture->fifo_lock, flags);
		capture->substream = substream;
		spin_unlock_irqrestore(&capture->fifo_lock, flags);
	}
	mutex_unlock(&capture->lock);
	return ret;
}

static int capture_close(struct snd_pcm_substream *substream)
{
	struct qin_capture *capture = snd_pcm_substream_chip(substream);
	unsigned long flags;
	int ret;

	/* Quiesce the notifier before cleanup gates any borrowed register bank. */
	snd_pcm_stream_lock(substream);
	mutex_lock(&capture->lock);
	ret = shutdown_capture(capture);
	spin_lock_irqsave(&capture->fifo_lock, flags);
	capture->substream = NULL;
	spin_unlock_irqrestore(&capture->fifo_lock, flags);
	mutex_unlock(&capture->lock);
	snd_pcm_stream_unlock(substream);
	return ret;
}

static int capture_prepare(struct snd_pcm_substream *substream)
{
	struct qin_capture *capture = snd_pcm_substream_chip(substream);
	unsigned long flags;
	int ret;

	mutex_lock(&capture->lock);
	if (capture->removing || capture->suspended) {
		ret = capture->removing ? -ENODEV : -ESTRPIPE;
		goto out;
	}
	ret = substream->stream == SNDRV_PCM_STREAM_PLAYBACK ?
		      prepare_playback(capture, substream->runtime->rate) :
		      prepare_hardware(capture);
	if (ret)
		goto out;
	spin_lock_irqsave(&capture->fifo_lock, flags);
	capture->hw_pos = 0;
	capture->capture_ptr = 0;
	capture->period_frames = 0;
	capture->playback = (struct qin_playback){};
	capture->captured_frames = 0;
	capture->fifo_wraps = 0;
	capture->period_pending = false;
	capture->drained_pending = false;
	capture->xrun_pending = false;
	spin_unlock_irqrestore(&capture->fifo_lock, flags);
out:
	mutex_unlock(&capture->lock);
	return ret;
}

static int capture_hw_free(struct snd_pcm_substream *substream)
{
	struct qin_capture *capture = snd_pcm_substream_chip(substream);
	int ret;

	/* HW_FREE also handles a PREPARED stream which never received STOP. */
	snd_pcm_stream_lock(substream);
	mutex_lock(&capture->lock);
	ret = shutdown_capture(capture);
	mutex_unlock(&capture->lock);
	snd_pcm_stream_unlock(substream);
	return ret;
}

static int start_playback(struct qin_capture *capture,
			  struct snd_pcm_runtime *runtime)
{
	struct qin_playback_buffer buffer;
	struct qin_hp_io io = hp_io(capture);
	unsigned long flags;
	int ret;

	ret = qin_hp_mute(&capture->hp, &io);
	if (ret)
		return ret;
	/* START owns an OFF attempt even if the enable transfer reports failure. */
	capture->amplifier_pending = true;
	ret = aw87339_start(capture->amplifier, &qin_hp_speaker_profile);
	if (ret)
		return ret;
	msleep(22);
	ret = qin_hp_set_volume(&capture->hp, &io, capture->volume);
	if (ret)
		return ret;
	buffer = playback_buffer(runtime);
	spin_lock_irqsave(&capture->fifo_lock, flags);
	capture->playback_status[0] =
		readl(capture->vbc + SC9820E_VBC_DAC0_STATUS);
	capture->playback_status[1] =
		readl(capture->vbc + SC9820E_VBC_DAC1_STATUS);
	ret = qin_playback_prime(&capture->playback, &buffer,
				 capture->playback_status[0],
				 capture->playback_status[1],
				 playback_write_frame, capture);
	if (!ret) {
		capture->playback_status[0] =
			readl(capture->vbc + SC9820E_VBC_DAC0_STATUS);
		capture->playback_status[1] =
			readl(capture->vbc + SC9820E_VBC_DAC1_STATUS);
		ret = qin_playback_check_prefill(&capture->playback,
						 capture->playback_status[0],
						 capture->playback_status[1]);
	}
	spin_unlock_irqrestore(&capture->fifo_lock, flags);
	if (ret) {
		dev_err(capture->dev,
			"playback prefill failed: %d submitted=%llu DAC0=%#x DAC1=%#x\n",
			ret, capture->playback.submitted_frames,
			capture->playback_status[0],
			capture->playback_status[1]);
		return ret;
	}
	dev_info(capture->dev,
		 "playback primed: submitted=%llu DAC0=%#x DAC1=%#x\n",
		 capture->playback.submitted_frames,
		 capture->playback_status[0], capture->playback_status[1]);
	/* Fill both lanes before the FIFO consumer is allowed to advance. */
	ret = capture_update(capture, QIN_CAPTURE_VBC, SC9820E_VBC_ENABLE,
			     SC9820E_VBC_DAC01 | SC9820E_VBC_DAC01_DMA,
			     SC9820E_VBC_DAC01);
	if (ret)
		return ret;
	capture->playback_dac_active = true;
	return capture_update(capture, QIN_CAPTURE_AUD, SC9820E_AUD_TOP,
			      SC9820E_AUD_DACS, SC9820E_AUD_DACS);
}

static int capture_trigger(struct snd_pcm_substream *substream, int command)
{
	struct qin_capture *capture = snd_pcm_substream_chip(substream);
	unsigned long flags;
	int cleanup_ret;
	int ret = 0;

	mutex_lock(&capture->lock);
	switch (command) {
	case SNDRV_PCM_TRIGGER_START:
		if (capture->removing || capture->suspended) {
			ret = capture->removing ? -ENODEV : -ESTRPIPE;
			break;
		}
		if (!capture->prepared) {
			ret = -EBADFD;
			break;
		}
		if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK &&
		    (!capture->hp.ready || capture->hp.fault)) {
			ret = -EIO;
			break;
		}
		ret = substream->stream == SNDRV_PCM_STREAM_PLAYBACK ?
			      start_playback(capture, substream->runtime) :
			      start_receiver(capture);
		if (ret) {
			cleanup_ret = shutdown_capture(capture);
			if (cleanup_ret)
				dev_err(capture->dev,
					"start cleanup failed: %d\n",
					cleanup_ret);
			break;
		}
		spin_lock_irqsave(&capture->fifo_lock, flags);
		capture->last_progress = jiffies;
		capture->running = true;
		spin_unlock_irqrestore(&capture->fifo_lock, flags);
		hrtimer_start(&capture->timer,
			      ns_to_ktime(QIN_CAPTURE_PERIOD_NS),
			      HRTIMER_MODE_REL);
		break;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
		ret = shutdown_capture(capture);
		if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
			dev_info(
				capture->dev,
				"playback stopped: submitted=%llu consumed=%llu DAC0=%#x DAC1=%#x\n",
				capture->playback.submitted_frames,
				capture->playback.consumed_frames,
				capture->playback_status[0],
				capture->playback_status[1]);
		else
			dev_info(capture->dev,
				 "capture stopped: frames=%llu read-wraps=%u\n",
				 capture->captured_frames, capture->fifo_wraps);
		break;
	default:
		ret = -EINVAL;
		break;
	}
	mutex_unlock(&capture->lock);
	return ret;
}

/* The caller owns the PCM mutex; notification takes the stream lock later. */
static bool playback_fault(struct qin_capture *capture, const char *reason)
{
	unsigned long flags;
	bool running;

	capture->prepared = false;
	capture->hp.ready = false;
	capture->hp.fault = true;
	spin_lock_irqsave(&capture->fifo_lock, flags);
	running = capture->running;
	if (running) {
		capture->xrun_reason = reason;
		capture->xrun_pending = true;
	}
	spin_unlock_irqrestore(&capture->fifo_lock, flags);
	return running;
}

static int master_volume_info(struct snd_kcontrol *control,
			      struct snd_ctl_elem_info *info)
{
	(void)control;
	info->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	info->count = 1;
	info->value.integer.min = 0;
	info->value.integer.max = QIN_HP_VOLUME_MAX;
	info->value.integer.step = 1;
	return 0;
}

static int master_volume_get(struct snd_kcontrol *control,
			     struct snd_ctl_elem_value *value)
{
	struct qin_capture *capture = snd_kcontrol_chip(control);

	mutex_lock(&capture->lock);
	value->value.integer.value[0] = capture->volume;
	mutex_unlock(&capture->lock);
	return 0;
}

static int master_volume_put(struct snd_kcontrol *control,
			     struct snd_ctl_elem_value *value)
{
	struct qin_capture *capture = snd_kcontrol_chip(control);
	struct qin_hp_io io = hp_io(capture);
	long requested = value->value.integer.value[0];
	unsigned long flags;
	bool apply;
	bool notify = false;
	int ret = 0;

	if (requested < 0 || requested > QIN_HP_VOLUME_MAX)
		return -EINVAL;
	mutex_lock(&capture->lock);
	if (capture->removing) {
		ret = -ENODEV;
		goto out;
	}
	if (capture->volume == requested)
		goto out;
	spin_lock_irqsave(&capture->fifo_lock, flags);
	apply = capture->running && !capture->xrun_pending &&
		!capture->drained_pending && capture->substream &&
		capture->substream->stream == SNDRV_PCM_STREAM_PLAYBACK;
	spin_unlock_irqrestore(&capture->fifo_lock, flags);
	if (apply) {
		ret = qin_hp_set_volume(&capture->hp, &io, requested);
		if (ret) {
			int cleanup_ret;

			notify =
				playback_fault(capture, "volume update failed");
			cleanup_ret = quiet_speaker(capture);
			if (cleanup_ret)
				dev_err(capture->dev,
					"volume cleanup failed: %d\n",
					cleanup_ret);
			goto out;
		}
	}
	/* An inactive or PREPARED route stores only the requested level. */
	capture->volume = requested;
	ret = 1;
out:
	mutex_unlock(&capture->lock);
	if (notify)
		wake_up(&capture->thread_wait);
	return ret;
}

static const struct snd_kcontrol_new master_volume_control = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "Master Playback Volume",
	.access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
	.info = master_volume_info,
	.get = master_volume_get,
	.put = master_volume_put,
};

static snd_pcm_uframes_t capture_pointer(struct snd_pcm_substream *substream)
{
	struct qin_capture *capture = snd_pcm_substream_chip(substream);
	snd_pcm_uframes_t position;
	unsigned long flags;

	spin_lock_irqsave(&capture->fifo_lock, flags);
	position = substream->stream == SNDRV_PCM_STREAM_PLAYBACK ?
			   capture->playback.hw_pos :
			   capture->hw_pos;
	spin_unlock_irqrestore(&capture->fifo_lock, flags);
	return position;
}

static int capture_ioctl(struct snd_pcm_substream *substream,
			 unsigned int command, void *argument)
{
	struct qin_capture *capture = snd_pcm_substream_chip(substream);
	struct snd_pcm_runtime *runtime = substream->runtime;
	unsigned long flags;
	int ret = 0;

	if (command != SNDRV_PCM_IOCTL1_RESET)
		return snd_pcm_lib_ioctl(substream, command, argument);
	/* RESET rebases ALSA pointers; an active FIFO copy cannot follow it. */
	snd_pcm_stream_lock(substream);
	if (snd_pcm_running(substream)) {
		ret = -EBUSY;
		goto out;
	}
	spin_lock_irqsave(&capture->fifo_lock, flags);
	capture->hw_pos = 0;
	capture->capture_ptr = 0;
	capture->period_frames = 0;
	capture->playback = (struct qin_playback){};
	capture->period_pending = false;
	capture->drained_pending = false;
	capture->xrun_pending = false;
	spin_unlock_irqrestore(&capture->fifo_lock, flags);
	/* The generic non-running RESET branch, with this stream lock held. */
	runtime->status->hw_ptr = 0;
	runtime->hw_ptr_wrap = 0;
out:
	snd_pcm_stream_unlock(substream);
	return ret;
}

static const struct snd_pcm_ops qin_capture_ops = {
	.open = capture_open,
	.close = capture_close,
	.ioctl = capture_ioctl,
	.hw_free = capture_hw_free,
	.prepare = capture_prepare,
	.trigger = capture_trigger,
	.pointer = capture_pointer,
};

/* Borrow unregistered banks described by DT; the PMIC owns its own transport. */
static void __iomem *map_phandle(struct device *dev, const char *property,
				 resource_size_t minimum_size)
{
	struct device_node *node;
	struct resource resource;
	int ret;

	node = of_parse_phandle(dev->of_node, property, 0);
	if (!node)
		return IOMEM_ERR_PTR(-EINVAL);
	ret = of_address_to_resource(node, 0, &resource);
	of_node_put(node);
	if (ret)
		return IOMEM_ERR_PTR(ret);
	if (resource_size(&resource) < minimum_size)
		return IOMEM_ERR_PTR(-EINVAL);
	return devm_ioremap(dev, resource.start, resource_size(&resource));
}

static void capture_remove(struct platform_device *pdev)
{
	struct qin_capture *capture = platform_get_drvdata(pdev);
	struct snd_pcm_substream *substream;

	snd_card_disconnect(capture->card);
	mutex_lock(&capture->lock);
	capture->removing = true;
	if (capture->hp.irq_enabled) {
		capture->hp.irq_enabled = false;
		disable_irq_nosync(capture->codec_irq);
	}
	stop_copying(capture);
	substream = capture->substream;
	mutex_unlock(&capture->lock);
	kthread_stop(capture->thread);
	if (substream) {
		snd_pcm_stream_lock(substream);
		snd_pcm_stream_unlock(substream);
	}
	mutex_lock(&capture->lock);
	shutdown_capture(capture);
	mutex_unlock(&capture->lock);
}

static irqreturn_t capture_codec_irq(int irq, void *data)
{
	struct qin_capture *capture = data;
	struct qin_hp_io io = hp_io(capture);
	u32 cause;
	u32 status;
	bool notify = false;
	bool running;
	int ret;
	int amplifier_ret;

	(void)irq;
	mutex_lock(&capture->lock);
	ret = qin_hp_interrupt(&capture->hp, &io, &cause, &status);
	if (!ret) {
		mutex_unlock(&capture->lock);
		return IRQ_NONE;
	}
	dev_err(capture->dev,
		"analog protection: result=%d IRQ=%#x status=%#x\n", ret, cause,
		status);
	running = playback_fault(capture, "analog protection");
	notify = running;
	amplifier_ret = stop_amplifier(capture);
	if (amplifier_ret)
		dev_err(capture->dev, "protection amplifier stop failed: %d\n",
			amplifier_ret);
	if (!running)
		shutdown_capture(capture);
	mutex_unlock(&capture->lock);
	if (notify)
		wake_up(&capture->thread_wait);
	return IRQ_HANDLED;
}

static int capture_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct qin_capture *capture;
	struct snd_card *card;
	struct snd_pcm *pcm;
	u32 analog_base;
	int ret;

	ret = of_property_read_u32(dev->of_node, "reg", &analog_base);
	if (ret || analog_base != SC2721_PMU0)
		return dev_err_probe(dev, -EINVAL,
				     "invalid SC2721 analog base\n");
	capture = devm_kzalloc(dev, sizeof(*capture), GFP_KERNEL);
	if (!capture)
		return -ENOMEM;
	capture->dev = dev;
	capture->volume = 1;
	ret = qin_audio_profile_load(dev, &capture->profile);
	if (ret)
		return ret;
	capture->amplifier = devm_aw87339_get(dev, "fplinux,speaker-amplifier");
	if (IS_ERR(capture->amplifier))
		return dev_err_probe(dev, PTR_ERR(capture->amplifier),
				     "speaker amplifier is unavailable\n");
	capture->pmic = dev_get_regmap(dev->parent, NULL);
	if (!capture->pmic)
		return dev_err_probe(dev, -ENODEV,
				     "PMIC parent has no regmap\n");
	capture->aon = syscon_regmap_lookup_by_phandle(dev->of_node,
						       "sprd,aon-syscon");
	if (IS_ERR(capture->aon))
		return dev_err_probe(dev, PTR_ERR(capture->aon),
				     "cannot find AON syscon\n");
	capture->pmu = syscon_regmap_lookup_by_phandle(dev->of_node,
						       "sprd,pmu-syscon");
	if (IS_ERR(capture->pmu))
		return dev_err_probe(dev, PTR_ERR(capture->pmu),
				     "cannot find PMU syscon\n");
	capture->g6 =
		syscon_regmap_lookup_by_phandle(dev->of_node, "sprd,g6-syscon");
	if (IS_ERR(capture->g6))
		return dev_err_probe(dev, PTR_ERR(capture->g6),
				     "cannot find G6 syscon\n");
	capture->aud = map_phandle(dev, "sprd,digital-codec", 0x2000);
	if (IS_ERR_OR_NULL(capture->aud))
		return dev_err_probe(
			dev, capture->aud ? PTR_ERR(capture->aud) : -ENOMEM,
			"cannot map digital codec\n");
	capture->vbc = map_phandle(dev, "sprd,vbc", 0x10000);
	if (IS_ERR_OR_NULL(capture->vbc))
		return dev_err_probe(
			dev, capture->vbc ? PTR_ERR(capture->vbc) : -ENOMEM,
			"cannot map VBC\n");
	capture->stable = map_phandle(dev, "sprd,clock-stable", 4);
	if (IS_ERR_OR_NULL(capture->stable))
		return dev_err_probe(
			dev,
			capture->stable ? PTR_ERR(capture->stable) : -ENOMEM,
			"cannot map clock status\n");
	mutex_init(&capture->lock);
	spin_lock_init(&capture->fifo_lock);
	init_waitqueue_head(&capture->thread_wait);
	hrtimer_setup(&capture->timer, capture_timer, CLOCK_MONOTONIC,
		      HRTIMER_MODE_REL);
	capture->codec_irq = platform_get_irq(pdev, 0);
	if (capture->codec_irq < 0)
		return capture->codec_irq;
	ret = devm_request_threaded_irq(dev, capture->codec_irq, NULL,
					capture_codec_irq,
					IRQF_ONESHOT | IRQF_NO_AUTOEN,
					dev_name(dev), capture);
	if (ret)
		return dev_err_probe(dev, ret, "failed to request codec IRQ\n");
	ret = snd_devm_card_new(dev, SNDRV_DEFAULT_IDX1, SNDRV_DEFAULT_STR1,
				THIS_MODULE, 0, &card);
	if (ret)
		return ret;
	capture->card = card;
	ret = snd_ctl_add(card, snd_ctl_new1(&master_volume_control, capture));
	if (ret)
		return snd_card_free_on_error(dev, ret);
	ret = snd_pcm_new(card, QIN_AUDIO_NAME, 0, 1, 1, &pcm);
	if (ret)
		return snd_card_free_on_error(dev, ret);
	capture->pcm = pcm;
	pcm->private_data = capture;
	pcm->nonatomic = true;
	strscpy(pcm->name, QIN_AUDIO_NAME);
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &qin_capture_ops);
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE, &qin_capture_ops);
	ret = snd_pcm_set_managed_buffer_all(pcm, SNDRV_DMA_TYPE_VMALLOC, NULL,
					     QIN_CAPTURE_BUFFER_BYTES,
					     QIN_CAPTURE_BUFFER_BYTES);
	if (ret)
		return snd_card_free_on_error(dev, ret);
	strscpy(card->driver, "SC9820E audio");
	strscpy(card->shortname, QIN_AUDIO_NAME);
	strscpy(card->longname, "SC9820E SC2721 PCM");
	platform_set_drvdata(pdev, capture);
	capture->thread =
		kthread_run(capture_thread, capture, "sc9820e-capture");
	if (IS_ERR(capture->thread))
		return snd_card_free_on_error(dev, PTR_ERR(capture->thread));
	sched_set_fifo_low(capture->thread);
	ret = snd_card_register(card);
	if (ret) {
		kthread_stop(capture->thread);
		return snd_card_free_on_error(dev, ret);
	}
	return 0;
}

static int capture_suspend(struct device *dev)
{
	struct qin_capture *capture = dev_get_drvdata(dev);
	int ret;
	int cleanup_ret;

	mutex_lock(&capture->lock);
	capture->suspended = true;
	mutex_unlock(&capture->lock);
	ret = snd_pcm_suspend_all(capture->pcm);
	mutex_lock(&capture->lock);
	cleanup_ret = shutdown_capture(capture);
	mutex_unlock(&capture->lock);
	return ret ? ret : cleanup_ret;
}

static int capture_resume(struct device *dev)
{
	struct qin_capture *capture = dev_get_drvdata(dev);

	mutex_lock(&capture->lock);
	capture->suspended = false;
	mutex_unlock(&capture->lock);
	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(qin_capture_pm_ops, capture_suspend,
				capture_resume);

static const struct of_device_id qin_capture_of_match[] = {
	{ .compatible = "fplinux,qin1splus-capture" },
	{}
};
MODULE_DEVICE_TABLE(of, qin_capture_of_match);

static struct platform_driver qin_capture_driver = {
	.probe = capture_probe,
	.remove = capture_remove,
	.shutdown = capture_remove,
	.driver = {
		.name = "qin1splus-capture",
		.of_match_table = qin_capture_of_match,
		.pm = pm_sleep_ptr(&qin_capture_pm_ops),
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(qin_capture_driver);

MODULE_DESCRIPTION("SC9820E SC2721 capture and speaker playback PCM");
MODULE_LICENSE("GPL");
