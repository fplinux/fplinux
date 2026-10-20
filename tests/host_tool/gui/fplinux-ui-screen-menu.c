/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The options menu of the high-level screens while the application changes
 * the screen under it. The harness links lib/fplinux/fplinux-ui-screen.c
 * with stand-ins for LVGL and the enclosing runtime:
 * LVGL objects retain only geometry and visibility,
 * a list view keeps only its row count and focus, and the menu
 * records its items and callback. It opens Options with the left soft key,
 * changes the screen and checks what a choice fires. It also checks the
 * content sizes that screens report before and after a surface resize.
 */
#include <errno.h>
#include <linux/input-event-codes.h>
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
	bool hidden;
	char text[256];
};
static struct object_geometry object_storage;
static struct object_geometry objects[512];
static size_t object_count;
static char local_legends[3][64];
static struct fplinux_ui_text text_field;
static const char *input_languages;
/* The session legends sent by the screen. */
static struct {
	char legends[3][64];
} drawn;
static const struct fplinux_ui_theme theme;
static const struct fplinux_ui_profile_spec spec = {
	.width = 240,
	.height = 320,
	.header_height = 28,
	.row_height = 30,
	.row_height_detail = 44,
	.alert_margin = 8,
	.alert_padding = 6,
	.alert_border = 2,
};
static const struct fplinux_ui_metrics metrics = {
	.screen_width = 240,
	.screen_height = 320,
};
/* The surface, which a mode switch resizes; the display stays. */
static int surface_width = 240;

/* The open menu, as fplinux_ui_menu_open() received it. */
static struct {
	bool open;
	fplinux_ui_menu_cb callback;
	void *context;
	size_t count;
	int actions[MENU_ITEMS];
	char texts[MENU_ITEMS][32];
} menu;

/* What the screens reported to the application. */
static struct fired {
	int commands;
	int command_id;
	int items;
	size_t item_index;
	int item_id;
} fired;

/* LVGL */

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
OBJECT(lv_label_create, lv_obj_t *parent)
OBJECT(lv_obj_create, lv_obj_t *parent)
lv_obj_t *lv_obj_get_child(const lv_obj_t *obj, int32_t index)
{
	return (lv_obj_t *)&object_storage;
}
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
ZERO(int32_t, lv_font_get_line_height, const lv_font_t *font)
ZERO(lv_result_t, lv_image_decoder_get_info, const void *src,
     lv_image_header_t *header)
NOTHING(lv_image_set_src, lv_obj_t *obj, const void *src)
ZERO(lv_image_src_t, lv_image_src_get_type, const void *src)
NOTHING(lv_label_set_long_mode, lv_obj_t *obj, lv_label_long_mode_t mode)
NOTHING(lv_label_set_text_fmt, lv_obj_t *obj, const char *format, ...)
NOTHING(lv_label_set_text_selection_end, lv_obj_t *obj, uint32_t index)
NOTHING(lv_label_set_text_selection_start, lv_obj_t *obj, uint32_t index)
ZERO(lv_event_dsc_t *, lv_obj_add_event_cb, lv_obj_t *obj,
     lv_event_cb_t callback, lv_event_code_t filter, void *data)
NOTHING(lv_obj_align, lv_obj_t *obj, lv_align_t align, int32_t x, int32_t y)
NOTHING(lv_obj_center, lv_obj_t *obj)
NOTHING(lv_obj_clean, lv_obj_t *obj)
NOTHING(lv_obj_delete, lv_obj_t *obj)
int32_t lv_obj_get_content_height(const lv_obj_t *obj)
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
int32_t lv_obj_get_style_width(const lv_obj_t *obj, lv_part_t part)
{
	return geometry(obj)->width;
}
ZERO(bool, lv_obj_is_hidden, const lv_obj_t *obj)
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
NOTHING(lv_obj_set_style_pad_all, lv_obj_t *obj, int32_t value,
	lv_style_selector_t selector)
NOTHING(lv_obj_set_style_pad_bottom, lv_obj_t *obj, int32_t value,
	lv_style_selector_t selector)
NOTHING(lv_obj_set_style_pad_column, lv_obj_t *obj, int32_t value,
	lv_style_selector_t selector)
NOTHING(lv_obj_set_style_pad_hor, lv_obj_t *obj, int32_t value,
	lv_style_selector_t selector)
