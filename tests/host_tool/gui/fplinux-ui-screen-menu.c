/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The options menu of the high-level screens while the application changes
 * the screen under it. The harness links lib/fplinux/fplinux-ui-screen.c
 * with stand-ins for LVGL and the enclosing runtime:
 * LVGL objects retain geometry, text, visibility and title scroll state,
 * a list view keeps only its row count and focus, and the menu
 * records its items and callback. It opens Options with the left soft key,
 * changes the screen and checks what a choice fires. It also checks the
 * content sizes that screens report before and after a surface resize.
 */
#include <errno.h>
#include <libintl.h>
#include <limits.h>
#include <linux/input-event-codes.h>
#include <lvgl/src/libs/lodepng/lodepng.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fplinux-ui-internal.h"
#include "fplinux-ui-listview.h"
#include "fplinux-ui-locale.h"
#include "fplinux-ui-screen.h"
#include "fplinux-ui-session.h"
#include "fplinux-ui-text-model.h"

#define NONE ((size_t)-1)
#define MENU_ITEMS 8

#define CHECK(condition)                                                       \
	do {                                                                   \
		if (!(condition)) {                                            \
			fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, \
				__LINE__, #condition);                         \
			++failures;                                            \
		}                                                              \
	} while (0)

#define DELETE_ID 10
#define DETAILS_ID 11
#define REFRESH_ID 20
#define BACK_ID 30
#define OPEN_ID 40
#define SHARE_ID 41

/* Stand-ins: their parameters are the declarations', mostly unused. */
#pragma GCC diagnostic ignored "-Wunused-parameter"

struct fplinux_ui {
	struct fplinux_ui_screens *screens;
	lv_display_t *display;
	lv_obj_t *legend_bar;
	enum fplinux_ui_session_mode config_mode;
	bool fullscreen;
	unsigned int modals;
	int height;
	struct fplinux_ui_metrics metrics;
	char legends[3][FPLINUX_UI_SESSION_TEXT_BYTES];
};

struct fplinux_ui_listview {
	size_t count;
	size_t selected;
};

struct fplinux_ui_text {
	struct fplinux_ui_text_model model;
};

static int failures;
static struct fplinux_ui ui = {
	.height = 320,
	.metrics = { .softkey_height = 16 },
	.config_mode = FPLINUX_UI_SESSION_INTEGRATED,
};
static max_align_t system_storage;
struct object_geometry {
	lv_obj_t *parent;
	int width, height, x, y;
	int pad_right, pad_row;
	bool hidden, deleted, title_scrolling;
	const lv_font_t *font;
	const void *image_src;
	lv_label_long_mode_t long_mode;
	char text[256];
};
static struct object_geometry object_storage;
static struct object_geometry objects[512];
static size_t object_count;
static char local_legends[3][64];
static struct fplinux_ui_text text_field;
static const char *input_languages;
static int legend_advance;
static bool russian_labels;
static int strdup_fail_after = -1;
static bool fail_next_label;
/* The session legends sent by the screen. */
static struct {
	char legends[3][64];
} drawn;
static const lv_font_t header_font = { .line_height = 16 };
static const lv_font_t body_font = { .line_height = 12 };
static const struct fplinux_ui_theme theme = {
	.font = &body_font,
	.font_large = &header_font,
};
static const struct fplinux_ui_profile_spec spec = {
	.width = 240,
	.height = 320,
	.header_height = 28,
	.margin = 4,
	.spacing = 4,
	.scrollbar_space = 4,
	.row_height = 30,
	.row_height_detail = 44,
	.alert_margin = 8,
	.alert_padding = 6,
	.alert_border = 2,
	.legend_gap = 4,
	.legend_margin = 2,
};
static struct fplinux_ui_metrics metrics = {
	.screen_width = 240,
	.screen_height = 320,
};
/* The surface, which a mode switch resizes; the display stays. */
static int surface_width = 240;

/* The open menu, as fplinux_ui_menu_open() received it. */
static struct {
	bool open;
	enum fplinux_ui_menu_origin origin;
	bool titled;
	fplinux_ui_menu_cb callback;
	void *context;
	size_t count;
	int actions[MENU_ITEMS];
	char texts[MENU_ITEMS][32];
	bool separators[MENU_ITEMS];
	struct fplinux_ui_menu_position position;
} menu;

/* What the screens reported to the application. */
static struct fired {
	int commands;
	int command_id;
	int items;
	size_t item_index;
	int item_id;
	int changes;
	size_t changed_index;
	int accepts;
	size_t accepted_index;
} fired;

/* LVGL */

char *__real_strdup(const char *text);

char *__wrap_strdup(const char *text)
{
	if (!strdup_fail_after) {
		errno = ENOMEM;
		return NULL;
	}
	if (strdup_fail_after > 0)
		--strdup_fail_after;
	return __real_strdup(text);
}

#define NOTHING(name, ...)     \
	void name(__VA_ARGS__) \
	{                      \
	}
#define ZERO(type, name, ...)   \
	type name(__VA_ARGS__)  \
	{                       \
		return (type)0; \
	}
#define OBJECT(name, ...)                  \
	lv_obj_t *name(__VA_ARGS__)        \
	{                                  \
		return new_object(parent); \
	}

static lv_obj_t *new_object(lv_obj_t *parent)
{
	if (object_count == sizeof(objects) / sizeof(objects[0]))
		abort();
	objects[object_count].parent = parent;
	return (lv_obj_t *)&objects[object_count++];
}

static struct object_geometry *geometry(const lv_obj_t *object)
{
	return (struct object_geometry *)object;
}

OBJECT(lv_arc_create, lv_obj_t *parent)
OBJECT(lv_bar_create, lv_obj_t *parent)
OBJECT(lv_canvas_create, lv_obj_t *parent)
OBJECT(lv_image_create, lv_obj_t *parent)
lv_obj_t *lv_label_create(lv_obj_t *parent)
{
	if (fail_next_label) {
		fail_next_label = false;
		return NULL;
	}
	return new_object(parent);
}
OBJECT(lv_obj_create, lv_obj_t *parent)
lv_obj_t *lv_obj_get_child(const lv_obj_t *obj, int32_t index)
{
	return (lv_obj_t *)&object_storage;
}
ZERO(int32_t, lv_obj_get_index, const lv_obj_t *obj)
NOTHING(lv_arc_set_angles, lv_obj_t *obj, lv_value_precise_t start,
	lv_value_precise_t end)
NOTHING(lv_arc_set_bg_angles, lv_obj_t *obj, lv_value_precise_t start,
	lv_value_precise_t end)
NOTHING(lv_arc_set_rotation, lv_obj_t *obj, int32_t rotation)
ZERO(lv_result_t, lv_async_call, lv_async_cb_t callback, void *data)
ZERO(lv_result_t, lv_async_call_cancel, lv_async_cb_t callback, void *data)
NOTHING(lv_bar_set_range, lv_obj_t *obj, int32_t min, int32_t max)
NOTHING(lv_bar_set_value, lv_obj_t *obj, int32_t value, lv_anim_enable_t anim)
NOTHING(lv_canvas_finish_layer, lv_obj_t *canvas, lv_layer_t *layer)
NOTHING(lv_canvas_init_layer, lv_obj_t *obj, lv_layer_t *layer)
NOTHING(lv_canvas_set_draw_buf, lv_obj_t *obj, lv_draw_buf_t *buffer)
NOTHING(lv_draw_buf_clear, lv_draw_buf_t *buffer, const lv_area_t *area)
ZERO(lv_draw_buf_t *, lv_draw_buf_create, uint32_t w, uint32_t h,
     lv_color_format_t format, uint32_t stride)
NOTHING(lv_draw_buf_destroy, lv_draw_buf_t *buffer)
NOTHING(lv_draw_image, lv_layer_t *layer, const lv_draw_image_dsc_t *dsc,
	const lv_area_t *coords)
NOTHING(lv_draw_image_dsc_init, lv_draw_image_dsc_t *dsc)
ZERO(void *, lv_event_get_user_data, lv_event_t *event)
int32_t lv_font_get_line_height(const lv_font_t *font)
{
	return font ? font->line_height : 0;
}
ZERO(lv_result_t, lv_image_decoder_get_info, const void *src,
     lv_image_header_t *header)
void lv_image_set_src(lv_obj_t *obj, const void *src)
{
	geometry(obj)->image_src = src;
}

const void *lv_image_get_src(lv_obj_t *obj)
{
	return geometry(obj)->image_src;
}

NOTHING(lv_image_set_inner_align, lv_obj_t *obj, lv_image_align_t align)
ZERO(lv_image_src_t, lv_image_src_get_type, const void *src)
unsigned int lodepng_decode32(unsigned char **out, unsigned int *width,
			      unsigned int *height, const unsigned char *input,
			      size_t size)
{
	*out = NULL;
	return 1;
}
void lv_label_set_long_mode(lv_obj_t *obj, lv_label_long_mode_t mode)
{
	geometry(obj)->long_mode = mode;
}
NOTHING(lv_label_set_text_fmt, lv_obj_t *obj, const char *format, ...)
NOTHING(lv_label_set_text_selection_end, lv_obj_t *obj, uint32_t index)
NOTHING(lv_label_set_text_selection_start, lv_obj_t *obj, uint32_t index)
ZERO(lv_event_dsc_t *, lv_obj_add_event_cb, lv_obj_t *obj,
     lv_event_cb_t callback, lv_event_code_t filter, void *data)
void lv_obj_align(lv_obj_t *obj, lv_align_t align, int32_t x, int32_t y)
{
	struct object_geometry *object = geometry(obj);
	struct object_geometry *parent = geometry(object->parent);

	object->x = x;
	object->y = y;
	if (align == LV_ALIGN_LEFT_MID || align == LV_ALIGN_RIGHT_MID)
		object->y += (parent->height - object->height) / 2;
	if (align == LV_ALIGN_RIGHT_MID)
		object->x += parent->width - object->width;
}
NOTHING(lv_obj_center, lv_obj_t *obj)
NOTHING(lv_obj_clean, lv_obj_t *obj)
void lv_obj_delete(lv_obj_t *obj)
{
	geometry(obj)->deleted = true;
	geometry(obj)->hidden = true;
	for (size_t i = 0; i < object_count; ++i)
		if (objects[i].parent == obj && !objects[i].deleted)
			lv_obj_delete((lv_obj_t *)&objects[i]);
}
int32_t lv_obj_get_content_height(const lv_obj_t *obj)
{
	return geometry(obj)->height;
}
int32_t lv_obj_get_height(const lv_obj_t *obj)
{
	return geometry(obj)->height;
}
int32_t lv_obj_get_content_width(const lv_obj_t *obj)
{
	return geometry(obj)->width;
}
int32_t lv_obj_get_style_height(const lv_obj_t *obj, lv_part_t part)
{
	return geometry(obj)->height;
}
ZERO(int32_t, lv_obj_get_style_pad_bottom, const lv_obj_t *obj, lv_part_t part)
ZERO(int32_t, lv_obj_get_style_pad_top, const lv_obj_t *obj, lv_part_t part)
int32_t lv_obj_get_style_pad_right(const lv_obj_t *obj, lv_part_t part)
{
	return geometry(obj)->pad_right;
}
int32_t lv_obj_get_style_pad_row(const lv_obj_t *obj, lv_part_t part)
{
	return geometry(obj)->pad_row;
}
ZERO(int32_t, lv_obj_get_style_text_letter_space, const lv_obj_t *obj,
     lv_part_t part)
