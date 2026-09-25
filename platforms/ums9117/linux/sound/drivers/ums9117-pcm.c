// SPDX-License-Identifier: GPL-2.0-only
#include <linux/err.h>
#include <linux/firmware.h>
#include <linux/hrtimer.h>
#include <linux/input.h>
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
#include <linux/unaligned.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

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
/* Keep the analog input's power-up transient outside the ALSA stream. */
#define UMS9117_PCM_CAPTURE_WARMUP_FRAMES 14400U
#define UMS9117_PCM_IDLE_RATE 48000U
#define UMS9117_PCM_THREAD_NAME "ums9117-pcm"
#define UMS9117_PCM_NAME "UMS9117 Headphones"
#define UMS9117_PCM_OUTPUTS_BOTH \
	(UMS9117_SC2720_OUTPUT_HEADPHONES | UMS9117_SC2720_OUTPUT_SPEAKER)
#define UMS9117_AUDIO_PAD_COUNT 4U
#define UMS9117_CAPTURE_PAD_COUNT 2U
#define UMS9117_AUDIO_PAD_CELLS 3U

#define UMS9117_AUDIO_PROFILE_MAGIC_SIZE 8U
#define UMS9117_AUDIO_PROFILE_COMPATIBLE_SIZE 24U
#define UMS9117_AUDIO_PROFILE_LEVEL_COUNT 9U
/*
 * The header is followed by the speaker section, the combined headphone and
 * speaker section, the playback processing and, on boards with speaker
 * vibration, the vibrate tone. Each output section starts with its PA word;
 * the combined section then adds its headphone PGA level before the gains.
 */
#define UMS9117_AUDIO_PROFILE_SPEAKER_BYTES \
	(sizeof(u16) + UMS9117_AUDIO_PROFILE_LEVEL_COUNT)
#define UMS9117_AUDIO_PROFILE_COMBINED_BYTES \
	(sizeof(u16) + sizeof(u8) + UMS9117_AUDIO_PROFILE_LEVEL_COUNT)
/*
 * The processing holds the headphone, speaker and combined routes in that
 * order. Each has S6, the ALC enable and its ALC words, then for each DAC rate
 * six EQ6 sections of scale, B0, B1, -A1, B2 and -A2, all little-endian.
 */
#define UMS9117_AUDIO_PROFILE_EQ_SECTION_BYTES (6 * sizeof(u16))
#define UMS9117_AUDIO_PROFILE_ROUTE_PROCESSING_BYTES                     \
	(sizeof(u16) + sizeof(u8) +                                      \
	 UMS9117_AUDIO_ALC_WORD_COUNT * sizeof(u16) +                    \
	 UMS9117_AUDIO_DAC_RATE_COUNT * UMS9117_AUDIO_EQ_SECTION_COUNT * \
		 UMS9117_AUDIO_PROFILE_EQ_SECTION_BYTES)
#define UMS9117_AUDIO_PROFILE_PROCESSING_BYTES \
	(3 * UMS9117_AUDIO_PROFILE_ROUTE_PROCESSING_BYTES)
/* Oscillator pairs for each DAC rate, then level, fall, rise and hold. */
#define UMS9117_AUDIO_PROFILE_VIBRATE_TONE_BYTES \
	(2 * UMS9117_AUDIO_DAC_RATE_COUNT * sizeof(u32) + 5 * sizeof(u16))
/* Q30 rotation pairs are rounded to one part in 2^30 of unit norm. */
#define UMS9117_AUDIO_PROFILE_VIBRATE_NORM BIT_ULL(60)
#define UMS9117_AUDIO_PROFILE_VIBRATE_NORM_TOLERANCE BIT_ULL(40)
#define UMS9117_AUDIO_PROFILE_HEADPHONE_PGA_MIN 2U
#define UMS9117_AUDIO_PROFILE_HEADPHONE_PGA_MAX 7U
#define UMS9117_AUDIO_PROFILE_PA_WORD_MAX 0xffU
#define UMS9117_AUDIO_PROFILE_DAC_GAIN_MAX 127U

#define UMS9117_VIBRATOR_NAME "UMS9117 speaker vibrator"
#define UMS9117_VIBRATOR_PHYS "fplinux/vibrator0"
#define UMS9117_VIBRATOR_MAX_ON_MS 5000U
/*
 * Keep the vibration path between the pulses of one pattern so that muting
 * and PA transitions do not repeat for each pulse. One second covers the
 * longest pause inside a Morse pattern with margin.
 */
#define UMS9117_VIBRATOR_HOLD_MS 1000U

static const u8 ums9117_audio_profile_magic[UMS9117_AUDIO_PROFILE_MAGIC_SIZE] = {
	'F', 'P', 'A', 'U', 'D', 'I', 'O', '\0'
};

struct ums9117_audio_profile_data {
	u8 magic[UMS9117_AUDIO_PROFILE_MAGIC_SIZE];
	u8 compatible[UMS9117_AUDIO_PROFILE_COMPATIBLE_SIZE];
	u8 headphone_pga;
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

static const char *const ums9117_capture_pad_names[UMS9117_CAPTURE_PAD_COUNT] = {
	"ADSYNC",
	"ADD0",
};

struct ums9117_audio_profile {
	u8 dac_gain[UMS9117_AUDIO_PROFILE_LEVEL_COUNT];
	u8 speaker_dac_gain[UMS9117_AUDIO_PROFILE_LEVEL_COUNT];
	/*
	 * Both outputs share one DAC gain at a fixed headphone level. The
	 * halved PCM samples make the headphones 6 dB quieter in this mode.
	 */
	u8 combined_dac_gain[UMS9117_AUDIO_PROFILE_LEVEL_COUNT];
	struct ums9117_audio_dac_processing headphone_processing;
	struct ums9117_audio_dac_processing speaker_processing;
	struct ums9117_audio_dac_processing combined_processing;
	struct ums9117_audio_vibrate_tone vibrate_tone;
	u16 speaker_pa_word;
	u16 combined_pa_word;
	u8 codec_volume;
	u8 combined_codec_volume;
	bool fitted;
};

/*
 * A speaker that also vibrates the phone plays the VBC vibrate tone. A
 * vibration session holds the DAC and the PA for the tone and mutes the
 * headphones; music and FM continue only on an enabled, unmuted speaker,
 * which shares the PA with the tone.
 */
struct ums9117_vibrator {
	struct work_struct play_work;
	struct delayed_work stop_work;
	struct delayed_work hold_work;
	/* Protects the request and lifecycle flags; play runs atomically. */
	spinlock_t state_lock;
	unsigned long stop_deadline;
	bool requested;
	bool suspended;
	bool stopping;
	bool cutoff_latched;
	bool off_pending;
	/* The PCM lifecycle mutex protects the hardware state below. */
	bool session;
	bool tone_on;
};

struct ums9117_pcm_stream {
	struct snd_pcm_substream *substream;
	snd_pcm_uframes_t hw_pos;
	snd_pcm_uframes_t period_frames;
	unsigned long last_progress;
	bool running;
	bool period_pending;
	bool xrun_pending;
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
	struct ums9117_audio_pad capture_pads[UMS9117_CAPTURE_PAD_COUNT];
	struct ums9117_audio_profile profile;
	struct ums9117_vibrator vibrator;
	unsigned int volume_left;
	unsigned int volume_right;
	unsigned int speaker_volume;
	enum ums9117_sc2720_capture_source capture_source;
	/* Requested UMS9117_SC2720_OUTPUT_* set; running hardware follows it. */
	unsigned int outputs;
	/* Fitted EQ6 and ALC switches, each controlling its own block. */
	bool eq_enabled;
	bool alc_enabled;
	bool codec_prepared;
	bool digital_prepared;
	bool capture_supported;
	bool capture_prepared;
	bool speaker_vibration;
	bool idle_silence;
	bool fm_enabled;
	bool suspended;
	bool removing;
	/*
	 * Protects the streaming state below. The timer callback takes it from
	 * hard interrupt context; process context disables interrupts and does
	 * not sleep or cancel the timer while holding it. Lifecycle code writes
	 * these fields with the mutex held as well.
	 */
	spinlock_t fifo_lock;
	struct ums9117_pcm_stream playback;
	struct ums9117_pcm_stream capture;
	snd_pcm_uframes_t submit_ptr;
	snd_pcm_uframes_t capture_ptr;
	u64 submitted_frames;
	u64 consumed_frames;
	u64 leading_silence_frames;
	/*
	 * DACS adds both lanes, so PCM samples are halved whenever the speaker
	 * is a requested output, with or without the headphones. FM samples
	 * are not halved.
	 */
	bool halve_samples;
	bool idle_running;
	bool capture_starting;
	unsigned int capture_warmup_frames;
	/* Results the timer leaves for the notification thread. */
	bool drained_pending;
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

static const struct snd_pcm_hardware ums9117_capture_hardware = {
	.info = SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_BLOCK_TRANSFER |
		SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_MMAP_VALID |
		SNDRV_PCM_INFO_FIFO_IN_FRAMES | SNDRV_PCM_INFO_NO_REWINDS,
	.formats = SNDRV_PCM_FMTBIT_S16_LE,
	.rates = SNDRV_PCM_RATE_48000,
	.rate_min = 48000,
	.rate_max = 48000,
	.channels_min = 1,
	.channels_max = 1,
	.buffer_bytes_max = UMS9117_PCM_BUFFER_BYTES_MAX,
	.period_bytes_min = UMS9117_PCM_PERIOD_BYTES_MIN,
	.period_bytes_max = UMS9117_PCM_PERIOD_BYTES_MAX,
	.periods_min = 2,
	.periods_max =
		UMS9117_PCM_BUFFER_BYTES_MAX / UMS9117_PCM_PERIOD_BYTES_MIN,
	.fifo_size = UMS9117_AUDIO_FIFO_FRAMES,
};

/* The hardware lifecycle mutex is held when inspecting the stream lease. */
static bool ums9117_pcm_capture_open(struct ums9117_pcm *audio)
{
	return audio->capture.substream != NULL;
}

static struct ums9117_pcm_stream *
ums9117_pcm_stream(struct ums9117_pcm *audio,
		   const struct snd_pcm_substream *substream)
{
	return substream->stream == SNDRV_PCM_STREAM_CAPTURE ? &audio->capture :
							       &audio->playback;
}

static int
ums9117_pcm_parse_vibrate_tone(const u8 *data,
			       struct ums9117_audio_vibrate_tone *tone)
{
	const u8 *cos_data = data + UMS9117_AUDIO_DAC_RATE_COUNT * sizeof(u32);
	const u8 *envelope =
		cos_data + UMS9117_AUDIO_DAC_RATE_COUNT * sizeof(u32);
	unsigned int i;

