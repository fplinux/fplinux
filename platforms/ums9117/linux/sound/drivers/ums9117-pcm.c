// SPDX-License-Identifier: GPL-2.0-only
#include <linux/err.h>
#include <linux/firmware.h>
#include <linux/hrtimer.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/sched.h>
#include <linux/property.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/wait.h>

#include <sound/control.h>
#include <sound/core.h>
#include <sound/initval.h>
#include <sound/pcm.h>
#include <sound/tlv.h>

#include "ums9117-sc2720-codec.h"
#include "ums9117-audio.h"

#define UMS9117_PCM_CHANNELS 2U
#define UMS9117_PCM_BUFFER_BYTES_MAX (256U * 1024U)
#define UMS9117_PCM_PERIOD_BYTES_MIN 384U
#define UMS9117_PCM_PERIOD_BYTES_MAX (32U * 1024U)
/*
 * A rate-converting application receives a period and buffer obtained from these
 * by integer division, and its playback speed follows that pair of sizes. Sizes
 * that are not a whole number of application frames therefore play at the wrong
 * speed. 480 frames divides every common conversion exactly, including 8000,
 * 16000, 24000, 32000 and 44100 Hz sources.
 */
#define UMS9117_PCM_SIZE_FRAMES_STEP 480U
/*
 * The FIFO is refilled from a timer interrupt so that a runnable but not yet
 * scheduled task cannot delay it. The 320-frame FIFO holds 6.7 ms at 48 kHz,
 * the clocksource resolves 1 ms, and the longest interrupts-off section in
 * the system is an ADI transaction whose polling budget is 3 ms. A 1 ms period
 * leaves the FIFO more than half full after both delays add up.
 */
#define UMS9117_PCM_SERVICE_PERIOD_NS NSEC_PER_MSEC
#define UMS9117_PCM_STALL_MS 100U
#define UMS9117_PCM_IDLE_RATE 48000U
#define UMS9117_PCM_THREAD_NAME "ums9117-pcm"
#define UMS9117_PCM_NAME "UMS9117 Headphones"
#define UMS9117_AUDIO_PAD_COUNT 4U
#define UMS9117_AUDIO_PAD_CELLS 3U

#define UMS9117_AUDIO_PROFILE_MAGIC_SIZE 8U
#define UMS9117_AUDIO_PROFILE_COMPATIBLE_SIZE 24U
#define UMS9117_AUDIO_PROFILE_LEVEL_COUNT 9U
#define UMS9117_AUDIO_PROFILE_HEADPHONE_PGA_MIN 2U
#define UMS9117_AUDIO_PROFILE_HEADPHONE_PGA_MAX 7U
#define UMS9117_AUDIO_PROFILE_SOURCE_EQ_BYPASS 0U
#define UMS9117_AUDIO_PROFILE_SOURCE_EQ_ACTIVE_OMITTED 1U

static const u8 ums9117_audio_profile_magic[UMS9117_AUDIO_PROFILE_MAGIC_SIZE] = {
	'F', 'P', 'A', 'U', 'D', 'I', 'O', '\0'
};

struct ums9117_audio_profile_data {
	u8 magic[UMS9117_AUDIO_PROFILE_MAGIC_SIZE];
	u8 compatible[UMS9117_AUDIO_PROFILE_COMPATIBLE_SIZE];
	u8 headphone_pga;
	u8 source_eq;
	u8 dac_gain[UMS9117_AUDIO_PROFILE_LEVEL_COUNT];
};

struct ums9117_audio_pad {
	u32 offset;
	u32 mux;
	u32 config;
};

static const char *const ums9117_audio_pad_names[UMS9117_AUDIO_PAD_COUNT] = {
	"SCLK",
	"DASYNC",
	"DAD0",
	"DAD1",
};

struct ums9117_audio_profile {
	u8 dac_gain[UMS9117_AUDIO_PROFILE_LEVEL_COUNT];
	u8 codec_volume;
	bool source_eq_omitted;
	bool fitted;
};

struct ums9117_pcm {
	struct device *dev;
	struct snd_card *card;
	struct snd_pcm *pcm;
	struct ums9117_audio *digital;
	struct ums9117_sc2720_codec *codec;
	void __iomem *pinmux;
	void __iomem *pinconf;
	struct task_struct *thread;
	wait_queue_head_t thread_wait;
	struct hrtimer timer;
	/* Serializes the hardware lifecycle; the timer never takes it. */
	struct mutex lock;
	unsigned int rate;
	struct ums9117_audio_pad pads[UMS9117_AUDIO_PAD_COUNT];
	struct ums9117_audio_profile profile;
	unsigned int volume_left;
	unsigned int volume_right;
	bool codec_prepared;
	bool digital_prepared;
	bool idle_silence;
	bool suspended;
	bool removing;
	/*
	 * Protects the streaming state below. The timer callback takes it from
	 * hard interrupt context; process context disables interrupts and does
	 * not sleep or cancel the timer while holding it. Lifecycle code writes
	 * these fields with the mutex held as well.
	 */
	spinlock_t fifo_lock;
	struct snd_pcm_substream *substream;
	snd_pcm_uframes_t submit_ptr;
	u64 submitted_frames;
	u64 consumed_frames;
	u64 leading_silence_frames;
	snd_pcm_uframes_t hw_pos;
	snd_pcm_uframes_t period_frames;
	unsigned long last_progress;
	bool running;
	bool idle_running;
	/* Results the timer leaves for the notification thread. */
	bool period_pending;
	bool drained_pending;
	bool xrun_pending;
	int idle_error;
};

enum ums9117_pcm_service_result {
	UMS9117_PCM_SERVICE_IDLE,
	UMS9117_PCM_SERVICE_PERIOD,
	UMS9117_PCM_SERVICE_DRAINED,
	UMS9117_PCM_SERVICE_XRUN,
};

static const struct snd_pcm_hardware ums9117_pcm_hardware = {
	.info = SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_BLOCK_TRANSFER |
		SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_MMAP_VALID |
		SNDRV_PCM_INFO_FIFO_IN_FRAMES | SNDRV_PCM_INFO_NO_REWINDS,
	.formats = SNDRV_PCM_FMTBIT_S16_LE,
	.rates = SNDRV_PCM_RATE_24000 | SNDRV_PCM_RATE_48000,
	.rate_min = 24000,
	.rate_max = 48000,
	.channels_min = UMS9117_PCM_CHANNELS,
	.channels_max = UMS9117_PCM_CHANNELS,
	.buffer_bytes_max = UMS9117_PCM_BUFFER_BYTES_MAX,
	.period_bytes_min = UMS9117_PCM_PERIOD_BYTES_MIN,
	.period_bytes_max = UMS9117_PCM_PERIOD_BYTES_MAX,
	.periods_min = 2,
	.periods_max =
		UMS9117_PCM_BUFFER_BYTES_MAX / UMS9117_PCM_PERIOD_BYTES_MIN,
	.fifo_size = UMS9117_AUDIO_FIFO_FRAMES,
};

