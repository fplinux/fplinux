/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_PCM_RESET_ALSA_H
#define FPLINUX_PCM_RESET_ALSA_H

#include "pcm-reset-kernel.h"

typedef unsigned long snd_pcm_uframes_t;
typedef long snd_pcm_sframes_t;
typedef int snd_pcm_state_t;
#define SNDRV_PCM_STREAM_PLAYBACK 0
#define SNDRV_PCM_STREAM_CAPTURE 1
#define SNDRV_PCM_IOCTL1_RESET 0
#define SNDRV_PCM_TRIGGER_START 1
#define SNDRV_PCM_TRIGGER_STOP 0
#define SNDRV_PCM_TRIGGER_SUSPEND 3
#define SNDRV_PCM_STATE_PREPARED 2
#define SNDRV_PCM_STATE_RUNNING 3
#define SNDRV_PCM_STATE_XRUN 4
#define SNDRV_PCM_STATE_DRAINING 5
#define SNDRV_PCM_INFO_INTERLEAVED 0x100
#define SNDRV_PCM_INFO_BLOCK_TRANSFER 0x10000
#define SNDRV_PCM_INFO_MMAP 1
#define SNDRV_PCM_INFO_MMAP_VALID 2
#define SNDRV_PCM_INFO_FIFO_IN_FRAMES 0x80000000
#define SNDRV_PCM_INFO_NO_REWINDS 0x20000000
#define SNDRV_PCM_FMTBIT_S16_LE 4
#define SNDRV_PCM_RATE_24000 0x800
#define SNDRV_PCM_RATE_48000 0x80
#define SNDRV_PCM_HW_PARAM_PERIOD_SIZE 13
#define SNDRV_PCM_HW_PARAM_BUFFER_SIZE 17
#define SNDRV_DMA_TYPE_VMALLOC 7
#define SNDRV_DEFAULT_IDX1 -1
#define SNDRV_DEFAULT_STR1 NULL

struct snd_pcm_hardware {
	unsigned int info, formats, rates, rate_min, rate_max;
	unsigned int channels_min, channels_max;
	size_t buffer_bytes_max, period_bytes_min, period_bytes_max;
	unsigned int periods_min, periods_max, fifo_size;
};
struct snd_pcm_mmap_status {
	snd_pcm_uframes_t hw_ptr;
};
struct snd_pcm_mmap_control {
	snd_pcm_uframes_t appl_ptr;
};
struct snd_pcm_runtime {
	struct snd_pcm_hardware hw;
	struct snd_pcm_mmap_status *status;
	struct snd_pcm_mmap_control *control;
	snd_pcm_state_t state;
	snd_pcm_uframes_t buffer_size, period_size, boundary;
	snd_pcm_uframes_t hw_ptr_base, hw_ptr_interrupt, hw_ptr_wrap;
	snd_pcm_uframes_t silence_start, silence_filled;
	snd_pcm_uframes_t stop_threshold;
	unsigned long hw_ptr_jiffies, hw_ptr_buffer_jiffies;
	unsigned int rate;
	void *dma_area;
	struct mutex buffer_mutex;
};
struct snd_pcm;
struct snd_pcm_substream {
	struct snd_pcm *pcm;
	struct snd_pcm_runtime *runtime;
	int stream;
	long wait_time;
	struct mutex stream_mutex;
};
struct snd_pcm_ops {
	int (*open)(struct snd_pcm_substream *substream);
	int (*close)(struct snd_pcm_substream *substream);
	int (*ioctl)(struct snd_pcm_substream *, unsigned int command,
		     void *argument);
	int (*prepare)(struct snd_pcm_substream *substream);
	int (*trigger)(struct snd_pcm_substream *substream, int command);
	snd_pcm_uframes_t (*pointer)(struct snd_pcm_substream *substream);
};
struct snd_pcm {
	void *private_data;
	bool nonatomic;
	char name[80];
	const struct snd_pcm_ops *ops[2];
};
struct snd_card {
	char driver[32], shortname[32], longname[80];
};
#define snd_pcm_substream_chip(substream) ((substream)->pcm->private_data)
#define snd_pcm_running(substream)                                   \
	((substream)->runtime->state == SNDRV_PCM_STATE_RUNNING ||   \
	 ((substream)->runtime->state == SNDRV_PCM_STATE_DRAINING && \
	  (substream)->stream == SNDRV_PCM_STREAM_PLAYBACK))