	for (i = 0; i < UMS9117_AUDIO_DAC_RATE_COUNT; i++) {
		s64 sin = (s32)get_unaligned_le32(data + i * sizeof(u32));
		s64 cos = (s32)get_unaligned_le32(cos_data + i * sizeof(u32));
		s64 norm = sin * sin + cos * cos;

		/* Any other norm makes the recursive oscillator decay or grow. */
		if (abs(norm - (s64)UMS9117_AUDIO_PROFILE_VIBRATE_NORM) >
		    (s64)UMS9117_AUDIO_PROFILE_VIBRATE_NORM_TOLERANCE)
			return -EINVAL;
		tone->sin[i] = (u32)sin;
		tone->cos[i] = (u32)cos;
	}
	tone->level[0] = get_unaligned_le16(envelope);
	tone->level[1] = get_unaligned_le16(envelope + 2);
	tone->fall = get_unaligned_le16(envelope + 4);
	tone->rise = get_unaligned_le16(envelope + 6);
	tone->hold = get_unaligned_le16(envelope + 8);
	/* A zero level is silent and a zero hold never starts the tone. */
	if (!tone->level[0] || !tone->level[1] || !tone->hold)
		return -EINVAL;
	return 0;
}

/* A larger DG code attenuates more; a louder level never has a larger one. */
static int ums9117_pcm_parse_dac_gains(const u8 *data, u8 *gains)
{
	unsigned int i;

	for (i = 0; i < UMS9117_AUDIO_PROFILE_LEVEL_COUNT; i++) {
		if (data[i] > UMS9117_AUDIO_PROFILE_DAC_GAIN_MAX ||
		    (i && data[i - 1] < data[i]))
			return -EINVAL;
		gains[i] = data[i];
	}
	return 0;
}

static bool ums9117_pcm_headphone_pga_valid(u8 pga)
{
	return pga >= UMS9117_AUDIO_PROFILE_HEADPHONE_PGA_MIN &&
	       pga <= UMS9117_AUDIO_PROFILE_HEADPHONE_PGA_MAX;
}

static s16 ums9117_pcm_read_s16(const u8 **data)
{
	s16 value = (s16)get_unaligned_le16(*data);

	*data += sizeof(u16);
	return value;
}

/*
 * An unstable section drives a full-scale DC or fs/2 square wave into the
 * speaker amplifier. With A0 as unity the quantized poles lie inside the unit
 * circle when |a2| < 1 and |a1| < 1 + a2.
 */
static bool
ums9117_pcm_eq_section_valid(const struct ums9117_audio_eq_section *section)
{
	int minus_a1 = section->minus_a1;
	int minus_a2 = section->minus_a2;

	return section->scale > 0 && abs(minus_a2) < UMS9117_AUDIO_EQ_A0 &&
	       abs(minus_a1) < UMS9117_AUDIO_EQ_A0 - minus_a2;
}

static int ums9117_pcm_parse_dac_processing(
	const u8 *data, struct ums9117_audio_dac_processing *processing)
{
	struct ums9117_audio_eq_section *section;
	unsigned int rate;
	unsigned int i;
	u8 alc_enabled;

	processing->output_scale = get_unaligned_le16(data);
	if (!processing->output_scale || processing->output_scale > S16_MAX)
		return -EINVAL;
	data += sizeof(u16);
	alc_enabled = *data++;
	if (alc_enabled > 1)
		return -EINVAL;
	processing->alc_enabled = alc_enabled;
	for (i = 0; i < UMS9117_AUDIO_ALC_WORD_COUNT; i++)
		processing->alc[i] = ums9117_pcm_read_s16(&data);
	for (rate = 0; rate < UMS9117_AUDIO_DAC_RATE_COUNT; rate++) {
		for (i = 0; i < UMS9117_AUDIO_EQ_SECTION_COUNT; i++) {
			section = &processing->section[rate][i];
			section->scale = ums9117_pcm_read_s16(&data);
			section->b0 = ums9117_pcm_read_s16(&data);
			section->b1 = ums9117_pcm_read_s16(&data);
			section->minus_a1 = ums9117_pcm_read_s16(&data);
			section->b2 = ums9117_pcm_read_s16(&data);
			section->minus_a2 = ums9117_pcm_read_s16(&data);
			if (!ums9117_pcm_eq_section_valid(section))
				return -EINVAL;
		}
	}
	return 0;
}

static int ums9117_pcm_parse_audio_profile(
	const char *machine_compatible, const struct firmware *firmware,
	bool speaker_vibration, struct ums9117_audio_profile *profile)
{
	u8 compatible[UMS9117_AUDIO_PROFILE_COMPATIBLE_SIZE] = {};
	const struct ums9117_audio_profile_data *data;
	const u8 *processing;
	const u8 *combined;
	const u8 *speaker;
	size_t compatible_length;
	size_t size;
	u8 combined_pga;
	int ret;

	size = sizeof(*data) + UMS9117_AUDIO_PROFILE_SPEAKER_BYTES +
	       UMS9117_AUDIO_PROFILE_COMBINED_BYTES +
	       UMS9117_AUDIO_PROFILE_PROCESSING_BYTES;
	if (speaker_vibration)
		size += UMS9117_AUDIO_PROFILE_VIBRATE_TONE_BYTES;
	if (firmware->size != size)
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
	if (!ums9117_pcm_headphone_pga_valid(data->headphone_pga))
		return -EINVAL;
	ret = ums9117_pcm_parse_dac_gains(data->dac_gain, profile->dac_gain);
	if (ret)
		return ret;
	profile->codec_volume = data->headphone_pga - 1U;

	speaker = firmware->data + sizeof(*data);
	profile->speaker_pa_word = get_unaligned_le16(speaker);
	if (profile->speaker_pa_word > UMS9117_AUDIO_PROFILE_PA_WORD_MAX)
		return -EINVAL;
	ret = ums9117_pcm_parse_dac_gains(speaker + sizeof(u16),
					  profile->speaker_dac_gain);
	if (ret)
		return ret;

	combined = speaker + UMS9117_AUDIO_PROFILE_SPEAKER_BYTES;
	profile->combined_pa_word = get_unaligned_le16(combined);
	if (profile->combined_pa_word > UMS9117_AUDIO_PROFILE_PA_WORD_MAX)
		return -EINVAL;
	combined_pga = combined[sizeof(u16)];
	if (!ums9117_pcm_headphone_pga_valid(combined_pga))
		return -EINVAL;
	profile->combined_codec_volume = combined_pga - 1U;
	ret = ums9117_pcm_parse_dac_gains(combined + sizeof(u16) + sizeof(u8),
					  profile->combined_dac_gain);
	if (ret)
		return ret;

	processing = combined + UMS9117_AUDIO_PROFILE_COMBINED_BYTES;
	ret = ums9117_pcm_parse_dac_processing(processing,
					       &profile->headphone_processing);
	if (ret)
		return ret;
	ret = ums9117_pcm_parse_dac_processing(
		processing + UMS9117_AUDIO_PROFILE_ROUTE_PROCESSING_BYTES,
		&profile->speaker_processing);
	if (ret)
		return ret;
	ret = ums9117_pcm_parse_dac_processing(
		processing + 2 * UMS9117_AUDIO_PROFILE_ROUTE_PROCESSING_BYTES,
		&profile->combined_processing);
	if (ret)
		return ret;

	if (speaker_vibration) {
		ret = ums9117_pcm_parse_vibrate_tone(
			processing + UMS9117_AUDIO_PROFILE_PROCESSING_BYTES,
			&profile->vibrate_tone);
		if (ret)
			return ret;
	}
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
				     "cannot load audio profile\n");

