/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef UMS9117_CM4_FM_H
#define UMS9117_CM4_FM_H

struct device;
struct ums9117_fm;

#ifdef CONFIG_RADIO_UMS9117_CM4
struct ums9117_fm *ums9117_fm_register(struct device *dev);
void ums9117_fm_unregister(struct ums9117_fm *radio);
#else
static inline struct ums9117_fm *ums9117_fm_register(struct device *dev)
{
	return 0;
}

static inline void ums9117_fm_unregister(struct ums9117_fm *radio)
{
}
#endif

#endif
