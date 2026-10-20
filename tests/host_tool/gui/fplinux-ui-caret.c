/* SPDX-License-Identifier: GPL-2.0-only */
#include "fplinux-ui-fonts.h"
#include "fplinux-ui-internal.h"
#include "fplinux-ui-screen.h"
#include "fplinux-multitap.h"

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <linux/input-event-codes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Session transport, palettes and marquee are outside these widget tests. */
struct fplinux_ui {
	lv_display_t *display;
	lv_obj_t *root;
	struct fplinux_ui_screens *screens;
	struct fplinux_ui_menu_view *menu;
	struct fplinux_ui_theme theme;
	struct fplinux_ui_metrics metrics;
	bool visible;
};

static struct fplinux_ui ui;
static uint8_t display_pixels[240 * 320 * 2];
static const char *language_directory;

void __real_fplinux_multitap_load_languages(
	struct fplinux_multitap_languages *languages, const char *directory,
	const char *first);

/* Only the installed language directory is replaced by owned test input. */
void __wrap_fplinux_multitap_load_languages(
	struct fplinux_multitap_languages *languages, const char *directory,
	const char *first)
{
	(void)directory;
	__real_fplinux_multitap_load_languages(languages, language_directory,
					       first);
}

const struct fplinux_ui_theme *
fplinux_ui_theme(const struct fplinux_ui *context)
{
	return &context->theme;
}

const struct fplinux_ui_metrics *
fplinux_ui_metrics(const struct fplinux_ui *context)
{
	return &context->metrics;
}

const struct fplinux_ui_profile_spec *
fplinux_ui_spec(const struct fplinux_ui *context)
{
	(void)context;
	return fplinux_ui_profile_spec(FPLINUX_UI_PROFILE_240X320);
}

lv_obj_t *fplinux_ui_screen(struct fplinux_ui *context)
{
	return context->root;
}

int fplinux_ui_width(const struct fplinux_ui *context)
{
	return lv_display_get_horizontal_resolution(context->display);
}

int fplinux_ui_height(const struct fplinux_ui *context)
{
	return lv_display_get_vertical_resolution(context->display);
}

uint64_t fplinux_ui_now_ms(void)
{
	return lv_tick_get();
}

const char *fplinux_ui_input_locale(const struct fplinux_ui *context)
{
	(void)context;
	return "en";
}

const char *fplinux_ui_locale(const struct fplinux_ui *context)
{
	(void)context;
	return "en";
}

bool fplinux_ui_is_visible(const struct fplinux_ui *context)
{
	return context->visible;
}

bool fplinux_ui_is_launcher(const struct fplinux_ui *context)
{
	(void)context;
	return false;
}

struct fplinux_ui_screens **fplinux_ui_screens_slot(struct fplinux_ui *context)
{
	return &context->screens;
}

void fplinux_ui_cancel_key_repeat(struct fplinux_ui *context,
				  const struct fplinux_ui_screen *canvas)
{
	(void)context;
	(void)canvas;
}

int fplinux_ui_modal_height(const struct fplinux_ui *context)
{
	return fplinux_ui_height(context);
}

void fplinux_ui_modal_set(struct fplinux_ui *context,
			  enum fplinux_ui_modal source, bool up)
{
	(void)context;
	(void)source;
	(void)up;
}

bool fplinux_ui_scrim_attach(struct fplinux_ui *context, lv_obj_t *popup)
{
	(void)context;
	(void)popup;
	return true;
}

void fplinux_ui_scrim_style(struct fplinux_ui *context, lv_obj_t *root)
{
	(void)context;
	(void)root;
}

void fplinux_ui_marquee_focus(struct fplinux_ui *context, lv_obj_t *first,
			      lv_obj_t *second)
{
	(void)context;
	(void)first;
	(void)second;
}

void fplinux_ui_marquee_blur(struct fplinux_ui *context, lv_obj_t *label)
{
	(void)context;
	(void)label;
}

void fplinux_ui_marquee_changed(struct fplinux_ui *context, lv_obj_t *first,
				lv_obj_t *second)
{
	(void)context;
	(void)first;
	(void)second;
}

void fplinux_ui_title_init(lv_obj_t *label, const lv_font_t *font)
{
	lv_obj_set_style_text_font(label, font, 0);
}

void fplinux_ui_title_scroll(lv_obj_t *label)
{
	(void)label;
}

void fplinux_ui_title_hide(lv_obj_t *label)
{
	(void)label;
}