	ret = of_property_read_string_index(of_root, "compatible", 0,
					    &machine_compatible);
	if (ret) {
		release_firmware(firmware);
		return dev_err_probe(audio->dev, ret,
				     "cannot read machine compatible\n");
	}
	ret = ums9117_pcm_parse_audio_profile(machine_compatible, firmware,
					      audio->speaker_vibration,
					      &audio->profile);
	release_firmware(firmware);
	if (ret)
		return dev_err_probe(audio->dev, ret,
				     "invalid audio profile\n");
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

/* Logical mute is analog; retain the lowest fitted digital level. */
static unsigned int ums9117_profile_level_index(unsigned int volume)
{
	return volume ? volume - 1 : 0;
}

static u8 ums9117_profile_dac_gain(const struct ums9117_audio_profile *profile,
				   unsigned int volume)
{
	return profile->dac_gain[ums9117_profile_level_index(volume)];
}

static void ums9117_pcm_apply_profile_dac_gain(struct ums9117_pcm *audio,
					       unsigned int left,
					       unsigned int right)
{
	u8 left_gain = ums9117_profile_dac_gain(&audio->profile, left);
	u8 right_gain = ums9117_profile_dac_gain(&audio->profile, right);

	ums9117_audio_set_dac_gain(audio->digital, left_gain, right_gain);
}

/*
 * Headphones alone follow their own left and right levels. The speaker, alone
 * or with the headphones, sets one gain for both lanes from its level.
 */
static void ums9117_pcm_apply_output_gain(struct ums9117_pcm *audio)
{
	const struct ums9117_audio_profile *profile = &audio->profile;
	unsigned int index = ums9117_profile_level_index(audio->speaker_volume);
	u8 gain;

	if (!profile->fitted)
		return;
	switch (audio->outputs) {
	case UMS9117_SC2720_OUTPUT_HEADPHONES:
		ums9117_pcm_apply_profile_dac_gain(audio, audio->volume_left,
						   audio->volume_right);
		return;
	case UMS9117_SC2720_OUTPUT_SPEAKER:
		gain = profile->speaker_dac_gain[index];
		break;
	case UMS9117_PCM_OUTPUTS_BOTH:
		gain = profile->combined_dac_gain[index];
		break;
	default:
		return;
	}
	ums9117_audio_set_dac_gain(audio->digital, gain, gain);
}

/* With both outputs a nonzero headphone level only unmutes its channel. */
static int ums9117_pcm_set_codec_volume_locked(struct ums9117_pcm *audio,
					       unsigned int left,
					       unsigned int right)
{
	unsigned int level = audio->outputs == UMS9117_PCM_OUTPUTS_BOTH ?
				     audio->profile.combined_codec_volume :
				     audio->profile.codec_volume;

	return ums9117_sc2720_codec_set_volume(audio->codec, left ? level : 0,
					       right ? level : 0);
}

static int ums9117_pcm_set_profile_volume_locked(struct ums9117_pcm *audio,
						 unsigned int left,
						 unsigned int right)
{
	unsigned int old_left = audio->volume_left;
	unsigned int old_right = audio->volume_right;
	/* FM follows the same digital gain as PCM. */
	bool headphone_digital = audio->outputs ==
				 UMS9117_SC2720_OUTPUT_HEADPHONES;
	int ret;

	if (left == old_left && right == old_right)
		return 0;
	/* Program DG before an analog unmute can expose the selected level. */
	if (headphone_digital)
		ums9117_pcm_apply_profile_dac_gain(audio, left, right);
	ret = ums9117_pcm_set_codec_volume_locked(audio, left, right);
	if (ret < 0) {
		if (headphone_digital)
			ums9117_pcm_apply_profile_dac_gain(audio, old_left,
							   old_right);
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

static int ums9117_pcm_disable_codec(struct ums9117_pcm *audio)
{
	int ret;

	if (!audio->codec_prepared)
		return 0;
	ret = ums9117_sc2720_codec_disable(audio->codec);
	if (ret) {
		dev_err(audio->dev, "cannot disable audio codec: %pe\n",
			ERR_PTR(ret));
		return ret;
	}
	audio->codec_prepared = false;
	return 0;
}

static void ums9117_pcm_clear_events_locked(struct ums9117_pcm_stream *stream)
{
	stream->period_pending = false;
	stream->xrun_pending = false;
}

/* The lifecycle mutex is held; the timer never takes it. */
static void ums9117_pcm_stop_playback_refill_locked(struct ums9117_pcm *audio)
{
	unsigned long flags;
	bool cancel;

	spin_lock_irqsave(&audio->fifo_lock, flags);
	audio->playback.running = false;
	audio->idle_running = false;
	ums9117_pcm_clear_events_locked(&audio->playback);
	audio->drained_pending = false;
	cancel = !audio->capture.running;
	spin_unlock_irqrestore(&audio->fifo_lock, flags);
	if (cancel)
		hrtimer_cancel(&audio->timer);
}

static void ums9117_pcm_stop_capture_refill_locked(struct ums9117_pcm *audio)
{
	unsigned long flags;
	bool cancel;

	spin_lock_irqsave(&audio->fifo_lock, flags);
	audio->capture.running = false;
	audio->capture_starting = false;
	audio->capture_warmup_frames = 0;
	ums9117_pcm_clear_events_locked(&audio->capture);
	cancel = !audio->playback.running && !audio->idle_running;
	spin_unlock_irqrestore(&audio->fifo_lock, flags);
	if (cancel)
		hrtimer_cancel(&audio->timer);
}

static int ums9117_pcm_shutdown_playback_locked(struct ums9117_pcm *audio)
{
	int ret;

	ums9117_pcm_stop_playback_refill_locked(audio);
	if (audio->digital_prepared)
		ums9117_audio_stop(audio->digital);
	ret = ums9117_pcm_disable_codec(audio);
	if (audio->digital_prepared)
		ums9117_audio_release(audio->digital);
	audio->digital_prepared = false;
	return ret;
}

static int ums9117_pcm_shutdown_capture_locked(struct ums9117_pcm *audio)
{
	int ret;

	ums9117_pcm_stop_capture_refill_locked(audio);
	if (!audio->capture_prepared)
		return 0;
	/* The analog producer stops while the digital receiver still has clocks. */
	ret = ums9117_sc2720_codec_stop_capture(audio->codec);
	if (ret)
		return ret;
	ret = ums9117_sc2720_codec_disable_capture(audio->codec);
	if (ret)
		return ret;
	ums9117_audio_stop_capture(audio->digital);
	ums9117_audio_release_capture(audio->digital);
	audio->capture_prepared = false;
	return 0;
}

static int ums9117_pcm_stop_playback_locked(struct ums9117_pcm *audio)
{
	int ret;

	ums9117_pcm_stop_playback_refill_locked(audio);
	if (audio->digital_prepared)
		ums9117_audio_stop(audio->digital);
	if (audio->codec_prepared) {
		ret = ums9117_sc2720_codec_stop(audio->codec);
		if (ret) {
			dev_err(audio->dev, "cannot stop playback codec: %pe\n",
				ERR_PTR(ret));
			ums9117_pcm_shutdown_playback_locked(audio);
			return ret;
		}
	}
	if (audio->digital_prepared)
		ums9117_audio_release(audio->digital);
	audio->digital_prepared = false;
	return 0;
}

static int ums9117_pcm_validate_pads(struct ums9117_pcm *audio,
				     const struct ums9117_audio_pad *pads,
				     const char *const *names,
				     unsigned int count)
{
	unsigned int i;

	for (i = 0; i < count; i++) {
		const struct ums9117_audio_pad *pad = &pads[i];
		u32 mux = readl(audio->pinmux + pad->offset);
		u32 config = readl(audio->pinconf + pad->offset);

		if (mux == pad->mux && config == pad->config)
			continue;
		dev_err(audio->dev,
			"audio pad %s unavailable: mux=%#x config=%#x\n",
			names[i], mux, config);
		return -EBUSY;
	}
	return 0;
}

static int ums9117_pcm_read_pads(struct device *dev,
				 struct ums9117_audio_pad *pads,
				 const char *property, const char *const *names,
				 unsigned int count,
				 resource_size_t pinmux_size,
				 resource_size_t pinconf_size)
{
	u32 settings[UMS9117_AUDIO_PAD_COUNT * UMS9117_AUDIO_PAD_CELLS];
	unsigned int i;
	int ret;

	ret = device_property_read_u32_array(dev, property, settings,
					     count * UMS9117_AUDIO_PAD_CELLS);
	if (ret)
		return dev_err_probe(dev, ret, "invalid audio pad settings\n");
	for (i = 0; i < count; i++) {
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
				names[i], pads[i].offset);
	}
	return 0;
}

static u16 ums9117_pcm_route_pa_word(const struct ums9117_pcm *audio)
{
	if (audio->outputs == UMS9117_PCM_OUTPUTS_BOTH)
		return audio->profile.combined_pa_word;
	/* Without the speaker the PA opens only for the vibrate tone. */
	return audio->profile.speaker_pa_word;
}

/* The stock Headset, Handsfree and Headfree processing of the outputs. */
static const struct ums9117_audio_dac_processing *
ums9117_pcm_route_processing(const struct ums9117_pcm *audio)
{
	const struct ums9117_audio_profile *profile = &audio->profile;

	if (!profile->fitted)
		return NULL;
	switch (audio->outputs) {
	case UMS9117_SC2720_OUTPUT_HEADPHONES:
		return &profile->headphone_processing;
	case UMS9117_SC2720_OUTPUT_SPEAKER:
		return &profile->speaker_processing;
	case UMS9117_PCM_OUTPUTS_BOTH:
		return &profile->combined_processing;
	default:
		return NULL;
	}
}

static int ums9117_pcm_apply_processing_locked(struct ums9117_pcm *audio)
{
	return ums9117_audio_set_dac_processing(
		audio->digital, ums9117_pcm_route_processing(audio),
		audio->eq_enabled, audio->alc_enabled);
}

/*
 * Programs the digital gain, processing, headphone level, speaker mute and
 * sample scaling of the requested outputs before the codec opens or closes
 * them.
 */
static int ums9117_pcm_apply_output_levels_locked(struct ums9117_pcm *audio)
{
	unsigned long flags;
	int ret;

	ums9117_pcm_apply_output_gain(audio);
	ret = ums9117_pcm_apply_processing_locked(audio);
	if (ret)
		return ret;
	if (audio->profile.fitted) {
		ret = ums9117_pcm_set_codec_volume_locked(
			audio, audio->volume_left, audio->volume_right);
		if (ret < 0)
			return ret;
	}
	if (audio->outputs & UMS9117_SC2720_OUTPUT_SPEAKER) {
		ret = ums9117_sc2720_codec_set_speaker_mute(
			audio->codec, !audio->speaker_volume);
		if (ret < 0)
			return ret;
	}
	spin_lock_irqsave(&audio->fifo_lock, flags);
	audio->halve_samples = audio->outputs & UMS9117_SC2720_OUTPUT_SPEAKER;
	spin_unlock_irqrestore(&audio->fifo_lock, flags);
	return 0;
}

static int ums9117_pcm_prepare_hardware_locked(struct ums9117_pcm *audio,
					       unsigned int rate, bool fm)
{
	int ret;

	if (!fm && audio->idle_silence && rate != UMS9117_PCM_IDLE_RATE)
		return -EINVAL;
	if (audio->digital_prepared) {
		if (audio->rate == rate)
			return 0;
		ret = ums9117_pcm_stop_playback_locked(audio);
		if (ret)
			return ret;
	}
	if ((audio->outputs & UMS9117_SC2720_OUTPUT_SPEAKER) &&
	    !audio->profile.fitted)
		return -ENODEV;
	ret = ums9117_pcm_validate_pads(audio, audio->pads,
					ums9117_audio_pad_names,
					ARRAY_SIZE(audio->pads));
	if (ret)
		goto failed;
	if (fm)
		ret = ums9117_audio_prepare_fm(audio->digital);
	else
		ret = ums9117_audio_prepare(audio->digital, rate);
	if (ret) {
		dev_err(audio->dev, "cannot prepare digital audio: %pe\n",
			ERR_PTR(ret));
		goto failed;
	}
	audio->digital_prepared = true;
	ret = ums9117_pcm_apply_output_levels_locked(audio);
	if (ret)
		goto failed;
	ret = ums9117_sc2720_codec_prepare(audio->codec, audio->outputs,
					   ums9117_pcm_route_pa_word(audio));
	if (ret) {
		dev_err(audio->dev, "cannot prepare playback codec: %pe\n",
			ERR_PTR(ret));
		goto failed;
	}
	audio->codec_prepared = true;
	audio->rate = rate;
	return 0;

failed:
	ums9117_pcm_shutdown_playback_locked(audio);
	return ret;
}

static int ums9117_pcm_prepare_capture_locked(struct ums9117_pcm *audio)
{
	int ret;

	ret = ums9117_pcm_shutdown_capture_locked(audio);
	if (ret)
		return ret;
	ret = ums9117_pcm_validate_pads(audio, audio->pads,
					ums9117_audio_pad_names,
					ARRAY_SIZE(audio->pads));
	if (ret)
		return ret;
	ret = ums9117_pcm_validate_pads(audio, audio->capture_pads,
					ums9117_capture_pad_names,
					ARRAY_SIZE(audio->capture_pads));
	if (ret)
		return ret;
	ret = ums9117_audio_prepare_capture(audio->digital);
	if (ret)
		return ret;
	/* Retain the cleanup obligation if a partial codec setup cannot unwind. */
	audio->capture_prepared = true;
	ret = ums9117_sc2720_codec_prepare_capture(audio->codec,
						   audio->capture_source);
	if (ret) {
		dev_err(audio->dev, "cannot prepare microphone codec: %pe\n",
			ERR_PTR(ret));
		ums9117_pcm_shutdown_capture_locked(audio);
	}
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

/*
 * Zero PCM keeps the enabled outputs open between streams, so stream starts
 * and stops do not reopen the headphone amplifiers or the speaker PA.
 */
static bool ums9117_pcm_idle_wanted(const struct ums9117_pcm *audio)
{
	if (audio->fm_enabled)
		return false;
	return audio->vibrator.session ||
	       (audio->idle_silence && audio->outputs);
}

static int ums9117_pcm_start_idle_locked(struct ums9117_pcm *audio)
{
	unsigned long flags;
	bool timer_active;
	int ret;

	if (audio->removing || audio->suspended || audio->idle_running ||
	    !ums9117_pcm_idle_wanted(audio))
		return 0;
	ret = ums9117_pcm_prepare_hardware_locked(audio, UMS9117_PCM_IDLE_RATE,
						  false);
	if (ret)
		return ret;
	/*
	 * After STOP the timer may still be refilling playback. Switch it to
	 * silence in the same critical section as the first silence fill so it
	 * never services a mixture of the two.
	 */
	spin_lock_irqsave(&audio->fifo_lock, flags);
	timer_active = audio->playback.running || audio->idle_running ||
		       audio->capture.running;
	audio->playback.running = false;
	audio->idle_running = true;
	ums9117_pcm_clear_events_locked(&audio->playback);
	audio->drained_pending = false;
	ret = ums9117_pcm_fill_silence_locked(audio);
	spin_unlock_irqrestore(&audio->fifo_lock, flags);
	if (ret)
		goto failed;
	ret = ums9117_sc2720_codec_enable(audio->codec);
	if (ret)
		goto failed;
	ums9117_audio_start(audio->digital);
	if (!timer_active)
		hrtimer_start(&audio->timer,
			      ns_to_ktime(UMS9117_PCM_SERVICE_PERIOD_NS),
			      HRTIMER_MODE_REL);
	return 0;

failed:
	dev_err(audio->dev, "cannot start idle silence: %pe\n", ERR_PTR(ret));
	ums9117_pcm_shutdown_playback_locked(audio);
	return ret;
}

static int ums9117_pcm_finish_locked(struct ums9117_pcm *audio)
{
	if (audio->suspended || audio->removing ||
	    !ums9117_pcm_idle_wanted(audio))
		return ums9117_pcm_stop_playback_locked(audio);
	/* Only the already submitted FIFO tail precedes silence after STOP. */
	return ums9117_pcm_start_idle_locked(audio);
}

/* Starts or stops idle silence when no stream or FM owns the DAC. */
static int ums9117_pcm_update_idle_locked(struct ums9117_pcm *audio)
{
	if (audio->playback.running || audio->fm_enabled)
		return 0;
	if (ums9117_pcm_idle_wanted(audio))
		return ums9117_pcm_start_idle_locked(audio);
	if (audio->idle_running)
		return ums9117_pcm_stop_playback_locked(audio);
	return 0;
}

static int ums9117_pcm_start_fm_locked(struct ums9117_pcm *audio)
{
	int ret;

	ret = ums9117_pcm_stop_playback_locked(audio);
	if (ret)
		return ret;
	ret = ums9117_pcm_prepare_hardware_locked(audio, 32000, true);
	if (ret)
		return ret;
	ret = ums9117_sc2720_codec_enable(audio->codec);
	if (ret) {
		ums9117_pcm_shutdown_playback_locked(audio);
		return ret;
	}
	/* The internal IIS source supplies FM samples without a refill timer. */
	ums9117_audio_start(audio->digital);
	return 0;
}

/*
 * Applies the vibration session to the current outputs. The PA carries the
 * tone together with music or FM only on an enabled, unmuted speaker;
 * otherwise they are muted so that the PA carries the tone alone. Open
 * headphones stay silent during the session. Lower layers keep these settings
 * across later prepares and output changes.
 */
static int ums9117_pcm_apply_vibration_locked(struct ums9117_pcm *audio)
{
	bool session = audio->vibrator.session;
	bool shared = (audio->outputs & UMS9117_SC2720_OUTPUT_SPEAKER) &&
		      audio->speaker_volume;
	int ret;

	ums9117_audio_set_music_mute(audio->digital, session && !shared);
	ret = ums9117_sc2720_codec_set_vibration(audio->codec, session);
	if (ret)
		return ret;
	/* A stream or FM already runs the DAC; idle only fills the gaps. */
	return ums9117_pcm_update_idle_locked(audio);
}

static void ums9117_pcm_set_tone_locked(struct ums9117_pcm *audio, bool on)
{
	ums9117_audio_set_vibration(audio->digital, on);
	audio->vibrator.tone_on = on;
}

static void ums9117_pcm_end_vibration_locked(struct ums9117_pcm *audio)
{
	struct ums9117_vibrator *vibrator = &audio->vibrator;
	int ret;

	if (!vibrator->session)
		return;
	ums9117_pcm_set_tone_locked(audio, false);
	vibrator->session = false;
	ret = ums9117_pcm_apply_vibration_locked(audio);
	if (ret)
		dev_err(audio->dev,
			"cannot restore audio after vibration: %pe\n",
			ERR_PTR(ret));
}

static int ums9117_pcm_vibrate_locked(struct ums9117_pcm *audio)
{
	struct ums9117_vibrator *vibrator = &audio->vibrator;
	int ret;

	if (audio->removing || audio->suspended)
		return -ESHUTDOWN;
	if (!audio->profile.fitted) {
		dev_info_once(
			audio->dev,
			"speaker vibration needs the fitted audio profile\n");
		return 0;
	}
	if (!vibrator->session) {
		vibrator->session = true;
		ret = ums9117_pcm_apply_vibration_locked(audio);
		if (ret) {
			ums9117_pcm_end_vibration_locked(audio);
			return ret;
		}
	}
	/* A hold that already started sees the tone on and keeps the session. */
	cancel_delayed_work(&vibrator->hold_work);
	if (!vibrator->tone_on)
		ums9117_pcm_set_tone_locked(audio, true);
	return 0;
}

static void ums9117_pcm_vibrate_off_locked(struct ums9117_pcm *audio)
{
	struct ums9117_vibrator *vibrator = &audio->vibrator;

	if (vibrator->tone_on)
		ums9117_pcm_set_tone_locked(audio, false);
	if (vibrator->session)
		mod_delayed_work(system_wq, &vibrator->hold_work,
				 msecs_to_jiffies(UMS9117_VIBRATOR_HOLD_MS));
}

/* Reapplies an active session after its route inputs changed. */
static int ums9117_pcm_update_vibration_locked(struct ums9117_pcm *audio)
{
	if (!audio->vibrator.session)
		return 0;
	return ums9117_pcm_apply_vibration_locked(audio);
}

static int ums9117_fm_playback_get(struct snd_kcontrol *kcontrol,
				   struct snd_ctl_elem_value *value)
{
	struct ums9117_pcm *audio = snd_kcontrol_chip(kcontrol);

	mutex_lock(&audio->lock);
	value->value.integer.value[0] = audio->fm_enabled;
	mutex_unlock(&audio->lock);
	return 0;
}

static int ums9117_fm_playback_put(struct snd_kcontrol *kcontrol,
				   struct snd_ctl_elem_value *value)
{
	struct ums9117_pcm *audio = snd_kcontrol_chip(kcontrol);
	long enabled = value->value.integer.value[0];
	int restore_ret;
	int ret = 0;

	if (enabled != 0 && enabled != 1)
		return -EINVAL;
	mutex_lock(&audio->lock);
	if (audio->removing || audio->suspended) {
		ret = audio->removing ? -ENODEV : -ESTRPIPE;
		goto out;
	}
	if (audio->fm_enabled == enabled)
		goto out;
	/* An open PCM owns the path even before prepare or after STOP. */
	if (audio->playback.substream || audio->capture.substream) {
		ret = -EBUSY;
		goto out;
	}
	if (enabled) {
		ret = ums9117_pcm_start_fm_locked(audio);
		if (ret) {
			restore_ret = ums9117_pcm_start_idle_locked(audio);
			if (restore_ret)
				dev_err(audio->dev,
					"cannot restore idle silence: %pe\n",
					ERR_PTR(restore_ret));
			goto failed;
		}
		audio->fm_enabled = true;
	} else {
		ret = ums9117_pcm_stop_playback_locked(audio);
		audio->fm_enabled = false;
		if (ret)
			goto failed;
		ret = ums9117_pcm_start_idle_locked(audio);
		if (ret)
			goto failed;
	}
	ret = ums9117_pcm_update_vibration_locked(audio);
	if (ret)
		goto failed;
	ret = 1;
	goto out;

failed:
	dev_err_ratelimited(audio->dev,
			    "cannot set FM playback route=%ld: %pe\n", enabled,
			    ERR_PTR(ret));
out:
	mutex_unlock(&audio->lock);
	return ret;
}

static const struct snd_kcontrol_new ums9117_fm_playback_control = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "FM Playback Switch",
	.info = snd_ctl_boolean_mono_info,
	.get = ums9117_fm_playback_get,
	.put = ums9117_fm_playback_put,
};

static int ums9117_capture_source_info(struct snd_kcontrol *kcontrol,
				       struct snd_ctl_elem_info *info)
{
	static const char *const names[] = { "Internal", "Headset" };

	return snd_ctl_enum_info(info, 1, ARRAY_SIZE(names), names);
}

static int ums9117_capture_source_get(struct snd_kcontrol *kcontrol,
				      struct snd_ctl_elem_value *value)
{
	struct ums9117_pcm *audio = snd_kcontrol_chip(kcontrol);

	mutex_lock(&audio->lock);
	value->value.enumerated.item[0] = audio->capture_source;
	mutex_unlock(&audio->lock);
	return 0;
}

static int ums9117_capture_source_put(struct snd_kcontrol *kcontrol,
				      struct snd_ctl_elem_value *value)
{
	struct ums9117_pcm *audio = snd_kcontrol_chip(kcontrol);
	unsigned int source = value->value.enumerated.item[0];
	int ret = 0;

	if (source > UMS9117_SC2720_CAPTURE_HEADSET)
		return -EINVAL;
	mutex_lock(&audio->lock);
	if (audio->removing)
		ret = -ENODEV;
	else if (audio->capture_source != source) {
		/* The selected route belongs to the next capture session. */
		if (ums9117_pcm_capture_open(audio) || audio->capture_prepared)
			ret = -EBUSY;
		else {
			audio->capture_source = source;
			ret = 1;
		}
	}
	mutex_unlock(&audio->lock);
	return ret;
}

static const struct snd_kcontrol_new ums9117_capture_source_control = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "Capture Source",
	.info = ums9117_capture_source_info,
	.get = ums9117_capture_source_get,
	.put = ums9117_capture_source_put,
};

static int ums9117_idle_silence_get(struct snd_kcontrol *kcontrol,
				    struct snd_ctl_elem_value *value)
{
	struct ums9117_pcm *audio = snd_kcontrol_chip(kcontrol);