const lv_font_t *lv_obj_get_style_text_font(const lv_obj_t *obj, lv_part_t part)
{
	const struct object_geometry *object = geometry(obj);

	if (object->font || !object->parent)
		return object->font;
	return lv_obj_get_style_text_font(object->parent, part);
}
int32_t lv_obj_get_style_width(const lv_obj_t *obj, lv_part_t part)
{
	return geometry(obj)->width;
}
bool lv_obj_is_hidden(const lv_obj_t *obj)
{
	return geometry(obj)->hidden;
}
NOTHING(lv_obj_move_to_index, lv_obj_t *obj, int32_t index)
NOTHING(lv_obj_remove_style_all, lv_obj_t *obj)
NOTHING(lv_obj_scroll_by_bounded, lv_obj_t *obj, int32_t dx, int32_t dy,
	lv_anim_enable_t anim)
NOTHING(lv_obj_scroll_to_view, lv_obj_t *obj, lv_anim_enable_t anim)
NOTHING(lv_obj_set_clickable, lv_obj_t *obj, bool enable)
NOTHING(lv_obj_set_flex_align, lv_obj_t *obj, lv_flex_align_t main_place,
	lv_flex_align_t cross_place, lv_flex_align_t track_place)
NOTHING(lv_obj_set_flex_flow, lv_obj_t *obj, lv_flex_flow_t flow)
NOTHING(lv_obj_set_flex_grow, lv_obj_t *obj, uint8_t grow)
void lv_obj_set_height(lv_obj_t *obj, int32_t h)
{
	geometry(obj)->height = h;
}
NOTHING(lv_obj_set_parent, lv_obj_t *obj, lv_obj_t *parent)
void lv_obj_set_pos(lv_obj_t *obj, int32_t x, int32_t y)
{
	geometry(obj)->x = x;
	geometry(obj)->y = y;
}

void lv_obj_set_y(lv_obj_t *obj, int32_t y)
{
	geometry(obj)->y = y;
}
NOTHING(lv_obj_set_scrollable, lv_obj_t *obj, bool enable)
NOTHING(lv_obj_set_scrollbar_mode, lv_obj_t *obj, lv_scrollbar_mode_t mode)
NOTHING(lv_obj_set_scroll_dir, lv_obj_t *obj, lv_dir_t dir)
void lv_obj_set_size(lv_obj_t *obj, int32_t w, int32_t h)
{
	geometry(obj)->width = w;
	geometry(obj)->height = h;
}
NOTHING(lv_obj_set_style_arc_color, lv_obj_t *obj, lv_color_t value,
	lv_style_selector_t selector)
NOTHING(lv_obj_set_style_arc_width, lv_obj_t *obj, int32_t value,
	lv_style_selector_t selector)
NOTHING(lv_obj_set_style_bg_color, lv_obj_t *obj, lv_color_t value,
	lv_style_selector_t selector)
NOTHING(lv_obj_set_style_bg_opa, lv_obj_t *obj, lv_opa_t value,
	lv_style_selector_t selector)
NOTHING(lv_obj_set_style_border_color, lv_obj_t *obj, lv_color_t value,
	lv_style_selector_t selector)
NOTHING(lv_obj_set_style_border_opa, lv_obj_t *obj, lv_opa_t value,
	lv_style_selector_t selector)
NOTHING(lv_obj_set_style_border_side, lv_obj_t *obj, lv_border_side_t value,
	lv_style_selector_t selector)
NOTHING(lv_obj_set_style_border_width, lv_obj_t *obj, int32_t value,
	lv_style_selector_t selector)
NOTHING(lv_obj_set_style_image_recolor, lv_obj_t *obj, lv_color_t value,
	lv_style_selector_t selector)
NOTHING(lv_obj_set_style_max_height, lv_obj_t *obj, int32_t value,
	lv_style_selector_t selector)
NOTHING(lv_obj_set_style_max_width, lv_obj_t *obj, int32_t value,
	lv_style_selector_t selector)
void lv_obj_set_style_pad_all(lv_obj_t *obj, int32_t value,
			      lv_style_selector_t selector)
{
	geometry(obj)->pad_right = value;
}
NOTHING(lv_obj_set_style_pad_bottom, lv_obj_t *obj, int32_t value,
	lv_style_selector_t selector)
NOTHING(lv_obj_set_style_pad_column, lv_obj_t *obj, int32_t value,
	lv_style_selector_t selector)
void lv_obj_set_style_pad_hor(lv_obj_t *obj, int32_t value,
			      lv_style_selector_t selector)
{
	geometry(obj)->pad_right = value;
}
NOTHING(lv_obj_set_style_pad_left, lv_obj_t *obj, int32_t value,
	lv_style_selector_t selector)
void lv_obj_set_style_pad_right(lv_obj_t *obj, int32_t value,
				lv_style_selector_t selector)
{
	geometry(obj)->pad_right = value;
}
NOTHING(lv_obj_set_style_radius, lv_obj_t *obj, int32_t value,
	lv_style_selector_t selector)
void lv_obj_set_style_pad_row(lv_obj_t *obj, int32_t value,
			      lv_style_selector_t selector)
{
	geometry(obj)->pad_row = value;
}
NOTHING(lv_obj_set_style_pad_ver, lv_obj_t *obj, int32_t value,
	lv_style_selector_t selector)
NOTHING(lv_obj_set_style_text_align, lv_obj_t *obj, lv_text_align_t value,
	lv_style_selector_t selector)
NOTHING(lv_obj_set_style_text_color, lv_obj_t *obj, lv_color_t value,
	lv_style_selector_t selector)
NOTHING(lv_obj_set_style_text_decor, lv_obj_t *obj, lv_text_decor_t value,
	lv_style_selector_t selector)
void lv_obj_set_style_text_font(lv_obj_t *obj, const lv_font_t *value,
				lv_style_selector_t selector)
{
	geometry(obj)->font = value;
}
NOTHING(lv_obj_set_style_width, lv_obj_t *obj, int32_t value,
	lv_style_selector_t selector)
void lv_obj_set_width(lv_obj_t *obj, int32_t w)
{
	geometry(obj)->width = w;
}
NOTHING(lv_obj_update_layout, const lv_obj_t *obj)
ZERO(lv_timer_t *, lv_timer_create, lv_timer_cb_t callback, uint32_t period,
     void *data)
NOTHING(lv_timer_delete, lv_timer_t *timer)
ZERO(void *, lv_timer_get_user_data, lv_timer_t *timer)

int32_t lv_pct(int32_t x)
{
	return x;
}

void lv_label_set_text(lv_obj_t *obj, const char *text)
{
	snprintf(geometry(obj)->text, sizeof(geometry(obj)->text), "%s", text);
}

void lv_label_set_text_static(lv_obj_t *obj, const char *text)
{
	lv_label_set_text(obj, text);
}

char *lv_label_get_text(const lv_obj_t *obj)
{
	return geometry(obj)->text;
}

void lv_obj_set_hidden(lv_obj_t *obj, bool enable)
{
	geometry(obj)->hidden = enable;
}

NOTHING(lv_obj_scroll_to_y, lv_obj_t *obj, int32_t y, lv_anim_enable_t anim)

void lv_obj_get_coords(const lv_obj_t *obj, lv_area_t *coords)
{
	*coords = (lv_area_t){ 0 };
}

/* Controlled character advances, independent of the screen assignment rules. */
void lv_text_get_size(lv_point_t *size, const char *text, const lv_font_t *font,
		      int32_t letter_space, int32_t line_space,
		      int32_t max_width, lv_text_flag_t flag)
{
	*size = (lv_point_t){ 0 };
	for (; *text; ++text)
		if (((unsigned char)*text & 0xc0) != 0x80)
			size->x += font == &header_font ? 8 : legend_advance;
}

/* The runtime around the screens */

char *dgettext(const char *domain, const char *text)
{
	if (russian_labels) {
		if (!strcmp(text, "Edit"))
			return "Изменить";
		if (!strcmp(text, "Cancel"))
			return "Отмена";
	}
	return (char *)text;
}

ZERO(bool, fplinux_ui_back, struct fplinux_ui *owner)
ZERO(bool, fplinux_ui_format_date, const struct fplinux_ui *owner, time_t when,
     enum fplinux_ui_date_style style, char *out, size_t size)
ZERO(bool, fplinux_ui_scroll_key, lv_obj_t *object,
     const struct fplinux_ui_key *key)
NOTHING(fplinux_ui_marquee_blur, struct fplinux_ui *owner, lv_obj_t *label)
NOTHING(fplinux_ui_marquee_changed, struct fplinux_ui *owner, lv_obj_t *first,
	lv_obj_t *second)
NOTHING(fplinux_ui_marquee_focus, struct fplinux_ui *owner, lv_obj_t *first,
	lv_obj_t *second)