bool fplinux_ui_set_softkeys(struct fplinux_ui *context, const char *left,
			     const char *centre, const char *right)
{
	(void)context;
	(void)left;
	(void)centre;
	(void)right;
	return true;
}

bool fplinux_ui_is_softkey(const struct fplinux_ui_key *key, bool left)
{
	return key->source == FPLINUX_UI_KEY_PHONE &&
	       key->code == (left ? KEY_F13 : KEY_F14);
}

bool fplinux_ui_menu_is_open(const struct fplinux_ui *context)
{
	return context->menu != NULL;
}

bool fplinux_ui_menu_open(struct fplinux_ui *context, const char *title,
			  const struct fplinux_ui_menu_item *items,
			  size_t count, fplinux_ui_menu_cb callback, void *data)
{
	(void)callback;
	(void)data;
	assert(!context->menu);
	context->menu =
		fplinux_ui_menu_view_create(context, title, items, count);
	return context->menu != NULL;
}

void fplinux_ui_menu_close(struct fplinux_ui *context)
{
	if (!context->menu)
		return;
	fplinux_ui_menu_view_delete(context->menu);
	context->menu = NULL;
	fplinux_ui_screens_reclaim(context->screens);
}

bool fplinux_ui_back(struct fplinux_ui *context)
{
	(void)context;
	abort();
}

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
	(void)area;
	(void)pixels;
	lv_display_flush_ready(display);
}

static void advance(uint32_t milliseconds)
{
	lv_tick_inc(milliseconds);
	lv_timer_handler();
	lv_refr_now(ui.display);
}

static lv_obj_t *textarea(lv_obj_t *root, unsigned int *index)
{
	if (lv_obj_check_type(root, &lv_textarea_class)) {
		if (!*index)
			return root;
		--*index;
	}
	for (unsigned int i = 0; i < lv_obj_get_child_count(root); ++i) {
		lv_obj_t *found =
			textarea(lv_obj_get_child(root, (int32_t)i), index);

		if (found)
			return found;
	}
	return NULL;
}

static lv_obj_t *area_at(unsigned int index)
{
	lv_obj_t *area = textarea(ui.root, &index);

	assert(area);
	return area;
}

static struct fplinux_ui_text *new_field(bool multiline, const char *text)
{
	struct fplinux_ui_text *field =
		fplinux_ui_text_create(&ui, ui.root, multiline, 0);

	assert(field);
	assert(fplinux_ui_text_set(field, text));
	lv_obj_update_layout(ui.root);
	advance(0);
	return field;
}

static lv_draw_buf_t *snapshot(lv_obj_t *object)
{
	lv_draw_buf_t *image;

	/* Sample the native phase at this exact controlled tick. */
	lv_anim_refr_now();
	image = lv_snapshot_take(object, LV_COLOR_FORMAT_RGB888);

	assert(image);
	return image;
}

static unsigned int changed_columns(const lv_draw_buf_t *first,
				    const lv_draw_buf_t *second)
{
	unsigned int columns = 0;

	assert(first->header.w == second->header.w);
	assert(first->header.h == second->header.h);
	for (uint32_t x = 0; x < first->header.w; ++x) {
		for (uint32_t y = 0; y < first->header.h; ++y) {
			if (memcmp(lv_draw_buf_goto_xy(first, x, y),
				   lv_draw_buf_goto_xy(second, x, y), 3)) {
				++columns;
				break;
			}
		}
	}
	return columns;
}

static void expect_blink(lv_obj_t *area)
{
	lv_obj_t *frame = lv_obj_get_parent(area);
	lv_draw_buf_t *visible = snapshot(frame);
	lv_draw_buf_t *other;

	advance(499);
	other = snapshot(frame);
	assert(changed_columns(visible, other) == 0);
	lv_draw_buf_destroy(other);
	advance(1);
	other = snapshot(frame);
	if (changed_columns(visible, other) != 1)
		fprintf(stderr,
			"caret delta=%u anim=%d duration=%u cursor=%u area=%dx%d scroll=%d,%d text=%s\n",
			changed_columns(visible, other),
			lv_anim_get(area, NULL) != NULL,
			lv_obj_get_style_anim_duration(area, LV_PART_CURSOR),
			lv_textarea_get_cursor_pos(area),
			lv_obj_get_width(area), lv_obj_get_height(area),
			lv_obj_get_scroll_x(area), lv_obj_get_scroll_y(area),
			lv_textarea_get_text(area));
	assert(changed_columns(visible, other) == 1);
	lv_draw_buf_destroy(other);
	advance(500);
	other = snapshot(frame);
	assert(changed_columns(visible, other) == 0);
	lv_draw_buf_destroy(other);
	lv_draw_buf_destroy(visible);
}

