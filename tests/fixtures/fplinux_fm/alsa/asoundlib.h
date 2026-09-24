/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_FM_TEST_ASOUNDLIB_H
#define FPLINUX_FM_TEST_ASOUNDLIB_H

/* Minimal external API double for host CLI tests without ALSA development files. */
typedef struct snd_ctl {
	int unused;
} snd_ctl_t;
typedef struct snd_ctl_elem_id {
	int unused;
} snd_ctl_elem_id_t;
typedef struct snd_ctl_elem_value {
	int enabled;
} snd_ctl_elem_value_t;

#define SND_CTL_ELEM_IFACE_MIXER 2
#define snd_ctl_elem_id_alloca(pointer)           \
	do {                                      \
		static snd_ctl_elem_id_t test_id; \
		*(pointer) = &test_id;            \
	} while (0)
#define snd_ctl_elem_value_alloca(pointer)              \
	do {                                            \
		static snd_ctl_elem_value_t test_value; \
		*(pointer) = &test_value;               \
	} while (0)

int snd_ctl_open(snd_ctl_t **control, const char *name, int mode);
int snd_ctl_close(snd_ctl_t *control);
void snd_ctl_elem_id_set_interface(snd_ctl_elem_id_t *id, int interface);
void snd_ctl_elem_id_set_name(snd_ctl_elem_id_t *id, const char *name);
void snd_ctl_elem_value_set_id(snd_ctl_elem_value_t *value,
			       const snd_ctl_elem_id_t *id);
int snd_ctl_elem_read(snd_ctl_t *control, snd_ctl_elem_value_t *value);
int snd_ctl_elem_value_get_boolean(const snd_ctl_elem_value_t *value,
				   unsigned int index);
void snd_ctl_elem_value_set_boolean(snd_ctl_elem_value_t *value,
				    unsigned int index, long enabled);
int snd_ctl_elem_write(snd_ctl_t *control, snd_ctl_elem_value_t *value);
const char *snd_strerror(int error);

#endif