NOTHING(lv_obj_set_style_pad_left, lv_obj_t *obj, int32_t value,
	lv_style_selector_t selector)
NOTHING(lv_obj_set_style_pad_right, lv_obj_t *obj, int32_t value,
	lv_style_selector_t selector)
NOTHING(lv_obj_set_style_pad_row, lv_obj_t *obj, int32_t value,
	lv_style_selector_t selector)
NOTHING(lv_obj_set_style_pad_ver, lv_obj_t *obj, int32_t value,
	lv_style_selector_t selector)
NOTHING(lv_obj_set_style_text_align, lv_obj_t *obj, lv_text_align_t value,
	lv_style_selector_t selector)
NOTHING(lv_obj_set_style_text_color, lv_obj_t *obj, lv_color_t value,
	lv_style_selector_t selector)
NOTHING(lv_obj_set_style_text_decor, lv_obj_t *obj, lv_text_decor_t value,
	lv_style_selector_t selector)
NOTHING(lv_obj_set_style_text_font, lv_obj_t *obj, const lv_font_t *value,
	lv_style_selector_t selector)
NOTHING(lv_obj_set_style_width, lv_obj_t *obj, int32_t value,
	lv_style_selector_t selector)
NOTHING(lv_obj_set_width, lv_obj_t *obj, int32_t w)
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

/* Every legend fits. */
void lv_text_get_size(lv_point_t *size, const char *text, const lv_font_t *font,
		      int32_t letter_space, int32_t line_space,
		      int32_t max_width, lv_text_flag_t flag)
{
	*size = (lv_point_t){ 0 };
}

/* The runtime around the screens */

char *dgettext(const char *domain, const char *text)
{
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
NOTHING(fplinux_ui_title_hide, lv_obj_t *label)
NOTHING(fplinux_ui_title_init, lv_obj_t *label, const lv_font_t *font)
NOTHING(fplinux_ui_title_scroll, lv_obj_t *label)
NOTHING(fplinux_ui_text_canvas_active, lv_obj_t *canvas, bool active)
ZERO(bool, fplinux_ui_text_is_empty, const struct fplinux_ui_text *field)
ZERO(bool, fplinux_ui_text_is_picker_open, const struct fplinux_ui_text *field)
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
     size_t index, lv_draw_buf_t *icon)
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

bool fplinux_ui_menu_open(struct fplinux_ui *owner, const char *title,
			  const struct fplinux_ui_menu_item *items,
			  size_t count, fplinux_ui_menu_cb callback,
			  void *context)
{
	if (menu.open || count > MENU_ITEMS) {
		errno = EBUSY;
		return false;
	}
	menu.open = true;
	menu.callback = callback;
	menu.context = context;
	menu.count = count;
	for (size_t i = 0; i < count; ++i) {
		menu.actions[i] = items[i].action;
		snprintf(menu.texts[i], sizeof(menu.texts[i]), "%s",
			 items[i].text);
	}
	return true;
}

bool fplinux_ui_menu_is_open(const struct fplinux_ui *owner)
{
	return menu.open;
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
	CHECK(fplinux_ui_menu_open(&ui, "Own", &own, 1, application_chosen,
				   NULL));
	fplinux_ui_screen_remove_item_command(list, 1, DELETE_ID);
	CHECK(fplinux_ui_list_delete(list, 0));
	CHECK(menu.open && menu.callback == application_chosen);
	finish();
}

/*
 * fplinux_ui_screen_get_content_size() follows the surface: below the title
 * band, the whole surface without one, an alert's box inside its margins,
 * border and padding; a resize with a menu open keeps the menu.
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
	if (argc != 2)
		return EXIT_FAILURE;
	input_languages = argv[1];
	test_row_inserted_above();
	test_row_deleted();
	test_item_command_removed();
	test_screen_command_removed();
	test_unrelated_changes_keep_menu();
	test_form_item_moved_or_hidden();
	test_screen_popped();
	test_application_menu_kept();
	test_content_size();
	test_form_appearance();
	test_form_get_focus_errors();
	test_hidden_text_box_stops_clear_repeat();
	if (failures)
		return EXIT_FAILURE;
	puts("screen commands: OK");
	return EXIT_SUCCESS;
}