static void key(struct fplinux_ui_text *field,
		enum fplinux_ui_key_source source, unsigned int code,
		bool pressed, const char *text)
{
	struct fplinux_ui_key input = {
		.source = source,
		.code = code,
		.pressed = pressed,
	};

	if (text)
		snprintf(input.text, sizeof(input.text), "%s", text);
	assert(fplinux_ui_text_key(field, &input));
	advance(0);
}

static void test_blink(void)
{
	new_field(false, "");
	expect_blink(area_at(0));
}

static void test_transitions(void)
{
	struct fplinux_ui_text *field = new_field(false, "AV");
	lv_obj_t *area = area_at(0);
	lv_draw_buf_t *editing;
	lv_draw_buf_t *resting;

	advance(500);
	editing = snapshot(lv_obj_get_parent(area));
	fplinux_ui_text_set_focused(field, false);
	assert(!lv_anim_get(area, NULL));
	resting = snapshot(lv_obj_get_parent(area));
	assert(changed_columns(editing, resting) == 0);
	lv_draw_buf_destroy(resting);
	lv_draw_buf_destroy(editing);
	fplinux_ui_text_set_focused(field, true);
	expect_blink(area);
	key(field, FPLINUX_UI_KEY_PHONE, KEY_NUMERIC_STAR, true, NULL);
	assert(fplinux_ui_text_is_picker_open(field));
	assert(!lv_anim_get(area, NULL));
	key(field, FPLINUX_UI_KEY_PHONE, KEY_F14, true, NULL);
	assert(!fplinux_ui_text_is_picker_open(field));
	expect_blink(area);
	assert(fplinux_ui_text_set_constraint(field,
					      FPLINUX_UI_TEXT_UNEDITABLE));
	assert(!lv_anim_get(area, NULL));
	assert(fplinux_ui_text_set(field, "Read only"));
	assert(!lv_anim_get(area, NULL));
	assert(fplinux_ui_text_set_constraint(field, FPLINUX_UI_TEXT_ANY));
	expect_blink(area);
}

static void test_scroll(void)
{
	struct fplinux_ui_text *field = new_field(
		true, "First\nSecond\nThird\nFourth\nFifth\nSixth\nLast");
	lv_obj_t *area = area_at(0);
	lv_point_t end;

	assert(lv_obj_get_scroll_y(area) > 0);
	lv_obj_get_scroll_end(area, &end);
	assert(end.y == lv_obj_get_scroll_y(area));
	expect_blink(area);
	key(field, FPLINUX_UI_KEY_KEYBOARD, KEY_X, true, "x");
	assert(!strcmp(fplinux_ui_text_get(field),
		       "First\nSecond\nThird\nFourth\nFifth\nSixth\nLastx"));
	expect_blink(area);
	lv_obj_set_height(fplinux_ui_text_object(field), 100);
	lv_obj_update_layout(ui.root);
	advance(0);
	expect_blink(area);
	assert(fplinux_ui_text_set_constraint(field,
					      FPLINUX_UI_TEXT_UNEDITABLE));
	assert(lv_obj_get_scroll_y(area) == 0);
	assert(!lv_anim_get(area, NULL));
}

static void test_screen(void)
{
	struct fplinux_ui_screen *form = fplinux_ui_form_new(&ui, NULL);
	struct fplinux_ui_screen *cover;
	lv_obj_t *area;

	assert(form);
	assert(fplinux_ui_form_append_text_field(form, NULL, "", 0,
						 FPLINUX_UI_TEXT_ANY) == 0);
	fplinux_ui_push(&ui, form);
	advance(0);
	area = area_at(0);
	expect_blink(area);
	ui.visible = false;
	fplinux_ui_screens_set_visible(ui.screens, false);
	assert(!lv_anim_get(area, NULL));
	advance(600);
	ui.visible = true;
	fplinux_ui_screens_set_visible(ui.screens, true);
	expect_blink(area);
	cover = fplinux_ui_form_new(&ui, NULL);
	assert(cover);
	fplinux_ui_push(&ui, cover);
	assert(!lv_anim_get(area, NULL));
	assert(fplinux_ui_pop(&ui));
	expect_blink(area);
	for (unsigned int i = 0; i < 10; ++i)
		assert(fplinux_ui_form_append_string(form, NULL, "Spacer") !=
		       SIZE_MAX);
	assert(fplinux_ui_form_append_text_field(form, NULL, "", 0,
						 FPLINUX_UI_TEXT_ANY) == 11);
	fplinux_ui_form_set_focus(form, 11);
	advance(0);
	assert(fplinux_ui_form_get_focus(form) == 11);
	area = area_at(1);
	assert(lv_obj_is_visible(area));
	expect_blink(area);
	assert(fplinux_ui_form_set_hidden(form, 11, true));
	assert(!lv_anim_get(area, NULL));
}

