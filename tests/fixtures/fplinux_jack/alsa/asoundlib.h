/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_JACK_TEST_ASOUNDLIB_H
#define FPLINUX_JACK_TEST_ASOUNDLIB_H

/*
 * Minimal ALSA control API double for host tests without ALSA development
 * files. Names and values follow alsa-lib; boundary.c scripts one card.
 */
typedef int snd_ctl_elem_iface_t;
typedef int snd_ctl_event_type_t;
typedef struct snd_ctl {
	int unused;
} snd_ctl_t;
typedef struct snd_ctl_elem_id {
	snd_ctl_elem_iface_t iface;
	const char *name;
} snd_ctl_elem_id_t;
typedef struct snd_ctl_elem_info {
	snd_ctl_elem_id_t id;
	unsigned int numid;
} snd_ctl_elem_info_t;
typedef struct snd_ctl_elem_value {
	snd_ctl_elem_id_t id;
	long boolean;
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
