/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Display-independent soft-key geometry; private, not installed. */
#ifndef FPLINUX_UI_SOFTKEY_LAYOUT_H
#define FPLINUX_UI_SOFTKEY_LAYOUT_H

#include <stddef.h>

struct fplinux_softkey_box {
	int x;
	int width;
};

/* Natural widths, narrowest first; long legends share the remaining room. */
__attribute__((visibility("hidden"))) void
fplinux_softkey_layout(int width, int margin, int gap, const int widths[3],
		       struct fplinux_softkey_box boxes[3]);

typedef int (*fplinux_softkey_measure_fn)(const char *text, void *context);

/*
 * UTF-8 prefix plus U+2026, without trailing spaces, or U+2026 alone even
 * when it is wider than width. Capacity retains snprintf's byte limit.
 * An empty legend stays empty.
 */
__attribute__((visibility("hidden"))) void
fplinux_softkey_fit_text(const char *text, int width,
			 fplinux_softkey_measure_fn measure, void *context,
			 char *result, size_t capacity);

#endif
