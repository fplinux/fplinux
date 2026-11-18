/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_AUDIOD_TEST_ASOUNDLIB_H
#define FPLINUX_AUDIOD_TEST_ASOUNDLIB_H

#include <poll.h>
#include <string.h>

typedef struct _snd_ctl snd_ctl_t;
typedef int snd_ctl_elem_iface_t;
typedef struct snd_ctl_card_info {
	int unused;
} snd_ctl_card_info_t;
typedef struct snd_ctl_elem_info {
	const char *name;
	snd_ctl_elem_iface_t iface;
	unsigned int numid;
} snd_ctl_elem_info_t;
typedef struct snd_ctl_elem_value {
	unsigned int numid;
	long boolean;
	long integer[2];
} snd_ctl_elem_value_t;
typedef struct snd_ctl_event {
	int unused;
} snd_ctl_event_t;

#define SND_CTL_NONBLOCK 1
#define SND_CTL_ELEM_IFACE_CARD 0
#define SND_CTL_ELEM_IFACE_MIXER 2
#define SND_CTL_ELEM_TYPE_BOOLEAN 1
#define SND_CTL_ELEM_TYPE_INTEGER 2
#define SND_CTL_EVENT_ELEM 0
#define SND_CTL_EVENT_MASK_REMOVE (~0U)
#define snd_ctl_card_info_alloca(pointer)                         \
	do {                                                      \
		*(pointer) = alloca(sizeof(snd_ctl_card_info_t)); \
	} while (0)
#define snd_ctl_elem_info_alloca(pointer)                         \
	do {                                                      \
		*(pointer) = alloca(sizeof(snd_ctl_elem_info_t)); \
	} while (0)
#define snd_ctl_elem_value_alloca(pointer)                         \
	do {                                                       \
		*(pointer) = alloca(sizeof(snd_ctl_elem_value_t)); \
	} while (0)
#define snd_ctl_event_alloca(pointer)                         \
	do {                                                  \
		*(pointer) = alloca(sizeof(snd_ctl_event_t)); \
	} while (0)

int snd_ctl_open(snd_ctl_t **control, const char *name, int mode);
int snd_ctl_close(snd_ctl_t *control);
int snd_ctl_card_info(snd_ctl_t *control, snd_ctl_card_info_t *info);
const char *snd_ctl_card_info_get_driver(const snd_ctl_card_info_t *info);
void snd_ctl_elem_info_set_interface(snd_ctl_elem_info_t *info,
				     snd_ctl_elem_iface_t iface);
void snd_ctl_elem_info_set_name(snd_ctl_elem_info_t *info, const char *name);
int snd_ctl_elem_info(snd_ctl_t *control, snd_ctl_elem_info_t *info);
int snd_ctl_elem_info_get_type(const snd_ctl_elem_info_t *info);
int snd_ctl_elem_info_is_readable(const snd_ctl_elem_info_t *info);
int snd_ctl_elem_info_is_writable(const snd_ctl_elem_info_t *info);
unsigned int snd_ctl_elem_info_get_count(const snd_ctl_elem_info_t *info);
long snd_ctl_elem_info_get_min(const snd_ctl_elem_info_t *info);
long snd_ctl_elem_info_get_max(const snd_ctl_elem_info_t *info);
long snd_ctl_elem_value_get_integer(const snd_ctl_elem_value_t *value,
				    unsigned int index);
void snd_ctl_elem_value_set_integer(snd_ctl_elem_value_t *value,
				    unsigned int index, long level);
unsigned int snd_ctl_elem_info_get_numid(const snd_ctl_elem_info_t *info);
void snd_ctl_elem_value_set_numid(snd_ctl_elem_value_t *value,
				  unsigned int numid);
int snd_ctl_elem_read(snd_ctl_t *control, snd_ctl_elem_value_t *value);
int snd_ctl_elem_value_get_boolean(const snd_ctl_elem_value_t *value,
				   unsigned int index);
void snd_ctl_elem_value_set_boolean(snd_ctl_elem_value_t *value,
				    unsigned int index, long enabled);
int snd_ctl_elem_write(snd_ctl_t *control, snd_ctl_elem_value_t *value);
int snd_ctl_subscribe_events(snd_ctl_t *control, int subscribe);
int snd_ctl_poll_descriptors_count(snd_ctl_t *control);
int snd_ctl_poll_descriptors(snd_ctl_t *control, struct pollfd *fds,
			     unsigned int count);
int snd_ctl_read(snd_ctl_t *control, snd_ctl_event_t *event);
int snd_ctl_event_get_type(const snd_ctl_event_t *event);
unsigned int snd_ctl_event_elem_get_numid(const snd_ctl_event_t *event);
unsigned int snd_ctl_event_elem_get_mask(const snd_ctl_event_t *event);

/* External ALSA API double for the service's playback and control calls. */
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