void snd_pcm_stream_lock(struct snd_pcm_substream *substream);
void snd_pcm_stream_unlock(struct snd_pcm_substream *substream);
int snd_pcm_lib_ioctl(struct snd_pcm_substream *substream, unsigned int command,
		      void *argument);
int snd_pcm_hw_constraint_step(struct snd_pcm_runtime *runtime,
			       unsigned int flags, unsigned int parameter,
			       unsigned long step);
int snd_devm_card_new(struct device *dev, int index, const char *id,
		      void *module, size_t extra, struct snd_card **card);
int snd_card_free_on_error(struct device *dev, int error);
int snd_card_register(struct snd_card *card);
void snd_card_disconnect(struct snd_card *card);
int snd_pcm_new(struct snd_card *card, const char *name, int index,
		unsigned int playback, unsigned int capture,
		struct snd_pcm **pcm);
void snd_pcm_set_ops(struct snd_pcm *pcm, int stream,
		     const struct snd_pcm_ops *ops);
int snd_pcm_set_managed_buffer_all(struct snd_pcm *pcm, int type, void *data,
				   size_t initial, size_t maximum);
int snd_pcm_stop(struct snd_pcm_substream *substream, snd_pcm_state_t state);
void snd_pcm_period_elapsed_under_stream_lock(
	struct snd_pcm_substream *substream);
void snd_pcm_drain_done(struct snd_pcm_substream *substream);
int snd_pcm_suspend_all(struct snd_pcm *pcm);

/* Controls are registered by probe, but their callbacks are not driven. */
#define SNDRV_CTL_ELEM_TYPE_INTEGER 2
#define SNDRV_CTL_ELEM_IFACE_MIXER 2
#define SNDRV_CTL_ELEM_ACCESS_READWRITE 3
#define SNDRV_CTL_ELEM_ACCESS_TLV_READ 16
#define DECLARE_TLV_DB_SCALE(name, minimum, step, mute) \
	unsigned int name[] = { 0 }
struct snd_kcontrol {
	void *private_data;
	unsigned long private_value;
};
struct snd_ctl_elem_info {
	unsigned int type, count;
	union {
		struct {
			long min, max;
		} integer;
	} value;
};
struct snd_ctl_elem_value {
	union {
		struct {
			long value[2];
		} integer;
		struct {
			unsigned int item[2];
		} enumerated;
	} value;
};
struct snd_kcontrol_new {
	unsigned int iface, access;
	const char *name;
	int (*info)(struct snd_kcontrol *, struct snd_ctl_elem_info *);
	int (*get)(struct snd_kcontrol *, struct snd_ctl_elem_value *);
	int (*put)(struct snd_kcontrol *, struct snd_ctl_elem_value *);
	unsigned long private_value;
	union {
		const unsigned int *p;
	} tlv;
};
#define snd_kcontrol_chip(control) ((control)->private_data)
int snd_ctl_boolean_mono_info(struct snd_kcontrol *,
			      struct snd_ctl_elem_info *);
int snd_ctl_enum_info(struct snd_ctl_elem_info *, unsigned int, unsigned int,
		      const char *const *names);
struct snd_kcontrol *snd_ctl_new1(const struct snd_kcontrol_new *control,
				  void *data);
int snd_ctl_add(struct snd_card *card, struct snd_kcontrol *control);

#endif
