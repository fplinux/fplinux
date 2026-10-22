/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_SETTINGS_OBSERVER_H
#define FPLINUX_SETTINGS_OBSERVER_H

#include <stdint.h>

struct settings_frame_observation {
	unsigned int frame;
	int theme;
	char locale[4];
	char labels[8192];
};

#endif
