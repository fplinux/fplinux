/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef FPLINUX_UI_MARQUEE_H
#define FPLINUX_UI_MARQUEE_H

#include "fplinux-ui-internal.h"

/*
 * A title's one marquee pass, else the focused
 * element's labels in turn. labels[turn] is the label that scrolls or
 * waits for its pass; armed while it is in LV_LABEL_LONG_MODE_SCROLL.
 */
struct fplinux_ui_marquee {
	lv_obj_t *title;
	lv_obj_t *labels[2];
	unsigned int count;
	unsigned int turn;
	bool armed;
	/* labels[turn] has been measured since it was armed. */
	bool checked;
	/* Labels that fit or have completed their one pass since focus. */
	unsigned int finished;
	/* A pass of the focused element has completed since then. */
	bool passed;
	/* Surface hiding deactivates screens without changing their selection. */
	bool hiding;
};

FPLINUX_UI_INTERNAL struct fplinux_ui_marquee *
fplinux_ui_marquee_state(struct fplinux_ui *ui);

FPLINUX_UI_INTERNAL void
fplinux_ui_marquee_watch_display(struct fplinux_ui *ui, lv_display_t *display);
FPLINUX_UI_INTERNAL void fplinux_ui_marquee_set_visible(struct fplinux_ui *ui,
							bool visible);
FPLINUX_UI_INTERNAL void fplinux_ui_marquee_restart(struct fplinux_ui *ui);

#endif
