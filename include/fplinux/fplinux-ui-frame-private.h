/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef FPLINUX_UI_FRAME_PRIVATE_H
#define FPLINUX_UI_FRAME_PRIVATE_H

#include <stdbool.h>

struct fplinux_ui;

/* UI-thread hook; pause borrowed timers before their UI is destroyed. */
void fplinux_ui_frames_set_visible(struct fplinux_ui *ui, bool visible);

#endif