static int
ums9117_pcm_parse_audio_profile(const char *machine_compatible,
				const struct firmware *firmware,
				struct ums9117_audio_profile *profile)
{
	u8 compatible[UMS9117_AUDIO_PROFILE_COMPATIBLE_SIZE] = {};
	const struct ums9117_audio_profile_data *data;
	size_t compatible_length;
	u8 headphone_pga;
	u8 source_eq;
	unsigned int i;

	if (firmware->size != sizeof(*data))
		return -EINVAL;
	data = (const struct ums9117_audio_profile_data *)firmware->data;
	if (memcmp(data->magic, ums9117_audio_profile_magic,
		   UMS9117_AUDIO_PROFILE_MAGIC_SIZE))
		return -EINVAL;
	compatible_length = strlen(machine_compatible);
	if (compatible_length >= sizeof(compatible))
		return -EINVAL;
	memcpy(compatible, machine_compatible, compatible_length);
	if (memcmp(data->compatible, compatible, sizeof(compatible)))
		return -EINVAL;
	headphone_pga = data->headphone_pga;
	if (headphone_pga < UMS9117_AUDIO_PROFILE_HEADPHONE_PGA_MIN ||
	    headphone_pga > UMS9117_AUDIO_PROFILE_HEADPHONE_PGA_MAX)
		return -EINVAL;
	source_eq = data->source_eq;
	if (source_eq != UMS9117_AUDIO_PROFILE_SOURCE_EQ_BYPASS &&
	    source_eq != UMS9117_AUDIO_PROFILE_SOURCE_EQ_ACTIVE_OMITTED)
		return -EINVAL;

	for (i = 0; i < UMS9117_AUDIO_PROFILE_LEVEL_COUNT; i++) {
		u8 gain = data->dac_gain[i];

		if (gain > 127U)
			return -EINVAL;
		if (i && data->dac_gain[i - 1] < gain)
			return -EINVAL;
		profile->dac_gain[i] = gain;
	}
	profile->codec_volume = headphone_pga - 1U;
	profile->source_eq_omitted =
		source_eq == UMS9117_AUDIO_PROFILE_SOURCE_EQ_ACTIVE_OMITTED;
	profile->fitted = true;
	return 0;
}

static int ums9117_pcm_load_audio_profile(struct ums9117_pcm *audio)
{
	const struct firmware *firmware;
	const char *machine_compatible;
	const char *firmware_name;
	int ret;

	if (!device_property_present(audio->dev, "firmware-name"))
		return 0;
	ret = device_property_read_string(audio->dev, "firmware-name",
					  &firmware_name);
	if (ret)
		return dev_err_probe(audio->dev, ret,
				     "invalid audio firmware name\n");
	ret = request_firmware(&firmware, firmware_name, audio->dev);
	if (ret == -ENOENT)
		return 0;
	if (ret)
		return dev_err_probe(audio->dev, ret,
				     "cannot load headphone audio profile\n");

	ret = of_property_read_string_index(of_root, "compatible", 0,
					    &machine_compatible);
	if (ret) {
		release_firmware(firmware);
		return dev_err_probe(audio->dev, ret,
				     "cannot read machine compatible\n");
	}
	ret = ums9117_pcm_parse_audio_profile(machine_compatible, firmware,
					      &audio->profile);
	release_firmware(firmware);
	if (ret)
		return dev_err_probe(audio->dev, ret,
				     "invalid headphone audio profile\n");
	return 0;
}

static const DECLARE_TLV_DB_SCALE(ums9117_headphone_volume_tlv, -1800, 300, 1);

static unsigned int
ums9117_headphone_volume_max(const struct ums9117_pcm *audio)
{
	if (audio->profile.fitted)
		return UMS9117_AUDIO_PROFILE_LEVEL_COUNT;
	return UMS9117_SC2720_HEADPHONE_VOLUME_MAX;
}

static u8 ums9117_profile_dac_gain(const struct ums9117_audio_profile *profile,
				   unsigned int volume)
{
	/* Logical mute is analog; retain the lowest fitted digital level. */
	return profile->dac_gain[volume ? volume - 1 : 0];
}

static void ums9117_pcm_apply_profile_dac_gain(struct ums9117_pcm *audio,
					       unsigned int left,
					       unsigned int right)
{
	u8 left_gain = ums9117_profile_dac_gain(&audio->profile, left);
	u8 right_gain = ums9117_profile_dac_gain(&audio->profile, right);

	ums9117_audio_set_dac_gain(audio->digital, left_gain, right_gain);
}

static int ums9117_pcm_set_profile_volume_locked(struct ums9117_pcm *audio,
						 unsigned int left,
						 unsigned int right)
{
	unsigned int old_left = audio->volume_left;
	unsigned int old_right = audio->volume_right;
	unsigned int codec_left;
	unsigned int codec_right;
	int ret;

	if (left == old_left && right == old_right)
		return 0;
	codec_left = left ? audio->profile.codec_volume : 0;
	codec_right = right ? audio->profile.codec_volume : 0;
	/* Program DG before an analog unmute can expose the selected level. */
	ums9117_pcm_apply_profile_dac_gain(audio, left, right);
	ret = ums9117_sc2720_codec_set_volume(audio->codec, codec_left,
					      codec_right);
	if (ret < 0) {
		ums9117_pcm_apply_profile_dac_gain(audio, old_left, old_right);
		return ret;
	}
	audio->volume_left = left;
	audio->volume_right = right;
	return 1;
}

static int ums9117_headphone_volume_info(struct snd_kcontrol *kcontrol,
					 struct snd_ctl_elem_info *info)
{
	struct ums9117_pcm *audio = snd_kcontrol_chip(kcontrol);

	info->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	info->count = 2;
	info->value.integer.min = 0;
	info->value.integer.max = ums9117_headphone_volume_max(audio);
	return 0;
}

