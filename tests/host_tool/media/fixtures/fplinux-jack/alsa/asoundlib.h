/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_JACK_TEST_ASOUNDLIB_H
#define FPLINUX_JACK_TEST_ASOUNDLIB_H

#include <poll.h>
#include <stdint.h>

/*
 * Minimal ALSA control API double for host tests without ALSA development
 * files. Names and values follow alsa-lib; boundary.c scripts one card.
 */
typedef int snd_ctl_elem_iface_t;
typedef int snd_ctl_event_type_t;
typedef struct _snd_ctl {
	int unused;
} snd_ctl_t;
typedef struct snd_ctl_elem_id {
	snd_ctl_elem_iface_t iface;
	const char *name;
	unsigned int numid;
} snd_ctl_elem_id_t;
typedef struct snd_ctl_elem_info {
	snd_ctl_elem_id_t id;
	unsigned int numid;
} snd_ctl_elem_info_t;
typedef struct snd_ctl_elem_value {
	snd_ctl_elem_id_t id;
	long boolean;
	long integer[2];
} snd_ctl_elem_value_t;
typedef struct snd_ctl_event {
	unsigned int numid;
	unsigned int mask;
} snd_ctl_event_t;

#define SND_CTL_ELEM_IFACE_CARD 0
#define SND_CTL_ELEM_IFACE_MIXER 2
#define SND_CTL_EVENT_ELEM 0
#define SND_CTL_EVENT_MASK_VALUE (1U << 0)
#define SND_CTL_EVENT_MASK_REMOVE (~0U)
#define SND_CTL_NONBLOCK 1
#define SND_CTL_ELEM_TYPE_BOOLEAN 1
#define SND_CTL_ELEM_TYPE_INTEGER 2

typedef struct snd_ctl_card_info {
	int unused;
} snd_ctl_card_info_t;
#define snd_ctl_card_info_alloca(pointer)                  \
	do {                                               \
		static snd_ctl_card_info_t test_card_info; \
		*(pointer) = &test_card_info;              \
	} while (0)

typedef struct _snd_pcm snd_pcm_t;
typedef long snd_pcm_sframes_t;
typedef struct snd_pcm_hw_params {
	int unused;
} snd_pcm_hw_params_t;
typedef struct snd_pcm_sw_params {
	int unused;
} snd_pcm_sw_params_t;
#define snd_pcm_hw_params_alloca(pointer)               \
	do {                                            \
		static snd_pcm_hw_params_t test_pcm_hw; \
		*(pointer) = &test_pcm_hw;              \
	} while (0)
#define snd_pcm_sw_params_alloca(pointer)               \
	do {                                            \
		static snd_pcm_sw_params_t test_pcm_sw; \
		*(pointer) = &test_pcm_sw;              \
	} while (0)
#define SND_PCM_ACCESS_RW_INTERLEAVED 3
#define SND_PCM_FORMAT_S16_LE 2
#define SND_PCM_STREAM_PLAYBACK 0
#define SND_PCM_NONBLOCK 1

int snd_ctl_card_info(snd_ctl_t *ctl, snd_ctl_card_info_t *info);
const char *snd_ctl_card_info_get_driver(const snd_ctl_card_info_t *info);
void snd_ctl_elem_info_set_interface(snd_ctl_elem_info_t *info,
				     snd_ctl_elem_iface_t iface);
void snd_ctl_elem_info_set_name(snd_ctl_elem_info_t *info, const char *name);
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
void snd_ctl_elem_value_set_numid(snd_ctl_elem_value_t *value,
				  unsigned int numid);
int snd_ctl_poll_descriptors_count(snd_ctl_t *ctl);
int snd_ctl_poll_descriptors(snd_ctl_t *ctl, struct pollfd *fds,
			     unsigned int count);
int snd_pcm_hw_params_any(snd_pcm_t *pcm, snd_pcm_hw_params_t *hw);
int snd_pcm_hw_params_set_access(snd_pcm_t *pcm, snd_pcm_hw_params_t *hw,
				 int access);
int snd_pcm_hw_params_set_format(snd_pcm_t *pcm, snd_pcm_hw_params_t *hw,
				 int format);
int snd_pcm_hw_params_set_channels(snd_pcm_t *pcm, snd_pcm_hw_params_t *hw,
				   unsigned int channels);
int snd_pcm_hw_params_set_rate(snd_pcm_t *pcm, snd_pcm_hw_params_t *hw,
			       unsigned int rate, int direction);
