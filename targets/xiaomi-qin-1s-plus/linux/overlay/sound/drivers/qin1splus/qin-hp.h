/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_QIN_HP_H
#define FPLINUX_QIN_HP_H

#include <linux/types.h>

#define QIN_HP_VOLUME_MAX 7

struct aw87339_profile;
extern const struct aw87339_profile qin_hp_speaker_profile;

struct qin_hp_io {
	void *context;
	int (*read)(void *context, unsigned int reg, u32 *value);
	int (*save)(void *context, unsigned int reg, u32 mask);
	int (*update)(void *context, unsigned int reg, u32 mask, u32 value);
	int (*command)(void *context, unsigned int reg, u32 mask, u32 value);
	int (*write)(void *context, unsigned int reg, u32 value);
	int (*pulse)(void *context, unsigned int reg, u32 mask,
		     unsigned int delay_us);
	void (*wait_ms)(void *context, unsigned int delay_ms);
	void (*irq)(void *context, bool enable);
};

struct qin_hp {
	bool active;
	bool irq_enabled;
	bool drivers_active;
	bool mixers_active;
	bool ready;
	bool fault;
};

/* The caller keeps its mutex, clocks and field ledger until stop succeeds. */
int qin_hp_prepare(struct qin_hp *hp, const struct qin_hp_io *io);
int qin_hp_stop(struct qin_hp *hp, const struct qin_hp_io *io);
int qin_hp_interrupt(struct qin_hp *hp, const struct qin_hp_io *io, u32 *cause,
		     u32 *status);
int qin_hp_merge_controls(unsigned int reg, u32 previous, u32 mask, u32 value,
			  u32 *word);
int qin_hp_mute(struct qin_hp *hp, const struct qin_hp_io *io);
int qin_hp_set_volume(struct qin_hp *hp, const struct qin_hp_io *io,
		      unsigned int volume);

#endif