static int ums9117_headphone_volume_get(struct snd_kcontrol *kcontrol,
					struct snd_ctl_elem_value *value)
{
	struct ums9117_pcm *audio = snd_kcontrol_chip(kcontrol);
	unsigned int left;
	unsigned int right;

	mutex_lock(&audio->lock);
	if (audio->profile.fitted) {
		left = audio->volume_left;
		right = audio->volume_right;
	} else {
		ums9117_sc2720_codec_get_volume(audio->codec, &left, &right);
	}
	mutex_unlock(&audio->lock);
	value->value.integer.value[0] = left;
	value->value.integer.value[1] = right;
	return 0;
}

static int ums9117_headphone_volume_put(struct snd_kcontrol *kcontrol,
					struct snd_ctl_elem_value *value)
{
	struct ums9117_pcm *audio = snd_kcontrol_chip(kcontrol);
	long left = value->value.integer.value[0];
	long right = value->value.integer.value[1];
	int ret;

	if (left < 0 || left > ums9117_headphone_volume_max(audio) ||
	    right < 0 || right > ums9117_headphone_volume_max(audio))
		return -EINVAL;
	mutex_lock(&audio->lock);
	if (audio->profile.fitted)
		ret = ums9117_pcm_set_profile_volume_locked(audio, left, right);
	else
		ret = ums9117_sc2720_codec_set_volume(audio->codec, left,
						      right);
	mutex_unlock(&audio->lock);
	return ret;
}

static const struct snd_kcontrol_new ums9117_headphone_generic_volume_control = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "Headphone Playback Volume",
	.access = SNDRV_CTL_ELEM_ACCESS_READWRITE |
		  SNDRV_CTL_ELEM_ACCESS_TLV_READ,
	.info = ums9117_headphone_volume_info,
	.get = ums9117_headphone_volume_get,
	.put = ums9117_headphone_volume_put,
	.tlv.p = ums9117_headphone_volume_tlv,
};

static const struct snd_kcontrol_new ums9117_headphone_profile_volume_control = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "Headphone Playback Volume",
	.access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
	.info = ums9117_headphone_volume_info,
	.get = ums9117_headphone_volume_get,
	.put = ums9117_headphone_volume_put,
};

static void ums9117_pcm_disable_codec(struct ums9117_pcm *audio)
{
	int ret;

	if (!audio->codec_prepared)
		return;
	ret = ums9117_sc2720_codec_disable(audio->codec);
	if (ret) {
		dev_err(audio->dev, "cannot disable headphone codec: %pe\n",
			ERR_PTR(ret));
		return;
	}
	audio->codec_prepared = false;
}

static void ums9117_pcm_clear_events_locked(struct ums9117_pcm *audio)
{
	audio->period_pending = false;
	audio->drained_pending = false;
	audio->xrun_pending = false;
}

/* Ends both refill modes and leaves no timer armed; the mutex is held. */
static void ums9117_pcm_stop_refill_locked(struct ums9117_pcm *audio)
{
	unsigned long flags;

	spin_lock_irqsave(&audio->fifo_lock, flags);
	audio->running = false;
	audio->idle_running = false;
	ums9117_pcm_clear_events_locked(audio);
	spin_unlock_irqrestore(&audio->fifo_lock, flags);
	hrtimer_cancel(&audio->timer);
}

static void ums9117_pcm_shutdown_locked(struct ums9117_pcm *audio)
{
	ums9117_pcm_stop_refill_locked(audio);
	if (audio->digital_prepared)
		ums9117_audio_stop(audio->digital);
	ums9117_pcm_disable_codec(audio);
	if (audio->digital_prepared)
		ums9117_audio_release(audio->digital);
	audio->digital_prepared = false;
}

static int ums9117_pcm_stop_locked(struct ums9117_pcm *audio)
{
	int ret;

	ums9117_pcm_stop_refill_locked(audio);
	if (audio->digital_prepared)
		ums9117_audio_stop(audio->digital);
	if (audio->codec_prepared) {
		ret = ums9117_sc2720_codec_stop(audio->codec);
		if (ret) {
			dev_err(audio->dev,
				"cannot stop headphone codec: %pe\n",
				ERR_PTR(ret));
			ums9117_pcm_shutdown_locked(audio);
			return ret;
		}
	}
	if (audio->digital_prepared)
		ums9117_audio_release(audio->digital);
	audio->digital_prepared = false;
	return 0;
}

static int ums9117_pcm_validate_pads(struct ums9117_pcm *audio)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(audio->pads); i++) {
		const struct ums9117_audio_pad *pad = &audio->pads[i];
		u32 mux = readl(audio->pinmux + pad->offset);
		u32 config = readl(audio->pinconf + pad->offset);

		if (mux == pad->mux && config == pad->config)
			continue;
		dev_err(audio->dev,
			"audio pad %s unavailable: mux=%#x config=%#x\n",
			ums9117_audio_pad_names[i], mux, config);
		return -EBUSY;
	}
	return 0;
}

static int ums9117_pcm_read_pads(struct device *dev,
				 struct ums9117_audio_pad *pads,
				 resource_size_t pinmux_size,
				 resource_size_t pinconf_size)
{
	u32 settings[UMS9117_AUDIO_PAD_COUNT * UMS9117_AUDIO_PAD_CELLS];
	unsigned int i;
	int ret;

	ret = device_property_read_u32_array(dev, "fplinux,pad-settings",
					     settings, ARRAY_SIZE(settings));
	if (ret)
		return dev_err_probe(dev, ret, "invalid audio pad settings\n");
	for (i = 0; i < UMS9117_AUDIO_PAD_COUNT; i++) {
		unsigned int base = i * UMS9117_AUDIO_PAD_CELLS;

		pads[i].offset = settings[base];
		pads[i].mux = settings[base + 1];
		pads[i].config = settings[base + 2];
		if (!IS_ALIGNED(pads[i].offset, sizeof(u32)) ||
		    pads[i].offset > pinmux_size - sizeof(u32) ||
		    pads[i].offset > pinconf_size - sizeof(u32))
			return dev_err_probe(
				dev, -EINVAL,
				"audio pad %s offset %#x is outside its resources\n",
				ums9117_audio_pad_names[i], pads[i].offset);
	}
	return 0;
}

