/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_PHONEME_TEST_LCDLF_EXPORT_H
#define FPLINUX_PHONEME_TEST_LCDLF_EXPORT_H

typedef struct java_graphics java_graphics;
typedef struct java_imagedata java_imagedata;

int lcdlf_ui_init(void);
void lcdlf_ui_finalize(void);
void lcdlf_refresh(int hardware_id, int x1, int y1, int x2, int y2);
int lcdlf_get_screen_width(int hardware_id);
int lcdlf_get_screen_height(int hardware_id);
void lcdlf_set_fullscreen_mode(int hardware_id, jboolean mode);

#endif