	mutex_lock(&audio->lock);
	value->value.integer.value[0] = audio->idle_silence;
	mutex_unlock(&audio->lock);
	return 0;
}

static int ums9117_idle_silence_put(struct snd_kcontrol *kcontrol,
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
	if (audio->fm_enabled) {
		ret = -EBUSY;
		goto out;
	}
	audio->idle_silence = enabled;
	ret = ums9117_pcm_update_idle_locked(audio);
	if (ret) {
		audio->idle_silence = !enabled;
		goto out;
	}
	ret = 1;
out:
	mutex_unlock(&audio->lock);
	return ret;
}

static const struct snd_kcontrol_new ums9117_idle_silence_control = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "Idle Silence Playback Switch",
	.info = snd_ctl_boolean_mono_info,
	.get = ums9117_idle_silence_get,
	.put = ums9117_idle_silence_put,
};

/* Applies the requested outputs to gains, codec, idle silence and vibration. */
static int ums9117_pcm_route_outputs_locked(struct ums9117_pcm *audio)
{
	int ret;

	ret = ums9117_pcm_apply_output_levels_locked(audio);
	if (ret)
		return ret;
	ret = ums9117_sc2720_codec_set_outputs(
		audio->codec, audio->outputs, ums9117_pcm_route_pa_word(audio));
	if (ret)
		return ret;
	ret = ums9117_pcm_update_idle_locked(audio);
	if (ret)
		return ret;
	return ums9117_pcm_update_vibration_locked(audio);
}

/*
 * Streams, idle silence and FM keep running across an output change; the
 * codec opens and closes the outputs under them.
 */
static int ums9117_pcm_set_outputs_locked(struct ums9117_pcm *audio,
					  unsigned int outputs)
{
	unsigned int old_outputs = audio->outputs;
	int restore_ret;
	int ret;

	if (audio->removing || audio->suspended)
		return audio->removing ? -ENODEV : -ESTRPIPE;
	if (audio->outputs == outputs)
		return 0;
	audio->outputs = outputs;
	ret = ums9117_pcm_route_outputs_locked(audio);
	if (!ret)
		return 1;
	audio->outputs = old_outputs;
	restore_ret = ums9117_pcm_route_outputs_locked(audio);
	if (restore_ret)
		dev_err(audio->dev, "cannot restore playback outputs: %pe\n",
			ERR_PTR(restore_ret));
	return ret;
}

static int ums9117_output_switch_get(struct snd_kcontrol *kcontrol,
				     struct snd_ctl_elem_value *value)
{
	struct ums9117_pcm *audio = snd_kcontrol_chip(kcontrol);
	unsigned int output = kcontrol->private_value;

