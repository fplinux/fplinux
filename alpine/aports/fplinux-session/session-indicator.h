/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_SESSION_INDICATOR_H
#define FPLINUX_SESSION_INDICATOR_H

#include <stdbool.h>

#include "fplinux-system-levels.h"

/* Borrowed presentation state; changing it never changes a system setting. */
struct fplinux_session_indicator {
	bool visible;
	bool softkeys;
	bool warning;
	enum fplinux_system_level_kind kind;
	struct fplinux_system_level_state level;
};

#endif
