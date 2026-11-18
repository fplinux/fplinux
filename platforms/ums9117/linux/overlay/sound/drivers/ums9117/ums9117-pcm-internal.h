/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef UMS9117_PCM_INTERNAL_H
#define UMS9117_PCM_INTERNAL_H

#include <linux/hrtimer.h>
#include <linux/kconfig.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/workqueue.h>
#include <sound/pcm.h>

#include "ums9117-audio-profile.h"
#include "ums9117-sc2720-codec.h"

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
	struct ums9117_jack *jack;
	struct task_struct *thread;
	wait_queue_head_t thread_wait;
	struct hrtimer timer;
	/* Serializes the hardware lifecycle; the timer never takes it. */
	struct mutex lock;
	unsigned int rate;
	struct ums9117_audio_profile profile;
	struct ums9117_vibrator vibrator;
	unsigned int volume_left;
	unsigned int volume_right;
	unsigned int speaker_volume;
	enum ums9117_sc2720_capture_source capture_source;
	/* Requested set selects fitted data; board policy selects codec outputs. */
	unsigned int outputs;
	/* Fitted EQ6 and ALC switches, each controlling its own block. */
	bool eq_enabled;
	bool alc_enabled;
	bool codec_prepared;
	bool digital_prepared;
	bool capture_supported;
#if IS_ENABLED(CONFIG_SND_UMS9117_HEADPHONE_FED_SPEAKER)
	bool speaker_from_headphones;
#endif
	bool speaker_supported;
	bool headphones_only;
	bool headfree_headphones_only;
	bool capture_prepared;
	bool speaker_vibration;
	bool idle_silence;
	bool fm_enabled;
	bool external_fm;
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
	 * The integrated speaker uses DACS, which adds both lanes. Its PCM samples
	 * are halved with or without the headphones. Headphone-fed speakers retain
	 * their stereo samples. FM samples
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

#if IS_ENABLED(CONFIG_SND_UMS9117_SPEAKER_VIBRATOR)
void ums9117_pcm_end_vibration_locked(struct ums9117_pcm *audio);
int ums9117_pcm_vibrate_locked(struct ums9117_pcm *audio);
void ums9117_pcm_vibrate_off_locked(struct ums9117_pcm *audio);
void ums9117_vibrator_init(struct ums9117_pcm *audio);
void ums9117_vibrator_stop(struct ums9117_pcm *audio);
void ums9117_vibrator_set_lifecycle(struct ums9117_pcm *audio, bool *flag,
				    bool value);
int ums9117_vibrator_register(struct ums9117_pcm *audio);
#endif

#endif