	mutex_lock(&audio->lock);
	value->value.integer.value[0] = !!(audio->outputs & output);
	mutex_unlock(&audio->lock);
	return 0;
}

static int ums9117_output_switch_put(struct snd_kcontrol *kcontrol,
				     struct snd_ctl_elem_value *value)
{
	struct ums9117_pcm *audio = snd_kcontrol_chip(kcontrol);
	unsigned int output = kcontrol->private_value;
	long enabled = value->value.integer.value[0];
	unsigned int outputs;
	int ret;

	if (enabled != 0 && enabled != 1)
		return -EINVAL;
	mutex_lock(&audio->lock);
	outputs = enabled ? audio->outputs | output : audio->outputs & ~output;
	ret = ums9117_pcm_set_outputs_locked(audio, outputs);
	mutex_unlock(&audio->lock);
	return ret;
}

static const struct snd_kcontrol_new ums9117_headphone_switch_control = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "Headphone Playback Switch",
	.info = snd_ctl_boolean_mono_info,
	.get = ums9117_output_switch_get,
	.put = ums9117_output_switch_put,
	.private_value = UMS9117_SC2720_OUTPUT_HEADPHONES,
};

static const struct snd_kcontrol_new ums9117_speaker_switch_control = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "Speaker Playback Switch",
	.info = snd_ctl_boolean_mono_info,
	.get = ums9117_output_switch_get,
	.put = ums9117_output_switch_put,
	.private_value = UMS9117_SC2720_OUTPUT_SPEAKER,
};

static int ums9117_speaker_volume_info(struct snd_kcontrol *kcontrol,
				       struct snd_ctl_elem_info *info)
{
	info->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	info->count = 1;
	info->value.integer.min = 0;
	info->value.integer.max = UMS9117_AUDIO_PROFILE_LEVEL_COUNT;
	return 0;
}

static int ums9117_speaker_volume_get(struct snd_kcontrol *kcontrol,
				      struct snd_ctl_elem_value *value)
{
	struct ums9117_pcm *audio = snd_kcontrol_chip(kcontrol);

	mutex_lock(&audio->lock);
	value->value.integer.value[0] = audio->speaker_volume;
	mutex_unlock(&audio->lock);
	return 0;
}

static int ums9117_speaker_volume_put(struct snd_kcontrol *kcontrol,
				      struct snd_ctl_elem_value *value)
{
	struct ums9117_pcm *audio = snd_kcontrol_chip(kcontrol);
	long level = value->value.integer.value[0];
	unsigned int old_level;
	int ret = 0;

	if (level < 0 || level > UMS9117_AUDIO_PROFILE_LEVEL_COUNT)
		return -EINVAL;
	mutex_lock(&audio->lock);
	if (audio->removing) {
		ret = -ENODEV;
		goto out;
	}
	old_level = audio->speaker_volume;
	if (old_level == level)
		goto out;
	/* FM follows the same digital gain as PCM. */
	if ((audio->outputs & UMS9117_SC2720_OUTPUT_SPEAKER) &&
	    audio->digital_prepared) {
		if (!level) {
			ret = ums9117_sc2720_codec_set_speaker_mute(
				audio->codec, true);
			if (ret < 0)
				goto out;
		}
		audio->speaker_volume = level;
		ums9117_pcm_apply_output_gain(audio);
		if (level) {
			ret = ums9117_sc2720_codec_set_speaker_mute(
				audio->codec, false);
			if (ret < 0) {
				audio->speaker_volume = old_level;
				ums9117_pcm_apply_output_gain(audio);
				goto out;
			}
		}
	}
	audio->speaker_volume = level;
	/* Muting the speaker also mutes the music it shared with the tone. */
	ret = ums9117_pcm_update_vibration_locked(audio);
	if (ret)
		goto out;
	ret = 1;
out:
	mutex_unlock(&audio->lock);
	return ret;
}

static const struct snd_kcontrol_new ums9117_speaker_volume_control = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "Speaker Playback Volume",
	.info = ums9117_speaker_volume_info,
	.get = ums9117_speaker_volume_get,
	.put = ums9117_speaker_volume_put,
};

enum ums9117_processing_switch {
	UMS9117_PROCESSING_SWITCH_EQ,
	UMS9117_PROCESSING_SWITCH_ALC,
};

static bool *
ums9117_processing_switch_state(struct ums9117_pcm *audio,
				const struct snd_kcontrol *kcontrol)
{
	if (kcontrol->private_value == UMS9117_PROCESSING_SWITCH_ALC)
		return &audio->alc_enabled;
	return &audio->eq_enabled;
}

static int ums9117_processing_switch_get(struct snd_kcontrol *kcontrol,
					 struct snd_ctl_elem_value *value)
{
	struct ums9117_pcm *audio = snd_kcontrol_chip(kcontrol);

	mutex_lock(&audio->lock);
	value->value.integer.value[0] =
		*ums9117_processing_switch_state(audio, kcontrol);
	mutex_unlock(&audio->lock);
	return 0;
}

/* Streams, idle silence and FM keep running; EQ6 fades across the change. */
static int ums9117_processing_switch_put(struct snd_kcontrol *kcontrol,
					 struct snd_ctl_elem_value *value)
{
	struct ums9117_pcm *audio = snd_kcontrol_chip(kcontrol);
	long enabled = value->value.integer.value[0];
	int restore_ret;
	bool *state;
	int ret = 0;

	if (enabled != 0 && enabled != 1)
		return -EINVAL;
	mutex_lock(&audio->lock);
	if (audio->removing || audio->suspended) {
		ret = audio->removing ? -ENODEV : -ESTRPIPE;
		goto out;
	}
	state = ums9117_processing_switch_state(audio, kcontrol);
	if (*state == enabled)
		goto out;
	*state = enabled;
	ret = ums9117_pcm_apply_processing_locked(audio);
	if (!ret) {
		ret = 1;
		goto out;
	}
	*state = !enabled;
	restore_ret = ums9117_pcm_apply_processing_locked(audio);
	if (restore_ret)
		dev_err(audio->dev, "cannot restore playback processing: %pe\n",
			ERR_PTR(restore_ret));
out:
	mutex_unlock(&audio->lock);
	return ret;
}