static int ums9117_pcm_prepare_hardware_locked(struct ums9117_pcm *audio,
					       unsigned int rate)
{
	int ret;

	if (audio->idle_silence && rate != UMS9117_PCM_IDLE_RATE)
		return -EINVAL;
	if (audio->digital_prepared) {
		if (audio->rate == rate)
			return 0;
		ret = ums9117_pcm_stop_locked(audio);
		if (ret)
			return ret;
	}
	ret = ums9117_pcm_validate_pads(audio);
	if (ret)
		goto failed;
	ret = ums9117_audio_prepare(audio->digital, rate);
	if (ret) {
		dev_err(audio->dev, "cannot prepare digital audio: %pe\n",
			ERR_PTR(ret));
		goto failed;
	}
	audio->digital_prepared = true;
	ret = ums9117_sc2720_codec_prepare(audio->codec);
	if (ret) {
		dev_err(audio->dev, "cannot prepare headphone codec: %pe\n",
			ERR_PTR(ret));
		goto failed;
	}
	audio->codec_prepared = true;
	audio->rate = rate;
	return 0;

failed:
	ums9117_pcm_shutdown_locked(audio);
	return ret;
}

/* Called with fifo_lock held, from the timer or with interrupts disabled. */
static int ums9117_pcm_fill_silence_locked(struct ums9117_pcm *audio)
{
	int queued = ums9117_audio_queued(audio->digital);

	if (queued < 0)
		return queued;
	for (; queued < UMS9117_AUDIO_FIFO_FRAMES; queued++)
		ums9117_audio_write(audio->digital, 0, 0);
	return 0;
}

static int ums9117_pcm_start_idle_locked(struct ums9117_pcm *audio)
{
	unsigned long flags;
	int ret;

	if (audio->removing || audio->suspended || audio->idle_running)
		return 0;
	ret = ums9117_pcm_prepare_hardware_locked(audio, UMS9117_PCM_IDLE_RATE);
	if (ret)
		return ret;
	/*
	 * After STOP the timer may still be refilling playback. Switch it to
	 * silence in the same critical section as the first silence fill so it
	 * never services a mixture of the two.
	 */
	spin_lock_irqsave(&audio->fifo_lock, flags);
	audio->running = false;
	audio->idle_running = true;
	ums9117_pcm_clear_events_locked(audio);
	ret = ums9117_pcm_fill_silence_locked(audio);
	spin_unlock_irqrestore(&audio->fifo_lock, flags);
	if (ret)
		goto failed;
	ret = ums9117_sc2720_codec_enable(audio->codec);
	if (ret)
		goto failed;
	ums9117_audio_start(audio->digital);
	hrtimer_start(&audio->timer, ns_to_ktime(UMS9117_PCM_SERVICE_PERIOD_NS),
		      HRTIMER_MODE_REL);
	return 0;

failed:
	dev_err(audio->dev, "cannot start headphone idle silence: %pe\n",
		ERR_PTR(ret));
	ums9117_pcm_shutdown_locked(audio);
	return ret;
}

static int ums9117_pcm_finish_locked(struct ums9117_pcm *audio)
{
	if (!audio->idle_silence || audio->suspended || audio->removing)
		return ums9117_pcm_stop_locked(audio);
	/* Only the already submitted FIFO tail precedes silence after STOP. */
	return ums9117_pcm_start_idle_locked(audio);
}

static int ums9117_headphone_idle_get(struct snd_kcontrol *kcontrol,
				      struct snd_ctl_elem_value *value)
{
	struct ums9117_pcm *audio = snd_kcontrol_chip(kcontrol);

	mutex_lock(&audio->lock);
	value->value.integer.value[0] = audio->idle_silence;
	mutex_unlock(&audio->lock);
	return 0;
}

static int ums9117_headphone_idle_put(struct snd_kcontrol *kcontrol,
				      struct snd_ctl_elem_value *value)
{
	struct ums9117_pcm *audio = snd_kcontrol_chip(kcontrol);
	long enabled = value->value.integer.value[0];
	int ret = 0;

	if (enabled != 0 && enabled != 1)
		return -EINVAL;
	/* Control callbacks never acquire the PCM stream lock. */
	mutex_lock(&audio->lock);
	if (audio->removing) {
		ret = -ENODEV;
		goto out;
	}
	if (audio->idle_silence == enabled)
		goto out;
	if (!audio->running) {
		if (enabled) {
			ret = ums9117_pcm_start_idle_locked(audio);
			if (ret)
				goto out;
		} else if (audio->idle_running) {
			ret = ums9117_pcm_stop_locked(audio);
			if (ret)
				goto out;
		}
	}
	audio->idle_silence = enabled;
	ret = 1;
out:
	mutex_unlock(&audio->lock);
	return ret;
}

static const struct snd_kcontrol_new ums9117_headphone_idle_control = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "Headphone Idle Silence Switch",
	.info = snd_ctl_boolean_mono_info,
	.get = ums9117_headphone_idle_get,
	.put = ums9117_headphone_idle_put,
};

static int ums9117_pcm_pending_frames(struct ums9117_pcm *audio,
				      struct snd_pcm_runtime *runtime,
				      snd_pcm_uframes_t *pending)
{
	snd_pcm_uframes_t appl_ptr = READ_ONCE(runtime->control->appl_ptr);
	snd_pcm_sframes_t frames = appl_ptr - audio->submit_ptr;

	if (frames < -(snd_pcm_sframes_t)(runtime->boundary / 2))
		frames += runtime->boundary;
	if (frames < 0 || frames > runtime->buffer_size)
		return -EPIPE;
	*pending = frames;
	return 0;
}

static void ums9117_pcm_write_frames(struct ums9117_pcm *audio,
				     struct snd_pcm_runtime *runtime,
				     snd_pcm_uframes_t frames)
{
	const __le16 *samples = (const __le16 *)runtime->dma_area;
	snd_pcm_uframes_t i;

	for (i = 0; i < frames; i++) {
		snd_pcm_uframes_t frame =
			audio->submit_ptr % runtime->buffer_size;
		snd_pcm_uframes_t sample = frame * UMS9117_PCM_CHANNELS;

		ums9117_audio_write(audio->digital,
				    le16_to_cpu(samples[sample]),
				    le16_to_cpu(samples[sample + 1]));
		audio->submit_ptr++;
		if (audio->submit_ptr >= runtime->boundary)
			audio->submit_ptr -= runtime->boundary;
	}
	audio->submitted_frames += frames;
}