static void test_deadlines(void)
{
	struct fplinux_ui_text *field = new_field(false, "");
	lv_obj_t *area = area_at(0);
	lv_obj_t *label = lv_textarea_get_label(area);

	key(field, FPLINUX_UI_KEY_PHONE, KEY_NUMERIC_2, true, NULL);
	key(field, FPLINUX_UI_KEY_PHONE, KEY_NUMERIC_2, false, NULL);
	assert(!strcmp(fplinux_ui_text_get(field), "A"));
	assert(lv_label_get_text_selection_start(label) == 0);
	advance(999);
	assert(lv_label_get_text_selection_start(label) == 0);
	advance(1);
	assert(lv_label_get_text_selection_start(label) ==
	       LV_DRAW_LABEL_NO_TXT_SEL);
	assert(!strcmp(fplinux_ui_text_get(field), "A"));
	assert(fplinux_ui_text_set(field, ""));
	key(field, FPLINUX_UI_KEY_PHONE, KEY_NUMERIC_2, true, NULL);
	advance(499);
	assert(!strcmp(fplinux_ui_text_get(field), "A"));
	advance(1);
	assert(!strcmp(fplinux_ui_text_get(field), "2"));
	key(field, FPLINUX_UI_KEY_PHONE, KEY_NUMERIC_2, false, NULL);
	assert(fplinux_ui_text_set(field, "abc"));
	key(field, FPLINUX_UI_KEY_PHONE, KEY_F14, true, NULL);
	assert(!strcmp(fplinux_ui_text_get(field), "ab"));
	advance(499);
	assert(!strcmp(fplinux_ui_text_get(field), "ab"));
	advance(1);
	assert(!strcmp(fplinux_ui_text_get(field), "a"));
	advance(100);
	assert(!strcmp(fplinux_ui_text_get(field), ""));
	key(field, FPLINUX_UI_KEY_PHONE, KEY_F14, false, NULL);
	expect_blink(area);
}

static void test_column(void)
{
	struct fplinux_ui_text *field = new_field(true, "WWWW\ni\nWWWW");
	lv_obj_t *area = area_at(0);

	key(field, FPLINUX_UI_KEY_KEYBOARD, KEY_HOME, true, NULL);
	key(field, FPLINUX_UI_KEY_KEYBOARD, KEY_RIGHT, true, NULL);
	key(field, FPLINUX_UI_KEY_KEYBOARD, KEY_RIGHT, true, NULL);
	assert(lv_textarea_get_cursor_pos(area) == 9);
	key(field, FPLINUX_UI_KEY_KEYBOARD, KEY_UP, true, NULL);
	assert(lv_textarea_get_cursor_pos(area) == 6);
	advance(500);
	key(field, FPLINUX_UI_KEY_KEYBOARD, KEY_UP, true, NULL);
	assert(lv_textarea_get_cursor_pos(area) == 2);
}

static void test_spacing(void)
{
	static const char *const samples[] = { "AV", "To", "ij", "Ая", "Тара" };
	struct fplinux_ui_text *field = new_field(false, "");
	lv_obj_t *area = area_at(0);

	for (size_t i = 0; i < sizeof(samples) / sizeof(*samples); ++i) {
		assert(fplinux_ui_text_set(field, samples[i]));
		assert(!strcmp(fplinux_ui_text_get(field), samples[i]));
		expect_blink(area);
		key(field, FPLINUX_UI_KEY_KEYBOARD, KEY_LEFT, true, NULL);
		assert(!strcmp(fplinux_ui_text_get(field), samples[i]));
		expect_blink(area);
	}
}

static lv_obj_t *label_with_text(lv_obj_t *root, const char *text)
{
	if (lv_obj_check_type(root, &lv_label_class) &&
	    !strcmp(lv_label_get_text(root), text))
		return root;
	for (uint32_t i = 0; i < lv_obj_get_child_count(root); ++i) {
		lv_obj_t *label = label_with_text(
			lv_obj_get_child(root, (int32_t)i), text);

		if (label)
			return label;
	}
	return NULL;
}