static const struct snd_kcontrol_new ums9117_eq_switch_control = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "EQ Playback Switch",
	.info = snd_ctl_boolean_mono_info,
	.get = ums9117_processing_switch_get,
	.put = ums9117_processing_switch_put,
	.private_value = UMS9117_PROCESSING_SWITCH_EQ,
};

static const struct snd_kcontrol_new ums9117_alc_switch_control = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "ALC Playback Switch",
	.info = snd_ctl_boolean_mono_info,
	.get = ums9117_processing_switch_get,
	.put = ums9117_processing_switch_put,
	.private_value = UMS9117_PROCESSING_SWITCH_ALC,
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
	bool halve = audio->halve_samples;
	snd_pcm_uframes_t i;

	for (i = 0; i < frames; i++) {
		snd_pcm_uframes_t frame =
			audio->submit_ptr % runtime->buffer_size;
		snd_pcm_uframes_t sample = frame * UMS9117_PCM_CHANNELS;

		if (halve) {
			s16 left = (s16)le16_to_cpu(samples[sample]);
			s16 right = (s16)le16_to_cpu(samples[sample + 1]);

			/* DACS adds both lanes; halve each before that sum. */
			ums9117_audio_write(audio->digital, left / 2,
					    right / 2);
		} else {
			ums9117_audio_write(audio->digital,
					    le16_to_cpu(samples[sample]),
					    le16_to_cpu(samples[sample + 1]));
		}
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
	struct ums9117_pcm_stream *playback = &audio->playback;
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
		playback->last_progress = jiffies;
	/* Idle frames ahead of playback are not part of the ALSA buffer. */
	if (audio->leading_silence_frames) {
		u64 silence = min(advanced, audio->leading_silence_frames);

		audio->leading_silence_frames -= silence;
		advanced -= silence;
	}
	if (advanced) {
		playback->hw_pos += (snd_pcm_uframes_t)advanced;
		if (playback->hw_pos >= runtime->buffer_size)
			playback->hw_pos %= runtime->buffer_size;
		playback->period_frames += (snd_pcm_uframes_t)advanced;
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
		       playback->last_progress +
			       msecs_to_jiffies(UMS9117_PCM_STALL_MS))) {
		dev_err(audio->dev,
			"playback FIFO stalled for %u ms (queued=%d)\n",
			UMS9117_PCM_STALL_MS, queued);
		return UMS9117_PCM_SERVICE_XRUN;
	}
	if (playback->period_frames >= runtime->period_size) {
		playback->period_frames %= runtime->period_size;
		return UMS9117_PCM_SERVICE_PERIOD;
	}
	return UMS9117_PCM_SERVICE_IDLE;
}

/* The producer pointer belongs to this driver, not ALSA's notification thread. */
static enum ums9117_pcm_service_result
ums9117_pcm_capture_locked(struct ums9117_pcm *audio,
			   struct snd_pcm_runtime *runtime)
{
	struct ums9117_pcm_stream *capture = &audio->capture;
	snd_pcm_uframes_t appl_ptr = READ_ONCE(runtime->control->appl_ptr);
	snd_pcm_sframes_t unread = audio->capture_ptr - appl_ptr;
	__le16 *samples = (__le16 *)runtime->dma_area;
	const char *error;
	int available;
	int i;

	if (unread < -(snd_pcm_sframes_t)(runtime->boundary / 2))
		unread += runtime->boundary;
	if (unread < 0 || unread > runtime->buffer_size) {
		error = "application pointer invalid";
		goto xrun;
	}
	available = ums9117_audio_capture_available(audio->digital);
	if (available < 0) {
		error = available == -EPIPE ? "FIFO overrun" :
					      "FIFO state invalid";
		goto xrun;
	}
	if (!audio->capture_starting &&
	    available > runtime->buffer_size - unread) {
		error = "buffer overrun";
		goto xrun;
	}
	/* One sampled occupancy bounds the interrupt work to less than one FIFO. */
	for (i = 0; i < available; i++) {
		u16 sample = ums9117_audio_read_capture(audio->digital);

		if (audio->capture_starting)
			continue;
		samples[capture->hw_pos] = cpu_to_le16(sample);
		if (++capture->hw_pos == runtime->buffer_size)
			capture->hw_pos = 0;
		if (++audio->capture_ptr == runtime->boundary)
			audio->capture_ptr = 0;
	}
	if (available)
		capture->last_progress = jiffies;
	else if (time_after(jiffies,
			    capture->last_progress +
				    msecs_to_jiffies(UMS9117_PCM_STALL_MS))) {
		error = "FIFO stalled";
		goto xrun;
	}
	if (audio->capture_starting) {
		unsigned int remaining = audio->capture_warmup_frames;

		audio->capture_warmup_frames -=
			min_t(unsigned int, remaining, available);
		if (remaining && !audio->capture_warmup_frames)
			audio->capture_starting = false;
		/* The entire batch remains outside ALSA at the handover. */
		return UMS9117_PCM_SERVICE_IDLE;
	}
	capture->period_frames += available;
	if (capture->period_frames >= runtime->period_size) {
		capture->period_frames %= runtime->period_size;
		return UMS9117_PCM_SERVICE_PERIOD;
	}
	return UMS9117_PCM_SERVICE_IDLE;

xrun:
	ums9117_audio_report_capture(audio->digital, error);
	return UMS9117_PCM_SERVICE_XRUN;
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
	} else if (audio->playback.running && !audio->playback.xrun_pending &&
		   audio->playback.substream) {
		/* A reported underrun stops the refill until STOP arrives. */
		runtime = audio->playback.substream->runtime;
		if (runtime)
			result = ums9117_pcm_service_locked(audio, runtime);
	}
	switch (result) {
	case UMS9117_PCM_SERVICE_PERIOD:
		audio->playback.period_pending = true;
		notify = true;
		break;
	case UMS9117_PCM_SERVICE_DRAINED:
		audio->drained_pending = true;
		notify = true;
		break;
	case UMS9117_PCM_SERVICE_XRUN:
		audio->playback.xrun_pending = true;
		notify = true;
		break;
	case UMS9117_PCM_SERVICE_IDLE:
		break;
	}
	result = UMS9117_PCM_SERVICE_IDLE;
	if (audio->capture.running && !audio->capture.xrun_pending &&
	    audio->capture.substream) {
		runtime = audio->capture.substream->runtime;
		if (runtime)
			result = ums9117_pcm_capture_locked(audio, runtime);
	}
	switch (result) {
	case UMS9117_PCM_SERVICE_PERIOD:
		audio->capture.period_pending = true;
		notify = true;
		break;
	case UMS9117_PCM_SERVICE_XRUN:
		audio->capture.xrun_pending = true;
		notify = true;
		break;
	case UMS9117_PCM_SERVICE_DRAINED:
	case UMS9117_PCM_SERVICE_IDLE:
		break;
	}
	restart = audio->playback.running || audio->capture.running ||
		  audio->idle_running;
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
	return READ_ONCE(audio->playback.period_pending) ||
	       READ_ONCE(audio->drained_pending) ||
	       READ_ONCE(audio->playback.xrun_pending) ||
	       READ_ONCE(audio->capture.period_pending) ||
	       READ_ONCE(audio->capture.xrun_pending) ||
	       READ_ONCE(audio->idle_error);
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
		dev_err(audio->dev, "cannot feed idle silence: %pe\n",
			ERR_PTR(error));
		/* The hardware may have been restarted meanwhile. */
		if (!audio->playback.running && !audio->idle_running &&
		    !audio->fm_enabled)
			ums9117_pcm_shutdown_playback_locked(audio);
	}
	mutex_unlock(&audio->lock);
}

/* Delivers one direction's results under only that ALSA stream lock. */
static void ums9117_pcm_notify_stream(struct ums9117_pcm *audio,
				      struct ums9117_pcm_stream *stream)
{
	struct snd_pcm_substream *substream;
	struct snd_pcm_runtime *runtime = NULL;
	unsigned long flags;
	bool period = false;
	bool drained = false;
	bool xrun = false;
	bool pending;

	spin_lock_irqsave(&audio->fifo_lock, flags);
	pending = stream->period_pending || stream->xrun_pending ||
		  (stream == &audio->playback && audio->drained_pending);
	substream = stream->substream;
	spin_unlock_irqrestore(&audio->fifo_lock, flags);
	if (!pending || !substream)
		return;
	snd_pcm_stream_lock(substream);
	spin_lock_irqsave(&audio->fifo_lock, flags);
	if (stream->substream == substream && substream->runtime &&
	    stream->running) {
		runtime = substream->runtime;
		period = stream->period_pending;
		if (stream == &audio->playback)
			drained = audio->drained_pending;
		xrun = stream->xrun_pending;
	} else {
		/* The stream that produced the events has already stopped. */
		stream->xrun_pending = false;
	}
	stream->period_pending = false;
	if (stream == &audio->playback)
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

static void ums9117_pcm_notify(struct ums9117_pcm *audio)
{
	ums9117_pcm_notify_stream(audio, &audio->playback);
	ums9117_pcm_notify_stream(audio, &audio->capture);
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
					  struct ums9117_pcm_stream *stream,
					  struct snd_pcm_substream *substream)
{
	unsigned long flags;

