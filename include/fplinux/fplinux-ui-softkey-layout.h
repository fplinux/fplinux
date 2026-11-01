/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Display-independent soft-key geometry; private, not installed. */
#ifndef FPLINUX_UI_SOFTKEY_LAYOUT_H
#define FPLINUX_UI_SOFTKEY_LAYOUT_H

struct fplinux_softkey_box {
	int x;
	int width;
};

/* Natural widths, narrowest first; long legends share the remaining room. */
__attribute__((visibility("hidden"))) void
fplinux_softkey_layout(int width, int margin, int gap, const int widths[3],
		       struct fplinux_softkey_box boxes[3]);

#endif