static void screen_key(unsigned int code)
{
	struct fplinux_ui_key input = {
		.source = FPLINUX_UI_KEY_PHONE,
		.code = code,
		.pressed = true,
	};

	assert(fplinux_ui_screens_key(ui.screens, &input));
	input.pressed = false;
	assert(fplinux_ui_screens_key(ui.screens, &input));
	advance(0);
}

static void test_menu_visibility(bool clear)
{
	const struct fplinux_ui_command back = {
		.id = 1,
		.type = FPLINUX_UI_COMMAND_BACK,
	};
	struct fplinux_ui_screen *box =
		fplinux_ui_text_box_new(&ui, NULL, clear ? "AV" : "", 0);
	struct fplinux_ui_key held = {
		.source = FPLINUX_UI_KEY_PHONE,
		.code = clear ? KEY_F14 : KEY_NUMERIC_2,
		.pressed = true,
	};
	lv_obj_t *area;
	lv_obj_t *label;
	lv_draw_buf_t *resting;
	lv_draw_buf_t *later;

	assert(box);
	assert(fplinux_ui_screen_add_command(box, &back));
	fplinux_ui_push(&ui, box);
	advance(0);
	area = area_at(0);
	label = lv_textarea_get_label(area);
	assert(fplinux_ui_screens_key(ui.screens, &held));
	assert(!strcmp(fplinux_ui_text_box_get(box), "A"));
	if (!clear)
		assert(lv_label_get_text_selection_start(label) == 0);
	screen_key(KEY_F13);
	assert(fplinux_ui_menu_is_open(&ui));
	assert(!lv_anim_get(area, NULL));
	assert(lv_label_get_text_selection_start(label) ==
	       LV_DRAW_LABEL_NO_TXT_SEL);
	advance(600);
	assert(!strcmp(fplinux_ui_text_box_get(box), "A"));
	ui.visible = false;
	fplinux_ui_screens_set_visible(ui.screens, false);
	advance(600);
	ui.visible = true;
	fplinux_ui_screens_set_visible(ui.screens, true);
	assert(fplinux_ui_menu_is_open(&ui));
	assert(!lv_anim_get(area, NULL));
	resting = snapshot(lv_obj_get_parent(area));
	advance(500);
	later = snapshot(lv_obj_get_parent(area));
	assert(changed_columns(resting, later) == 0);
	lv_draw_buf_destroy(later);
	lv_draw_buf_destroy(resting);
	assert(!strcmp(fplinux_ui_text_box_get(box), "A"));
	held.pressed = false;
	assert(fplinux_ui_screens_key(ui.screens, &held));
	fplinux_ui_menu_close(&ui);
	expect_blink(area);
	assert(!strcmp(fplinux_ui_text_box_get(box), "A"));
	screen_key(held.code);
	assert(!strcmp(fplinux_ui_text_box_get(box), clear ? "" : "Aa"));
}

static void test_canvas_menu_visibility(void)
{
	const struct fplinux_ui_menu_item action = { .action = 1,
						     .text = "AV" };
	struct fplinux_ui_screen *canvas = fplinux_ui_canvas_new(&ui, NULL);
	struct fplinux_ui_text *first;
	struct fplinux_ui_text *second;
	lv_obj_t *parent;

	assert(canvas);
	parent = fplinux_ui_canvas_object(canvas);
	first = fplinux_ui_text_create(&ui, parent, false, 0);
	second = fplinux_ui_text_create(&ui, parent, false, 0);
	assert(first && second);
	fplinux_ui_text_set_focused(second, false);
	fplinux_ui_push(&ui, canvas);
	advance(0);
	assert(lv_anim_get(area_at(0), NULL));
	assert(!lv_anim_get(area_at(1), NULL));
	assert(fplinux_ui_menu_open(&ui, NULL, &action, 1, NULL, NULL));
	assert(!lv_anim_get(area_at(0), NULL));
	ui.visible = false;
	fplinux_ui_screens_set_visible(ui.screens, false);
	ui.visible = true;
	fplinux_ui_screens_set_visible(ui.screens, true);
	assert(!lv_anim_get(area_at(0), NULL));
	assert(!lv_anim_get(area_at(1), NULL));
	fplinux_ui_menu_close(&ui);
	expect_blink(area_at(0));
	assert(!lv_anim_get(area_at(1), NULL));
}

