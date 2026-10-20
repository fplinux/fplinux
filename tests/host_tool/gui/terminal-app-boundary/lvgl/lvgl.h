/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_TERMINAL_APP_TEST_LVGL_H
#define FPLINUX_TERMINAL_APP_TEST_LVGL_H

#include <stdbool.h>
#include <stdint.h>

/* Only the object, text-measurement and timer boundary is needed here. */
typedef uint32_t lv_color_t;
typedef uint8_t lv_opa_t;
typedef struct lv_font {
	int line_height;
} lv_font_t;
typedef struct lv_area {
	int x1, y1, x2, y2;
} lv_area_t;
typedef struct lv_point {
	int x, y;
} lv_point_t;
typedef struct lv_layer {
	int unused;
} lv_layer_t;
typedef struct lv_draw_buf lv_draw_buf_t;
typedef struct lv_obj lv_obj_t;
typedef struct lv_event lv_event_t;
typedef struct lv_timer lv_timer_t;
typedef void (*lv_event_cb_t)(lv_event_t *event);
typedef void (*lv_timer_cb_t)(lv_timer_t *timer);

#define LV_COORD_MAX 32767
#define LV_TEXT_FLAG_NONE 0
#define LV_EVENT_DRAW_MAIN 1

void lv_obj_set_scrollable(lv_obj_t *object, bool scrollable);
void lv_obj_add_event_cb(lv_obj_t *object, lv_event_cb_t callback, int type,
			 void *context);
void lv_obj_get_coords(lv_obj_t *object, lv_area_t *area);
void lv_obj_invalidate(lv_obj_t *object);
void *lv_event_get_user_data(lv_event_t *event);
lv_layer_t *lv_event_get_layer(lv_event_t *event);
void lv_text_get_size(lv_point_t *size, const char *text, const lv_font_t *font,
		      int letter_space, int line_space, int width, int flags);
lv_timer_t *lv_timer_create(lv_timer_cb_t callback, unsigned int period,
			    void *context);
void lv_timer_pause(lv_timer_t *timer);
void lv_timer_resume(lv_timer_t *timer);
void lv_timer_delete(lv_timer_t *timer);
void *lv_timer_get_user_data(lv_timer_t *timer);

#endif
