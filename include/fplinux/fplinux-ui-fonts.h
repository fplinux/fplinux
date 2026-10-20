/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * The LVGL bitmap fonts compiled into libfplinux-ui; not installed and not
 * exported. While the package is built, scripts/fplinux_ui_fonts.py
 * rasterizes Noto Sans for exactly the font ids that the profile table of
 * fplinux-ui-profile.c names and generates fplinux-ui-fonts.c with this
 * function. The fonts are shared read-only data: no loader and no heap.
 */

#ifndef FPLINUX_UI_FONTS_H
#define FPLINUX_UI_FONTS_H

#include <lvgl/lvgl.h>

/*
 * The compiled-in font with a profile font id, such as
 * "noto-sans-semicondensed-12"; NULL for an unknown id.
 */
const lv_font_t *fplinux_ui_builtin_font(const char *id);

#endif