int snd_pcm_hw_params_set_period_size(snd_pcm_t *pcm, snd_pcm_hw_params_t *hw,
				      unsigned long frames, int direction);
int snd_pcm_hw_params_set_buffer_size(snd_pcm_t *pcm, snd_pcm_hw_params_t *hw,
				      unsigned long frames);
int snd_pcm_hw_params(snd_pcm_t *pcm, snd_pcm_hw_params_t *hw);
int snd_pcm_sw_params_current(snd_pcm_t *pcm, snd_pcm_sw_params_t *sw);
int snd_pcm_sw_params_set_start_threshold(snd_pcm_t *pcm,
					  snd_pcm_sw_params_t *sw,
					  unsigned long frames);
int snd_pcm_sw_params_set_avail_min(snd_pcm_t *pcm, snd_pcm_sw_params_t *sw,
				    unsigned long frames);
int snd_pcm_sw_params(snd_pcm_t *pcm, snd_pcm_sw_params_t *sw);
int snd_pcm_open(snd_pcm_t **pcm, const char *name, int stream, int mode);
int snd_pcm_close(snd_pcm_t *pcm);
int snd_pcm_poll_descriptors_count(snd_pcm_t *pcm);
int snd_pcm_poll_descriptors(snd_pcm_t *pcm, struct pollfd *fds,
			     unsigned int count);
int snd_pcm_poll_descriptors_revents(snd_pcm_t *pcm, struct pollfd *fds,
				     unsigned int count,
				     unsigned short *revents);
long snd_pcm_avail_update(snd_pcm_t *pcm);
long snd_pcm_writei(snd_pcm_t *pcm, const void *frames, unsigned long count);
int snd_pcm_prepare(snd_pcm_t *pcm);
int snd_pcm_delay(snd_pcm_t *pcm, snd_pcm_sframes_t *delay);

#define snd_ctl_elem_id_alloca(pointer)           \
	do {                                      \
		static snd_ctl_elem_id_t test_id; \
		*(pointer) = &test_id;            \
	} while (0)
#define snd_ctl_elem_info_alloca(pointer)             \
	do {                                          \
		static snd_ctl_elem_info_t test_info; \
		*(pointer) = &test_info;              \
	} while (0)
#define snd_ctl_elem_value_alloca(pointer)              \
	do {                                            \
		static snd_ctl_elem_value_t test_value; \
		*(pointer) = &test_value;               \
	} while (0)
#define snd_ctl_event_alloca(pointer)              \
	do {                                       \
		static snd_ctl_event_t test_event; \
		*(pointer) = &test_event;          \
	} while (0)

int snd_ctl_open(snd_ctl_t **ctl, const char *name, int mode);
int snd_ctl_close(snd_ctl_t *ctl);
int snd_ctl_subscribe_events(snd_ctl_t *ctl, int subscribe);
void snd_ctl_elem_id_set_interface(snd_ctl_elem_id_t *id,
				   snd_ctl_elem_iface_t iface);
void snd_ctl_elem_id_set_name(snd_ctl_elem_id_t *id, const char *name);
void snd_ctl_elem_info_set_id(snd_ctl_elem_info_t *info,
			      const snd_ctl_elem_id_t *id);
int snd_ctl_elem_info(snd_ctl_t *ctl, snd_ctl_elem_info_t *info);
unsigned int snd_ctl_elem_info_get_numid(const snd_ctl_elem_info_t *info);
void snd_ctl_elem_value_set_id(snd_ctl_elem_value_t *value,
			       const snd_ctl_elem_id_t *id);
int snd_ctl_elem_value_get_boolean(const snd_ctl_elem_value_t *value,
				   unsigned int index);
void snd_ctl_elem_value_set_boolean(snd_ctl_elem_value_t *value,
				    unsigned int index, long boolean);
int snd_ctl_elem_read(snd_ctl_t *ctl, snd_ctl_elem_value_t *value);
int snd_ctl_elem_write(snd_ctl_t *ctl, snd_ctl_elem_value_t *value);
int snd_ctl_read(snd_ctl_t *ctl, snd_ctl_event_t *event);
snd_ctl_event_type_t snd_ctl_event_get_type(const snd_ctl_event_t *event);
unsigned int snd_ctl_event_elem_get_numid(const snd_ctl_event_t *event);
unsigned int snd_ctl_event_elem_get_mask(const snd_ctl_event_t *event);
const char *snd_strerror(int error);

#endif
