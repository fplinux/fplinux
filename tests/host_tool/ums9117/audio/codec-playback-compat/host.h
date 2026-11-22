/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_CODEC_PLAYBACK_HOST_H
#define FPLINUX_CODEC_PLAYBACK_HOST_H

#include <errno.h>
#include <stdlib.h>

#include <linux/types.h>

#define GFP_KERNEL 0
#define EPROBE_DEFER 517
#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))
#define FIELD_GET(mask, value) (((value) & (mask)) >> __builtin_ctzl(mask))
#define FIELD_PREP(mask, value) (((value) << __builtin_ctzl(mask)) & (mask))
#define FIELD_PREP_CONST(mask, value) \
	(((value) * ((mask) & (~(mask) + 1UL))) & (mask))
#define PTR_ERR(pointer) ((int)(intptr_t)(pointer))
#define ERR_CAST(pointer) ((void *)(pointer))
#define IS_ERR(pointer) ((uintptr_t)(pointer) >= (uintptr_t)-4095)

struct device {
	void *of_node;
	bool headphones_only;
	bool headphone_fed_speaker;
	const u32 *speaker_enable_delays_us;
	int speaker_enable_edges;
	u32 speaker_off_delay_us;
};

struct clk {
	unsigned int users;
};

struct regmap {
	u16 words[1024];
	unsigned int writes;
};

void *devm_kzalloc(struct device *dev, size_t size, int flags);
int devm_add_action_or_reset(struct device *dev, void (*action)(void *),
			     void *data);
struct clk *devm_clk_get(struct device *dev, const char *name);
int clk_prepare_enable(struct clk *clock);
void clk_disable_unprepare(struct clk *clock);
struct regmap *syscon_regmap_lookup_by_phandle(void *node, const char *name);
void udelay(unsigned int microseconds);

#endif