static int ums9117_pcm_fill(struct ums9117_pcm *audio,
			    struct snd_pcm_runtime *runtime, int queued,
			    snd_pcm_uframes_t *written,
			    snd_pcm_uframes_t *pending_after)
{
	snd_pcm_uframes_t pending;
	snd_pcm_uframes_t space;
	int ret;

	ret = ums9117_pcm_pending_frames(audio, runtime, &pending);
	if (ret)
		return ret;
	space = UMS9117_AUDIO_FIFO_FRAMES - queued;
	*written = min(pending, space);
	ums9117_pcm_write_frames(audio, runtime, *written);
	*pending_after = pending - *written;
	return 0;
}

/*
 * Accounts for consumed frames and refills the FIFO with fifo_lock held. The
 * stream lock is not held here: the drain state is only a hint, and the thread
 * re-checks it under the stream lock before completing the drain.
 */
static enum ums9117_pcm_service_result
ums9117_pcm_service_locked(struct ums9117_pcm *audio,
			   struct snd_pcm_runtime *runtime)
{
	snd_pcm_uframes_t pending;
	snd_pcm_uframes_t written;
	u64 consumed;
	u64 advanced;
	int queued;
	int ret;

	queued = ums9117_audio_queued(audio->digital);
	if (queued < 0) {
		dev_err(audio->dev,
			"cannot read playback FIFO occupancy: %pe\n",
			ERR_PTR(queued));
		return UMS9117_PCM_SERVICE_XRUN;
	}
	if ((u64)queued > audio->submitted_frames - audio->consumed_frames) {
		dev_err(audio->dev,
			"playback FIFO accounting invalid: queued=%d submitted=%llu consumed=%llu\n",
			queued, audio->submitted_frames,
			audio->consumed_frames);
		return UMS9117_PCM_SERVICE_XRUN;
	}
	consumed = audio->submitted_frames - queued;
	if (consumed < audio->consumed_frames) {
		dev_err(audio->dev,
			"playback FIFO occupancy moved backwards: queued=%d\n",
			queued);
		return UMS9117_PCM_SERVICE_XRUN;
	}
	advanced = consumed - audio->consumed_frames;
	audio->consumed_frames = consumed;
	if (advanced)
		audio->last_progress = jiffies;
	/* Idle frames ahead of playback are not part of the ALSA buffer. */
	if (audio->leading_silence_frames) {
		u64 silence = min(advanced, audio->leading_silence_frames);

		audio->leading_silence_frames -= silence;
		advanced -= silence;
	}
	if (advanced) {
		audio->hw_pos += (snd_pcm_uframes_t)advanced;
		if (audio->hw_pos >= runtime->buffer_size)
			audio->hw_pos %= runtime->buffer_size;
		audio->period_frames += (snd_pcm_uframes_t)advanced;
	}

	ret = ums9117_pcm_pending_frames(audio, runtime, &pending);
	if (ret) {
		dev_err(audio->dev,
			"invalid playback application pointer: %pe\n",
			ERR_PTR(ret));
		return UMS9117_PCM_SERVICE_XRUN;
	}
	if (!queued) {
		if (!pending && runtime->state == SNDRV_PCM_STATE_DRAINING)
			return UMS9117_PCM_SERVICE_DRAINED;
		dev_err(audio->dev, "playback underrun: FIFO is empty\n");
		return UMS9117_PCM_SERVICE_XRUN;
	}

	ret = ums9117_pcm_fill(audio, runtime, queued, &written, &pending);
	if (ret) {
		dev_err(audio->dev,
			"invalid playback application pointer or FIFO state: %pe\n",
			ERR_PTR(ret));
		return UMS9117_PCM_SERVICE_XRUN;
	}
	if ((queued || written) && !advanced &&
	    time_after(jiffies,
		       audio->last_progress +
			       msecs_to_jiffies(UMS9117_PCM_STALL_MS))) {
		dev_err(audio->dev,
			"playback FIFO stalled for %u ms (queued=%d)\n",
			UMS9117_PCM_STALL_MS, queued);
		return UMS9117_PCM_SERVICE_XRUN;
	}
	if (audio->period_frames >= runtime->period_size) {
		audio->period_frames %= runtime->period_size;
		return UMS9117_PCM_SERVICE_PERIOD;
	}
	return UMS9117_PCM_SERVICE_IDLE;
}

/*
 * Hard interrupt context. Only the FIFO is touched here; ALSA calls and the
 * hardware lifecycle need process context and are left to the thread.
 */
static enum hrtimer_restart ums9117_pcm_timer(struct hrtimer *timer)
{
	struct ums9117_pcm *audio =
		container_of(timer, struct ums9117_pcm, timer);
	struct snd_pcm_runtime *runtime;
	enum ums9117_pcm_service_result result = UMS9117_PCM_SERVICE_IDLE;
	bool notify = false;
	bool restart;
	int ret;

	spin_lock(&audio->fifo_lock);
	if (audio->idle_running) {
		ret = ums9117_pcm_fill_silence_locked(audio);
		if (ret) {
			/* Releasing the hardware sleeps; the thread does it. */
			audio->idle_running = false;
			audio->idle_error = ret;
			notify = true;
		}
	} else if (audio->running && !audio->xrun_pending && audio->substream) {
		/* A reported underrun stops the refill until STOP arrives. */
		runtime = audio->substream->runtime;
		if (runtime)
			result = ums9117_pcm_service_locked(audio, runtime);
	}
	switch (result) {
	case UMS9117_PCM_SERVICE_PERIOD:
		audio->period_pending = true;
		notify = true;
		break;
	case UMS9117_PCM_SERVICE_DRAINED:
		audio->drained_pending = true;
		notify = true;
		break;
	case UMS9117_PCM_SERVICE_XRUN:
		audio->xrun_pending = true;
		notify = true;
		break;
	case UMS9117_PCM_SERVICE_IDLE:
		break;
	}
	restart = audio->running || audio->idle_running;
	spin_unlock(&audio->fifo_lock);
	if (notify)
		wake_up(&audio->thread_wait);
	if (!restart)
		return HRTIMER_NORESTART;
	hrtimer_forward_now(timer, ns_to_ktime(UMS9117_PCM_SERVICE_PERIOD_NS));
	return HRTIMER_RESTART;
}

static bool ums9117_pcm_has_event(struct ums9117_pcm *audio)
{
	return READ_ONCE(audio->period_pending) ||
	       READ_ONCE(audio->drained_pending) ||
	       READ_ONCE(audio->xrun_pending) || READ_ONCE(audio->idle_error);
}

