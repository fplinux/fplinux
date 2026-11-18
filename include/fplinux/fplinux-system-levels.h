/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef FPLINUX_SYSTEM_LEVELS_H
#define FPLINUX_SYSTEM_LEVELS_H

#include <stdbool.h>

enum fplinux_system_level_kind {
	FPLINUX_SYSTEM_LEVEL_MEDIA,
	FPLINUX_SYSTEM_LEVEL_ALARM,
	FPLINUX_SYSTEM_LEVEL_BRIGHTNESS,
	FPLINUX_SYSTEM_LEVEL_COUNT,
};

enum fplinux_system_level_availability {
	FPLINUX_SYSTEM_LEVEL_UNKNOWN,
	FPLINUX_SYSTEM_LEVEL_READY,
	FPLINUX_SYSTEM_LEVEL_UNAVAILABLE,
};

/* Only READY gives value a meaning; pending never changes that value. */
struct fplinux_system_level_state {
	enum fplinux_system_level_availability availability;
	unsigned int minimum;
	unsigned int maximum;
	unsigned int value;
	bool pending;
};

struct fplinux_system_levels {
	struct fplinux_system_level_state level[FPLINUX_SYSTEM_LEVEL_COUNT];
};

#endif