lv_obj_t *fplinux_ui_screen(struct fplinux_ui *owner)
{
	return (lv_obj_t *)&object_storage;
}
NOTHING(fplinux_ui_scrim_style, struct fplinux_ui *owner, lv_obj_t *root)
void fplinux_ui_title_hide(lv_obj_t *label)
{
	geometry(label)->title_scrolling = false;
}
void fplinux_ui_title_init(lv_obj_t *label, const lv_font_t *font)
{
	lv_obj_set_style_text_font(label, font, 0);
	lv_obj_set_height(label, lv_font_get_line_height(font));
	lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
}
void fplinux_ui_title_scroll(lv_obj_t *label)
{
	lv_point_t size;

	lv_text_get_size(&size, lv_label_get_text(label), geometry(label)->font,
			 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
	geometry(label)->title_scrolling = size.x > geometry(label)->width;
	lv_label_set_long_mode(label, geometry(label)->title_scrolling ?
					      LV_LABEL_LONG_MODE_SCROLL :
					      LV_LABEL_LONG_MODE_DOTS);
}
NOTHING(fplinux_ui_text_canvas_active, lv_obj_t *canvas, bool active)
bool fplinux_ui_text_is_empty(const struct fplinux_ui_text *field)
{
	return fplinux_ui_text_model_is_empty(&field->model);
}
bool fplinux_ui_text_is_picker_open(const struct fplinux_ui_text *field)
{
	return field->model.input.picker_open;
}
void fplinux_ui_text_cancel_picker(struct fplinux_ui_text *field)
{
	fplinux_ui_text_model_cancel_picker(&field->model);
}
ZERO(bool, fplinux_ui_text_is_read_only, const struct fplinux_ui_text *field)
bool fplinux_ui_text_key(struct fplinux_ui_text *field,
			 const struct fplinux_ui_key *key)
{
	struct fplinux_ui_text_model_key input = {
		.keyboard = key->source == FPLINUX_UI_KEY_KEYBOARD,
		.code = key->code,
		.pressed = key->pressed,
		.text = key->text,
	};

	return fplinux_ui_text_model_key(&field->model, &input, 1000) ==
	       FPLINUX_UI_TEXT_MODEL_CONSUMED;
}
lv_obj_t *fplinux_ui_text_object(struct fplinux_ui_text *field)
{
	return (lv_obj_t *)&object_storage;
}
bool fplinux_ui_text_set(struct fplinux_ui_text *field, const char *text)
{
	return fplinux_ui_text_model_set_text(&field->model, text);
}
NOTHING(fplinux_ui_text_set_change_cb, struct fplinux_ui_text *field,
	fplinux_ui_text_change_cb callback, void *context)
bool fplinux_ui_text_set_constraint(struct fplinux_ui_text *field,
				    enum fplinux_ui_text_constraint constraint)
{
	return true;
}
void fplinux_ui_text_set_focused(struct fplinux_ui_text *field, bool focused)
{
	fplinux_ui_text_model_set_focused(&field->model, focused);
}
ZERO(bool, fplinux_ui_text_takes_newline, const struct fplinux_ui_text *field)
NOTHING(fplinux_ui_listview_invalidate, struct fplinux_ui_listview *list,
	size_t first, size_t count)
ZERO(bool, fplinux_ui_listview_is_marked,
     const struct fplinux_ui_listview *list, size_t index)
ZERO(bool, fplinux_ui_listview_is_virtual,
     const struct fplinux_ui_listview *list)
ZERO(bool, fplinux_ui_listview_key, struct fplinux_ui_listview *list,
     const struct fplinux_ui_key *key)
lv_obj_t *fplinux_ui_listview_object(struct fplinux_ui_listview *list)
{
	return (lv_obj_t *)&object_storage;
}
lv_obj_t *fplinux_ui_listview_selected_row(struct fplinux_ui_listview *list)
{
	return (lv_obj_t *)&object_storage;
}
NOTHING(fplinux_ui_listview_set_active, struct fplinux_ui_listview *list,
	bool active)
NOTHING(fplinux_ui_listview_set_callbacks, struct fplinux_ui_listview *list,
	fplinux_ui_listview_cb select, fplinux_ui_listview_cb activate,
	void *context)
ZERO(bool, fplinux_ui_listview_set_icon, struct fplinux_ui_listview *list,
     size_t index, lv_draw_buf_t *icon, enum fplinux_ui_list_icon_kind kind)
ZERO(bool, fplinux_ui_listview_set_emphasis, struct fplinux_ui_listview *list,
     size_t index, bool emphasis)
ZERO(bool, fplinux_ui_listview_set_trailing_icon,
     struct fplinux_ui_listview *list, size_t index, const void *source)
NOTHING(fplinux_ui_listview_set_inline_detail, struct fplinux_ui_listview *list,
	bool inline_detail)
NOTHING(fplinux_ui_listview_set_marked, struct fplinux_ui_listview *list,
	size_t index, bool marked)
NOTHING(fplinux_ui_listview_set_selectable, struct fplinux_ui_listview *list,
	bool selectable)
ZERO(bool, fplinux_ui_listview_set_text, struct fplinux_ui_listview *list,
     size_t index, const char *text, const char *detail)
ZERO(struct fplinux_ui_listview *, fplinux_ui_listview_create_virtual,
     struct fplinux_ui *owner, lv_obj_t *parent,
     enum fplinux_ui_listview_marker marker, size_t count,
     fplinux_ui_listview_row_cb row, void *context)
NOTHING(fplinux_ui_listview_rows_deleted, struct fplinux_ui_listview *list,
	size_t index, size_t count)
NOTHING(fplinux_ui_listview_rows_inserted, struct fplinux_ui_listview *list,
	size_t index, size_t count)

struct fplinux_ui_screens **fplinux_ui_screens_slot(struct fplinux_ui *owner)
{
	return &owner->screens;
}

void fplinux_ui_cancel_key_repeat(struct fplinux_ui *owner,
				  const struct fplinux_ui_screen *canvas)
{
	(void)owner;
	(void)canvas;
}

const struct fplinux_ui_theme *fplinux_ui_theme(const struct fplinux_ui *owner)
{
	return &theme;
}

/* This command harness does not draw scroll indicators. */
void fplinux_ui_style_scrollbar(struct fplinux_ui *owner, lv_obj_t *object)
{
}

void fplinux_ui_scrollbar_layout(lv_obj_t *object)
{
}

/* Alert colours are observed by the native widget tests. */
lv_color_t fplinux_ui_alert_color(const struct fplinux_ui *owner,
				  enum fplinux_ui_alert_kind kind)
{
	return theme.focus;
}

const struct fplinux_ui_profile_spec *
fplinux_ui_spec(const struct fplinux_ui *owner)
{
	return &spec;
}

const struct fplinux_ui_metrics *
fplinux_ui_metrics(const struct fplinux_ui *owner)
{
	return &metrics;
}

int fplinux_ui_width(const struct fplinux_ui *owner)
{
	return surface_width;
}

int fplinux_ui_height(const struct fplinux_ui *owner)
{
	return owner->height;
}

lv_obj_t *lv_display_get_layer_sys(lv_display_t *display)
{
	return (lv_obj_t *)&system_storage;
}

lv_obj_t *fplinux_ui_softkey_bar_create(struct fplinux_ui *owner,
					lv_obj_t *parent)
{
	return new_object(parent);
}

bool fplinux_ui_softkey_bar_set(struct fplinux_ui *owner, lv_obj_t *bar,
				const char *left, const char *centre,
				const char *right)
{
	const char *legends[3] = { left, centre, right };

	for (int i = 0; i < 3; ++i)
		snprintf(local_legends[i], sizeof(local_legends[i]), "%s",
			 legends[i]);
	return true;
}

bool fplinux_ui_is_launcher(const struct fplinux_ui *owner)
{
	return false;
}

/* The screen component receives a fixed surface from its runtime stand-in. */
int fplinux_ui_modal_height(const struct fplinux_ui *owner)
{
	return owner->height;
}

NOTHING(fplinux_ui_modal_set, struct fplinux_ui *owner,
	enum fplinux_ui_modal source, bool up)

const char *fplinux_ui_locale(const struct fplinux_ui *owner)
{
	return "en";
}

bool fplinux_ui_is_visible(const struct fplinux_ui *owner)
{
	return true;
}

bool fplinux_ui_is_softkey(const struct fplinux_ui_key *key, bool left)
{
	return key->source == FPLINUX_UI_KEY_PHONE &&
	       key->code == (left ? KEY_F13 : KEY_F14);
}

bool fplinux_ui_set_softkeys(struct fplinux_ui *owner, const char *left,
			     const char *centre, const char *right)
{
	const char *legends[3] = { left, centre, right };

	for (int i = 0; i < 3; ++i)
		snprintf(drawn.legends[i], sizeof(drawn.legends[i]), "%s",
			 legends[i]);
	if (!menu.open) {
		for (int i = 0; i < 3; ++i)
			snprintf(owner->legends[i], sizeof(owner->legends[i]),
				 "%s", legends[i]);
	}
	return true;
}

struct fplinux_ui_text *fplinux_ui_text_create(struct fplinux_ui *owner,
					       lv_obj_t *parent, bool multiline,
					       size_t max_chars)
{
	CHECK(fplinux_ui_text_model_init(&text_field.model, multiline,
					 max_chars, input_languages, "en"));
	return &text_field;
}

const char *fplinux_ui_text_get(const struct fplinux_ui_text *field)
{
	return field->model.display;
}

/* A list view: its row count and focus, which follows its row. */

/* A list view goes with its object; these go with the process. */
struct fplinux_ui_listview *
fplinux_ui_listview_create(struct fplinux_ui *owner, lv_obj_t *parent,
			   enum fplinux_ui_listview_marker marker)
{
	struct fplinux_ui_listview *list = calloc(1, sizeof(*list));

	if (list)
		list->selected = NONE;
	return list;
}

size_t fplinux_ui_listview_count(const struct fplinux_ui_listview *list)
{
	return list->count;
}

size_t fplinux_ui_listview_selected(const struct fplinux_ui_listview *list)
{
	return list->selected;
}

size_t fplinux_ui_listview_first_visible(const struct fplinux_ui_listview *list)
{
	return list->count ? 0 : NONE;
}

void fplinux_ui_listview_select(struct fplinux_ui_listview *list, size_t index)
{
	if (index < list->count)
		list->selected = index;
}

size_t fplinux_ui_listview_insert(struct fplinux_ui_listview *list,
				  size_t index, const char *text,
				  const char *detail)
{
	if (index > list->count)
		return NONE;
	++list->count;
	if (list->selected == NONE)
		list->selected = 0;
	else if (list->selected >= index)
		++list->selected;
	return index;
}

size_t fplinux_ui_listview_add(struct fplinux_ui_listview *list,
			       const char *text, const char *detail)
{
	return fplinux_ui_listview_insert(list, list->count, text, detail);
}

bool fplinux_ui_listview_remove(struct fplinux_ui_listview *list, size_t index)
{
	if (index >= list->count)
		return false;
	--list->count;
	if (!list->count)
		list->selected = NONE;
	else if (list->selected > index || list->selected == list->count)
		--list->selected;
	return true;
}

void fplinux_ui_listview_clear(struct fplinux_ui_listview *list)
{
	list->count = 0;
	list->selected = NONE;
}

/* The options menu: fplinux-ui.c's contract, without a view. */

bool fplinux_ui_menu_open(struct fplinux_ui *owner,
			  enum fplinux_ui_menu_origin origin, const char *title,
			  const struct fplinux_ui_menu_item *items,
			  size_t count, fplinux_ui_menu_cb callback,
			  void *context)
{
	if (menu.open || count > MENU_ITEMS) {
		errno = EBUSY;
		return false;
	}
	menu.open = true;
	menu.origin = origin;
	menu.titled = title && title[0];
	menu.callback = callback;
	menu.context = context;
	menu.count = count;
	menu.position = (struct fplinux_ui_menu_position){ 0 };
	for (size_t i = 0; i < count; ++i) {
		menu.actions[i] = items[i].action;
		menu.separators[i] = items[i].separator_before;
		snprintf(menu.texts[i], sizeof(menu.texts[i]), "%s",
			 items[i].text);
	}
	return true;
}

bool fplinux_ui_menu_is_open(const struct fplinux_ui *owner)
{
	return menu.open;
}

void fplinux_ui_menu_set_position(
	struct fplinux_ui *owner,
	const struct fplinux_ui_menu_position *position)
{
	CHECK(menu.open);
	CHECK(position->selected < menu.count);
	menu.position = *position;
}

void fplinux_ui_menu_close(struct fplinux_ui *owner)
{
	if (!menu.open)
		return;
	menu.open = false;
	menu.callback = NULL;
	menu.context = NULL;
	fplinux_ui_screens_reclaim(owner->screens);
}

/* The user picks text: the menu closes, then reports, as finish_menu(). */
static void choose(const char *text)
{
	fplinux_ui_menu_cb callback = menu.callback;
	void *context = menu.context;

	CHECK(menu.open);
	for (size_t i = 0; menu.open && i < menu.count; ++i) {
		if (strcmp(menu.texts[i], text))
			continue;
		menu.position.selected = i;
		fplinux_ui_screens_menu_position(ui.screens, &menu.position);
		fplinux_ui_menu_close(&ui);
		callback(&ui, menu.actions[i], context);
		return;
	}
	CHECK(!"the menu offers text");
}

/* The application */

static void on_command(struct fplinux_ui *owner,
		       struct fplinux_ui_screen *screen, int id, void *context)
{
	++fired.commands;
	fired.command_id = id;
}

static void on_item(struct fplinux_ui *owner, struct fplinux_ui_screen *screen,
		    size_t index, int id, void *context)
{
	++fired.items;
	fired.item_index = index;
	fired.item_id = id;
}

static void on_change(struct fplinux_ui *owner,
		      struct fplinux_ui_screen *screen, size_t index,
		      void *context)
{
	++fired.changes;
	fired.changed_index = index;
}

static void on_edit(struct fplinux_ui *owner, struct fplinux_ui_screen *screen,
		    size_t index, void *context)
{
	++fired.accepts;
	fired.accepted_index = index;
	CHECK(!fplinux_ui_form_is_editing(screen));
	/* Acceptance is visible before the corresponding changed value. */
	CHECK(fired.changes == 0);
}

static const struct fplinux_ui_command delete_command = {
	.id = DELETE_ID,
	.type = FPLINUX_UI_COMMAND_ITEM,
	.label = "Delete",
};
static const struct fplinux_ui_command details_command = {
	.id = DETAILS_ID,
	.type = FPLINUX_UI_COMMAND_ITEM,
	.label = "Details",
};
static const struct fplinux_ui_command refresh_command = {
	.id = REFRESH_ID,
	.type = FPLINUX_UI_COMMAND_SCREEN,
	.label = "Refresh",
};
static const struct fplinux_ui_command back_command = {
	.id = BACK_ID,
	.type = FPLINUX_UI_COMMAND_BACK,
};

static void press_options(void)
{
	struct fplinux_ui_key key = {
		.source = FPLINUX_UI_KEY_PHONE,
		.code = KEY_F13,
		.pressed = true,
	};

	CHECK(fplinux_ui_screens_key(ui.screens, &key));
	key.pressed = false;
	CHECK(fplinux_ui_screens_key(ui.screens, &key));
	CHECK(menu.open);
	CHECK(menu.origin == FPLINUX_UI_MENU_ORIGIN_LEFT && !menu.titled);
	fired = (struct fired){ 0 };
}

/* Rows A, B, C; B has focus and Delete; the screen has Refresh and Back. */
static struct fplinux_ui_screen *open_list_options(void)
{
	struct fplinux_ui_screen *list =
		fplinux_ui_list_new(&ui, "Files", FPLINUX_UI_LIST_IMPLICIT);

	CHECK(list);
	fplinux_ui_list_append(list, "A", NULL);
	fplinux_ui_list_append(list, "B", NULL);
	fplinux_ui_list_append(list, "C", NULL);
	fplinux_ui_list_set_focused(list, 1);
	CHECK(fplinux_ui_list_add_item_command(list, 1, &delete_command));
	CHECK(fplinux_ui_screen_add_command(list, &refresh_command));
	CHECK(fplinux_ui_screen_add_command(list, &back_command));
	fplinux_ui_screen_set_listener(list, on_command, NULL, NULL);
	fplinux_ui_screen_set_item_listener(list, on_item, NULL);
	fplinux_ui_push(&ui, list);
	press_options();
	return list;
}

/* Each case starts from an empty stack, as fplinux_ui_close() leaves it. */
static void finish(void)
{
	fplinux_ui_menu_close(&ui);
	fplinux_ui_screens_delete(ui.screens);
	fplinux_ui_text_model_cleanup(&text_field.model);
	ui.screens = NULL;
	ui.modals = 0;
	ui.legend_bar = NULL;
	object_count = 0;
	memset(objects, 0, sizeof(objects));
	legend_advance = 0;
	strdup_fail_after = -1;
	fail_next_label = false;
	russian_labels = false;
	metrics.screen_width = 240;
	surface_width = 240;
}

struct navigation {
	bool plain_push;
	bool alert;
	bool replace_owner;
	size_t item;
};

static void navigate_command(struct fplinux_ui *owner,
			     struct fplinux_ui_screen *screen, int id,
			     void *context)
{
	struct navigation *navigation = context;
	struct fplinux_ui_screen *child;

	if (navigation->replace_owner) {
		struct fplinux_ui_screen *replacement =
			fplinux_ui_form_new(owner, "Replacement");

		CHECK(replacement);
		fplinux_ui_replace(owner, replacement);
	}
	child = navigation->alert ?
			fplinux_ui_alert_new(owner, FPLINUX_UI_ALERT_INFO,
					     "Help", "Navigation help", 0) :
			fplinux_ui_form_new(owner, "Child");
	CHECK(child);
	if (navigation->plain_push)
		fplinux_ui_push(owner, child);
	else
		fplinux_ui_navigate(owner, child);
}

static void navigate_item(struct fplinux_ui *owner,
			  struct fplinux_ui_screen *screen, size_t index,
			  int id, void *context)
{
	struct navigation *navigation = context;

	navigation->item = index;
	navigate_command(owner, screen, id, context);
}

static void test_navigation_restores_current_command_and_menu_position(void)
{
	struct navigation navigation = { 0 };
	struct fplinux_ui_screen *list = open_list_options();
	const struct fplinux_ui_command first = {
		.id = 400,
		.type = FPLINUX_UI_COMMAND_HELP,
		.label = "First",
		.priority = -1,
	};
	const struct fplinux_ui_command renamed = {
		.id = REFRESH_ID,
		.type = FPLINUX_UI_COMMAND_SCREEN,
		.label = "Updated",
		.priority = 2,
	};

	fplinux_ui_screen_set_listener(list, navigate_command, NULL,
				       &navigation);
	menu.position.scroll_y = 57;
	choose("Refresh");
	CHECK(fplinux_ui_current(&ui) != list && !menu.open);
	CHECK(fplinux_ui_screen_add_command(list, &first));
	CHECK(fplinux_ui_screen_add_command(list, &renamed));
	fplinux_ui_go_back(&ui);
	CHECK(fplinux_ui_current(&ui) == list && menu.open);
	CHECK(menu.count == 3 && menu.position.selected == 2);
	CHECK(!strcmp(menu.texts[2], "Updated"));
	CHECK(menu.position.scroll_y == 57);
	finish();
}

static void test_navigation_pop_and_plain_push_do_not_restore_options(void)
{
	struct navigation navigation = { 0 };
	struct fplinux_ui_screen *list = open_list_options();

	fplinux_ui_screen_set_listener(list, navigate_command, NULL,
				       &navigation);
	choose("Refresh");
	CHECK(fplinux_ui_pop(&ui));
	CHECK(fplinux_ui_current(&ui) == list && !menu.open);
	press_options();
	navigation.plain_push = true;
	choose("Refresh");
	fplinux_ui_go_back(&ui);
	CHECK(fplinux_ui_current(&ui) == list && !menu.open);
	/* A later navigate outside the command callback has no menu origin. */
	navigate_command(&ui, list, REFRESH_ID, &(struct navigation){ 0 });
	fplinux_ui_go_back(&ui);
	CHECK(fplinux_ui_current(&ui) == list && !menu.open);
	finish();
}

static void test_navigation_item_origin_follows_inserted_row(void)
{
	struct navigation navigation = { .item = NONE };
	struct fplinux_ui_screen *list = open_list_options();

	fplinux_ui_screen_set_item_listener(list, navigate_item, &navigation);
	choose("Delete");
	CHECK(navigation.item == 1);
	CHECK(fplinux_ui_list_insert(list, 0, "New", NULL) == 0);
	fplinux_ui_go_back(&ui);
	CHECK(menu.open &&
	      !strcmp(menu.texts[menu.position.selected], "Delete"));
	choose("Delete");
	CHECK(navigation.item == 2);
	finish();
}

static void
test_navigation_removed_command_and_replaced_owner_are_not_restored(void)
{
	struct navigation navigation = { 0 };
	struct fplinux_ui_screen *list = open_list_options();
	const struct fplinux_ui_command help = {
		.id = 400,
		.type = FPLINUX_UI_COMMAND_HELP,
	};

	fplinux_ui_screen_set_listener(list, navigate_command, NULL,
				       &navigation);
	choose("Refresh");
	CHECK(fplinux_ui_screen_add_command(list, &help));
	fplinux_ui_screen_remove_command(list, REFRESH_ID);
	fplinux_ui_go_back(&ui);
	CHECK(fplinux_ui_current(&ui) == list && !menu.open);
	press_options();
	navigation.replace_owner = true;
	choose("Help");
	fplinux_ui_go_back(&ui);
	CHECK(!menu.open && fplinux_ui_current(&ui) != list);
	finish();
}

static void test_only_navigated_informational_alert_close_restores_options(void)
{
	struct navigation navigation = { .alert = true };
	struct fplinux_ui_screen *list = open_list_options();
	struct fplinux_ui_key close = {
		.source = FPLINUX_UI_KEY_PHONE,
		.code = KEY_F14,
		.pressed = true,
	};

	fplinux_ui_screen_set_listener(list, navigate_command, NULL,
				       &navigation);
	choose("Refresh");
	CHECK(fplinux_ui_screens_key(ui.screens, &close));
	CHECK(fplinux_ui_current(&ui) == list && menu.open);
	CHECK(!strcmp(menu.texts[menu.position.selected], "Refresh"));
	navigation.plain_push = true;
	choose("Refresh");
	CHECK(fplinux_ui_screens_key(ui.screens, &close));
	CHECK(fplinux_ui_current(&ui) == list && !menu.open);
	finish();
}

/* A row inserted above: Delete fires for B at its new index. */
static void test_row_inserted_above(void)
{
	struct fplinux_ui_screen *list = open_list_options();

	CHECK(fplinux_ui_list_insert(list, 0, "New", NULL) == 0);
	CHECK(menu.open);
	choose("Delete");
	CHECK(fired.items == 1);
	CHECK(fired.item_index == 2 && fired.item_id == DELETE_ID);
	finish();
}

/* The focused row deleted: the menu closes; nothing fires for C. */
static void test_row_deleted(void)
{
	struct fplinux_ui_screen *list = open_list_options();

	CHECK(fplinux_ui_list_delete(list, 1));
	CHECK(!menu.open);
	CHECK(!fired.items && !fired.commands);
	finish();
}

/* A listed item command removed: the menu closes without a choice. */
static void test_item_command_removed(void)
{
	struct fplinux_ui_screen *list = open_list_options();

	fplinux_ui_screen_remove_item_command(list, 1, DELETE_ID);
	CHECK(!menu.open);
	CHECK(!fired.items && !fired.commands);
	finish();
}

/* A listed screen command removed: the menu closes without a choice. */
static void test_screen_command_removed(void)
{
	struct fplinux_ui_screen *list = open_list_options();

	fplinux_ui_screen_remove_command(list, REFRESH_ID);
	CHECK(!menu.open);
	CHECK(!fired.items && !fired.commands);
	finish();
}

/* Changes that leave every entry valid keep the menu and its choices. */
static void test_unrelated_changes_keep_menu(void)
{
	struct fplinux_ui_screen *list = open_list_options();

	fplinux_ui_list_append(list, "D", NULL);
	CHECK(fplinux_ui_list_delete(list, 3));
	CHECK(fplinux_ui_list_add_item_command(list, 2, &details_command));
	fplinux_ui_screen_remove_item_command(list, 2, DETAILS_ID);
	CHECK(fplinux_ui_screen_set_title(list, "Files 2"));
	fplinux_ui_list_set_focused(list, 0);
	CHECK(menu.open);
	choose("Delete");
	CHECK(fired.items == 1 && fired.item_index == 1);
	finish();

	list = open_list_options();
	CHECK(fplinux_ui_list_delete(list, 2));
	CHECK(menu.open);
	choose("Refresh");
	CHECK(fired.commands == 1 && fired.command_id == REFRESH_ID);
	CHECK(!fired.items);
	finish();
}

/* A form item that moves keeps its command; one that hides closes it. */
static void test_form_item_moved_or_hidden(void)
{
	struct fplinux_ui_screen *form = fplinux_ui_form_new(&ui, "Volume");

	CHECK(form);
	for (int i = 0; i < 3; ++i)
		CHECK(fplinux_ui_form_append_gauge(form, "Level", 0, 10, 5,
						   true) == (size_t)i);
	CHECK(fplinux_ui_form_add_item_command(form, 1, &delete_command));
	CHECK(fplinux_ui_screen_add_command(form, &refresh_command));
	fplinux_ui_screen_set_item_listener(form, on_item, NULL);
	fplinux_ui_push(&ui, form);
	fplinux_ui_form_set_focus(form, 1);
	press_options();

	CHECK(fplinux_ui_form_move_item(form, 1, 2));
	CHECK(menu.open);
	choose("Delete");
	CHECK(fired.items == 1 && fired.item_index == 2);

	fplinux_ui_form_set_focus(form, 2);
	press_options();
	CHECK(fplinux_ui_form_set_hidden(form, 2, true));
	CHECK(!menu.open);
	CHECK(!fired.items);
	finish();
}

/* The screen goes while its menu is open: the menu goes without a choice. */
static void test_screen_popped(void)
{
	struct fplinux_ui_screen *below =
		fplinux_ui_list_new(&ui, "Below", FPLINUX_UI_LIST_IMPLICIT);

	CHECK(below);
	fplinux_ui_push(&ui, below);
	open_list_options();
	CHECK(fplinux_ui_pop(&ui));
	CHECK(!menu.open);
	CHECK(!fired.items && !fired.commands);
	finish();
}

/*
 * The application closed Options with fplinux_ui_menu_close() and opened a
 * menu of its own: later screen changes leave that menu alone.
 */
static void application_chosen(struct fplinux_ui *owner, int action,
			       void *context)
{
}

static void test_application_menu_kept(void)
{
	static const struct fplinux_ui_menu_item own = { .text = "Own" };
	struct fplinux_ui_screen *list = open_list_options();

	fplinux_ui_menu_close(&ui);
	CHECK(fplinux_ui_menu_open(&ui, FPLINUX_UI_MENU_ORIGIN_RIGHT, "Own",
				   &own, 1, application_chosen, NULL));
	fplinux_ui_screen_remove_item_command(list, 1, DELETE_ID);
	CHECK(fplinux_ui_list_delete(list, 0));
	CHECK(menu.open && menu.callback == application_chosen);
	finish();
}

/*
 * fplinux_ui_screen_get_content_size() follows the surface: below the title
 * band, the whole surface without one, an alert's box inside its margins,
 * border and padding; a resize with a menu open keeps the
 * menu.
 */
static void test_content_size(void)
{
	struct fplinux_ui_screen *list = open_list_options();
	struct fplinux_ui_screen *canvas = fplinux_ui_canvas_new(&ui, NULL);
	struct fplinux_ui_screen *alert = fplinux_ui_alert_new(
		&ui, FPLINUX_UI_ALERT_INFO, "Note", "Text", 0);
	int width = -1;
	int height = -1;

	CHECK(canvas && alert);
	CHECK(fplinux_ui_screen_get_content_size(list, &width, &height));
	CHECK(width == 240 && height == 320 - 28);
	CHECK(fplinux_ui_screen_get_content_size(canvas, &width, &height));
	CHECK(width == 240 && height == 320);
	CHECK(fplinux_ui_screen_get_content_size(alert, &width, &height));
	CHECK(width == 240 - 16 - 16 && height == 320 - 16 - 16);
	ui.height = 268;
	fplinux_ui_screens_resize(ui.screens);
	CHECK(menu.open);
	CHECK(fplinux_ui_screen_get_content_size(list, &width, &height));
	CHECK(width == 240 && height == 268 - 28);
	CHECK(fplinux_ui_screen_get_content_size(canvas, &width, &height));
	CHECK(width == 240 && height == 268);
	CHECK(fplinux_ui_screen_get_content_size(alert, &width, &height));
	CHECK(width == 240 - 16 - 16 && height == 268 - 16 - 16);
	errno = 0;
	CHECK(!fplinux_ui_screen_get_content_size(NULL, &width, &height));
	CHECK(errno == EINVAL);
	errno = 0;
	CHECK(!fplinux_ui_screen_get_content_size(list, NULL, &height));
	CHECK(errno == EINVAL);
	errno = 0;
	CHECK(!fplinux_ui_screen_get_content_size(list, &width, NULL));
	CHECK(errno == EINVAL);
	ui.height = 320;
	finish();
}

/* Text and geometry are observed through the Canvas's public content object. */
static lv_obj_t *find_header_text(struct fplinux_ui_screen *canvas,
				  const char *text)
{
	lv_obj_t *root = geometry(fplinux_ui_canvas_object(canvas))->parent;

	for (size_t i = 0; i < object_count; ++i) {
		const struct object_geometry *object = &objects[i];

		if (!object->deleted && object->parent &&
		    geometry(object->parent)->parent == root &&
		    !strcmp(object->text, text))
			return (lv_obj_t *)object;
	}
	return NULL;
}

static lv_obj_t *header_text(struct fplinux_ui_screen *canvas, const char *text)
{
	lv_obj_t *label = find_header_text(canvas, text);

	CHECK(label);
	return label ? label : (lv_obj_t *)&object_storage;
}

static void test_header_status_fits_resizes_and_clears(void)
{
	struct fplinux_ui_screen *canvas =
		fplinux_ui_canvas_new(&ui, "Terminal");
	char full[] = "Ctrl+Alt+Shift  abc";
	char compact[] = "C+A+S  abc";
	lv_obj_t *title;
	lv_obj_t *status;
	lv_obj_t *content;
	int width, height;

	CHECK(canvas);
	legend_advance = 6;
	CHECK(fplinux_ui_screen_set_header_status(canvas, full, compact));
	full[0] = compact[0] = '?';
	fplinux_ui_push(&ui, canvas);
	content = fplinux_ui_canvas_object(canvas);
	title = header_text(canvas, "Terminal");
	status = header_text(canvas, "Ctrl+Alt+Shift  abc");
	CHECK(geometry(content)->y == 28 && geometry(content)->height == 292);
	CHECK(geometry(title)->x == 4 && geometry(title)->width == 114);
	CHECK(geometry(status)->x == 122 && geometry(status)->width == 114);
	CHECK(geometry(status)->height == 12 && geometry(status)->y == 8);
	CHECK(geometry(status)->long_mode == LV_LABEL_LONG_MODE_CLIP);
	CHECK(geometry(title)->long_mode == LV_LABEL_LONG_MODE_DOTS);
	CHECK(!geometry(title)->title_scrolling);
	CHECK(fplinux_ui_screen_get_content_size(canvas, &width, &height));
	CHECK(width == 240 && height == 292);

	surface_width = 128;
	fplinux_ui_screens_resize(ui.screens);
	CHECK(!strcmp(lv_label_get_text(status), "C+A+S  abc"));
	CHECK(geometry(title)->width == 56);
	CHECK(geometry(status)->x == 64 && geometry(status)->width == 60);
	CHECK(geometry(title)->x + geometry(title)->width + 4 ==
	      geometry(status)->x);
	CHECK(geometry(status)->x + geometry(status)->width == 124);
	CHECK(fplinux_ui_screen_get_content_size(canvas, &width, &height));
	CHECK(width == 128 && height == 292);
	CHECK(geometry(content)->y == 28 && geometry(content)->height == 292);

	surface_width = 240;
	fplinux_ui_screens_resize(ui.screens);
	CHECK(!strcmp(lv_label_get_text(status), "Ctrl+Alt+Shift  abc"));
	CHECK(fplinux_ui_screen_set_title(
		canvas, "An ordinary long title that needs more room"));
	CHECK(!strcmp(lv_label_get_text(status), "C+A+S  abc"));
	CHECK(!geometry(title)->title_scrolling);
	CHECK(fplinux_ui_screen_set_header_status(canvas, "", NULL));
	CHECK(!find_header_text(canvas, "C+A+S  abc"));
	CHECK(geometry(title)->width == 232);
	CHECK(geometry(title)->title_scrolling);
	CHECK(geometry(content)->y == 28 && geometry(content)->height == 292);

	CHECK(fplinux_ui_screen_set_header_status(canvas, "Ctrl", ""));
	CHECK(geometry(title)->width == 232);
	CHECK(!geometry(title)->title_scrolling);
	CHECK(fplinux_ui_screen_set_header_status(canvas, NULL, "ignored"));
	CHECK(geometry(title)->width == 232);
	CHECK(geometry(title)->title_scrolling);
	finish();
}

static void test_header_status_hidden_parent_keeps_child_header(void)
{
	const char *long_title = "An ordinary long title that needs more room";
	struct fplinux_ui_screen *parent;
	struct fplinux_ui_screen *child;
	lv_obj_t *title;
	lv_obj_t *status;
	lv_obj_t *child_title;
	lv_obj_t *parent_root;
	lv_obj_t *child_root;

	legend_advance = 6;
	surface_width = 128;
	parent = fplinux_ui_canvas_new(&ui, long_title);
	child = fplinux_ui_canvas_new(&ui, "Child");
	CHECK(parent && child);
	fplinux_ui_push(&ui, parent);
	title = header_text(parent, long_title);
	CHECK(geometry(title)->title_scrolling);
	CHECK(fplinux_ui_screen_set_header_status(parent, "Ctrl+Alt+Shift  abc",
						  "C+A+S  abc"));
	status = header_text(parent, "C+A+S  abc");
	CHECK(!geometry(title)->title_scrolling);
	fplinux_ui_push(&ui, child);
	child_title = header_text(child, "Child");
	parent_root = geometry(fplinux_ui_canvas_object(parent))->parent;
	child_root = geometry(fplinux_ui_canvas_object(child))->parent;
	CHECK(geometry(parent_root)->hidden && !geometry(child_root)->hidden);
	CHECK(fplinux_ui_screen_set_title(parent, "Terminal"));
	CHECK(fplinux_ui_current(&ui) == child);
	CHECK(!strcmp(lv_label_get_text(child_title), "Child"));
	CHECK(!strcmp(lv_label_get_text(status), "C+A+S  abc"));
	surface_width = 240;
	fplinux_ui_screens_resize(ui.screens);
	CHECK(!strcmp(lv_label_get_text(status), "Ctrl+Alt+Shift  abc"));
	CHECK(geometry(child_title)->width == 232);
	CHECK(geometry(parent_root)->hidden && !geometry(child_root)->hidden);
	CHECK(fplinux_ui_screen_set_title(parent, long_title));
	CHECK(!strcmp(lv_label_get_text(status), "C+A+S  abc"));
	CHECK(!strcmp(lv_label_get_text(child_title), "Child"));
	CHECK(fplinux_ui_pop(&ui));
	CHECK(fplinux_ui_current(&ui) == parent);
	CHECK(!geometry(parent_root)->hidden);
	CHECK(!geometry(title)->title_scrolling);
	CHECK(geometry(title)->long_mode == LV_LABEL_LONG_MODE_DOTS);
	CHECK(fplinux_ui_screen_set_header_status(parent, NULL, NULL));
	CHECK(geometry(title)->title_scrolling);
	finish();
}

static void test_header_status_without_title_and_allocation_failures(void)
{
	struct fplinux_ui_screen *canvas = fplinux_ui_canvas_new(&ui, NULL);
	struct fplinux_ui_screen *alert = fplinux_ui_alert_new(
		&ui, FPLINUX_UI_ALERT_INFO, "Note", "Text", 0);
	lv_obj_t *content;
	lv_obj_t *title;
	lv_obj_t *status;
	int width, height;

	CHECK(canvas && alert);
	legend_advance = 6;
	content = fplinux_ui_canvas_object(canvas);
	CHECK(fplinux_ui_screen_set_header_status(canvas, "Ready", "R"));
	status = header_text(canvas, "Ready");
	CHECK(geometry(geometry(status)->parent)->hidden);
	CHECK(fplinux_ui_screen_get_content_size(canvas, &width, &height));
	CHECK(width == 240 && height == 320);
	CHECK(geometry(content)->y == 0 && geometry(content)->height == 320);
	CHECK(fplinux_ui_screen_set_title(canvas, "Shell"));
	fplinux_ui_push(&ui, canvas);
	title = header_text(canvas, "Shell");
	CHECK(!geometry(geometry(status)->parent)->hidden);
	CHECK(geometry(title)->width == 198);

	for (int fail_after = 0; fail_after <= 1; ++fail_after) {
		strdup_fail_after = fail_after;
		errno = 0;
		CHECK(!fplinux_ui_screen_set_header_status(canvas, "Changed",
							   "C"));
		CHECK(errno == ENOMEM);
		CHECK(!strcmp(lv_label_get_text(status), "Ready"));
		CHECK(geometry(title)->width == 198);
	}
	strdup_fail_after = 0;
	CHECK(fplinux_ui_screen_set_header_status(canvas, "Ready", "R"));
	CHECK(fplinux_ui_screen_set_title(canvas, "Shell"));
	strdup_fail_after = -1;
	CHECK(fplinux_ui_screen_set_header_status(canvas, "Ready", NULL));
	strdup_fail_after = 0;
	CHECK(fplinux_ui_screen_set_header_status(canvas, "Ready", "Ready"));
	CHECK(fplinux_ui_screen_set_header_status(canvas, NULL, NULL));
	CHECK(geometry(title)->width == 232);
	CHECK(!find_header_text(canvas, "Ready"));
	strdup_fail_after = -1;
	fail_next_label = true;
	errno = 0;
	CHECK(!fplinux_ui_screen_set_header_status(canvas, "New", NULL));
	CHECK(errno == ENOMEM && geometry(title)->width == 232);
	CHECK(!find_header_text(canvas, "New"));
	CHECK(fplinux_ui_screen_set_title(canvas, NULL));
	strdup_fail_after = 0;
	CHECK(fplinux_ui_screen_set_title(canvas, NULL));
	CHECK(fplinux_ui_screen_set_header_status(canvas, "", "ignored"));
	CHECK(geometry(content)->y == 0 && geometry(content)->height == 320);
	errno = 0;
	CHECK(!fplinux_ui_screen_set_header_status(NULL, "Ready", NULL));
	CHECK(errno == EINVAL);
	errno = 0;
	CHECK(!fplinux_ui_screen_set_header_status(alert, "Ready", NULL));
	CHECK(errno == EINVAL);
	errno = 0;
	CHECK(!fplinux_ui_screen_set_header_status(alert, NULL, NULL));
	CHECK(errno == EINVAL);
	finish();
}

static void press(unsigned int code)
{
	struct fplinux_ui_key key = {
		.source = FPLINUX_UI_KEY_PHONE,
		.code = code,
		.pressed = true,
	};

	CHECK(fplinux_ui_screens_key(ui.screens, &key));
	key.pressed = false;
	CHECK(fplinux_ui_screens_key(ui.screens, &key));
}

static bool legends_are(const char *left, const char *centre, const char *right)
{
	return !strcmp(drawn.legends[0], left) &&
	       !strcmp(drawn.legends[1], centre) &&
	       !strcmp(drawn.legends[2], right);
}

/* Runtime image resources are outside this command/callback harness. */
const void *fplinux_ui_glyph(struct fplinux_ui *owner, const char *name,
			     enum fplinux_ui_glyph_size size)
{
	errno = ENOENT;
	return NULL;
}

static void test_options_keeps_back_last_and_delivers_visible_actions(void)
{
	const struct fplinux_ui_command commands[] = {
		{ .id = 30, .type = FPLINUX_UI_COMMAND_BACK, .priority = -50 },
		{ .id = 31,
		  .type = FPLINUX_UI_COMMAND_CANCEL,
		  .priority = -40,
		  .separator_before = true },
		{ .id = 32, .type = FPLINUX_UI_COMMAND_EXIT, .priority = -30 },
		{ .id = 33, .type = FPLINUX_UI_COMMAND_STOP, .priority = -20 },
		{ .id = 20,
		  .type = FPLINUX_UI_COMMAND_SCREEN,
		  .label = "Refresh",
		  .short_label = "R",
		  .separator_before = true },
		{ .id = 21,
		  .type = FPLINUX_UI_COMMAND_SCREEN,
		  .label = "Details" },
		{ .id = 34,
		  .type = FPLINUX_UI_COMMAND_HELP,
		  .priority = 1,
		  .separator_before = true },
	};
	const char *const expected[] = {
		"Cancel", "Exit", "Stop", "Refresh", "Details", "Help", "Back",
	};
	const int ids[] = { 31, 32, 33, 20, 21, 34, 30 };
	const bool separators[] = {
		true, false, false, true, false, true, false
	};
	struct fplinux_ui_screen *screen =
		fplinux_ui_text_box_new(&ui, "Name", "abcdef", 32);

	CHECK(screen);
	for (size_t index = 0; index < sizeof(commands) / sizeof(commands[0]);
	     ++index)
		CHECK(fplinux_ui_screen_add_command(screen, &commands[index]));
	fplinux_ui_screen_set_listener(screen, on_command, NULL, NULL);
	fplinux_ui_push(&ui, screen);
	CHECK(legends_are("Options", "", "Clear"));
	for (size_t choice = 0; choice < sizeof(ids) / sizeof(ids[0]);
	     ++choice) {
		press_options();
		CHECK(menu.count == sizeof(expected) / sizeof(expected[0]));
		for (size_t row = 0; row < menu.count; ++row) {
			CHECK(!strcmp(menu.texts[row], expected[row]));
			CHECK(menu.separators[row] == separators[row]);
		}
		choose(expected[choice]);
		CHECK(fired.commands == 1 && fired.command_id == ids[choice]);
		CHECK(!strcmp(fplinux_ui_text_box_get(screen), "abcdef"));
	}
	press(KEY_F14);
	CHECK(!strcmp(fplinux_ui_text_box_get(screen), "abcde"));
	CHECK(fired.commands == 1);
	finish();
}

static void test_options_distinguishes_synthetic_clear_from_back(bool back)
{
	const struct fplinux_ui_command cancel = {
		.id = 31,
		.type = FPLINUX_UI_COMMAND_CANCEL,
		.keep_visible = true,
		.priority = -100,
	};
	const struct fplinux_ui_command help = {
		.id = 34,
		.type = FPLINUX_UI_COMMAND_HELP,
		.priority = 1,
	};
	struct fplinux_ui_command early_back = back_command;
	struct fplinux_ui_screen *screen =
		fplinux_ui_text_box_new(&ui, "Name", "abcdef", 32);

	early_back.priority = -50;
	CHECK(screen);
	CHECK(fplinux_ui_screen_add_command(screen, &cancel));
	if (back)
		CHECK(fplinux_ui_screen_add_command(screen, &early_back));
	CHECK(fplinux_ui_screen_add_command(screen, &refresh_command));
	CHECK(fplinux_ui_screen_add_command(screen, &help));
	fplinux_ui_screen_set_listener(screen, on_command, NULL, NULL);
	fplinux_ui_push(&ui, screen);
	CHECK(legends_are("Options", "", "Cancel"));
	press_options();
	CHECK(menu.count == (back ? 4 : 3));
	CHECK(!strcmp(menu.texts[0], "Refresh"));
	CHECK(!strcmp(menu.texts[1], "Help"));
	CHECK(!strcmp(menu.texts[2], "Clear"));
	if (back)
		CHECK(!strcmp(menu.texts[3], "Back"));
	choose("Clear");
	CHECK(!fired.commands);
	CHECK(!strcmp(fplinux_ui_text_box_get(screen), "abcde"));
	press(KEY_F14);
	CHECK(fired.commands == 1 && fired.command_id == 31);
	CHECK(!strcmp(fplinux_ui_text_box_get(screen), "abcde"));
	finish();
}

static bool has_text(const char *text)
{
	for (size_t i = 0; i < object_count; ++i)
		if (!strcmp(objects[i].text, text))
			return true;
	return false;
}

static void test_duration_commits_or_cancels_without_changing_form_focus(void)
{
	struct fplinux_ui_screen *form = fplinux_ui_form_new(&ui, "Timer");
	size_t duration =
		fplinux_ui_form_append_duration(form, "Duration", 300);
	size_t gauge =
		fplinux_ui_form_append_gauge(form, "Level", 0, 10, 5, true);

	CHECK(form && duration == 0 && gauge == 1);
	fplinux_ui_screen_set_listener(form, NULL, on_change, NULL);
	fplinux_ui_form_set_edit_listener(form, on_edit, NULL);
	fplinux_ui_push(&ui, form);
	fired = (struct fired){ 0 };
	CHECK(!fplinux_ui_form_is_editing(form));
	CHECK(has_text("00:05:00"));
	CHECK(legends_are("", "Edit", ""));
	press(KEY_OK);
	CHECK(fplinux_ui_form_is_editing(form));
	CHECK(legends_are("", "Done", "Cancel"));
	press(KEY_RIGHT);
	press(KEY_UP);
	CHECK(fplinux_ui_form_get_focus(form) == duration);
	CHECK(fplinux_ui_form_get_duration(form, duration) == 300);
	CHECK(fired.changes == 0);
	press(KEY_F14);
	CHECK(!fplinux_ui_form_is_editing(form));
	CHECK(fplinux_ui_form_get_duration(form, duration) == 300);
	CHECK(fired.changes == 0 && fired.accepts == 0 && has_text("00:05:00"));
	press(KEY_OK);
	press(KEY_RIGHT);
	press(KEY_UP);
	press(KEY_RIGHT);
	press(KEY_UP);
	press(KEY_OK);
	CHECK(fplinux_ui_form_get_duration(form, duration) == 361);
	CHECK(fired.changes == 1 && fired.changed_index == duration);
	CHECK(fired.accepts == 1 && fired.accepted_index == duration);
	CHECK(has_text("00:06:01"));
	press(KEY_DOWN);
	CHECK(fplinux_ui_form_get_focus(form) == gauge);
	CHECK(fplinux_ui_form_set_duration(form, duration, 359999));
	CHECK(has_text("99:59:59"));
	errno = 0;
	CHECK(!fplinux_ui_form_set_duration(form, duration, 360000));
	CHECK(errno == ERANGE);
	CHECK(fplinux_ui_form_get_duration(form, duration) == 359999);
	errno = 0;
	CHECK(fplinux_ui_form_append_duration(form, "Too long", 360000) ==
	      NONE);
	CHECK(errno == ERANGE);
	errno = 0;
	CHECK(fplinux_ui_form_get_duration(form, gauge) == UINT_MAX);
	CHECK(errno == EINVAL);
	finish();
}

static void test_unchanged_date_done_reports_acceptance_without_change(void)
{
	struct fplinux_ui_screen *form = fplinux_ui_form_new(&ui, "Date");
	size_t date =
		fplinux_ui_form_append_date(form, "Date", FPLINUX_UI_DATE, 0);

	CHECK(form && date == 0);
	fplinux_ui_screen_set_listener(form, NULL, on_change, NULL);
	fplinux_ui_form_set_edit_listener(form, on_edit, NULL);
	fplinux_ui_push(&ui, form);
	fired = (struct fired){ 0 };
	press(KEY_OK);
	CHECK(fplinux_ui_form_is_editing(form));
	press(KEY_OK);
	CHECK(!fplinux_ui_form_is_editing(form));
	CHECK(fired.accepts == 1 && fired.accepted_index == date);
	CHECK(fired.changes == 0);
	CHECK(fplinux_ui_form_get_date(form, date) == 0);
	press(KEY_OK);
	press(KEY_UP);
	press(KEY_F14);
	CHECK(fired.accepts == 1 && fired.changes == 0);
	CHECK(fplinux_ui_form_get_date(form, date) == 0);
	fplinux_ui_form_set_edit_listener(form, NULL, NULL);
	press(KEY_OK);
	press(KEY_OK);
	CHECK(fired.accepts == 1 && fired.changes == 0);
	CHECK(!fplinux_ui_form_is_editing(NULL));
	finish();
}

static void test_retained_save_stays_on_narrow_form_with_edit_and_cancel(void)
{
	struct fplinux_ui_command save = {
		.id = REFRESH_ID,
		.type = FPLINUX_UI_COMMAND_SCREEN,
		.label = "Сохранить",
		.keep_visible = true,
	};
	const struct fplinux_ui_command cancel = {
		.id = BACK_ID,
		.type = FPLINUX_UI_COMMAND_CANCEL,
	};
	struct fplinux_ui_screen *form = fplinux_ui_form_new(&ui, "Alarm");

	CHECK(form);
	CHECK(fplinux_ui_form_append_date(form, "Time", FPLINUX_UI_TIME, 0) ==
	      0);
	CHECK(fplinux_ui_screen_add_command(form, &save));
	CHECK(fplinux_ui_screen_add_command(form, &cancel));
	fplinux_ui_screen_set_listener(form, on_command, NULL, NULL);
	legend_advance = 6;
	russian_labels = true;
	metrics.screen_width = surface_width = 128;
	fplinux_ui_push(&ui, form);
	CHECK(legends_are("Сохранить", "Изменить", "Отмена"));
	fired = (struct fired){ 0 };
	press(KEY_F13);
	CHECK(!menu.open && fired.command_id == REFRESH_ID);
	press(KEY_OK);
	CHECK(legends_are("", "Done", "Отмена"));
	press(KEY_F14);
	CHECK(legends_are("Сохранить", "Изменить", "Отмена"));
	press(KEY_F14);
	CHECK(fired.command_id == BACK_ID);
	/* Other callers retain the existing collapse policy. */
	save.keep_visible = false;
	CHECK(fplinux_ui_screen_add_command(form, &save));
	CHECK(legends_are("Options", "Изменить", "Отмена"));
	finish();
}

static void test_retained_cancel_leaves_phone_text_correction_available(void)
{
	const struct fplinux_ui_command done = {
		.id = OPEN_ID,
		.type = FPLINUX_UI_COMMAND_OK,
		.label = "Done",
	};
	const struct fplinux_ui_command cancel = {
		.id = BACK_ID,
		.type = FPLINUX_UI_COMMAND_CANCEL,
		.keep_visible = true,
	};
	struct fplinux_ui_screen *form = fplinux_ui_form_new(&ui, "Name");
	size_t name = fplinux_ui_form_append_text_field(
		form, NULL, "abcdef", 32, FPLINUX_UI_TEXT_ANY);

	CHECK(form && name == 0);
	CHECK(fplinux_ui_screen_add_command(form, &done));
	CHECK(fplinux_ui_screen_add_command(form, &cancel));
	fplinux_ui_screen_set_listener(form, on_command, NULL, NULL);
	fplinux_ui_push(&ui, form);
	CHECK(legends_are("Clear", "Done", "Cancel"));
	fired = (struct fired){ 0 };
	press(KEY_F14);
	CHECK(fired.commands == 1 && fired.command_id == BACK_ID);
	CHECK(!strcmp(fplinux_ui_form_get_text(form, name), "abcdef"));
	press(KEY_F13);
	CHECK(!strcmp(fplinux_ui_form_get_text(form, name), "abcde"));
	CHECK(fired.commands == 1);
	CHECK(fplinux_ui_screen_add_command(form, &refresh_command));
	CHECK(legends_are("Options", "Done", "Cancel"));
	press_options();
	choose("Clear");
	CHECK(!strcmp(fplinux_ui_form_get_text(form, name), "abcd"));
	press(KEY_OK);
	CHECK(fired.command_id == OPEN_ID);
	finish();
}

/*
 * A HYPERLINK or BUTTON string item takes the focus while it has item
 * commands: OK runs the first by priority, named by the centre legend,
 * and the soft keys offer the others. Losing its last item command, being
 * hidden or going back to PLAIN passes the focus on; a PLAIN item stays
 * read-only with item commands.
 */
static void test_form_appearance(void)
{
	static const struct fplinux_ui_command open = {
		.id = OPEN_ID,
		.type = FPLINUX_UI_COMMAND_ITEM,
		.label = "Open",
		.priority = 2,
	};
	static const struct fplinux_ui_command share = {
		.id = SHARE_ID,
		.type = FPLINUX_UI_COMMAND_ITEM,
		.label = "Share",
		.priority = 1,
	};
	struct fplinux_ui_screen *form = fplinux_ui_form_new(&ui, "Links");
	size_t link;
	size_t gauge;

	CHECK(form);
	link = fplinux_ui_form_append_string(form, NULL, "example.org");
	gauge = fplinux_ui_form_append_gauge(form, "Level", 0, 10, 5, true);
	CHECK(link == 0 && gauge == 1);
	CHECK(fplinux_ui_form_add_item_command(form, link, &open));
	CHECK(fplinux_ui_screen_add_command(form, &refresh_command));
	CHECK(fplinux_ui_screen_add_command(form, &back_command));
	fplinux_ui_screen_set_listener(form, on_command, NULL, NULL);
	fplinux_ui_screen_set_item_listener(form, on_item, NULL);
	CHECK(fplinux_ui_form_get_focus(form) == NONE);
	fplinux_ui_push(&ui, form);
	CHECK(fplinux_ui_form_get_focus(form) == gauge);
	fplinux_ui_form_set_focus(form, link);
	CHECK(fplinux_ui_form_get_focus(form) == gauge);

	CHECK(fplinux_ui_form_set_appearance(form, link,
					     FPLINUX_UI_APPEARANCE_HYPERLINK));
	fplinux_ui_form_set_focus(form, link);
	CHECK(fplinux_ui_form_get_focus(form) == link);
	CHECK(legends_are("Refresh", "Open", "Back"));
	fired = (struct fired){ 0 };
	press(KEY_OK);
	CHECK(fired.items == 1 && fired.item_index == link &&
	      fired.item_id == OPEN_ID && !fired.commands);

	/* Share comes first: OK runs it; Open joins Refresh in Options. */
	CHECK(fplinux_ui_form_add_item_command(form, link, &share));
	CHECK(legends_are("Options", "Share", "Back"));
	press_options();
	CHECK(menu.count == 2 && !strcmp(menu.texts[0], "Refresh") &&
	      !strcmp(menu.texts[1], "Open"));
	choose("Open");
	CHECK(fired.items == 1 && fired.item_index == link &&
	      fired.item_id == OPEN_ID);

	CHECK(fplinux_ui_form_set_appearance(form, link,
					     FPLINUX_UI_APPEARANCE_BUTTON));
	CHECK(fplinux_ui_form_get_focus(form) == link);
	fired = (struct fired){ 0 };
	press(KEY_OK);
	CHECK(fired.items == 1 && fired.item_id == SHARE_ID);

	fplinux_ui_screen_remove_item_command(form, link, SHARE_ID);
	CHECK(fplinux_ui_form_get_focus(form) == link);
	CHECK(legends_are("Refresh", "Open", "Back"));
	fplinux_ui_screen_remove_item_command(form, link, OPEN_ID);
	CHECK(fplinux_ui_form_get_focus(form) == gauge);
	CHECK(legends_are("Refresh", "", "Back"));

	CHECK(fplinux_ui_form_add_item_command(form, link, &open));
	fplinux_ui_form_set_focus(form, link);
	CHECK(fplinux_ui_form_get_focus(form) == link);
	CHECK(fplinux_ui_form_set_appearance(form, link,
					     FPLINUX_UI_APPEARANCE_PLAIN));
	CHECK(fplinux_ui_form_get_focus(form) == gauge);

	CHECK(fplinux_ui_form_set_appearance(form, link,
					     FPLINUX_UI_APPEARANCE_HYPERLINK));
	fplinux_ui_form_set_focus(form, link);
	CHECK(fplinux_ui_form_get_focus(form) == link);
	CHECK(fplinux_ui_form_set_hidden(form, link, true));
	CHECK(fplinux_ui_form_get_focus(form) == gauge);

	errno = 0;
	CHECK(!fplinux_ui_form_set_appearance(form, gauge,
					      FPLINUX_UI_APPEARANCE_BUTTON));
	CHECK(errno == EINVAL);
	errno = 0;
	CHECK(!fplinux_ui_form_set_appearance(form, 2,
					      FPLINUX_UI_APPEARANCE_BUTTON));
	CHECK(errno == EINVAL);
	errno = 0;
	CHECK(!fplinux_ui_form_set_appearance(form, link,
					      (enum fplinux_ui_appearance)3));
	CHECK(errno == EINVAL);
	errno = 0;
	CHECK(!fplinux_ui_form_set_appearance(NULL, 0,
					      FPLINUX_UI_APPEARANCE_PLAIN));
	CHECK(errno == EINVAL);
	fplinux_ui_form_delete_all(form);
	CHECK(fplinux_ui_form_get_focus(form) == NONE);
	finish();
}

/* fplinux_ui_form_get_focus() refuses another screen and NULL. */
static void test_form_get_focus_errors(void)
{
	struct fplinux_ui_screen *list =
		fplinux_ui_list_new(&ui, "Files", FPLINUX_UI_LIST_IMPLICIT);

	CHECK(list);
	fplinux_ui_list_append(list, "A", NULL);
	fplinux_ui_push(&ui, list);
	errno = 0;
	CHECK(fplinux_ui_form_get_focus(list) == NONE);
	CHECK(errno == EINVAL);
	errno = 0;
	CHECK(fplinux_ui_form_get_focus(NULL) == NONE);
	CHECK(errno == EINVAL);
	errno = 0;
	CHECK(!fplinux_ui_form_set_appearance(list, 0,
					      FPLINUX_UI_APPEARANCE_HYPERLINK));
	CHECK(errno == EINVAL);
	finish();
}

static void test_text_box_cancels_picker_without_replacing_the_note(void)
{
	struct fplinux_ui_screen *box =
		fplinux_ui_text_box_new(&ui, "Note", "hello", 0);
	struct fplinux_ui_key symbols = {
		.source = FPLINUX_UI_KEY_PHONE,
		.code = KEY_NUMERIC_STAR,
		.pressed = true,
	};

	CHECK(box);
	fplinux_ui_push(&ui, box);
	fplinux_ui_text_model_set_caret(&text_field.model, 2);
	CHECK(fplinux_ui_screens_key(ui.screens, &symbols));
	symbols.pressed = false;
	CHECK(fplinux_ui_screens_key(ui.screens, &symbols));
	CHECK(fplinux_ui_text_is_picker_open(&text_field));
	CHECK(fplinux_ui_text_box_cancel_picker(box));
	CHECK(!fplinux_ui_text_is_picker_open(&text_field));
	CHECK(!strcmp(fplinux_ui_text_box_get(box), "hello"));
	CHECK(fplinux_ui_text_model_caret(&text_field.model) == 2);
	CHECK(fplinux_ui_current(&ui) == box);
	CHECK(fplinux_ui_text_box_cancel_picker(box));
	CHECK(fplinux_ui_text_model_caret(&text_field.model) == 2);
	errno = 0;
	CHECK(!fplinux_ui_text_box_cancel_picker(NULL));
	CHECK(errno == EINVAL);
	finish();
}

static void test_hidden_text_box_stops_clear_repeat(void)
{
	struct fplinux_ui_screen *box =
		fplinux_ui_text_box_new(&ui, "Note", "abcdef", 0);
	struct fplinux_ui_key clear = {
		.source = FPLINUX_UI_KEY_PHONE,
		.code = KEY_F14,
		.pressed = true,
	};
	uint64_t deadline;

	CHECK(box);
	fplinux_ui_push(&ui, box);
	CHECK(fplinux_ui_screens_key(ui.screens, &clear));
	CHECK(!strcmp(fplinux_ui_text_box_get(box), "abcde"));
	CHECK(fplinux_ui_text_model_deadline(&text_field.model, &deadline));
	fplinux_ui_screens_set_visible(ui.screens, false);
	fplinux_ui_text_model_tick(&text_field.model, 2000);
	CHECK(!strcmp(fplinux_ui_text_box_get(box), "abcde"));
	CHECK(!fplinux_ui_text_model_deadline(&text_field.model, &deadline));
	fplinux_ui_screens_set_visible(ui.screens, true);
	fplinux_ui_text_model_tick(&text_field.model, 3000);
	CHECK(!strcmp(fplinux_ui_text_box_get(box), "abcde"));
	finish();
}

int main(int argc, char *argv[])
{
	if (argc != 2 && argc != 3)
		return EXIT_FAILURE;
	input_languages = argv[1];
	if (argc == 3) {
		if (!strcmp(argv[2], "back-last"))
			test_options_keeps_back_last_and_delivers_visible_actions();
		else if (!strcmp(argv[2], "synthetic-clear"))
			test_options_distinguishes_synthetic_clear_from_back(
				true);
		else if (!strcmp(argv[2], "no-invented-back"))
			test_options_distinguishes_synthetic_clear_from_back(
				false);
		else
			return EXIT_FAILURE;
		return failures ? EXIT_FAILURE : EXIT_SUCCESS;
	}
	test_navigation_restores_current_command_and_menu_position();
	test_navigation_pop_and_plain_push_do_not_restore_options();
	test_navigation_item_origin_follows_inserted_row();
	test_navigation_removed_command_and_replaced_owner_are_not_restored();
	test_only_navigated_informational_alert_close_restores_options();
	test_row_inserted_above();
	test_row_deleted();
	test_item_command_removed();
	test_screen_command_removed();
	test_unrelated_changes_keep_menu();
	test_form_item_moved_or_hidden();
	test_screen_popped();
	test_application_menu_kept();
	test_content_size();
	test_header_status_fits_resizes_and_clears();
	test_header_status_hidden_parent_keeps_child_header();
	test_header_status_without_title_and_allocation_failures();
	test_duration_commits_or_cancels_without_changing_form_focus();
	test_unchanged_date_done_reports_acceptance_without_change();
	test_retained_save_stays_on_narrow_form_with_edit_and_cancel();
	test_retained_cancel_leaves_phone_text_correction_available();
	test_form_appearance();
	test_form_get_focus_errors();
	test_hidden_text_box_stops_clear_repeat();
	test_text_box_cancels_picker_without_replacing_the_note();
	if (failures)
		return EXIT_FAILURE;
	puts("screen commands: OK");
	return EXIT_SUCCESS;
}
