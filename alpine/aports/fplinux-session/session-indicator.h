/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_SESSION_INDICATOR_H
#define FPLINUX_SESSION_INDICATOR_H

#include <stdbool.h>

#include "fplinux-system-levels.h"
#include "fplinux-ui-session.h"

/* Borrowed presentation state; changing it never changes a system setting. */
struct fplinux_session_indicator {
	bool visible;
	bool softkeys;
	bool warning;
	enum fplinux_system_level_kind kind;
	struct fplinux_system_level_state level;
	bool wifi_russian;
	char wifi_name[FPLINUX_UI_WIFI_DISPLAY_BYTES];
};

#endif
