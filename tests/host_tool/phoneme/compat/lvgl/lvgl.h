/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_PHONEME_TEST_LVGL_H
#define FPLINUX_PHONEME_TEST_LVGL_H

#include <stdbool.h>
#include <stdint.h>

/* The app shell uses images and timers; widgets belong to the UI double. */
typedef uint32_t lv_color_t;
typedef uint8_t lv_opa_t;
typedef struct lv_font lv_font_t;
typedef struct lv_obj lv_obj_t;
typedef struct lv_event lv_event_t;
typedef struct lv_draw_buf lv_draw_buf_t;
typedef struct lv_area {
	int x1, y1, x2, y2;
} lv_area_t;
typedef struct lv_point {
	int x, y;
} lv_point_t;
typedef struct lv_timer lv_timer_t;
typedef void (*lv_timer_cb_t)(lv_timer_t *timer);
typedef struct {
	struct {
		unsigned int magic, cf, w, h, stride;
	} header;
	uint32_t data_size;
	const unsigned char *data;
} lv_image_dsc_t;

#define LV_IMAGE_HEADER_MAGIC 1
#define LV_COLOR_FORMAT_RAW_ALPHA 1
#define LV_COLOR_FORMAT_RGB565 2
#define LV_COLOR_FORMAT_RGB565A8 3

lv_timer_t *lv_timer_create(lv_timer_cb_t callback, uint32_t period,
			    void *context);
void lv_timer_pause(lv_timer_t *timer);
void lv_timer_resume(lv_timer_t *timer);
void lv_timer_delete(lv_timer_t *timer);
void lv_timer_ready(lv_timer_t *timer);
void lv_timer_reset(lv_timer_t *timer);
void lv_timer_set_period(lv_timer_t *timer, uint32_t period);
void *lv_timer_get_user_data(lv_timer_t *timer);
void lv_refr_now(void *display);

#endif
