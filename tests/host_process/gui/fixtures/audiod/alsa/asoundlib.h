/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_AUDIOD_TEST_ASOUNDLIB_H
#define FPLINUX_AUDIOD_TEST_ASOUNDLIB_H

#include <poll.h>
#include <string.h>

/* Minimal external API double: the playback calls audiod makes. */
typedef struct _snd_pcm snd_pcm_t;
typedef unsigned long snd_pcm_uframes_t;
typedef long snd_pcm_sframes_t;
typedef enum { SND_PCM_STREAM_PLAYBACK = 0 } snd_pcm_stream_t;
typedef enum { SND_PCM_ACCESS_RW_INTERLEAVED = 3 } snd_pcm_access_t;
typedef enum { SND_PCM_FORMAT_S16_LE = 2 } snd_pcm_format_t;
typedef struct snd_pcm_hw_params {
	snd_pcm_access_t access;
	snd_pcm_format_t format;
	unsigned int channels;
	unsigned int rate;
	snd_pcm_uframes_t period;
	snd_pcm_uframes_t buffer;
} snd_pcm_hw_params_t;
typedef struct snd_pcm_sw_params {
	snd_pcm_uframes_t start_threshold;
	snd_pcm_uframes_t avail_min;
} snd_pcm_sw_params_t;

#define SND_PCM_NONBLOCK 1
#define snd_pcm_hw_params_alloca(pointer)                           \
	do {                                                        \
		*(pointer) = alloca(sizeof(snd_pcm_hw_params_t));   \
		memset(*(pointer), 0, sizeof(snd_pcm_hw_params_t)); \
	} while (0)
#define snd_pcm_sw_params_alloca(pointer)                           \
	do {                                                        \
		*(pointer) = alloca(sizeof(snd_pcm_sw_params_t));   \
		memset(*(pointer), 0, sizeof(snd_pcm_sw_params_t)); \
	} while (0)

int snd_pcm_open(snd_pcm_t **pcm, const char *name, snd_pcm_stream_t stream,
		 int mode);
int snd_pcm_close(snd_pcm_t *pcm);
int snd_pcm_hw_params_any(snd_pcm_t *pcm, snd_pcm_hw_params_t *params);
int snd_pcm_hw_params_set_access(snd_pcm_t *pcm, snd_pcm_hw_params_t *params,
				 snd_pcm_access_t access);
int snd_pcm_hw_params_set_format(snd_pcm_t *pcm, snd_pcm_hw_params_t *params,
				 snd_pcm_format_t format);
int snd_pcm_hw_params_set_channels(snd_pcm_t *pcm, snd_pcm_hw_params_t *params,
				   unsigned int channels);
int snd_pcm_hw_params_set_rate(snd_pcm_t *pcm, snd_pcm_hw_params_t *params,
			       unsigned int rate, int direction);
int snd_pcm_hw_params_set_period_size(snd_pcm_t *pcm,
				      snd_pcm_hw_params_t *params,
				      snd_pcm_uframes_t frames, int direction);
int snd_pcm_hw_params_set_buffer_size(snd_pcm_t *pcm,
				      snd_pcm_hw_params_t *params,
				      snd_pcm_uframes_t frames);
int snd_pcm_hw_params(snd_pcm_t *pcm, snd_pcm_hw_params_t *params);
int snd_pcm_sw_params_current(snd_pcm_t *pcm, snd_pcm_sw_params_t *params);
int snd_pcm_sw_params_set_start_threshold(snd_pcm_t *pcm,
					  snd_pcm_sw_params_t *params,
					  snd_pcm_uframes_t frames);
int snd_pcm_sw_params_set_avail_min(snd_pcm_t *pcm, snd_pcm_sw_params_t *params,
				    snd_pcm_uframes_t frames);
int snd_pcm_sw_params(snd_pcm_t *pcm, snd_pcm_sw_params_t *params);
int snd_pcm_prepare(snd_pcm_t *pcm);
int snd_pcm_poll_descriptors_count(snd_pcm_t *pcm);
int snd_pcm_poll_descriptors(snd_pcm_t *pcm, struct pollfd *fds,
			     unsigned int space);
int snd_pcm_poll_descriptors_revents(snd_pcm_t *pcm, struct pollfd *fds,
				     unsigned int count,
				     unsigned short *revents);
snd_pcm_sframes_t snd_pcm_avail_update(snd_pcm_t *pcm);
snd_pcm_sframes_t snd_pcm_writei(snd_pcm_t *pcm, const void *frames,
				 snd_pcm_uframes_t count);
int snd_pcm_delay(snd_pcm_t *pcm, snd_pcm_sframes_t *delay);
const char *snd_strerror(int error);

#endif
