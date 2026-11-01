/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_ACTIVITY_H
#define FPLINUX_ACTIVITY_H

#include <stddef.h>
#include <stdint.h>

#define FPLINUX_STATUS_ACTIVITY_PHASES 8U
#define FPLINUX_STATUS_ACTIVITY_STEP_MS 125U

struct fplinux_status_mask {
	unsigned int size;
	const uint8_t *alpha;
	const uint8_t *track;
};

/* A generated profile mask with static lifetime; unsupported sizes return NULL. */
const struct fplinux_status_mask *fplinux_activity_mask(unsigned int size,
							unsigned int width,
							unsigned int phase);

#endif