	spin_lock_irqsave(&audio->fifo_lock, flags);
	stream->substream = substream;
	spin_unlock_irqrestore(&audio->fifo_lock, flags);
}

static int ums9117_pcm_open(struct snd_pcm_substream *substream)
{
	struct ums9117_pcm *audio = snd_pcm_substream_chip(substream);
	struct ums9117_pcm_stream *stream =
		ums9117_pcm_stream(audio, substream);
	int ret = 0;

	mutex_lock(&audio->lock);
	if (audio->removing)
		ret = -ENODEV;
	else if (stream->substream || audio->fm_enabled)
		ret = -EBUSY;
	else {
		substream->runtime->hw =
			substream->stream == SNDRV_PCM_STREAM_CAPTURE ?
				ums9117_capture_hardware :
				ums9117_pcm_hardware;
		ret = ums9117_pcm_constrain_sizes(substream->runtime);
		if (ret >= 0)
			ums9117_pcm_set_stream_locked(audio, stream, substream);
	}
	mutex_unlock(&audio->lock);
	return ret;
}

static int ums9117_pcm_close(struct snd_pcm_substream *substream)
{
	struct ums9117_pcm *audio = snd_pcm_substream_chip(substream);
	struct ums9117_pcm_stream *stream =
		ums9117_pcm_stream(audio, substream);
	int ret;

	mutex_lock(&audio->lock);
	if (substream->stream == SNDRV_PCM_STREAM_CAPTURE)
		ret = ums9117_pcm_shutdown_capture_locked(audio);
	else
		ret = ums9117_pcm_finish_locked(audio);
	if (stream->substream == substream)
		ums9117_pcm_set_stream_locked(audio, stream, NULL);
	mutex_unlock(&audio->lock);
	/* A pending notification must finish before ALSA can free this runtime. */
	snd_pcm_stream_lock(substream);
	snd_pcm_stream_unlock(substream);
	return ret;
}

static int ums9117_pcm_prepare(struct snd_pcm_substream *substream)
{
	struct ums9117_pcm *audio = snd_pcm_substream_chip(substream);
	struct ums9117_pcm_stream *stream =
		ums9117_pcm_stream(audio, substream);
	unsigned long flags;
	int ret;

	mutex_lock(&audio->lock);
	if (audio->removing || audio->suspended) {
		ret = audio->removing ? -ENODEV : -ESTRPIPE;
		goto out;
	}
	if (substream->stream == SNDRV_PCM_STREAM_CAPTURE) {
		struct snd_pcm_runtime *runtime = substream->runtime;
		long ordinary_wait_ms;

		ret = ums9117_pcm_prepare_capture_locked(audio);
		if (ret)
			goto out;
		/* ALSA must wait through pre-roll and its normal buffer budget. */
		ordinary_wait_ms = max_t(
			long, 100, runtime->buffer_size * 1100 / runtime->rate);
		substream->wait_time =
			DIV_ROUND_UP(UMS9117_PCM_CAPTURE_WARMUP_FRAMES *
					     MSEC_PER_SEC,
				     runtime->rate) +
			ordinary_wait_ms;
		goto reset_pointers;
	}
	if (!audio->idle_running) {
		ret = ums9117_pcm_stop_playback_locked(audio);
		if (ret)
			goto out;
	}
	ret = ums9117_pcm_prepare_hardware_locked(
		audio, substream->runtime->rate, false);
	if (ret)
		goto out;
	ret = ums9117_pcm_start_idle_locked(audio);
	if (ret)
		goto out;
reset_pointers:
	spin_lock_irqsave(&audio->fifo_lock, flags);
	if (stream == &audio->playback) {
		audio->submit_ptr = 0;
		audio->submitted_frames = 0;
		audio->consumed_frames = 0;
		audio->leading_silence_frames = 0;
		audio->drained_pending = false;
	} else {
		audio->capture_ptr = 0;
	}
	stream->hw_pos = 0;
	stream->period_frames = 0;
	stream->last_progress = jiffies;
	ums9117_pcm_clear_events_locked(stream);
	spin_unlock_irqrestore(&audio->fifo_lock, flags);
out:
	mutex_unlock(&audio->lock);
	return ret;
}

static int ums9117_pcm_start_capture_locked(struct ums9117_pcm *audio)
{
	unsigned long flags;
	bool timer_active;
	int ret;

	if (!audio->capture_prepared)
		return -EBADFD;
	ums9117_audio_start_capture(audio->digital);
	ret = ums9117_sc2720_codec_enable_capture(audio->codec);
	if (ret) {
		ums9117_pcm_shutdown_capture_locked(audio);
		return ret;
	}
	spin_lock_irqsave(&audio->fifo_lock, flags);
	timer_active = audio->playback.running || audio->idle_running;
	audio->capture.last_progress = jiffies;
	audio->capture_starting = true;
	audio->capture_warmup_frames = UMS9117_PCM_CAPTURE_WARMUP_FRAMES;
	audio->capture.running = true;
	spin_unlock_irqrestore(&audio->fifo_lock, flags);
	if (!timer_active)
		hrtimer_start(&audio->timer,
			      ns_to_ktime(UMS9117_PCM_SERVICE_PERIOD_NS),
			      HRTIMER_MODE_REL);
	return 0;
}

static int ums9117_pcm_trigger(struct snd_pcm_substream *substream, int command)
{
	struct ums9117_pcm *audio = snd_pcm_substream_chip(substream);
	struct snd_pcm_runtime *runtime = substream->runtime;
	snd_pcm_uframes_t pending;
	snd_pcm_uframes_t written;
	unsigned long flags;
	bool timer_active;
	int queued;
	int ret = 0;

	mutex_lock(&audio->lock);
	switch (command) {
	case SNDRV_PCM_TRIGGER_START:
		if (audio->removing || audio->suspended) {
			ret = audio->removing ? -ENODEV : -ESTRPIPE;
			if (substream->stream == SNDRV_PCM_STREAM_CAPTURE)
				ums9117_pcm_shutdown_capture_locked(audio);
			else
				ums9117_pcm_shutdown_playback_locked(audio);
			break;
		}
		if (substream->stream == SNDRV_PCM_STREAM_CAPTURE) {
			ret = ums9117_pcm_start_capture_locked(audio);
			break;
		}
		/* A control change may have released hardware after prepare. */
		ret = ums9117_pcm_prepare_hardware_locked(audio, runtime->rate,
							  false);
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
			ums9117_pcm_shutdown_playback_locked(audio);
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
			ums9117_pcm_shutdown_playback_locked(audio);
			break;
		}
		ret = ums9117_sc2720_codec_enable(audio->codec);
		if (ret) {
			dev_err(audio->dev,
				"cannot enable playback codec: %pe\n",
				ERR_PTR(ret));
			ums9117_pcm_shutdown_playback_locked(audio);
			break;
		}
		ums9117_audio_start(audio->digital);
		spin_lock_irqsave(&audio->fifo_lock, flags);
		timer_active = audio->capture.running;
		audio->playback.last_progress = jiffies;
		audio->playback.running = true;
		spin_unlock_irqrestore(&audio->fifo_lock, flags);
		if (!timer_active)
			hrtimer_start(
				&audio->timer,
				ns_to_ktime(UMS9117_PCM_SERVICE_PERIOD_NS),
				HRTIMER_MODE_REL);
		break;
	case SNDRV_PCM_TRIGGER_STOP:
		if (substream->stream == SNDRV_PCM_STREAM_CAPTURE)
			ret = ums9117_pcm_shutdown_capture_locked(audio);
		else
			ret = ums9117_pcm_finish_locked(audio);
		break;
	case SNDRV_PCM_TRIGGER_SUSPEND:
		if (substream->stream == SNDRV_PCM_STREAM_CAPTURE)
			ret = ums9117_pcm_shutdown_capture_locked(audio);
		else
			ret = ums9117_pcm_shutdown_playback_locked(audio);
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
	struct ums9117_pcm_stream *stream =
		ums9117_pcm_stream(audio, substream);
	snd_pcm_uframes_t position;
	unsigned long flags;

	spin_lock_irqsave(&audio->fifo_lock, flags);
	position = stream->hw_pos;
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

static void ums9117_vibrator_play_work(struct work_struct *work)
{
	struct ums9117_vibrator *vibrator =
		container_of(work, struct ums9117_vibrator, play_work);
	struct ums9117_pcm *audio =
		container_of(vibrator, struct ums9117_pcm, vibrator);
	unsigned long flags;
	bool on;
	int ret = 0;

	spin_lock_irqsave(&vibrator->state_lock, flags);
	on = vibrator->requested && !vibrator->suspended && !vibrator->stopping;
	spin_unlock_irqrestore(&vibrator->state_lock, flags);

	mutex_lock(&audio->lock);
	if (on)
		ret = ums9117_pcm_vibrate_locked(audio);
	if (!on || ret)
		ums9117_pcm_vibrate_off_locked(audio);
	mutex_unlock(&audio->lock);
	if (ret)
		dev_err_ratelimited(audio->dev, "cannot start vibration: %pe\n",
				    ERR_PTR(ret));
	if (on && !ret)
		return;

	spin_lock_irqsave(&vibrator->state_lock, flags);
	if (ret)
		vibrator->requested = false;
	vibrator->cutoff_latched = false;
	vibrator->off_pending = false;
	spin_unlock_irqrestore(&vibrator->state_lock, flags);
	cancel_delayed_work(&vibrator->stop_work);
}

/* Ends an activation at its deadline; a new one needs the confirmed stop. */
static void ums9117_vibrator_stop_work(struct work_struct *work)
{
	struct ums9117_vibrator *vibrator = container_of(
		to_delayed_work(work), struct ums9117_vibrator, stop_work);
	unsigned long delay = 0;
	unsigned long flags;
	bool stop = false;

	spin_lock_irqsave(&vibrator->state_lock, flags);
	if (vibrator->off_pending) {
		stop = true;
	} else if (vibrator->requested) {
		if (time_before(jiffies, vibrator->stop_deadline)) {
			delay = vibrator->stop_deadline - jiffies;
		} else {
			vibrator->requested = false;
			vibrator->cutoff_latched = true;
			vibrator->off_pending = true;
			stop = true;
		}
	}
	spin_unlock_irqrestore(&vibrator->state_lock, flags);
	if (delay)
		mod_delayed_work(system_wq, &vibrator->stop_work, delay);
	else if (stop)
		schedule_work(&vibrator->play_work);
}

static void ums9117_vibrator_hold_work(struct work_struct *work)
{
	struct ums9117_vibrator *vibrator = container_of(
		to_delayed_work(work), struct ums9117_vibrator, hold_work);
	struct ums9117_pcm *audio =
		container_of(vibrator, struct ums9117_pcm, vibrator);

	mutex_lock(&audio->lock);
	if (!vibrator->tone_on)
		ums9117_pcm_end_vibration_locked(audio);
	mutex_unlock(&audio->lock);
}

/* Called atomically by ff-memless; hardware changes run in play_work. */
static int ums9117_vibrator_play(struct input_dev *input, void *data,
				 struct ff_effect *effect)
{
	struct ums9117_pcm *audio = input_get_drvdata(input);
	struct ums9117_vibrator *vibrator = &audio->vibrator;
	bool on = effect->u.rumble.strong_magnitude ||
		  effect->u.rumble.weak_magnitude;
	unsigned long delay = 0;
	unsigned long flags;
	bool force_stop = false;
	int ret = 0;

	spin_lock_irqsave(&vibrator->state_lock, flags);
	if (vibrator->suspended || vibrator->stopping) {
		ret = -ESHUTDOWN;
	} else if (on && (vibrator->cutoff_latched || vibrator->off_pending)) {
		ret = -EBUSY;
	} else {
		if (on) {
			if (!vibrator->requested)
				vibrator->stop_deadline =
					jiffies +
					msecs_to_jiffies(
						UMS9117_VIBRATOR_MAX_ON_MS);
			if (time_before(jiffies, vibrator->stop_deadline)) {
				delay = vibrator->stop_deadline - jiffies;
			} else {
				on = false;
				vibrator->cutoff_latched = true;
				force_stop = true;
				ret = -EBUSY;
			}
		}
		vibrator->requested = on;
		if (!on)
			vibrator->off_pending = true;
	}
	spin_unlock_irqrestore(&vibrator->state_lock, flags);
	if (ret && !force_stop)
		return ret;
	if (on)
		mod_delayed_work(system_wq, &vibrator->stop_work, delay);
	schedule_work(&vibrator->play_work);
	return ret;
}

/* Stops at once without the hold; the caller has blocked new activations. */
static void ums9117_vibrator_stop(struct ums9117_pcm *audio)
{
	struct ums9117_vibrator *vibrator = &audio->vibrator;
	unsigned long flags;

	spin_lock_irqsave(&vibrator->state_lock, flags);
	vibrator->requested = false;
	vibrator->off_pending = true;
	spin_unlock_irqrestore(&vibrator->state_lock, flags);
	cancel_delayed_work_sync(&vibrator->stop_work);
	cancel_work_sync(&vibrator->play_work);
	cancel_delayed_work_sync(&vibrator->hold_work);
	mutex_lock(&audio->lock);
	ums9117_pcm_end_vibration_locked(audio);
	mutex_unlock(&audio->lock);
	spin_lock_irqsave(&vibrator->state_lock, flags);
	vibrator->cutoff_latched = false;
	vibrator->off_pending = false;
	spin_unlock_irqrestore(&vibrator->state_lock, flags);
}

static void ums9117_vibrator_close(struct input_dev *input)
{
	ums9117_vibrator_stop(input_get_drvdata(input));
}

static void ums9117_vibrator_set_lifecycle(struct ums9117_pcm *audio,
					   bool *flag, bool value)
{
	unsigned long flags;

	spin_lock_irqsave(&audio->vibrator.state_lock, flags);
	*flag = value;
	spin_unlock_irqrestore(&audio->vibrator.state_lock, flags);
}

static int ums9117_vibrator_register(struct ums9117_pcm *audio)
{
	struct input_dev *input;
	int ret;

	input = devm_input_allocate_device(audio->dev);
	if (!input)
		return -ENOMEM;
	input->name = UMS9117_VIBRATOR_NAME;
	input->phys = UMS9117_VIBRATOR_PHYS;
	input->id.bustype = BUS_HOST;
	input->close = ums9117_vibrator_close;
	input_set_drvdata(input, audio);
	input_set_capability(input, EV_FF, FF_RUMBLE);
	/* ff-memless frees its data pointer, so pass none. */
	ret = input_ff_create_memless(input, NULL, ums9117_vibrator_play);
	if (ret)
		return ret;
	return input_register_device(input);
}

static void ums9117_pcm_remove(struct platform_device *pdev);

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
	audio->capture_source = UMS9117_SC2720_CAPTURE_INTERNAL;
	audio->outputs = UMS9117_SC2720_OUTPUT_HEADPHONES;
	audio->speaker_volume = 1;
	audio->idle_silence = true;
	audio->eq_enabled = true;
	audio->alc_enabled = true;
	mutex_init(&audio->lock);
	spin_lock_init(&audio->fifo_lock);
	spin_lock_init(&audio->vibrator.state_lock);
	INIT_WORK(&audio->vibrator.play_work, ums9117_vibrator_play_work);
	INIT_DELAYED_WORK(&audio->vibrator.stop_work,
			  ums9117_vibrator_stop_work);
	INIT_DELAYED_WORK(&audio->vibrator.hold_work,
			  ums9117_vibrator_hold_work);
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
				    "fplinux,pad-settings",
				    ums9117_audio_pad_names,
				    ARRAY_SIZE(audio->pads),
				    resource_size(pinmux_resource),
				    resource_size(pinconf_resource));
	if (ret)
		return ret;
	audio->capture_supported = device_property_present(
		&pdev->dev, "fplinux,capture-pad-settings");
	if (audio->capture_supported) {
		ret = ums9117_pcm_read_pads(&pdev->dev, audio->capture_pads,
					    "fplinux,capture-pad-settings",
					    ums9117_capture_pad_names,
					    ARRAY_SIZE(audio->capture_pads),
					    resource_size(pinmux_resource),
					    resource_size(pinconf_resource));
		if (ret)
			return ret;
	}
	audio->speaker_vibration = device_property_read_bool(
		&pdev->dev, "fplinux,speaker-vibration");
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
	if (audio->speaker_vibration && audio->profile.fitted)
		ums9117_audio_set_vibrate_tone(audio->digital,
					       &audio->profile.vibrate_tone);
	if (audio->profile.fitted) {
		mutex_lock(&audio->lock);
		ret = ums9117_pcm_set_profile_volume_locked(audio, 1, 1);
		/* The first prepare programs the selected processing. */
		if (ret >= 0)
			ret = ums9117_pcm_apply_processing_locked(audio);
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
	ret = snd_ctl_add(card, snd_ctl_new1(&ums9117_headphone_switch_control,
					     audio));
	if (ret)
		return snd_card_free_on_error(&pdev->dev, ret);
	ret = snd_ctl_add(card,
			  snd_ctl_new1(&ums9117_idle_silence_control, audio));
	if (ret)
		return snd_card_free_on_error(&pdev->dev, ret);
	ret = snd_ctl_add(card,
			  snd_ctl_new1(&ums9117_fm_playback_control, audio));
	if (ret)
		return snd_card_free_on_error(&pdev->dev, ret);
	if (audio->profile.fitted) {
		ret = snd_ctl_add(card,
				  snd_ctl_new1(&ums9117_speaker_switch_control,
					       audio));
		if (ret)
			return snd_card_free_on_error(&pdev->dev, ret);
		ret = snd_ctl_add(card,
				  snd_ctl_new1(&ums9117_speaker_volume_control,
					       audio));
		if (ret)
			return snd_card_free_on_error(&pdev->dev, ret);
		ret = snd_ctl_add(card, snd_ctl_new1(&ums9117_eq_switch_control,
						     audio));
		if (ret)
			return snd_card_free_on_error(&pdev->dev, ret);
		ret = snd_ctl_add(
			card, snd_ctl_new1(&ums9117_alc_switch_control, audio));
		if (ret)
			return snd_card_free_on_error(&pdev->dev, ret);
	}
	if (audio->capture_supported) {
		ret = snd_ctl_add(card,
				  snd_ctl_new1(&ums9117_capture_source_control,
					       audio));
		if (ret)
			return snd_card_free_on_error(&pdev->dev, ret);
	}
	ret = snd_pcm_new(card, UMS9117_PCM_NAME, 0, 1,
			  audio->capture_supported, &pcm);
	if (ret)
		return snd_card_free_on_error(&pdev->dev, ret);
	audio->pcm = pcm;
	pcm->private_data = audio;
	pcm->nonatomic = true;
	strscpy(pcm->name, UMS9117_PCM_NAME);
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &ums9117_pcm_ops);
	if (audio->capture_supported)
		snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE,
				&ums9117_pcm_ops);
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
	if (audio->speaker_vibration) {
		/* Applications find the vibrator even before device data exists. */
		ret = ums9117_vibrator_register(audio);
		if (ret) {
			ums9117_pcm_remove(pdev);
			return ret;
		}
	}
	dev_dbg(audio->dev, "headphone volume source: %s\n",
		audio->profile.fitted ? "fitted gain profile" :
					"generic defaults");
	return 0;
}

