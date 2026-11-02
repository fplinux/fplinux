/* SPDX-License-Identifier: GPL-2.0-only */
#include "fplinux-activity.h"

struct fplinux_activity_entry {
	unsigned int width, phase;
	struct fplinux_status_mask mask;
};

#include "fplinux-activity-masks.h"

const struct fplinux_status_mask *
fplinux_activity_mask(unsigned int size, unsigned int width, unsigned int phase)
{
	phase %= FPLINUX_STATUS_ACTIVITY_PHASES;
	for (size_t i = 0;
	     i < sizeof(activity_masks) / sizeof(activity_masks[0]); ++i)
		if (activity_masks[i].mask.size == size &&
		    activity_masks[i].width == width &&
		    activity_masks[i].phase == phase)
			return &activity_masks[i].mask;
	return NULL;
}