struct alert_result {
	struct fplinux_ui_screen *below;
	int id;
	unsigned int commands;
	unsigned int deleted;
};

static void alert_command(struct fplinux_ui *context,
			  struct fplinux_ui_screen *screen, int id, void *data)
{
	struct alert_result *result = data;

	assert(context == &ui);
	assert(fplinux_ui_current(context) == result->below);
	assert(fplinux_ui_screen_get_data(screen) == result);
	assert(!result->deleted);
	assert(id == result->id);
	++result->commands;
}

static void alert_label_deleted(lv_event_t *event)
{
	struct alert_result *result = lv_event_get_user_data(event);

	++result->deleted;
}

static void test_alert_command_ids(void)
{
	static const int ids[] = { -3, INT_MIN, 0, INT_MAX };
	struct fplinux_ui_screen *below = fplinux_ui_form_new(&ui, NULL);

	assert(below);
	fplinux_ui_push(&ui, below);
	for (size_t i = 0; i < sizeof(ids) / sizeof(*ids); ++i) {
		struct alert_result result = { .below = below, .id = ids[i] };
		struct fplinux_ui_command command = {
			.id = ids[i],
			.type = FPLINUX_UI_COMMAND_CANCEL,
		};
		struct fplinux_ui_screen *alert = fplinux_ui_alert_new(
			&ui, FPLINUX_UI_ALERT_INFO, NULL, "AV", 5000);
		lv_obj_t *message;

		assert(alert);
		assert(fplinux_ui_screen_add_command(alert, &command));
		fplinux_ui_screen_set_data(alert, &result);
		fplinux_ui_screen_set_listener(alert, alert_command, NULL,
					       &result);
		fplinux_ui_push(&ui, alert);
		message = label_with_text(ui.root, "AV");
		assert(message);
		lv_obj_add_event_cb(message, alert_label_deleted,
				    LV_EVENT_DELETE, &result);
		screen_key(KEY_F14);
		assert(result.commands == 1);
		assert(result.deleted == 1);
		assert(fplinux_ui_current(&ui) == below);
		advance(5000);
		assert(result.commands == 1);
		assert(fplinux_ui_current(&ui) == below);
	}
	for (unsigned int timeout = 0; timeout < 2; ++timeout) {
		struct alert_result result = { .below = below };
		struct fplinux_ui_command reserved = {
			.id = -2,
			.type = FPLINUX_UI_COMMAND_CANCEL,
		};
		struct fplinux_ui_screen *alert =
			fplinux_ui_alert_new(&ui, FPLINUX_UI_ALERT_INFO, NULL,
					     "AV", timeout ? 100 : 0);
		lv_obj_t *message;

		assert(alert);
		errno = 0;
		assert(!fplinux_ui_screen_add_command(alert, &reserved));
		assert(errno == EINVAL);
		fplinux_ui_screen_set_listener(alert, alert_command, NULL,
					       &result);
		fplinux_ui_push(&ui, alert);
		message = label_with_text(ui.root, "AV");
		assert(message);
		lv_obj_add_event_cb(message, alert_label_deleted,
				    LV_EVENT_DELETE, &result);
		if (timeout)
			advance(100);
		else
			screen_key(KEY_F14);
		assert(result.commands == 0);
		assert(result.deleted == 1);
		assert(fplinux_ui_current(&ui) == below);
	}
}