static void ums9117_pcm_shutdown_failed_idle(struct ums9117_pcm *audio)
{
	unsigned long flags;
	int error;

	mutex_lock(&audio->lock);
	spin_lock_irqsave(&audio->fifo_lock, flags);
	error = audio->idle_error;
	audio->idle_error = 0;
	spin_unlock_irqrestore(&audio->fifo_lock, flags);
	if (error) {
		dev_err(audio->dev, "cannot feed headphone idle silence: %pe\n",
			ERR_PTR(error));
		/* The hardware may have been restarted meanwhile. */
		if (!audio->running && !audio->idle_running)
			ums9117_pcm_shutdown_locked(audio);
	}
	mutex_unlock(&audio->lock);
}

/* Delivers the timer's results to ALSA under the stream lock. */
static void ums9117_pcm_notify(struct ums9117_pcm *audio)
{
	struct snd_pcm_substream *substream;
	struct snd_pcm_runtime *runtime;
	unsigned long flags;
	bool period = false;
	bool drained = false;
	bool xrun = false;

	spin_lock_irqsave(&audio->fifo_lock, flags);
	substream = audio->substream;
	spin_unlock_irqrestore(&audio->fifo_lock, flags);
	if (!substream)
		return;
	snd_pcm_stream_lock(substream);
	runtime = substream->runtime;
	spin_lock_irqsave(&audio->fifo_lock, flags);
	if (audio->substream == substream && runtime && audio->running) {
		period = audio->period_pending;
		drained = audio->drained_pending;
		xrun = audio->xrun_pending;
	} else {
		/* The stream that produced the events has already stopped. */
		audio->xrun_pending = false;
	}
	audio->period_pending = false;
	audio->drained_pending = false;
	spin_unlock_irqrestore(&audio->fifo_lock, flags);
	if (xrun) {
		/* The resulting STOP clears xrun_pending and the refill. */
		snd_pcm_stop(substream, SNDRV_PCM_STATE_XRUN);
	} else if (period || drained) {
		/* The last period may be short; report it before drain ends. */
		snd_pcm_period_elapsed_under_stream_lock(substream);
		if (drained && runtime->state == SNDRV_PCM_STATE_DRAINING)
			snd_pcm_drain_done(substream);
	}
	snd_pcm_stream_unlock(substream);
}

static int ums9117_pcm_thread(void *data)
{
	struct ums9117_pcm *audio = data;

	while (!kthread_should_stop()) {
		wait_event_interruptible(audio->thread_wait,
					 kthread_should_stop() ||
						 ums9117_pcm_has_event(audio));
		if (kthread_should_stop())
			break;
		if (READ_ONCE(audio->idle_error))
			ums9117_pcm_shutdown_failed_idle(audio);
		ums9117_pcm_notify(audio);
	}
	return 0;
}

static int ums9117_pcm_constrain_sizes(struct snd_pcm_runtime *runtime)
{
	int ret;

	ret = snd_pcm_hw_constraint_step(runtime, 0,
					 SNDRV_PCM_HW_PARAM_PERIOD_SIZE,
					 UMS9117_PCM_SIZE_FRAMES_STEP);
	if (ret < 0)
		return ret;
	return snd_pcm_hw_constraint_step(runtime, 0,
					  SNDRV_PCM_HW_PARAM_BUFFER_SIZE,
					  UMS9117_PCM_SIZE_FRAMES_STEP);
}

static void ums9117_pcm_set_stream_locked(struct ums9117_pcm *audio,
					  struct snd_pcm_substream *substream)
{
	unsigned long flags;

	spin_lock_irqsave(&audio->fifo_lock, flags);
	audio->substream = substream;
	spin_unlock_irqrestore(&audio->fifo_lock, flags);
}

static int ums9117_pcm_open(struct snd_pcm_substream *substream)
{
	struct ums9117_pcm *audio = snd_pcm_substream_chip(substream);
	int ret = 0;

	mutex_lock(&audio->lock);
	if (audio->removing)
		ret = -ENODEV;
	else if (audio->substream)
		ret = -EBUSY;
	else
		ums9117_pcm_set_stream_locked(audio, substream);
	mutex_unlock(&audio->lock);
	if (ret)
		return ret;
	substream->runtime->hw = ums9117_pcm_hardware;
	ret = ums9117_pcm_constrain_sizes(substream->runtime);
	if (ret < 0) {
		mutex_lock(&audio->lock);
		if (audio->substream == substream)
			ums9117_pcm_set_stream_locked(audio, NULL);
		mutex_unlock(&audio->lock);
		return ret;
	}
	return 0;
}

static int ums9117_pcm_close(struct snd_pcm_substream *substream)
{
	struct ums9117_pcm *audio = snd_pcm_substream_chip(substream);
	int ret;

	mutex_lock(&audio->lock);
	/* Playback refill ends here, before the runtime goes away. */
	ret = ums9117_pcm_finish_locked(audio);
	if (audio->substream == substream)
		ums9117_pcm_set_stream_locked(audio, NULL);
	mutex_unlock(&audio->lock);
	return ret;
}

static int ums9117_pcm_prepare(struct snd_pcm_substream *substream)
{
	struct ums9117_pcm *audio = snd_pcm_substream_chip(substream);
	unsigned long flags;
	int ret;

	mutex_lock(&audio->lock);
	if (audio->removing || audio->suspended) {
		ret = audio->removing ? -ENODEV : -ESTRPIPE;
		goto failed;
	}
	if (!audio->idle_running) {
		ret = ums9117_pcm_stop_locked(audio);
		if (ret)
			goto out;
	}
	ret = ums9117_pcm_prepare_hardware_locked(audio,
						  substream->runtime->rate);
	if (ret)
		goto out;
	if (audio->idle_silence) {
		ret = ums9117_pcm_start_idle_locked(audio);
		if (ret)
			goto out;
	}
	spin_lock_irqsave(&audio->fifo_lock, flags);
	audio->submit_ptr = 0;
	audio->submitted_frames = 0;
	audio->consumed_frames = 0;
	audio->leading_silence_frames = 0;
	audio->hw_pos = 0;
	audio->period_frames = 0;
	audio->last_progress = jiffies;
	spin_unlock_irqrestore(&audio->fifo_lock, flags);
	goto out;

failed:
	ums9117_pcm_shutdown_locked(audio);
out:
	mutex_unlock(&audio->lock);
	return ret;
}

