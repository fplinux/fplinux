/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_UMS9117_SC2720_CODEC_H
#define FPLINUX_UMS9117_SC2720_CODEC_H

#include <linux/types.h>

struct device;
struct ums9117_sc2720_codec;

#define UMS9117_SC2720_HEADPHONE_VOLUME_MAX 6U

/* The PCM owner serializes every operation and quiesces before devm release. */
struct ums9117_sc2720_codec *ums9117_sc2720_codec_create(struct device *dev);
void ums9117_sc2720_codec_get_volume(struct ums9117_sc2720_codec *codec,
				     unsigned int *left, unsigned int *right);
int ums9117_sc2720_codec_set_volume(struct ums9117_sc2720_codec *codec,
				    unsigned int left, unsigned int right);
int ums9117_sc2720_codec_prepare(struct ums9117_sc2720_codec *codec);
int ums9117_sc2720_codec_enable(struct ums9117_sc2720_codec *codec);
/* Stop retains calibration; callers must disable after any stop error. */
int ums9117_sc2720_codec_stop(struct ums9117_sc2720_codec *codec);
/* Restore the original PMIC state and release the shared clock lease. */
int ums9117_sc2720_codec_disable(struct ums9117_sc2720_codec *codec);

#endif
