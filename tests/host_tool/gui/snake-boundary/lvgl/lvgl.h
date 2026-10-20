/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_SNAKE_TEST_LVGL_H
#define FPLINUX_SNAKE_TEST_LVGL_H

/* A host double for drawing objects and timers, without an LVGL runtime. */
#include <stdbool.h>
#include <stdint.h>

typedef uint32_t lv_color_t;
typedef uint8_t lv_opa_t;
typedef int lv_border_side_t;
typedef struct lv_draw_buf lv_draw_buf_t;
typedef struct lv_font {
	int line_height;
} lv_font_t;
typedef struct lv_area {
	int x1;
	int y1;
	int x2;
	int y2;
} lv_area_t;
typedef struct lv_point {
	int x;
	int y;
} lv_point_t;
typedef struct lv_layer {
	int unused;
} lv_layer_t;
typedef struct lv_draw_rect_dsc {
	lv_color_t bg_color;
	lv_opa_t bg_opa;
} lv_draw_rect_dsc_t;
typedef struct lv_obj lv_obj_t;
typedef struct lv_event lv_event_t;
typedef struct lv_timer lv_timer_t;
typedef void (*lv_event_cb_t)(lv_event_t *event);
typedef void (*lv_timer_cb_t)(lv_timer_t *timer);

#define LV_OPA_COVER 255
#define LV_COORD_MAX 32767
#define LV_TEXT_FLAG_NONE 0
#define LV_TEXT_ALIGN_CENTER 0
#define LV_BORDER_SIDE_BOTTOM 1
#define LV_ALIGN_TOP_RIGHT 2
#define LV_EVENT_DRAW_MAIN 3

lv_obj_t *lv_obj_create(lv_obj_t *parent);
lv_obj_t *lv_label_create(lv_obj_t *parent);
void lv_obj_set_pos(lv_obj_t *object, int x, int y);
void lv_obj_set_size(lv_obj_t *object, int width, int height);
void lv_obj_set_width(lv_obj_t *object, int width);
void lv_obj_set_height(lv_obj_t *object, int height);
void lv_obj_align(lv_obj_t *object, int alignment, int x, int y);
void lv_obj_set_hidden(lv_obj_t *object, bool hidden);
void lv_obj_add_event_cb(lv_obj_t *object, lv_event_cb_t callback, int type,
			 void *context);
void lv_obj_get_coords(lv_obj_t *object, lv_area_t *area);
void lv_label_set_text(lv_obj_t *object, const char *text);
void *lv_event_get_user_data(lv_event_t *event);
lv_layer_t *lv_event_get_layer(lv_event_t *event);
void lv_draw_rect_dsc_init(lv_draw_rect_dsc_t *rectangle);
void lv_draw_rect(lv_layer_t *layer, const lv_draw_rect_dsc_t *rectangle,
		  const lv_area_t *area);
void lv_text_get_size(lv_point_t *size, const char *text, const lv_font_t *font,
		      int letter_space, int line_space, int width, int flags);
lv_timer_t *lv_timer_create(lv_timer_cb_t callback, unsigned int period,
			    void *context);
void lv_timer_pause(lv_timer_t *timer);
void lv_timer_resume(lv_timer_t *timer);
void lv_timer_reset(lv_timer_t *timer);
void lv_timer_delete(lv_timer_t *timer);
void *lv_timer_get_user_data(lv_timer_t *timer);

/* Styling is outside this double's geometry and listener boundary. */
#define lv_obj_remove_style_all(object) ((void)(object))
#define lv_obj_center(object) ((void)(object))
#define lv_obj_invalidate(object) ((void)(object))
#define lv_obj_set_scrollable(object, value) ((void)(object), (void)(value))
#define lv_obj_set_style_bg_color(object, value, selector) \
	((void)(object), (void)(value), (void)(selector))
#define lv_obj_set_style_bg_opa(object, value, selector) \
	((void)(object), (void)(value), (void)(selector))
#define lv_obj_set_style_border_color(object, value, selector) \
	((void)(object), (void)(value), (void)(selector))
#define lv_obj_set_style_border_width(object, value, selector) \
	((void)(object), (void)(value), (void)(selector))
#define lv_obj_set_style_border_side(object, value, selector) \
	((void)(object), (void)(value), (void)(selector))
#define lv_obj_set_style_text_font(object, value, selector) \
	((void)(object), (void)(value), (void)(selector))
#define lv_obj_set_style_text_color(object, value, selector) \
	((void)(object), (void)(value), (void)(selector))
#define lv_obj_set_style_pad_all(object, value, selector) \
	((void)(object), (void)(value), (void)(selector))
#define lv_obj_set_style_text_align(object, value, selector) \
	((void)(object), (void)(value), (void)(selector))

static inline int lv_font_get_line_height(const lv_font_t *font)
{
	return font->line_height;
}

#endif
