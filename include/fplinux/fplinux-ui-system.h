/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef FPLINUX_UI_SYSTEM_H
#define FPLINUX_UI_SYSTEM_H

#include <stdbool.h>

#include "fplinux-system-levels.h"

struct fplinux_ui;

/*
 * Reads the Session's last applied state. UNKNOWN and UNAVAILABLE are not
 * zero levels; only READY gives value a meaning. Returns false with EINVAL
 * for a NULL argument or invalid kind, EAGAIN before the first snapshot.
 * FPLINUX_UI_EVENT_SYSTEM_LEVELS runs after the cached snapshot is updated.
 */
bool fplinux_ui_system_level_get(struct fplinux_ui *ui,
				 enum fplinux_system_level_kind kind,
				 struct fplinux_system_level_state *state);

#endif