static void ums9117_pcm_remove(struct platform_device *pdev)
{
	struct ums9117_pcm *audio = platform_get_drvdata(pdev);
	struct snd_pcm_substream *playback;
	struct snd_pcm_substream *capture;

	ums9117_vibrator_set_lifecycle(audio, &audio->vibrator.stopping, true);
	ums9117_vibrator_stop(audio);
	mutex_lock(&audio->lock);
	audio->removing = true;
	ums9117_pcm_stop_playback_refill_locked(audio);
	ums9117_pcm_stop_capture_refill_locked(audio);
	playback = audio->playback.substream;
	capture = audio->capture.substream;
	mutex_unlock(&audio->lock);
	snd_card_disconnect(audio->card);
	kthread_stop(audio->thread);
	if (playback) {
		snd_pcm_stream_lock(playback);
		snd_pcm_stream_unlock(playback);
	}
	if (capture) {
		snd_pcm_stream_lock(capture);
		snd_pcm_stream_unlock(capture);
	}
	mutex_lock(&audio->lock);
	ums9117_pcm_shutdown_capture_locked(audio);
	ums9117_pcm_shutdown_playback_locked(audio);
	mutex_unlock(&audio->lock);
}

static int ums9117_pcm_suspend(struct device *dev)
{
	struct ums9117_pcm *audio = dev_get_drvdata(dev);
	int ret;

	ums9117_vibrator_set_lifecycle(audio, &audio->vibrator.suspended, true);
	ums9117_vibrator_stop(audio);
	mutex_lock(&audio->lock);
	audio->suspended = true;
	mutex_unlock(&audio->lock);
	ret = snd_pcm_suspend_all(audio->pcm);
	mutex_lock(&audio->lock);
	ums9117_pcm_shutdown_capture_locked(audio);
	ums9117_pcm_shutdown_playback_locked(audio);
	mutex_unlock(&audio->lock);
	return ret;
}

static int ums9117_pcm_resume(struct device *dev)
{
	struct ums9117_pcm *audio = dev_get_drvdata(dev);
	int ret = 0;

	mutex_lock(&audio->lock);
	audio->suspended = false;
	if (audio->fm_enabled)
		ret = ums9117_pcm_start_fm_locked(audio);
	else
		ret = ums9117_pcm_start_idle_locked(audio);
	mutex_unlock(&audio->lock);
	/* An interrupted pulse is not resumed; new requests are accepted. */
	ums9117_vibrator_set_lifecycle(audio, &audio->vibrator.suspended,
				       false);
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

MODULE_DESCRIPTION("UMS9117 PIO audio driver");
MODULE_LICENSE("GPL");
