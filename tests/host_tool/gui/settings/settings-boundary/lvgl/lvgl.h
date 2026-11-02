/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_SETTINGS_TEST_LVGL_H
#define FPLINUX_SETTINGS_TEST_LVGL_H

#include <stdint.h>

/* Host declarations for the external LVGL boundary; no rendering is tested. */
typedef struct lv_obj_t lv_obj_t;
typedef struct lv_font_t lv_font_t;
typedef struct lv_draw_buf_t lv_draw_buf_t;
typedef uint32_t lv_color_t;
typedef uint8_t lv_opa_t;
typedef struct lv_timer_t lv_timer_t;
typedef void (*lv_timer_cb_t)(lv_timer_t *timer);

lv_timer_t *lv_timer_create(lv_timer_cb_t callback, uint32_t period,
			    void *data);
void lv_timer_pause(lv_timer_t *timer);
void lv_timer_resume(lv_timer_t *timer);
void lv_timer_reset(lv_timer_t *timer);
void lv_timer_set_period(lv_timer_t *timer, uint32_t period);
void *lv_timer_get_user_data(lv_timer_t *timer);
void lv_timer_delete(lv_timer_t *timer);

#endif