static void test_alert_text(void)
{
	static const char long_message[] =
		"First\nSecond\nThird\nFourth\nFifth\nSixth\nLast\n"
		"First\nSecond\nThird\nFourth\nFifth\nSixth\nLast\n"
		"First\nSecond\nThird\nFourth\nFifth\nSixth\nLast";
	struct fplinux_ui_screen *below = fplinux_ui_form_new(&ui, NULL);
	struct fplinux_ui_screen *alert =
		fplinux_ui_alert_new(&ui, FPLINUX_UI_ALERT_INFO, "To", "", 0);
	lv_obj_t *title, *message, *box;
	int32_t empty_height, short_height, scroll;

	assert(below && alert);
	fplinux_ui_push(&ui, below);
	fplinux_ui_push(&ui, alert);
	assert(fplinux_ui_alert_set_progress(alert, 0, 0));
	advance(0);
	title = label_with_text(ui.root, "To");
	assert(title);
	box = lv_obj_get_parent(title);
	message = label_with_text(box, "");
	assert(message && lv_obj_is_hidden(message));
	empty_height = lv_obj_get_height(box);
	assert(fplinux_ui_alert_set_text(alert, "AV"));
	advance(0);
	assert(!strcmp(lv_label_get_text(message), "AV"));
	assert(lv_obj_is_visible(message));
	short_height = lv_obj_get_height(box);
	assert(short_height > empty_height);
	assert(!strcmp(lv_label_get_text(title), "To"));
	assert(fplinux_ui_alert_set_text(alert, long_message));
	advance(0);
	assert(!strcmp(lv_label_get_text(message), long_message));
	assert(lv_obj_get_height(box) > short_height);
	lv_obj_scroll_to_y(box, 1000, LV_ANIM_OFF);
	advance(0);
	scroll = lv_obj_get_scroll_y(box);
	assert(scroll > 0);
	assert(fplinux_ui_alert_set_text(alert, long_message));
	advance(0);
	assert(lv_obj_get_scroll_y(box) == scroll);
	assert(fplinux_ui_alert_set_text(alert, "AV"));
	advance(0);
	assert(lv_obj_get_scroll_y(box) == 0);
	assert(lv_obj_get_height(box) == short_height);
	assert(fplinux_ui_alert_set_text(alert, NULL));
	advance(0);
	assert(!strcmp(lv_label_get_text(message), ""));
	assert(lv_obj_is_hidden(message));
	assert(lv_obj_get_height(box) == empty_height);
	assert(fplinux_ui_alert_set_text(alert, "AV"));
	assert(fplinux_ui_alert_set_text(alert, ""));
	advance(0);
	assert(!strcmp(lv_label_get_text(message), ""));
	assert(lv_obj_is_hidden(message));
	assert(lv_obj_get_height(box) == empty_height);
	assert(!strcmp(lv_label_get_text(title), "To"));
	assert(fplinux_ui_current(&ui) == alert);
	errno = 0;
	assert(!fplinux_ui_alert_set_text(below, "AV"));
	assert(errno == EINVAL);
	errno = 0;
	assert(!fplinux_ui_alert_set_text(NULL, "AV"));
	assert(errno == EINVAL);
}

static void item_changed(struct fplinux_ui *context,
			 struct fplinux_ui_screen *screen, size_t item,
			 void *data)
{
	unsigned int *changes = data;

	(void)context;
	(void)screen;
	(void)item;
	++*changes;
}

static void test_hidden_date(void)
{
	struct fplinux_ui_screen *form = fplinux_ui_form_new(&ui, NULL);
	struct tm date = {
		.tm_year = 124,
		.tm_mon = 0,
		.tm_mday = 15,
		.tm_hour = 12,
		.tm_isdst = -1,
	};
	time_t saved = mktime(&date);
	unsigned int changes = 0;
	lv_obj_t *value;

	assert(form && saved != (time_t)-1);
	assert(fplinux_ui_form_append_date(form, NULL, FPLINUX_UI_DATE,
					   saved) == 0);
	assert(fplinux_ui_form_append_gauge(form, NULL, 0, 10, 5, true) == 1);
	fplinux_ui_screen_set_listener(form, NULL, item_changed, &changes);
	fplinux_ui_push(&ui, form);
	advance(0);
	value = label_with_text(ui.root, "01/15/2024");
	assert(value && lv_obj_is_visible(value));
	assert(fplinux_ui_form_get_focus(form) == 0);
	screen_key(KEY_OK);
	screen_key(KEY_UP);
	assert(!strcmp(lv_label_get_text(value), "02/15/2024"));
	assert(lv_label_get_text_selection_start(value) == 0);
	assert(fplinux_ui_form_get_date(form, 0) == saved);
	assert(fplinux_ui_form_set_hidden(form, 0, true));
	assert(!lv_obj_is_visible(value));
	assert(fplinux_ui_form_get_focus(form) == 1);
	assert(fplinux_ui_form_set_hidden(form, 0, false));
	advance(0);
	assert(lv_obj_is_visible(value));
	assert(!strcmp(lv_label_get_text(value), "01/15/2024"));
	assert(lv_label_get_text_selection_start(value) ==
	       LV_DRAW_LABEL_NO_TXT_SEL);
	assert(lv_label_get_text_selection_end(value) ==
	       LV_DRAW_LABEL_NO_TXT_SEL);
	assert(fplinux_ui_form_get_date(form, 0) == saved);
	assert(changes == 0);
}

static void expect_exclusive(struct fplinux_ui_screen *list, size_t marked)
{
	for (size_t i = 0; i < fplinux_ui_list_size(list); ++i)
		assert(fplinux_ui_list_is_selected(list, i) == (i == marked));
}

