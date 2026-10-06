/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_ROTATE_ROTA_H
#define FPLINUX_ROTATE_ROTA_H

#include "fplinux-rotate.h"

struct fplinux_rotate_rota_timing {
	uint64_t setup_us;
	uint64_t copy_in_us;
	uint64_t queue_us;
	uint64_t wait_us;
	uint64_t copy_out_us;
	uint64_t guard_us;
	uint64_t teardown_us;
};

/* Each operation releases its device and mappings before returning. */
bool fplinux_rotate_rota_run(const char *requested_device,
			     const struct fplinux_rotate_transform *transform,
			     bool verify,
			     const struct fplinux_rotate_image *source,
			     struct fplinux_rotate_image *destination,
			     struct fplinux_rotate_rota_timing *timing);

#endif