static int ums9117_pcm_trigger(struct snd_pcm_substream *substream, int command)
{
	struct ums9117_pcm *audio = snd_pcm_substream_chip(substream);
	struct snd_pcm_runtime *runtime = substream->runtime;
	snd_pcm_uframes_t pending;
	snd_pcm_uframes_t written;
	unsigned long flags;
	int queued;
	int ret = 0;

	mutex_lock(&audio->lock);
	switch (command) {
	case SNDRV_PCM_TRIGGER_START:
		if (audio->removing || audio->suspended) {
			ret = audio->removing ? -ENODEV : -ESTRPIPE;
			ums9117_pcm_shutdown_locked(audio);
			break;
		}
		/* A control change may have released hardware after prepare. */
		ret = ums9117_pcm_prepare_hardware_locked(audio, runtime->rate);
		if (ret)
			break;
		/*
		 * The idle refill must not add silence between reading the
		 * occupancy and the prefill, so the hand-over is one critical
		 * section. It ends the idle refill; the codec and DAC start
		 * outside the lock, and only then does playback refill begin.
		 */
		spin_lock_irqsave(&audio->fifo_lock, flags);
		queued = ums9117_audio_queued(audio->digital);
		if (queued < 0 || (!audio->idle_running && queued)) {
			spin_unlock_irqrestore(&audio->fifo_lock, flags);
			ret = queued < 0 ? queued : -EIO;
			dev_err(audio->dev,
				"playback FIFO is not empty before start: %d\n",
				queued);
			ums9117_pcm_shutdown_locked(audio);
			break;
		}
		/* Keep the existing idle FIFO; never clear newly written samples. */
		audio->submitted_frames = queued;
		audio->consumed_frames = 0;
		audio->leading_silence_frames = queued;
		ret = ums9117_pcm_fill(audio, runtime, queued, &written,
				       &pending);
		audio->idle_running = false;
		spin_unlock_irqrestore(&audio->fifo_lock, flags);
		if (ret || (!written && !pending)) {
			if (!ret)
				ret = -EPIPE;
			dev_err(audio->dev,
				"cannot prefill playback FIFO: %pe\n",
				ERR_PTR(ret));
			ums9117_pcm_shutdown_locked(audio);
			break;
		}
		ret = ums9117_sc2720_codec_enable(audio->codec);
		if (ret) {
			dev_err(audio->dev,
				"cannot enable headphone codec: %pe\n",
				ERR_PTR(ret));
			ums9117_pcm_shutdown_locked(audio);
			break;
		}
		ums9117_audio_start(audio->digital);
		spin_lock_irqsave(&audio->fifo_lock, flags);
		audio->last_progress = jiffies;
		audio->running = true;
		spin_unlock_irqrestore(&audio->fifo_lock, flags);
		hrtimer_start(&audio->timer,
			      ns_to_ktime(UMS9117_PCM_SERVICE_PERIOD_NS),
			      HRTIMER_MODE_REL);
		break;
	case SNDRV_PCM_TRIGGER_STOP:
		ret = ums9117_pcm_finish_locked(audio);
		break;
	case SNDRV_PCM_TRIGGER_SUSPEND:
		ums9117_pcm_shutdown_locked(audio);
		break;
	default:
		ret = -EINVAL;
		break;
	}
	mutex_unlock(&audio->lock);
	return ret;
}

static snd_pcm_uframes_t
ums9117_pcm_pointer(struct snd_pcm_substream *substream)
{
	struct ums9117_pcm *audio = snd_pcm_substream_chip(substream);
	snd_pcm_uframes_t position;
	unsigned long flags;

	spin_lock_irqsave(&audio->fifo_lock, flags);
	position = audio->hw_pos;
	spin_unlock_irqrestore(&audio->fifo_lock, flags);
	return position;
}

static const struct snd_pcm_ops ums9117_pcm_ops = {
	.open = ums9117_pcm_open,
	.close = ums9117_pcm_close,
	.ioctl = snd_pcm_lib_ioctl,
	.prepare = ums9117_pcm_prepare,
	.trigger = ums9117_pcm_trigger,
	.pointer = ums9117_pcm_pointer,
};