static void test_exclusive_first_row(bool insert)
{
	struct fplinux_ui_screen *list =
		fplinux_ui_list_new(&ui, NULL, FPLINUX_UI_LIST_EXCLUSIVE);
	unsigned int changes = 0;

	assert(list);
	fplinux_ui_screen_set_listener(list, NULL, item_changed, &changes);
	fplinux_ui_push(&ui, list);
	assert(fplinux_ui_list_get_focused(list) == SIZE_MAX);
	if (insert)
		assert(fplinux_ui_list_insert(list, 0, "AV", NULL) == 0);
	else
		assert(fplinux_ui_list_append(list, "AV", NULL) == 0);
	assert(fplinux_ui_list_get_focused(list) == 0);
	expect_exclusive(list, 0);
	assert(fplinux_ui_list_append(list, "To", NULL) == 1);
	fplinux_ui_list_set_focused(list, 1);
	fplinux_ui_list_set_selected(list, 1, true);
	assert(fplinux_ui_list_insert(list, 0, "ij", NULL) == 0);
	assert(fplinux_ui_list_get_focused(list) == 2);
	expect_exclusive(list, 2);
	assert(fplinux_ui_list_append(list, "AV", NULL) == 3);
	assert(fplinux_ui_list_get_focused(list) == 2);
	expect_exclusive(list, 2);
	fplinux_ui_list_delete_all(list);
	assert(fplinux_ui_list_get_focused(list) == SIZE_MAX);
	assert(fplinux_ui_list_size(list) == 0);
	if (insert)
		assert(fplinux_ui_list_append(list, "To", NULL) == 0);
	else
		assert(fplinux_ui_list_insert(list, 0, "To", NULL) == 0);
	assert(fplinux_ui_list_get_focused(list) == 0);
	expect_exclusive(list, 0);
	assert(changes == 0);
	screen_key(KEY_OK);
	assert(changes == 1);
	expect_exclusive(list, 0);
}

int main(int argc, char **argv)
{
	assert(argc == 3);
	language_directory = argv[2];
	lv_init();
	lv_tick_inc(100);
	ui.display = lv_display_create(240, 320);
	assert(ui.display);
	lv_display_set_buffers(ui.display, display_pixels, NULL,
			       sizeof(display_pixels),
			       LV_DISPLAY_RENDER_MODE_FULL);
	lv_display_set_flush_cb(ui.display, flush);
	ui.root = lv_display_get_screen_active(ui.display);
	lv_obj_remove_style_all(ui.root);
	lv_obj_set_style_bg_opa(ui.root, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(ui.root, lv_color_black(), 0);
	ui.theme.foreground = lv_color_white();
	ui.theme.font = fplinux_ui_builtin_font("caret-14");
	ui.theme.font_large = ui.theme.font;
	ui.theme.font_softkey = ui.theme.font;
	assert(ui.theme.font);
	ui.visible = true;
	if (!strcmp(argv[1], "blink"))
		test_blink();
	else if (!strcmp(argv[1], "transitions"))
		test_transitions();
	else if (!strcmp(argv[1], "scroll"))
		test_scroll();
	else if (!strcmp(argv[1], "screen"))
		test_screen();
	else if (!strcmp(argv[1], "deadlines"))
		test_deadlines();
	else if (!strcmp(argv[1], "column"))
		test_column();
	else if (!strcmp(argv[1], "spacing"))
		test_spacing();
	else if (!strcmp(argv[1], "alert-command-ids"))
		test_alert_command_ids();
	else if (!strcmp(argv[1], "alert-text"))
		test_alert_text();
	else if (!strcmp(argv[1], "hidden-date"))
		test_hidden_date();
	else if (!strcmp(argv[1], "exclusive-append"))
		test_exclusive_first_row(false);
	else if (!strcmp(argv[1], "exclusive-insert"))
		test_exclusive_first_row(true);
	else if (!strcmp(argv[1], "menu-pending-visibility"))
		test_menu_visibility(false);
	else if (!strcmp(argv[1], "menu-clear-visibility"))
		test_menu_visibility(true);
	else if (!strcmp(argv[1], "canvas-menu-visibility"))
		test_canvas_menu_visibility();
	else
		abort();
	fplinux_ui_screens_delete(ui.screens);
	lv_display_delete(ui.display);
	lv_deinit();
	printf("%s: ok\n", argv[1]);
	return 0;
}