static int ums9117_pcm_probe(struct platform_device *pdev)
{
	const struct snd_kcontrol_new *volume_control;
	const char *card_longname;
	struct resource *pinmux_resource;
	struct resource *pinconf_resource;
	struct ums9117_pcm *audio;
	struct snd_card *card;
	struct snd_pcm *pcm;
	int ret;

	audio = devm_kzalloc(&pdev->dev, sizeof(*audio), GFP_KERNEL);
	if (!audio)
		return -ENOMEM;
	audio->dev = &pdev->dev;
	audio->rate = UMS9117_PCM_IDLE_RATE;
	audio->idle_silence =
		device_property_read_bool(&pdev->dev, "fplinux,idle-silence");
	mutex_init(&audio->lock);
	spin_lock_init(&audio->fifo_lock);
	init_waitqueue_head(&audio->thread_wait);
	hrtimer_setup(&audio->timer, ums9117_pcm_timer, CLOCK_MONOTONIC,
		      HRTIMER_MODE_REL);
	ret = device_property_read_string(&pdev->dev, "fplinux,card-longname",
					  &card_longname);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "invalid audio card long name\n");
	pinmux_resource =
		platform_get_resource_byname(pdev, IORESOURCE_MEM, "pinmux");
	pinconf_resource =
		platform_get_resource_byname(pdev, IORESOURCE_MEM, "pinconf");
	if (!pinmux_resource || !pinconf_resource ||
	    resource_size(pinmux_resource) < sizeof(u32) ||
	    resource_size(pinconf_resource) < sizeof(u32))
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "invalid audio pad resources\n");
	ret = ums9117_pcm_read_pads(&pdev->dev, audio->pads,
				    resource_size(pinmux_resource),
				    resource_size(pinconf_resource));
	if (ret)
		return ret;
	ret = ums9117_pcm_load_audio_profile(audio);
	if (ret)
		return ret;
	audio->pinmux = devm_ioremap_resource(&pdev->dev, pinmux_resource);
	if (IS_ERR(audio->pinmux))
		return PTR_ERR(audio->pinmux);
	audio->pinconf = devm_ioremap_resource(&pdev->dev, pinconf_resource);
	if (IS_ERR(audio->pinconf))
		return PTR_ERR(audio->pinconf);
	audio->digital = ums9117_audio_create(pdev);
	if (IS_ERR(audio->digital))
		return PTR_ERR(audio->digital);
	audio->codec = ums9117_sc2720_codec_create(&pdev->dev);
	if (IS_ERR(audio->codec))
		return PTR_ERR(audio->codec);
	if (audio->profile.fitted) {
		mutex_lock(&audio->lock);
		ret = ums9117_pcm_set_profile_volume_locked(audio, 1, 1);
		mutex_unlock(&audio->lock);
		if (ret < 0)
			return ret;
	}

	ret = snd_devm_card_new(&pdev->dev, SNDRV_DEFAULT_IDX1,
				SNDRV_DEFAULT_STR1, THIS_MODULE, 0, &card);
	if (ret)
		return ret;
	audio->card = card;
	volume_control = audio->profile.fitted ?
				 &ums9117_headphone_profile_volume_control :
				 &ums9117_headphone_generic_volume_control;
	ret = snd_ctl_add(card, snd_ctl_new1(volume_control, audio));
	if (ret)
		return snd_card_free_on_error(&pdev->dev, ret);
	ret = snd_ctl_add(card,
			  snd_ctl_new1(&ums9117_headphone_idle_control, audio));
	if (ret)
		return snd_card_free_on_error(&pdev->dev, ret);
	ret = snd_pcm_new(card, UMS9117_PCM_NAME, 0, 1, 0, &pcm);
	if (ret)
		return snd_card_free_on_error(&pdev->dev, ret);
	audio->pcm = pcm;
	pcm->private_data = audio;
	pcm->nonatomic = true;
	strscpy(pcm->name, UMS9117_PCM_NAME);
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &ums9117_pcm_ops);
	/*
	 * Applications can only use as much buffer as is reserved here. On this
	 * single-processor phone an ordinary concurrent command delays the
	 * application's next write, and a removable card serving a large write
	 * alongside the playback file can hold a read for over half a second.
	 * Reserve the whole supported buffer so such a delay does not run the
	 * queued audio dry.
	 */
	ret = snd_pcm_set_managed_buffer_all(pcm, SNDRV_DMA_TYPE_VMALLOC, NULL,
					     UMS9117_PCM_BUFFER_BYTES_MAX,
					     UMS9117_PCM_BUFFER_BYTES_MAX);
	if (ret)
		return snd_card_free_on_error(&pdev->dev, ret);

	strscpy(card->driver, dev_driver_string(&pdev->dev));
	strscpy(card->shortname, UMS9117_PCM_NAME);
	strscpy(card->longname, card_longname);
	platform_set_drvdata(pdev, audio);
	audio->thread =
		kthread_run(ums9117_pcm_thread, audio, UMS9117_PCM_THREAD_NAME);
	if (IS_ERR(audio->thread)) {
		ret = PTR_ERR(audio->thread);
		audio->thread = NULL;
		return snd_card_free_on_error(&pdev->dev, ret);
	}
	/*
	 * The FIFO refill runs from the timer interrupt; this thread only
	 * delivers period, drain and underrun notifications. A late period
	 * notification still delays the application's next write on this
	 * single-processor phone, so take the lowest real-time priority, which
	 * remains under the kernel's real-time throttling.
	 */
	sched_set_fifo_low(audio->thread);
	ret = snd_card_register(card);
	if (ret) {
		kthread_stop(audio->thread);
		audio->thread = NULL;
		return snd_card_free_on_error(&pdev->dev, ret);
	}
	if (audio->idle_silence) {
		mutex_lock(&audio->lock);
		ret = ums9117_pcm_start_idle_locked(audio);
		mutex_unlock(&audio->lock);
		if (ret) {
			kthread_stop(audio->thread);
			audio->thread = NULL;
			return snd_card_free_on_error(&pdev->dev, ret);
		}
	}
	if (audio->profile.fitted && audio->profile.source_eq_omitted)
		dev_dbg(audio->dev,
			"headphone volume source: fitted gain profile; source EQ omitted\n");
	else
		dev_dbg(audio->dev, "headphone volume source: %s\n",
			audio->profile.fitted ? "fitted gain profile" :
						"generic defaults");
	return 0;
}

static void ums9117_pcm_remove(struct platform_device *pdev)
{
	struct ums9117_pcm *audio = platform_get_drvdata(pdev);
	struct snd_pcm_substream *substream;

	mutex_lock(&audio->lock);
	audio->removing = true;
	ums9117_pcm_stop_refill_locked(audio);
	substream = audio->substream;
	mutex_unlock(&audio->lock);
	snd_card_disconnect(audio->card);
	kthread_stop(audio->thread);
	if (substream)
		snd_pcm_stream_lock(substream);
	mutex_lock(&audio->lock);
	ums9117_pcm_shutdown_locked(audio);
	mutex_unlock(&audio->lock);
	if (substream)
		snd_pcm_stream_unlock(substream);
}

static int ums9117_pcm_suspend(struct device *dev)
{
	struct ums9117_pcm *audio = dev_get_drvdata(dev);
	int ret;

	mutex_lock(&audio->lock);
	audio->suspended = true;
	mutex_unlock(&audio->lock);
	ret = snd_pcm_suspend_all(audio->pcm);
	mutex_lock(&audio->lock);
	ums9117_pcm_shutdown_locked(audio);
	mutex_unlock(&audio->lock);
	return ret;
}

static int ums9117_pcm_resume(struct device *dev)
{
	struct ums9117_pcm *audio = dev_get_drvdata(dev);
	int ret = 0;

	mutex_lock(&audio->lock);
	audio->suspended = false;
	if (audio->idle_silence)
		ret = ums9117_pcm_start_idle_locked(audio);
	mutex_unlock(&audio->lock);
	return ret;
}

static DEFINE_SIMPLE_DEV_PM_OPS(ums9117_pcm_pm_ops, ums9117_pcm_suspend,
				ums9117_pcm_resume);

static const struct of_device_id ums9117_pcm_of_match[] = {
	{ .compatible = "fplinux,ums9117-audio" },
	{}
};
MODULE_DEVICE_TABLE(of, ums9117_pcm_of_match);

static struct platform_driver ums9117_pcm_driver = {
	.probe = ums9117_pcm_probe,
	.remove = ums9117_pcm_remove,
	.shutdown = ums9117_pcm_remove,
	.driver = {
		.name = "snd-ums9117",
		.of_match_table = ums9117_pcm_of_match,
		.pm = pm_sleep_ptr(&ums9117_pcm_pm_ops),
	},
};
module_platform_driver(ums9117_pcm_driver);

MODULE_DESCRIPTION("UMS9117 PIO headphone driver");
MODULE_LICENSE("GPL");
