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
	const struct fplinux_ui_profile_spec *spec;
	struct fplinux_ui_theme theme;
	struct fplinux_ui_metrics metrics;
	int modal_height;
	bool visible;
};

static struct fplinux_ui ui;
static uint8_t display_pixels[240 * 320 * 2];
static const char *language_directory;
static char requested_glyphs[8][24];
static size_t requested_glyph_count;
static bool missing_back_glyph;

/* Resource lookup is peripheral; menu decoding and drawing stay real. */
const void *fplinux_ui_glyph(struct fplinux_ui *context, const char *name,
			     enum fplinux_ui_glyph_size size)
{
	static const uint8_t masks[][25] = {
		{ 255, 0, 0,   0, 0, 255, 0, 0,	  0, 0, 255, 0, 0,
		  0,   0, 255, 0, 0, 0,	  0, 255, 0, 0, 0,   0 },
		{ 0, 0,	  0, 0, 255, 0, 0,   0, 0, 255, 0, 0,  0,
		  0, 255, 0, 0, 0,   0, 255, 0, 0, 0,	0, 255 },
		{ 0, 0, 255, 0, 0,   0, 0, 255, 0, 0,	0, 0, 255,
		  0, 0, 0,   0, 255, 0, 0, 0,	0, 255, 0, 0 },
	};
	static const lv_image_dsc_t images[] = {
		{ .header = { .magic = LV_IMAGE_HEADER_MAGIC,
			      .cf = LV_COLOR_FORMAT_A8,
			      .w = 5,
			      .h = 5,
			      .stride = 5 },
		  .data_size = 25,
		  .data = masks[0] },
		{ .header = { .magic = LV_IMAGE_HEADER_MAGIC,
			      .cf = LV_COLOR_FORMAT_A8,
			      .w = 5,
			      .h = 5,
			      .stride = 5 },
		  .data_size = 25,
		  .data = masks[1] },
		{ .header = { .magic = LV_IMAGE_HEADER_MAGIC,
			      .cf = LV_COLOR_FORMAT_A8,
			      .w = 5,
			      .h = 5,
			      .stride = 5 },
		  .data_size = 25,
		  .data = masks[2] },
	};
	(void)context;
	assert(size == FPLINUX_UI_GLYPH_ROW);
	assert(requested_glyph_count < 8);
	snprintf(requested_glyphs[requested_glyph_count++], 24, "%s", name);
	if (!strcmp(name, "back") && !missing_back_glyph)
		return &images[0];
	if (!strcmp(name, "help"))
		return &images[1];
	if (!strcmp(name, "backspace"))
		return &images[2];
	errno = ENOENT;
	return NULL;
}

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
	return context->spec;
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

void fplinux_ui_claim_text_key(struct fplinux_ui *context,
			       struct fplinux_ui_text *field,
			       const struct fplinux_ui_key *input)
{
	(void)context;
	(void)field;
	(void)input;
}

void fplinux_ui_cancel_text_key_repeat(struct fplinux_ui *context,
				       const struct fplinux_ui_text *field)
{
	(void)context;
	(void)field;
}

int fplinux_ui_modal_height(const struct fplinux_ui *context)
{
	return context->modal_height ? context->modal_height :
				       fplinux_ui_height(context);
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

bool fplinux_ui_menu_open(struct fplinux_ui *context,
			  enum fplinux_ui_menu_origin origin, const char *title,
			  const struct fplinux_ui_menu_item *items,
			  size_t count, fplinux_ui_menu_cb callback, void *data)
{
	(void)callback;
	(void)data;
	assert(!context->menu);
	context->menu = fplinux_ui_menu_view_create(context, origin, title,
						    items, count);
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

void fplinux_ui_menu_set_position(
	struct fplinux_ui *context,
	const struct fplinux_ui_menu_position *position)
{
	fplinux_ui_menu_view_set_position(context->menu, position);
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
	key(field, FPLINUX_UI_KEY_PHONE, KEY_NUMERIC_STAR, false, NULL);
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

static void test_vertical_scroll(void)
{
	const char *text = "First\nSecond\nThird\nFourth\nFifth\nSixth\nLast";
	struct fplinux_ui_text *field = new_field(true, text);
	lv_obj_t *area = area_at(0);
	int32_t bottom = lv_obj_get_scroll_y(area);

	assert(bottom > 0);
	for (unsigned int direction = 0; direction < 2; ++direction) {
		for (unsigned int row = 0; row < 6; ++row) {
			uint32_t before = lv_textarea_get_cursor_pos(area);
			lv_point_t end;
			int32_t shown;

			key(field, FPLINUX_UI_KEY_PHONE,
			    direction ? KEY_DOWN : KEY_UP, true, NULL);
			assert(direction ? lv_textarea_get_cursor_pos(area) >
						   before :
					   lv_textarea_get_cursor_pos(area) <
						   before);
			lv_obj_get_scroll_end(area, &end);
			shown = lv_obj_get_scroll_y(area);
			assert(end.y == shown);
			advance(32);
			assert(lv_obj_get_scroll_y(area) == shown);
		}
		assert(lv_obj_get_scroll_y(area) == (direction ? bottom : 0));
	}
	assert(!strcmp(fplinux_ui_text_get(field), text));
	{
		lv_obj_t *frame = lv_obj_get_parent(area);
		lv_draw_buf_t *first = snapshot(frame);
		lv_draw_buf_t *other;

		/* Movement has already advanced the clock into a blink cycle. */
		advance(500);
		other = snapshot(frame);
		assert(changed_columns(first, other) == 1);
		lv_draw_buf_destroy(other);
		advance(500);
		other = snapshot(frame);
		assert(changed_columns(first, other) == 0);
		lv_draw_buf_destroy(other);
		lv_draw_buf_destroy(first);
	}
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

static void expect_picker_bounds(int x, int y, int width, int height)
{
	lv_obj_t *layer = lv_display_get_layer_top(ui.display);
	lv_obj_t *popup;
	lv_area_t area;

	assert(lv_obj_get_child_count(layer) == 1);
	popup = lv_obj_get_child(layer, 0);
	lv_obj_update_layout(layer);
	lv_obj_get_coords(popup, &area);
	fprintf(stderr, "picker bounds: %d,%d %dx%d\n", area.x1, area.y1,
		lv_area_get_width(&area), lv_area_get_height(&area));
	assert(area.x1 == x && area.y1 == y);
	assert(lv_area_get_width(&area) == width);
	assert(lv_area_get_height(&area) == height);
}

static void test_symbol_context(const char *context, int modal_height, int x,
				int y, int width, int height)
{
	struct fplinux_ui_screen *screen;
	struct fplinux_ui_text *field = NULL;
	size_t item = 0;
	bool raw = !strncmp(context, "raw-", 4);
	bool form = !strncmp(context, "form", 4);

	ui.modal_height = modal_height;
	lv_display_set_resolution(ui.display, fplinux_ui_width(&ui),
				  modal_height);
	if (raw) {
		screen = fplinux_ui_canvas_new(&ui, "First");
		assert(screen);
		fplinux_ui_push(&ui, screen);
		field = fplinux_ui_text_create(
			&ui, fplinux_ui_canvas_object(screen), false, 0);
		assert(field && fplinux_ui_text_set(field, "AV"));
		lv_obj_set_pos(fplinux_ui_text_object(field), 0,
			       !strcmp(context, "raw-low") ? modal_height / 2 :
							     0);
		advance(0);
		key(field, FPLINUX_UI_KEY_PHONE, KEY_NUMERIC_STAR, true, NULL);
		key(field, FPLINUX_UI_KEY_PHONE, KEY_NUMERIC_STAR, false, NULL);
		assert(fplinux_ui_text_is_picker_open(field));
	} else {
		if (form) {
			screen = fplinux_ui_form_new(&ui, "First");
			assert(screen);
			if (!strcmp(context, "form-scrolled")) {
				for (size_t index = 0; index < 20; ++index)
					assert(fplinux_ui_form_append_string(
						       screen, NULL,
						       "Spacer") == index);
				item = 20;
			}
			assert(fplinux_ui_form_append_text_field(
				       screen, "Second", "AV", 16,
				       FPLINUX_UI_TEXT_ANY) == item);
			fplinux_ui_form_set_focus(screen, item);
		} else {
			screen =
				fplinux_ui_text_box_new(&ui, "First", "AV", 16);
			assert(screen);
		}
		fplinux_ui_push(&ui, screen);
		advance(0);
		if (item) {
			lv_obj_t *content = lv_obj_get_parent(lv_obj_get_parent(
				label_with_text(ui.root, "Second")));

			assert(lv_obj_get_scroll_y(content) > 0);
		}
		screen_key(KEY_NUMERIC_STAR);
	}
	expect_picker_bounds(x, y, width, height);
	if (raw) {
		key(field, FPLINUX_UI_KEY_PHONE, KEY_F14, true, NULL);
		key(field, FPLINUX_UI_KEY_PHONE, KEY_F14, false, NULL);
		assert(!fplinux_ui_text_is_picker_open(field));
		assert(!strcmp(fplinux_ui_text_get(field), "AV"));
		fplinux_ui_text_delete(field);
	} else {
		screen_key(KEY_F14);
		assert(!strcmp(form ? fplinux_ui_form_get_text(screen, item) :
				      fplinux_ui_text_box_get(screen),
			       "AV"));
	}
	assert(lv_obj_get_child_count(lv_display_get_layer_top(ui.display)) ==
	       0);
}

static void test_form_pages(void)
{
	struct fplinux_ui_screen *form = fplinux_ui_form_new(&ui, NULL);
	lv_obj_t *content;
	lv_obj_t *first;
	int32_t page, line;
	const char *choices[] = { "AV", "To" };

	assert(form);
	for (unsigned int row = 0; row < 40; ++row)
		assert(fplinux_ui_form_append_string(
			       form, NULL, row ? "Spacer" : "First") == row);
	fplinux_ui_push(&ui, form);
	advance(0);
	first = label_with_text(ui.root, "First");
	assert(first);
	content = lv_obj_get_parent(first);
	while (content && (!lv_obj_is_scrollable(content) ||
			   lv_obj_get_scroll_bottom(content) <= 0))
		content = lv_obj_get_parent(content);
	assert(content && lv_obj_get_scroll_y(content) == 0);
	/* A page of the owned face leaves one visible line of overlap. */
	line = lv_font_get_line_height(ui.theme.font);
	page = lv_obj_get_content_height(content) - line;
	assert(page > line);
	screen_key(KEY_RIGHT);
	assert(lv_obj_get_scroll_y(content) == page);
	screen_key(KEY_LEFT);
	assert(lv_obj_get_scroll_y(content) == 0);
	screen_key(KEY_LEFT);
	assert(lv_obj_get_scroll_y(content) == 0);

	form = fplinux_ui_form_new(&ui, NULL);
	assert(form);
	assert(fplinux_ui_form_append_choice(form, NULL,
					     FPLINUX_UI_CHOICE_EXCLUSIVE,
					     choices, 2) == 0);
	for (unsigned int row = 0; row < 40; ++row)
		assert(fplinux_ui_form_append_string(form, NULL, "Spacer") ==
		       row + 1);
	fplinux_ui_replace(&ui, form);
	advance(0);
	assert(fplinux_ui_form_get_focus(form) == 0);
	screen_key(KEY_RIGHT);
	assert(fplinux_ui_form_get_focus(form) == SIZE_MAX);
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
	assert(fplinux_ui_menu_open(&ui, FPLINUX_UI_MENU_ORIGIN_LEFT, NULL,
				    &action, 1, NULL, NULL));
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

static void test_alert_style(void)
{
	static const struct {
		enum fplinux_ui_alert_kind kind;
		uint32_t frame;
		uint32_t title;
	} cases[] = {
		{ FPLINUX_UI_ALERT_INFO, 0xff2468ac, 0xffffffff },
		{ FPLINUX_UI_ALERT_CONFIRMATION, 0xff2468ac, 0xffffffff },
		{ FPLINUX_UI_ALERT_WARNING, 0xffe08020, 0xffe08020 },
		{ FPLINUX_UI_ALERT_ERROR, 0xffc02040, 0xffc02040 },
	};
	struct fplinux_ui_screen *below = fplinux_ui_form_new(&ui, NULL);

	assert(below);
	fplinux_ui_push(&ui, below);
	ui.theme.focus = lv_color_hex(0x2468ac);
	ui.theme.warning = lv_color_hex(0xe08020);
	ui.theme.danger = lv_color_hex(0xc02040);
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
		struct fplinux_ui_screen *alert = fplinux_ui_alert_new(
			&ui, cases[i].kind, "To", "First", 0);
		lv_obj_t *title;
		lv_obj_t *box;

		assert(alert);
		fplinux_ui_push(&ui, alert);
		advance(0);
		title = label_with_text(ui.root, "To");
		assert(title);
		box = lv_obj_get_parent(title);
		assert(lv_color_to_u32(lv_obj_get_style_border_color(box, 0)) ==
		       cases[i].frame);
		assert(lv_color_to_u32(lv_obj_get_style_text_color(title, 0)) ==
		       cases[i].title);
		assert(lv_color_to_u32(fplinux_ui_alert_color(
			       &ui, cases[i].kind)) == cases[i].frame);
		assert(fplinux_ui_pop(&ui));
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

static void test_choice_ownership(enum fplinux_ui_choice_kind kind)
{
	struct fplinux_ui_screen *form = fplinux_ui_form_new(&ui, NULL);
	char first[] = "AV", second[] = "To", third[] = "ij";
	const char *choices[] = { first, second, third };

	assert(form);
	assert(fplinux_ui_form_append_choice(form, "First", kind, choices, 3) ==
	       0);
	strcpy(first, "xx");
	strcpy(second, "xx");
	strcpy(third, "xx");
	fplinux_ui_push(&ui, form);
	advance(0);
	assert(label_with_text(ui.root, "AV"));
	assert(!label_with_text(ui.root, "xx"));
	if (kind == FPLINUX_UI_CHOICE_MULTIPLE) {
		assert(label_with_text(ui.root, "To"));
		assert(label_with_text(ui.root, "ij"));
		fplinux_ui_form_set_chosen(form, 0, 0, true);
		fplinux_ui_form_set_chosen(form, 0, 2, true);
		assert(fplinux_ui_form_is_chosen(form, 0, 0));
		assert(!fplinux_ui_form_is_chosen(form, 0, 1));
		assert(fplinux_ui_form_is_chosen(form, 0, 2));
	} else {
		assert(fplinux_ui_form_get_choice(form, 0) == 0);
		fplinux_ui_form_set_choice(form, 0, 2);
		assert(fplinux_ui_form_get_choice(form, 0) == 2);
		assert(!fplinux_ui_form_is_chosen(form, 0, 0));
		assert(fplinux_ui_form_is_chosen(form, 0, 2));
		fplinux_ui_form_set_choice(form, 0, 3);
		assert(fplinux_ui_form_get_choice(form, 0) == 2);
		if (kind == FPLINUX_UI_CHOICE_POPUP) {
			assert(label_with_text(ui.root, "ij"));
			screen_key(KEY_OK);
			assert(fplinux_ui_current(&ui) != form);
			assert(label_with_text(ui.root, "AV"));
			assert(label_with_text(ui.root, "To"));
			assert(label_with_text(ui.root, "ij"));
			screen_key(KEY_UP);
			screen_key(KEY_OK);
			assert(fplinux_ui_current(&ui) == form);
			assert(fplinux_ui_form_get_choice(form, 0) == 1);
			assert(label_with_text(ui.root, "To"));
		} else {
			assert(label_with_text(ui.root, "To"));
			assert(label_with_text(ui.root, "ij"));
		}
	}
	assert(fplinux_ui_form_delete(form, 0));
	assert(fplinux_ui_form_append_choice(form, NULL, kind, choices, 3) ==
	       0);
	fplinux_ui_form_delete_all(form);
}

static void test_popup_choice_height(bool overflow)
{
	const char *choices[] = { "First", "Second", "Third", "Fourth",
				  "Fifth", "Sixth",  "Last",  "Read only" };
	struct fplinux_ui_screen *form = fplinux_ui_form_new(&ui, "AV");
	lv_obj_t *popup, *row, *content, *box;
	lv_area_t row_area, content_area, box_area;

	assert(fplinux_ui_form_append_choice(form, "To",
					     FPLINUX_UI_CHOICE_POPUP, choices,
					     overflow ? 8 : 7) == 0);
	fplinux_ui_push(&ui, form);
	screen_key(KEY_OK);
	assert(fplinux_ui_current(&ui) != form);
	popup = lv_obj_get_child(ui.root, -1);
	row = lv_obj_get_parent(label_with_text(popup, "First"));
	content = lv_obj_get_parent(lv_obj_get_parent(row));
	box = lv_obj_get_parent(content);
	lv_obj_update_layout(box);
	lv_obj_get_coords(row, &row_area);
	lv_obj_get_coords(content, &content_area);
	lv_obj_get_coords(box, &box_area);
	assert(lv_area_get_height(&box_area) == (overflow ? 304 : 284));
	assert(row_area.y1 == content_area.y1);
	assert(lv_area_get_height(&content_area) == (overflow ? 272 : 252));
	assert(content_area.y2 == box_area.y2 - 2);
	if (!overflow) {
		lv_obj_t *last =
			lv_obj_get_parent(label_with_text(popup, "Last"));
		lv_area_t last_area;

		lv_obj_get_coords(last, &last_area);
		assert(last_area.y2 == content_area.y2);
	}
}

static lv_obj_t *image_under(lv_obj_t *object)
{
	if (lv_obj_check_type(object, &lv_image_class))
		return object;
	for (uint32_t index = 0; index < lv_obj_get_child_count(object);
	     ++index) {
		lv_obj_t *image =
			image_under(lv_obj_get_child(object, (int32_t)index));

		if (image)
			return image;
	}
	return NULL;
}

static bool glyph_was_requested(const char *name)
{
	for (size_t index = 0; index < requested_glyph_count; ++index)
		if (!strcmp(requested_glyphs[index], name))
			return true;
	return false;
}

static void expect_command_icon(const char *label, int column, bool colour)
{
	lv_obj_t *layer = lv_display_get_layer_top(ui.display);
	lv_obj_t *text = label_with_text(layer, label);
	lv_obj_t *row, *image;
	lv_area_t row_area, image_area;
	lv_draw_buf_t *pixels;
	const uint8_t *ink, *clear;

	assert(text);
	row = lv_obj_get_parent(text);
	image = image_under(row);
	assert(image);
	lv_obj_get_coords(image, &image_area);
	lv_obj_get_coords(row, &row_area);
	assert(lv_area_get_width(&image_area) == 5);
	assert(lv_area_get_height(&image_area) == 5);
	pixels = snapshot(row);
	ink = lv_draw_buf_goto_xy(
		pixels, (uint32_t)(image_area.x1 - row_area.x1 + column),
		(uint32_t)(image_area.y1 - row_area.y1 + 2));
	if (colour) {
		assert(ink[0] == 0x20 && ink[1] == 0x90 && ink[2] == 0x50);
	} else {
		assert(ink[0] == 255 && ink[1] == 255 && ink[2] == 255);
		clear = lv_draw_buf_goto_xy(
			pixels,
			(uint32_t)(image_area.x1 - row_area.x1 +
				   (column ? 0 : 4)),
			(uint32_t)(image_area.y1 - row_area.y1 + 2));
		assert(clear[0] == 0 && clear[1] == 0 && clear[2] == 0);
	}
	lv_draw_buf_destroy(pixels);
}

static void test_command_glyphs(const char *scenario)
{
	static lv_color32_t colour_pixels[25];
	const lv_image_dsc_t override = {
		.header = { .magic = LV_IMAGE_HEADER_MAGIC,
			    .cf = LV_COLOR_FORMAT_ARGB8888,
			    .w = 5,
			    .h = 5,
			    .stride = 20 },
		.data_size = sizeof(colour_pixels),
		.data = (const uint8_t *)colour_pixels,
	};
	struct fplinux_ui_command back = {
		.id = 31,
		.type = FPLINUX_UI_COMMAND_BACK,
	};
	const struct fplinux_ui_command help = {
		.id = 41,
		.type = FPLINUX_UI_COMMAND_HELP,
	};
	const struct fplinux_ui_command cancel = {
		.id = 51,
		.type = FPLINUX_UI_COMMAND_CANCEL,
		.keep_visible = true,
		.priority = -1,
	};
	struct fplinux_ui_screen *screen;
	bool explicit = !strcmp(scenario, "explicit");
	bool clear = !strcmp(scenario, "clear");
	lv_obj_t *text;

	for (size_t index = 0; index < 25; ++index)
		colour_pixels[index] = (lv_color32_t){
			.red = 0x50,
			.green = 0x90,
			.blue = 0x20,
			.alpha = 255,
		};
	if (explicit)
		back.icon = &override;
	missing_back_glyph = !strcmp(scenario, "missing");
	ui.theme.popup = lv_color_black();
	ui.theme.accent = lv_color_black();
	ui.theme.accent_text = lv_color_white();
	screen = fplinux_ui_text_box_new(&ui, NULL, "AV", 16);
	assert(screen);
	assert(fplinux_ui_screen_add_command(screen, &back));
	assert(fplinux_ui_screen_add_command(screen, &help));
	if (clear)
		assert(fplinux_ui_screen_add_command(screen, &cancel));
	fplinux_ui_push(&ui, screen);
	advance(0);
	screen_key(KEY_F13);
	if (missing_back_glyph) {
		text = label_with_text(lv_display_get_layer_top(ui.display),
				       "Back");
		assert(text && !image_under(lv_obj_get_parent(text)));
		assert(glyph_was_requested("back"));
	} else {
		expect_command_icon("Back", 0, explicit);
		assert(glyph_was_requested("back") == !explicit);
	}
	expect_command_icon("Help", 4, false);
	assert(glyph_was_requested("help"));
	if (clear) {
		expect_command_icon("Clear", 2, false);
		assert(glyph_was_requested("backspace"));
	}
	fplinux_ui_menu_close(&ui);
}

static void expect_icon_pixels(const char *text, bool selected, bool tile,
			       bool glyph, int image_size)
{
	lv_obj_t *row = lv_obj_get_parent(label_with_text(ui.root, text));
	lv_obj_t *image = image_under(row);
	lv_obj_t *box = lv_obj_get_parent(image);
	lv_draw_buf_t *frame;
	lv_area_t row_area, box_area, image_area;
	const uint8_t *pixel;

	lv_obj_update_layout(row);
	lv_obj_get_coords(row, &row_area);
	lv_obj_get_coords(box, &box_area);
	lv_obj_get_coords(image, &image_area);
	assert(lv_area_get_width(&image_area) == image_size);
	assert(lv_area_get_height(&image_area) == image_size);
	frame = snapshot(row);
	pixel = lv_draw_buf_goto_xy(frame, box_area.x1 - row_area.x1 - 1,
				    (box_area.y1 + box_area.y2) / 2 -
					    row_area.y1);
	if (selected && tile)
		assert(pixel[2] == 0xf0 && pixel[1] == 0xd0 &&
		       pixel[0] == 0x20);
	else if (selected)
		assert(pixel[2] == 0x20 && pixel[1] == 0x40 &&
		       pixel[0] == 0x60);
	else
		assert(pixel[2] == 0 && pixel[1] == 0 && pixel[0] == 0);
	pixel = lv_draw_buf_goto_xy(
		frame, (image_area.x1 + image_area.x2) / 2 - row_area.x1,
		(image_area.y1 + image_area.y2) / 2 - row_area.y1);
	if (glyph) {
		uint8_t expected = selected ? 0xff : 0x33;

		assert(pixel[0] == expected);
		assert(pixel[1] == (selected ? 0xff : 0x22));
		assert(pixel[2] == (selected ? 0xff : 0x11));
	} else {
		assert(pixel[2] == 0xd0 && pixel[1] == 0x30 &&
		       pixel[0] == 0x50);
	}
	lv_draw_buf_destroy(frame);
}

struct icon_rows {
	lv_image_dsc_t colour;
	lv_image_dsc_t glyph;
	char labels[40][16];
};

static bool icon_row(struct fplinux_ui *context, struct fplinux_ui_screen *list,
		     size_t index, struct fplinux_ui_virtual_row *row,
		     void *data)
{
	struct icon_rows *icons = data;

	(void)context;
	(void)list;
	row->text = icons->labels[index];
	row->icon = index % 3 == 2 ? &icons->glyph : &icons->colour;
	row->icon_kind = index % 3 != 1 ? FPLINUX_UI_LIST_ICON_APP_TILE :
					  FPLINUX_UI_LIST_ICON_IMAGE;
	return true;
}

static void test_list_icons(bool is_virtual)
{
	lv_color32_t colour[24 * 24];
	uint8_t alpha[8 * 8];
	struct icon_rows icons = {
		.colour = {
			.header = { .magic = LV_IMAGE_HEADER_MAGIC,
				    .cf = LV_COLOR_FORMAT_ARGB8888,
				    .w = 24, .h = 24, .stride = 24 * 4 },
			.data_size = sizeof(colour),
			.data = (const uint8_t *)colour,
		},
		.glyph = {
			.header = { .magic = LV_IMAGE_HEADER_MAGIC,
				    .cf = LV_COLOR_FORMAT_A8,
				    .w = 8, .h = 8, .stride = 8 },
			.data_size = sizeof(alpha),
			.data = alpha,
		},
	};
	struct fplinux_ui_screen *list;
	const char *first = is_virtual ? icons.labels[0] : "First";
	const char *second = is_virtual ? icons.labels[1] : "Second";
	const char *third = is_virtual ? icons.labels[2] : "Third";

	for (size_t index = 0; index < 40; ++index)
		snprintf(icons.labels[index], sizeof(icons.labels[index]),
			 "First %zu", index);
	for (size_t index = 0; index < 24 * 24; ++index)
		colour[index] = (lv_color32_t){
			.red = 0xd0, .green = 0x30, .blue = 0x50, .alpha = 0xff
		};
	memset(alpha, 0xff, sizeof(alpha));
	ui.theme.foreground = lv_color_hex(0x112233);
	ui.theme.accent = lv_color_hex(0x204060);
	ui.theme.accent_text = lv_color_white();
	ui.theme.tile_ring = lv_color_hex(0xf0d020);
	if (is_virtual) {
		list = fplinux_ui_list_new_virtual(&ui, NULL,
						   FPLINUX_UI_LIST_IMPLICIT, 40,
						   icon_row, &icons);
	} else {
		list = fplinux_ui_list_new(&ui, NULL, FPLINUX_UI_LIST_IMPLICIT);
		assert(fplinux_ui_list_append(list, "First", NULL) == 0);
		assert(fplinux_ui_list_append(list, "Second", NULL) == 1);
		assert(fplinux_ui_list_append(list, "Third", NULL) == 2);
		assert(fplinux_ui_list_set_icon(list, 0, &icons.colour,
						FPLINUX_UI_LIST_ICON_APP_TILE));
		assert(fplinux_ui_list_set_icon(list, 1, &icons.colour,
						FPLINUX_UI_LIST_ICON_IMAGE));
		assert(fplinux_ui_list_set_icon(list, 2, &icons.glyph,
						FPLINUX_UI_LIST_ICON_APP_TILE));
		assert(!fplinux_ui_list_set_icon(
			list, 0, &icons.colour,
			(enum fplinux_ui_list_icon_kind) - 1));
		assert(errno == EINVAL);
		memset(colour, 0, sizeof(colour));
	}
	assert(list);
	fplinux_ui_push(&ui, list);
	advance(0);
	expect_icon_pixels(first, true, true, false, 24);
	fplinux_ui_list_set_focused(list, 1);
	expect_icon_pixels(first, false, true, false, 24);
	expect_icon_pixels(second, true, false, false, 24);
	fplinux_ui_list_set_focused(list, 2);
	expect_icon_pixels(third, true, false, true, 8);
	fplinux_ui_list_set_focused(list, 0);
	expect_icon_pixels(third, false, false, true, 8);
	if (is_virtual) {
		for (size_t index = 24; index < 30; ++index) {
			fplinux_ui_list_set_focused(list, index);
			assert(fplinux_ui_list_get_first_visible(list) > 0);
			expect_icon_pixels(icons.labels[index], true,
					   index % 3 == 0, index % 3 == 2,
					   index % 3 == 2 ? 8 : 24);
		}
	} else {
		assert(fplinux_ui_list_insert(list, 0, "Fourth", NULL) == 0);
		expect_icon_pixels("First", true, true, false, 24);
		assert(fplinux_ui_list_delete(list, 0));
		expect_icon_pixels("First", true, true, false, 24);
		assert(fplinux_ui_list_set_icon(list, 0, NULL,
						FPLINUX_UI_LIST_ICON_IMAGE));
		assert(!image_under(
			lv_obj_get_parent(label_with_text(ui.root, "First"))));
	}
}

struct screen_filter_state {
	unsigned int presses;
	unsigned int releases;
	bool cover;
	bool replace;
	bool consume;
};

static bool screen_filter(struct fplinux_ui *context,
			  struct fplinux_ui_screen *screen,
			  const struct fplinux_ui_key *input, void *data)
{
	struct screen_filter_state *state = data;

	(void)screen;
	if (input->code != KEY_DOWN && input->code != KEY_NUMERIC_2)
		return false;
	if (!input->pressed) {
		++state->releases;
		/* Ownership does not depend on the filter accepting its release. */
		return false;
	}
	++state->presses;
	if (state->cover || state->replace) {
		struct fplinux_ui_screen *next =
			fplinux_ui_canvas_new(context, NULL);

		assert(next);
		if (state->replace)
			fplinux_ui_replace(context, next);
		else
			fplinux_ui_push(context, next);
	}
	return state->consume;
}

static void test_screen_filter(const char *kind)
{
	struct screen_filter_state state = { .consume = true };
	struct fplinux_ui_screen *screen;
	struct fplinux_ui_key input = {
		.source = FPLINUX_UI_KEY_PHONE,
		.code = KEY_NUMERIC_2,
		.pressed = true,
	};
	bool is_list = !strcmp(kind, "list");
	bool is_form = !strcmp(kind, "form");
	bool is_box = !strcmp(kind, "text-box");
	bool is_alert = !strcmp(kind, "alert");

	if (is_list) {
		screen = fplinux_ui_list_new(&ui, NULL,
					     FPLINUX_UI_LIST_IMPLICIT);
		assert(fplinux_ui_list_append(screen, "First", NULL) == 0);
		assert(fplinux_ui_list_append(screen, "Second", NULL) == 1);
		input.code = KEY_DOWN;
	} else if (is_form) {
		screen = fplinux_ui_form_new(&ui, NULL);
		assert(fplinux_ui_form_append_text_field(screen, NULL, "AV", 8,
							 FPLINUX_UI_TEXT_ANY) ==
		       0);
	} else if (is_box) {
		screen = fplinux_ui_text_box_new(&ui, NULL, "AV", 8);
	} else if (is_alert) {
		screen = fplinux_ui_alert_new(&ui, FPLINUX_UI_ALERT_INFO, NULL,
					      "AV", 0);
	} else {
		screen = fplinux_ui_canvas_new(&ui, NULL);
	}
	assert(screen);
	fplinux_ui_screen_set_key_filter(screen, screen_filter, &state);
	fplinux_ui_push(&ui, screen);
	assert(fplinux_ui_screens_key(ui.screens, &input) ==
	       FPLINUX_UI_KEY_HANDLED);
	input.pressed = false;
	assert(fplinux_ui_screens_key(ui.screens, &input) ==
	       FPLINUX_UI_KEY_HANDLED);
	assert(state.presses == 1 && state.releases == 1);
	if (is_list) {
		assert(fplinux_ui_list_get_focused(screen) == 0);
		fplinux_ui_screen_set_key_filter(screen, NULL, NULL);
		screen_key(KEY_DOWN);
		assert(fplinux_ui_list_get_focused(screen) == 1);
	} else if (is_form) {
		assert(!strcmp(fplinux_ui_form_get_text(screen, 0), "AV"));
	} else if (is_box) {
		assert(!strcmp(fplinux_ui_text_box_get(screen), "AV"));
	}
}

static void test_filter_screen_change(bool replace, bool consume)
{
	struct screen_filter_state state = {
		.cover = !replace,
		.replace = replace,
		.consume = consume,
	};
	struct fplinux_ui_screen *screen =
		fplinux_ui_list_new(&ui, NULL, FPLINUX_UI_LIST_IMPLICIT);
	struct fplinux_ui_key input = {
		.source = FPLINUX_UI_KEY_KEYBOARD,
		.code = KEY_DOWN,
		.pressed = true,
	};

	assert(screen);
	assert(fplinux_ui_list_append(screen, "First", NULL) == 0);
	fplinux_ui_screen_set_key_filter(screen, screen_filter, &state);
	fplinux_ui_push(&ui, screen);
	assert(fplinux_ui_screens_key(ui.screens, &input) ==
	       FPLINUX_UI_KEY_HANDLED);
	assert(state.presses == 1);
	assert(fplinux_ui_current(&ui) != screen);
	if (consume) {
		/* A held key must not repeat into a covered or replaced screen. */
		assert(fplinux_ui_screens_key(ui.screens, &input) ==
		       FPLINUX_UI_KEY_HANDLED);
		assert(state.presses == 1);
	}
	input.pressed = false;
	assert(fplinux_ui_screens_key(ui.screens, &input) ==
	       FPLINUX_UI_KEY_HANDLED);
	assert(state.releases == (unsigned int)(consume && !replace));
}

struct list_listener_state {
	unsigned int focus_calls;
	unsigned int value_calls;
	size_t focused;
	bool replace;
};

static void list_value_changed(struct fplinux_ui *context,
			       struct fplinux_ui_screen *list, size_t index,
			       void *data)
{
	struct list_listener_state *state = data;

	(void)context;
	(void)list;
	assert(index == 1);
	++state->value_calls;
}

static void list_focus_changed(struct fplinux_ui *context,
			       struct fplinux_ui_screen *list, size_t index,
			       void *data)
{
	struct list_listener_state *state = data;

	assert(fplinux_ui_list_get_focused(list) == index);
	++state->focus_calls;
	state->focused = index;
	if (state->replace)
		fplinux_ui_replace(context,
				   fplinux_ui_canvas_new(context, NULL));
}

static void test_list_focus_listener(void)
{
	struct list_listener_state state = { 0 };
	struct fplinux_ui_screen *list =
		fplinux_ui_list_new(&ui, NULL, FPLINUX_UI_LIST_EXCLUSIVE);

	assert(list);
	assert(fplinux_ui_list_append(list, "First", NULL) == 0);
	assert(fplinux_ui_list_append(list, "Second", NULL) == 1);
	assert(fplinux_ui_list_append(list, "Third", NULL) == 2);
	fplinux_ui_screen_set_listener(list, NULL, list_value_changed, &state);
	fplinux_ui_list_set_focus_listener(list, list_focus_changed, &state);
	fplinux_ui_push(&ui, list);
	screen_key(KEY_DOWN);
	assert(state.focus_calls == 1 && state.focused == 1);
	assert(state.value_calls == 0);
	assert(fplinux_ui_list_is_selected(list, 0));
	assert(!fplinux_ui_list_is_selected(list, 1));
	screen_key(KEY_OK);
	assert(state.focus_calls == 1 && state.value_calls == 1);
	assert(fplinux_ui_list_is_selected(list, 1));
	fplinux_ui_list_set_focused(list, 2);
	assert(fplinux_ui_list_insert(list, 0, "Fourth", NULL) == 0);
	assert(fplinux_ui_list_delete(list, 0));
	assert(fplinux_ui_list_get_focused(list) == 2);
	assert(state.focus_calls == 1 && state.value_calls == 1);
	state.replace = true;
	screen_key(KEY_UP);
	assert(state.focus_calls == 2 && state.focused == 1);
	assert(state.value_calls == 1);
	assert(fplinux_ui_current(&ui) != list);
}

static unsigned int visible_scrollbars(lv_obj_t *object)
{
	lv_area_t horizontal, vertical, viewport;
	unsigned int count = 0;

	if (!lv_obj_is_visible(object))
		return 0;
	lv_obj_get_scrollbar_area(object, &horizontal, &vertical);
	assert(horizontal.x2 < horizontal.x1);
	if (vertical.x2 >= vertical.x1 && vertical.y2 >= vertical.y1) {
		lv_obj_get_coords(object, &viewport);
		assert(lv_area_get_width(&vertical) == 2);
		assert(viewport.x2 - vertical.x2 == 1);
		if (lv_obj_get_scroll_y(object) == 0)
			assert(vertical.y1 - viewport.y1 == 1);
		if (lv_obj_get_scroll_bottom(object) == 0)
			assert(viewport.y2 - vertical.y2 == 1);
		assert(lv_color_to_u32(lv_obj_get_style_bg_color(
			       object, LV_PART_SCROLLBAR)) == 0xff2468acU);
		assert(lv_obj_get_style_bg_opa(object, LV_PART_SCROLLBAR) ==
		       LV_OPA_COVER);
		++count;
	}
	for (uint32_t index = 0; index < lv_obj_get_child_count(object);
	     ++index)
		count += visible_scrollbars(
			lv_obj_get_child(object, (int32_t)index));
	return count;
}

static void expect_scrollbars(unsigned int expected)
{
	unsigned int count;

	lv_obj_update_layout(ui.root);
	count = visible_scrollbars(ui.root);
	count += visible_scrollbars(lv_display_get_layer_top(ui.display));
	if (count != expected)
		fprintf(stderr, "idle scrollbars: expected %u, observed %u\n",
			expected, count);
	assert(count == expected);
}

static void test_scrollbars(void)
{
	struct fplinux_ui_screen *screen;
	struct fplinux_ui_menu_item items[40];
	struct fplinux_ui_text *field;
	char text[256] = "";

	ui.theme.focus = lv_color_hex(0x2468ac);
	screen = fplinux_ui_list_new(&ui, NULL, FPLINUX_UI_LIST_IMPLICIT);
	assert(screen);
	assert(fplinux_ui_list_append(screen, "AV", NULL) == 0);
	fplinux_ui_push(&ui, screen);
	expect_scrollbars(0);
	for (size_t index = 1; index < 40; ++index)
		assert(fplinux_ui_list_append(screen, "AV", NULL) == index);
	expect_scrollbars(1);
	fplinux_ui_list_set_focused(screen, 39);
	expect_scrollbars(1);
	fplinux_ui_list_delete_all(screen);
	assert(fplinux_ui_list_append(screen, "AV", NULL) == 0);
	expect_scrollbars(0);

	screen = fplinux_ui_form_new(&ui, NULL);
	assert(screen);
	assert(fplinux_ui_form_append_string(screen, NULL, "AV") == 0);
	fplinux_ui_replace(&ui, screen);
	expect_scrollbars(0);
	for (size_t index = 1; index < 40; ++index)
		assert(fplinux_ui_form_append_string(screen, NULL, "AV") ==
		       index);
	expect_scrollbars(1);
	fplinux_ui_form_delete_all(screen);
	assert(fplinux_ui_form_append_string(screen, NULL, "AV") == 0);
	expect_scrollbars(0);

	screen = fplinux_ui_canvas_new(&ui, NULL);
	assert(screen);
	fplinux_ui_replace(&ui, screen);
	for (size_t index = 0; index < 40; ++index)
		items[index] = (struct fplinux_ui_menu_item){
			.action = (int)index,
			.text = "AV",
		};
	assert(fplinux_ui_menu_open(&ui, FPLINUX_UI_MENU_ORIGIN_LEFT, NULL,
				    items, 1, NULL, NULL));
	expect_scrollbars(0);
	fplinux_ui_menu_close(&ui);
	assert(fplinux_ui_menu_open(&ui, FPLINUX_UI_MENU_ORIGIN_LEFT, NULL,
				    items, 40, NULL, NULL));
	expect_scrollbars(1);
	fplinux_ui_menu_close(&ui);
	expect_scrollbars(0);

	field = fplinux_ui_text_create(&ui, fplinux_ui_canvas_object(screen),
				       true, 0);
	assert(field);
	assert(fplinux_ui_text_set_constraint(field,
					      FPLINUX_UI_TEXT_UNEDITABLE));
	assert(fplinux_ui_text_set(field, "AV"));
	expect_scrollbars(0);
	for (size_t index = 0; index < 40; ++index)
		strcat(text, "AV\n");
	assert(fplinux_ui_text_set(field, text));
	expect_scrollbars(1);
	key(field, FPLINUX_UI_KEY_PHONE, KEY_RIGHT, true, NULL);
	expect_scrollbars(1);
	assert(fplinux_ui_text_set(field, "AV"));
	expect_scrollbars(0);
	fplinux_ui_text_delete(field);
}

static void expect_label_insets(lv_obj_t *label, lv_obj_t *viewport, int left,
				int right)
{
	lv_area_t text, view;

	lv_obj_get_content_coords(label, &text);
	lv_obj_get_coords(viewport, &view);
	assert(text.x1 - view.x1 == left);
	assert(view.x2 - text.x2 == right);
}

static void test_form_content_insets(int margin)
{
	static const uint8_t pixels[320];
	const lv_image_dsc_t source = {
		.header = {
			.magic = LV_IMAGE_HEADER_MAGIC,
			.cf = LV_COLOR_FORMAT_A8,
			.w = 320,
			.h = 1,
			.stride = 320,
		},
		.data_size = sizeof(pixels),
		.data = pixels,
	};
	const char *choices[] = { "AV", "To" };
	struct fplinux_ui_screen *form = fplinux_ui_form_new(&ui, NULL);
	lv_obj_t *heading, *value, *content, *choice, *image;
	lv_area_t text, horizontal, vertical;

	assert(form);
	assert(fplinux_ui_form_append_string(form, "First", "Second") == 0);
	fplinux_ui_push(&ui, form);
	advance(0);
	heading = label_with_text(ui.root, "First");
	value = label_with_text(ui.root, "Second");
	assert(heading && value);
	content = lv_obj_get_parent(lv_obj_get_parent(heading));
	expect_label_insets(heading, content, margin, margin);
	expect_label_insets(value, content, margin, margin);
	lv_obj_get_scrollbar_area(content, &horizontal, &vertical);
	assert(vertical.x2 < vertical.x1);

	assert(fplinux_ui_form_append_choice(form, "Third",
					     FPLINUX_UI_CHOICE_EXCLUSIVE,
					     choices, 2) == 1);
	assert(fplinux_ui_form_append_image(form, "Last", &source) == 2);
	for (size_t index = 3; index < 40; ++index)
		assert(fplinux_ui_form_append_string(form, NULL, "Spacer") ==
		       index);
	advance(0);
	choice = label_with_text(ui.root, "Third");
	image = label_with_text(ui.root, "Last");
	assert(choice && image);
	image = lv_obj_get_child(lv_obj_get_parent(image), 1);
	assert(lv_obj_check_type(image, &lv_image_class));
	expect_label_insets(heading, content, margin, margin + 4);
	expect_label_insets(value, content, margin, margin + 4);
	expect_label_insets(choice, content, margin, margin + 4);
	lv_obj_get_coords(image, &text);
	assert(lv_area_get_width(&text) ==
	       fplinux_ui_width(&ui) - 2 * margin - 4);
	lv_obj_get_scrollbar_area(content, &horizontal, &vertical);
	assert(lv_area_get_width(&vertical) == 2);
	lv_obj_get_content_coords(value, &text);
	assert(text.x2 < vertical.x1);
}

static void expect_form_image(lv_obj_t *image, int width, int height, bool mask,
			      bool link)
{
	lv_obj_t *frame = lv_obj_get_parent(image);
	lv_draw_buf_t *pixels;
	const uint8_t *centre;
	lv_area_t area, outer;

	advance(0);
	lv_obj_get_coords(image, &area);
	lv_obj_get_coords(frame, &outer);
	fprintf(stderr, "form image: %dx%d frame: %dx%d\n",
		lv_area_get_width(&area), lv_area_get_height(&area),
		lv_area_get_width(&outer), lv_area_get_height(&outer));
	assert(lv_area_get_width(&area) == width);
	assert(lv_area_get_height(&area) == height);
	assert(area.x1 >= outer.x1 && area.x2 <= outer.x2);
	assert(area.y1 >= outer.y1 && area.y2 <= outer.y2);
	pixels = snapshot(image);
	centre = lv_draw_buf_goto_xy(pixels, (uint32_t)width / 2,
				     (uint32_t)height / 2);
	if (mask) {
		assert(centre[0] == (link ? 0x60 : 0x33));
		assert(centre[1] == (link ? 0x50 : 0x22));
		assert(centre[2] == (link ? 0x40 : 0x11));
	} else {
		assert(centre[0] == 0x20 && centre[1] == 0x50 &&
		       centre[2] == 0x90);
	}
	lv_draw_buf_destroy(pixels);
}

static void test_form_image_shape(bool mask, bool overflow)
{
	static uint8_t mask_pixels[48 * 48];
	static lv_color32_t colour_pixels[400 * 200];
	const struct fplinux_ui_command open = {
		.id = 41,
		.type = FPLINUX_UI_COMMAND_ITEM,
		.label = "Open",
	};
	int width = overflow ? 400 : mask ? 48 : 96;
	int height = overflow ? 200 : 48;
	lv_image_dsc_t source = {
		.header = {
			.magic = LV_IMAGE_HEADER_MAGIC,
			.cf = mask ? LV_COLOR_FORMAT_A8 : LV_COLOR_FORMAT_ARGB8888,
			.w = (uint32_t)width,
			.h = (uint32_t)height,
			.stride = (uint32_t)width * (mask ? 1 : 4),
		},
		.data_size = (uint32_t)(width * height * (mask ? 1 : 4)),
		.data = mask ? mask_pixels : (const uint8_t *)colour_pixels,
	};
	struct fplinux_ui_screen *form = fplinux_ui_form_new(&ui, NULL);
	lv_obj_t *image, *box, *content;
	lv_area_t frame, view, horizontal, vertical;
	const void *original_source = NULL;
	lv_image_header_t original_header;

	memset(mask_pixels, 0xff, sizeof(mask_pixels));
	for (size_t index = 0;
	     index < sizeof(colour_pixels) / sizeof(colour_pixels[0]); ++index)
		colour_pixels[index] = (lv_color32_t){
			.red = 0x90,
			.green = 0x50,
			.blue = 0x20,
			.alpha = 0xff,
		};
	ui.theme.foreground = lv_color_hex(0x112233);
	ui.theme.accent = lv_color_hex(0x405060);
	ui.theme.accent_text = ui.theme.accent;
	ui.theme.focus = ui.theme.accent;
	assert(form);
	assert(fplinux_ui_form_append_image(form, "First", &source) == 0);
	if (overflow) {
		image = image_under(
			lv_obj_get_parent(label_with_text(ui.root, "First")));
		assert(image);
		original_source = lv_image_get_src(image);
		assert(lv_image_decoder_get_info(original_source,
						 &original_header) ==
		       LV_RESULT_OK);
		assert(original_header.w == 224 && original_header.h == 112);
	}
	assert(fplinux_ui_form_set_appearance(form, 0,
					      FPLINUX_UI_APPEARANCE_HYPERLINK));
	assert(fplinux_ui_form_add_item_command(form, 0, &open));
	if (overflow)
		for (size_t index = 1; index < 30; ++index)
			assert(fplinux_ui_form_append_string(
				       form, NULL, "Spacer") == index);
	fplinux_ui_push(&ui, form);
	advance(0);
	box = lv_obj_get_parent(label_with_text(ui.root, "First"));
	image = image_under(box);
	assert(image);
	content = lv_obj_get_parent(box);
	if (overflow) {
		lv_obj_get_scrollbar_area(content, &horizontal, &vertical);
		assert(lv_area_get_width(&vertical) == 2);
		lv_obj_get_coords(lv_obj_get_parent(image), &frame);
		lv_obj_get_content_coords(box, &view);
		fprintf(stderr,
			"framed overflow: right=%d view=%d scrollbar=%d\n",
			frame.x2, view.x2, vertical.x1);
		assert(frame.x1 >= view.x1 && frame.x2 <= view.x2);
		assert(frame.x2 < vertical.x1);
		expect_form_image(image, 204, 102, false, true);
		assert(lv_image_get_src(image) != original_source);
		assert(fplinux_ui_form_set_appearance(
			form, 0, FPLINUX_UI_APPEARANCE_BUTTON));
		expect_form_image(image, 204, 102, false, false);
		assert(lv_image_get_src(image) != original_source);
		assert(fplinux_ui_form_set_appearance(
			form, 0, FPLINUX_UI_APPEARANCE_PLAIN));
		assert(lv_obj_get_parent(image) == box);
		assert(lv_image_get_src(image) == original_source);
		expect_form_image(image, 220, 112, false, false);
		assert(fplinux_ui_form_set_appearance(
			form, 0, FPLINUX_UI_APPEARANCE_HYPERLINK));
		expect_form_image(image, 204, 102, false, true);
		assert(lv_image_get_src(image) != original_source);
		fplinux_ui_screen_remove_item_command(form, 0, open.id);
		assert(lv_obj_get_parent(image) == box);
		assert(lv_image_get_src(image) == original_source);
		expect_form_image(image, 220, 112, false, false);
		assert(fplinux_ui_form_add_item_command(form, 0, &open));
		expect_form_image(image, 204, 102, false, true);
		assert(lv_image_get_src(image) != original_source);
		lv_obj_get_coords(lv_obj_get_parent(image), &frame);
		lv_obj_get_content_coords(box, &view);
		assert(frame.x1 >= view.x1 && frame.x2 <= view.x2);
		assert(frame.x2 < vertical.x1);
		return;
	}
	expect_form_image(image, width, height, mask, true);
	assert(fplinux_ui_form_set_appearance(form, 0,
					      FPLINUX_UI_APPEARANCE_BUTTON));
	expect_form_image(image, width, height, mask, false);
	assert(fplinux_ui_form_set_appearance(form, 0,
					      FPLINUX_UI_APPEARANCE_PLAIN));
	assert(lv_obj_get_parent(image) == box);
	expect_form_image(image, width, height, mask, false);
	assert(fplinux_ui_form_set_appearance(form, 0,
					      FPLINUX_UI_APPEARANCE_HYPERLINK));
	expect_form_image(image, width, height, mask, true);
	fplinux_ui_screen_remove_item_command(form, 0, open.id);
	assert(lv_obj_get_parent(image) == box);
	expect_form_image(image, width, height, mask, false);
	assert(fplinux_ui_form_add_item_command(form, 0, &open));
	expect_form_image(image, width, height, mask, true);
}

static void test_read_only_content_insets(int padding, const char *long_text)
{
	struct fplinux_ui_screen *canvas = fplinux_ui_canvas_new(&ui, NULL);
	struct fplinux_ui_text *field;
	lv_obj_t *frame, *area, *label;
	lv_area_t before, after, text, horizontal, vertical;

	assert(canvas);
	fplinux_ui_replace(&ui, canvas);
	field = fplinux_ui_text_create(&ui, fplinux_ui_canvas_object(canvas),
				       true, 0);
	assert(field);
	assert(fplinux_ui_text_set(field, "AV"));
	advance(0);
	frame = fplinux_ui_text_object(field);
	area = area_at(0);
	label = lv_textarea_get_label(area);
	lv_obj_get_coords(frame, &before);
	expect_label_insets(label, area, 1, 0);
	assert(lv_obj_get_style_border_width(frame, 0) == 2);
	assert(lv_obj_get_style_pad_left(frame, 0) == padding);
	assert(lv_obj_get_style_pad_right(frame, 0) == padding);

	assert(fplinux_ui_text_set_constraint(field,
					      FPLINUX_UI_TEXT_UNEDITABLE));
	advance(0);
	lv_obj_get_coords(frame, &after);
	assert(before.x1 == after.x1 && before.y1 == after.y1 &&
	       before.x2 == after.x2 && before.y2 == after.y2);
	expect_label_insets(label, area, 1, 1);
	lv_obj_get_scrollbar_area(area, &horizontal, &vertical);
	assert(vertical.x2 < vertical.x1);
	assert(fplinux_ui_text_set(field, long_text));
	advance(0);
	expect_label_insets(label, area, 1, 5);
	lv_obj_get_content_coords(label, &text);
	lv_obj_get_scrollbar_area(area, &horizontal, &vertical);
	assert(lv_area_get_width(&vertical) == 2);
	assert(vertical.x1 - text.x2 == 3);
	key(field, FPLINUX_UI_KEY_PHONE, KEY_RIGHT, true, NULL);
	assert(lv_obj_get_scroll_y(area) > 0);
	expect_label_insets(label, area, 1, 5);

	assert(fplinux_ui_text_set_constraint(field, FPLINUX_UI_TEXT_ANY));
	advance(0);
	expect_label_insets(label, area, 1, 0);
	lv_obj_get_scrollbar_area(area, &horizontal, &vertical);
	assert(vertical.x2 < vertical.x1);
	fplinux_ui_text_delete(field);
}

static void test_alert_content_insets(int margin, int padding,
				      const char *long_text)
{
	struct fplinux_ui_screen *alert = fplinux_ui_alert_new(
		&ui, FPLINUX_UI_ALERT_INFO, "First", "AV", 0);
	lv_obj_t *title, *message, *box;
	lv_area_t view, text, horizontal, vertical;
	int width, height;

	assert(alert);
	fplinux_ui_push(&ui, alert);
	advance(0);
	title = label_with_text(ui.root, "First");
	message = label_with_text(ui.root, "AV");
	assert(title && message);
	box = lv_obj_get_parent(message);
	lv_obj_get_coords(box, &view);
	assert(view.x1 == margin);
	assert(lv_area_get_width(&view) == fplinux_ui_width(&ui) - 2 * margin);
	assert(lv_obj_get_style_border_width(box, 0) == 2);
	expect_label_insets(title, box, 2 + padding, 2 + padding);
	expect_label_insets(message, box, 2 + padding, 2 + padding);
	assert(fplinux_ui_screen_get_content_size(alert, &width, &height));
	assert(width == fplinux_ui_width(&ui) - 2 * margin - 2 * (2 + padding));
	assert(height ==
	       fplinux_ui_height(&ui) - 2 * margin - 2 * (2 + padding));
	assert(lv_obj_get_width(message) == width);
	lv_obj_get_scrollbar_area(box, &horizontal, &vertical);
	assert(vertical.x2 < vertical.x1);

	assert(fplinux_ui_alert_set_text(alert, long_text));
	advance(0);
	expect_label_insets(title, box, 2 + padding, 2 + padding + 4);
	expect_label_insets(message, box, 2 + padding, 2 + padding + 4);
	lv_obj_get_coords(box, &view);
	lv_obj_get_content_coords(message, &text);
	lv_obj_get_scrollbar_area(box, &horizontal, &vertical);
	assert(lv_area_get_width(&vertical) == 2);
	assert(view.x2 - vertical.x2 == 3);
	assert(text.x2 < vertical.x1);
	screen_key(KEY_RIGHT);
	assert(lv_obj_get_scroll_y(box) > 0);
	expect_label_insets(message, box, 2 + padding, 2 + padding + 4);
	assert(fplinux_ui_alert_set_text(alert, "AV"));
	advance(0);
	expect_label_insets(message, box, 2 + padding, 2 + padding);
	lv_obj_get_scrollbar_area(box, &horizontal, &vertical);
	assert(vertical.x2 < vertical.x1);
}

static void test_content_insets(int margin, int field_padding, int alert_margin,
				int alert_padding)
{
	char long_text[256] = "";

	for (unsigned int row = 0; row < 40; ++row)
		strcat(long_text, "AV\n");
	test_form_content_insets(margin);
	test_read_only_content_insets(field_padding, long_text);
	test_alert_content_insets(alert_margin, alert_padding, long_text);
}

static lv_obj_t *menu_popup(const char *text)
{
	lv_obj_t *layer = lv_display_get_layer_top(ui.display);
	lv_obj_t *popup = label_with_text(layer, text);

	assert(popup);
	while (lv_obj_get_parent(popup) != layer)
		popup = lv_obj_get_parent(popup);
	lv_obj_update_layout(popup);
	return popup;
}

static void menu_fonts(void)
{
	/* The owned fonts have exact six- and twelve-pixel advances. */
	ui.theme.font = fplinux_ui_builtin_font("menu-10");
	ui.theme.font_large = fplinux_ui_builtin_font("menu-title-20");
	assert(ui.theme.font && ui.theme.font_large);
}

static void test_conditional_scrollbar(void)
{
	lv_obj_t *viewport = lv_obj_create(ui.root);
	lv_obj_t *label;
	lv_area_t horizontal, vertical;
	char text[128];

	menu_fonts();
	lv_obj_remove_style_all(viewport);
	lv_obj_set_size(viewport, 50, lv_font_get_line_height(ui.theme.font));
	lv_obj_set_style_pad_hor(viewport, 4, 0);
	lv_obj_set_style_text_font(viewport, ui.theme.font, 0);
	lv_obj_set_scroll_dir(viewport, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(viewport, LV_SCROLLBAR_MODE_AUTO);
	fplinux_ui_style_scrollbar(&ui, viewport);
	fplinux_ui_style_scrollbar(&ui, viewport);
	label = lv_label_create(viewport);
	lv_obj_set_width(label, lv_pct(100));
	lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
	/* Seven six-pixel glyphs fit only without the four-pixel lane. */
	lv_label_set_text(label, "AAAAAAA");
	advance(0);
	assert(lv_obj_get_style_pad_right(viewport, 0) == 4);
	lv_obj_get_scrollbar_area(viewport, &horizontal, &vertical);
	assert(vertical.x2 < vertical.x1);
	memset(text, 'A', sizeof(text) - 1);
	text[sizeof(text) - 1] = '\0';
	lv_label_set_text(label, text);
	advance(0);
	assert(lv_obj_get_style_pad_right(viewport, 0) == 8);
	lv_obj_get_scrollbar_area(viewport, &horizontal, &vertical);
	assert(lv_area_get_width(&vertical) == 2);
	advance(30);
	assert(lv_obj_get_style_pad_right(viewport, 0) == 8);
	lv_obj_set_scrollbar_mode(viewport, LV_SCROLLBAR_MODE_OFF);
	advance(0);
	assert(lv_obj_get_style_pad_right(viewport, 0) == 4);
	lv_obj_get_scrollbar_area(viewport, &horizontal, &vertical);
	assert(vertical.x2 < vertical.x1);
	lv_obj_set_scrollbar_mode(viewport, LV_SCROLLBAR_MODE_AUTO);
	advance(0);
	assert(lv_obj_get_style_pad_right(viewport, 0) == 8);
	lv_label_set_text(label, "AAAAAAA");
	advance(0);
	assert(lv_obj_get_style_pad_right(viewport, 0) == 4);
	assert(lv_obj_get_height(label) ==
	       lv_font_get_line_height(ui.theme.font));
	advance(30);
	assert(lv_obj_get_style_pad_right(viewport, 0) == 4);
	lv_label_set_text(label, text);
	lv_obj_set_scrollbar_mode(viewport, LV_SCROLLBAR_MODE_OFF);
	fplinux_ui_style_scrollbar(&ui, viewport);
	advance(0);
	assert(lv_obj_get_style_pad_right(viewport, 0) == 4);
	lv_obj_get_scrollbar_area(viewport, &horizontal, &vertical);
	assert(vertical.x2 < vertical.x1);
	lv_obj_delete(viewport);
}

static void expect_menu_geometry(int row_height, int band, int width, int x,
				 bool titled, bool clipped)
{
	lv_obj_t *layer = lv_display_get_layer_top(ui.display);
	lv_obj_t *first_label = label_with_text(layer, "First");
	lv_obj_t *last_label = label_with_text(layer, "Third");
	lv_obj_t *first, *last, *popup = menu_popup("Second");
	lv_area_t first_area, last_area, popup_area;
	int top = 2 + (titled ? band : 0);
	int surface = fplinux_ui_modal_height(&ui);
	int height = top + (clipped ? 2 : 3) * row_height;

	assert(first_label && last_label);
	first = lv_obj_get_parent(first_label);
	last = lv_obj_get_parent(last_label);
	lv_obj_get_coords(popup, &popup_area);
	lv_obj_get_coords(first, &first_area);
	lv_obj_get_coords(last, &last_area);
	assert(lv_area_get_height(&popup_area) == height);
	assert(popup_area.y1 == surface - height);
	assert(popup_area.x1 == x);
	assert(lv_area_get_width(&popup_area) == width);
	assert(first_area.y1 - popup_area.y1 == top);
	assert(first_area.x1 == popup_area.x1);
	assert(popup_area.x2 - first_area.x2 == (clipped ? 4 : 0));
	assert(lv_area_get_height(&first_area) == row_height);
	if (!clipped)
		assert(popup_area.y2 == last_area.y2);
}

static void test_menu_geometry(int margin, int row_height, int band, int width,
			       enum fplinux_ui_menu_origin origin, int x,
			       bool titled)
{
	const struct fplinux_ui_menu_item items[] = {
		{ .action = 1, .text = "First" },
		{ .action = 2, .text = "Second" },
		{ .action = 3, .text = "Third" },
	};
	struct fplinux_ui_key down = {
		.source = FPLINUX_UI_KEY_PHONE,
		.code = KEY_DOWN,
		.pressed = true,
	};
	lv_area_t last_area, viewport;
	lv_obj_t *last;
	int action;
	int clipped_width = width + (titled ? 0 : 4);
	int clipped_x = x - (titled				     ? 0 :
			     origin == FPLINUX_UI_MENU_ORIGIN_CENTRE ? 2 :
			     origin == FPLINUX_UI_MENU_ORIGIN_RIGHT  ? 4 :
								       0);

	menu_fonts();
	assert(fplinux_ui_menu_open(&ui, origin, titled ? "AAAAA" : NULL, items,
				    3, NULL, NULL));
	expect_menu_geometry(row_height, band, width, x, titled, false);
	if (titled) {
		lv_obj_t *title = label_with_text(
			lv_display_get_layer_top(ui.display), "AAAAA");

		assert(title && lv_obj_get_width(title) == width - 2 * margin);
	}
	ui.modal_height = 2 + (titled ? band : 0) + 2 * row_height;
	fplinux_ui_menu_view_resize(ui.menu);
	expect_menu_geometry(row_height, band, clipped_width, clipped_x, titled,
			     true);
	ui.modal_height += row_height / 2;
	fplinux_ui_menu_view_resize(ui.menu);
	expect_menu_geometry(row_height, band, clipped_width, clipped_x, titled,
			     true);
	assert(!fplinux_ui_menu_view_key(ui.menu, &down, &action));
	assert(!fplinux_ui_menu_view_key(ui.menu, &down, &action));
	advance(0);
	last = lv_obj_get_parent(
		label_with_text(lv_display_get_layer_top(ui.display), "Third"));
	lv_obj_get_coords(last, &last_area);
	lv_obj_get_coords(lv_obj_get_parent(last), &viewport);
	assert(viewport.y2 == last_area.y2);
	assert(last_area.y1 >= viewport.y1);
	assert(lv_obj_get_width(menu_popup("Second")) == clipped_width);
	ui.modal_height = 0;
	fplinux_ui_menu_view_resize(ui.menu);
	expect_menu_geometry(row_height, band, width, x, titled, false);
	fplinux_ui_menu_close(&ui);
}

static void test_menu_width(const char *scenario, int width)
{
	static const char long_text[] =
		"АААААААААААААААААААААААААААААААААААААААА";
	lv_color32_t pixels[2 * 2] = {
		{ .red = 0xd0, .green = 0x30, .blue = 0x50, .alpha = 0xff },
		{ .red = 0xd0, .green = 0x30, .blue = 0x50, .alpha = 0xff },
		{ .red = 0xd0, .green = 0x30, .blue = 0x50, .alpha = 0xff },
		{ .red = 0xd0, .green = 0x30, .blue = 0x50, .alpha = 0xff },
	};
	lv_image_dsc_t image = {
		.header = { .magic = LV_IMAGE_HEADER_MAGIC,
			    .cf = LV_COLOR_FORMAT_ARGB8888,
			    .w = 2,
			    .h = 2,
			    .stride = 2 * 4 },
		.data_size = sizeof(pixels),
		.data = (const uint8_t *)pixels,
	};
	struct fplinux_ui_menu_item items[] = {
		{ .action = 1, .text = "First" },
		{ .action = 2, .text = "Second" },
		{ .action = 3, .text = "Third" },
	};
	struct fplinux_ui_key down = {
		.source = FPLINUX_UI_KEY_PHONE,
		.code = KEY_DOWN,
		.pressed = true,
	};
	lv_obj_t *popup, *first, *second;
	lv_area_t first_area, second_area;
	size_t count = 3;
	int action;

	menu_fonts();
	if (!strcmp(scenario, "single")) {
		items[0].text = "A";
		count = 1;
	} else if (!strcmp(scenario, "long")) {
		/* The widest item is initially offscreen, not the selected label. */
		items[2].text = long_text;
		ui.modal_height = 74;
	} else if (!strcmp(scenario, "icons")) {
		items[1].icon = &image;
	} else {
		assert(!strcmp(scenario, "missing-icon"));
		items[1].icon = "/missing-menu-test-icon.png";
	}
	assert(fplinux_ui_menu_open(&ui, FPLINUX_UI_MENU_ORIGIN_LEFT, NULL,
				    items, count, NULL, NULL));
	popup = menu_popup(items[0].text);
	assert(lv_obj_get_width(popup) == width);
	if (!strcmp(scenario, "icons")) {
		lv_obj_t *layer = lv_display_get_layer_top(ui.display);
		lv_obj_t *icon;
		lv_draw_buf_t *frame;
		lv_area_t icon_area, popup_area;
		const uint8_t *pixel;

		first = label_with_text(layer, "First");
		second = label_with_text(layer, "Second");
		lv_obj_get_coords(first, &first_area);
		lv_obj_get_coords(second, &second_area);
		assert(first_area.x1 == second_area.x1);
		assert(first_area.x1 - lv_obj_get_x(popup) == 40);
		assert(!image_under(lv_obj_get_parent(first)));
		icon = image_under(lv_obj_get_parent(second));
		assert(icon);
		lv_obj_get_coords(icon, &icon_area);
		lv_obj_get_coords(popup, &popup_area);
		frame = snapshot(popup);
		pixel = lv_draw_buf_goto_xy(
			frame,
			(icon_area.x1 + icon_area.x2) / 2 - popup_area.x1,
			(icon_area.y1 + icon_area.y2) / 2 - popup_area.y1);
		assert(pixel[2] == 0xd0 && pixel[1] == 0x30 &&
		       pixel[0] == 0x50);
		lv_draw_buf_destroy(frame);
	}
	for (size_t index = 0; index < count; ++index) {
		assert(!fplinux_ui_menu_view_key(ui.menu, &down, &action));
		assert(lv_obj_get_width(popup) == width);
	}
	fplinux_ui_menu_close(&ui);
}

static void expect_menu_selection(size_t selected, const char *text, int width,
				  int x, int height)
{
	struct fplinux_ui_menu_position position;
	lv_obj_t *layer = lv_display_get_layer_top(ui.display);
	lv_obj_t *row = lv_obj_get_parent(label_with_text(layer, text));
	lv_obj_t *popup = menu_popup(text);
	lv_area_t row_area, viewport;

	fplinux_ui_menu_view_get_position(ui.menu, &position);
	assert(position.selected == selected);
	lv_obj_update_layout(popup);
	lv_obj_get_coords(row, &row_area);
	lv_obj_get_coords(lv_obj_get_parent(row), &viewport);
	assert(row_area.y1 >= viewport.y1 && row_area.y2 <= viewport.y2);
	assert(lv_obj_get_width(popup) == width);
	assert(lv_obj_get_x(popup) == x);
	assert(lv_obj_get_height(popup) == height);
	assert(lv_area_get_height(&viewport) % 36 == 0);
}

static void test_menu_resize(void)
{
	struct fplinux_ui_menu_item items[20];
	char labels[20][40];
	struct fplinux_ui_key key = {
		.source = FPLINUX_UI_KEY_PHONE,
		.code = KEY_DOWN,
		.pressed = true,
	};
	int action;

	menu_fonts();
	for (size_t index = 0; index < 20; ++index) {
		snprintf(labels[index], sizeof(labels[index]), "First %02zu",
			 index);
		items[index] = (struct fplinux_ui_menu_item){
			.action = (int)index,
			.text = labels[index],
		};
	}
	items[10].text = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
	assert(fplinux_ui_menu_open(&ui, FPLINUX_UI_MENU_ORIGIN_RIGHT, NULL,
				    items, 20, NULL, NULL));
	expect_menu_selection(0, labels[0], 200, 40, 290);
	key.code = KEY_RIGHT;
	assert(!fplinux_ui_menu_view_key(ui.menu, &key, &action));
	expect_menu_selection(8, labels[8], 200, 40, 290);
	key.code = KEY_DOWN;
	for (unsigned int index = 0; index < 11; ++index)
		assert(!fplinux_ui_menu_view_key(ui.menu, &key, &action));
	expect_menu_selection(19, labels[19], 200, 40, 290);
	lv_display_set_resolution(ui.display, 128, 128);
	fplinux_ui_menu_view_resize(ui.menu);
	expect_menu_selection(19, labels[19], 128, 0, 110);
	lv_display_set_resolution(ui.display, 240, 320);
	fplinux_ui_menu_view_resize(ui.menu);
	expect_menu_selection(19, labels[19], 200, 40, 290);
	assert(!fplinux_ui_menu_view_key(ui.menu, &key, &action));
	expect_menu_selection(0, labels[0], 200, 40, 290);
	key.code = KEY_UP;
	assert(!fplinux_ui_menu_view_key(ui.menu, &key, &action));
	expect_menu_selection(19, labels[19], 200, 40, 290);
	fplinux_ui_menu_close(&ui);
}

static void test_menu_position(void)
{
	struct fplinux_ui_menu_item items[20];
	struct fplinux_ui_menu_position position, unchanged;
	struct fplinux_ui_key down = {
		.source = FPLINUX_UI_KEY_PHONE,
		.code = KEY_DOWN,
		.pressed = true,
	};
	lv_obj_t *row, *viewport;
	lv_area_t row_area, view_area;
	int action;

	for (unsigned int index = 0; index < 20; ++index)
		items[index] = (struct fplinux_ui_menu_item){
			.action = (int)index,
			.text = index == 15 ? "First" : "Spacer",
		};
	assert(fplinux_ui_menu_open(&ui, FPLINUX_UI_MENU_ORIGIN_LEFT, NULL,
				    items, 20, NULL, NULL));
	for (unsigned int index = 0; index < 15; ++index)
		assert(!fplinux_ui_menu_view_key(ui.menu, &down, &action));
	fplinux_ui_menu_view_get_position(ui.menu, &position);
	assert(position.selected == 15 && position.scroll_y > 0);
	fplinux_ui_menu_close(&ui);

	assert(fplinux_ui_menu_open(&ui, FPLINUX_UI_MENU_ORIGIN_LEFT, NULL,
				    items, 20, NULL, NULL));
	fplinux_ui_menu_view_set_position(ui.menu, &position);
	fplinux_ui_menu_view_get_position(ui.menu, &unchanged);
	assert(unchanged.selected == 15 &&
	       unchanged.scroll_y == position.scroll_y);
	fplinux_ui_menu_close(&ui);
	items[0] =
		(struct fplinux_ui_menu_item){ .action = 15, .text = "First" };
	items[15] =
		(struct fplinux_ui_menu_item){ .action = 0, .text = "Spacer" };
	position.selected = 0;
	assert(fplinux_ui_menu_open(&ui, FPLINUX_UI_MENU_ORIGIN_LEFT, NULL,
				    items, 20, NULL, NULL));
	fplinux_ui_menu_view_set_position(ui.menu, &position);
	advance(0);
	row = lv_obj_get_parent(
		label_with_text(lv_display_get_layer_top(ui.display), "First"));
	viewport = lv_obj_get_parent(row);
	lv_obj_get_coords(row, &row_area);
	lv_obj_get_coords(viewport, &view_area);
	assert(row_area.y1 >= view_area.y1 && row_area.y2 <= view_area.y2);
	assert(lv_obj_get_scroll_y(viewport) == 0);
	fplinux_ui_menu_close(&ui);
}

int main(int argc, char **argv)
{
	int width = 240, height = 320;

	assert(argc == 3 || argc == 4 || argc == 5 || argc == 9 || argc == 10 ||
	       argc == 11);
	if (argc >= 5) {
		width = atoi(argv[3]);
		height = atoi(argv[4]);
		if (argc == 5)
			assert(!strcmp(argv[1], "scrollbars"));
		assert(width > 0 && height > 0 && width * height <= 240 * 320);
	}
	language_directory = argv[2];
	lv_init();
	lv_tick_inc(100);
	ui.display = lv_display_create(width, height);
	assert(ui.display);
	ui.spec = fplinux_ui_profile_spec(
		fplinux_ui_profile_for_screen(width, height));
	assert(ui.spec);
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
	else if (!strcmp(argv[1], "vertical-scroll"))
		test_vertical_scroll();
	else if (!strcmp(argv[1], "form-pages"))
		test_form_pages();
	else if (!strncmp(argv[1], "symbols-", 8)) {
		assert(argc == 10);
		test_symbol_context(argv[1] + 8, atoi(argv[5]), atoi(argv[6]),
				    atoi(argv[7]), atoi(argv[8]),
				    atoi(argv[9]));
	} else if (!strcmp(argv[1], "form-image-square"))
		test_form_image_shape(true, false);
	else if (!strcmp(argv[1], "form-image-landscape"))
		test_form_image_shape(false, false);
	else if (!strcmp(argv[1], "form-image-overflow"))
		test_form_image_shape(false, true);
	else if (!strncmp(argv[1], "command-glyph-", 14))
		test_command_glyphs(argv[1] + 14);
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
	else if (!strcmp(argv[1], "alert-style"))
		test_alert_style();
	else if (!strcmp(argv[1], "hidden-date"))
		test_hidden_date();
	else if (!strcmp(argv[1], "exclusive-append"))
		test_exclusive_first_row(false);
	else if (!strcmp(argv[1], "exclusive-insert"))
		test_exclusive_first_row(true);
	else if (!strcmp(argv[1], "choice-exclusive"))
		test_choice_ownership(FPLINUX_UI_CHOICE_EXCLUSIVE);
	else if (!strcmp(argv[1], "choice-multiple"))
		test_choice_ownership(FPLINUX_UI_CHOICE_MULTIPLE);
	else if (!strcmp(argv[1], "choice-popup"))
		test_choice_ownership(FPLINUX_UI_CHOICE_POPUP);
	else if (!strcmp(argv[1], "choice-height-fit"))
		test_popup_choice_height(false);
	else if (!strcmp(argv[1], "choice-height-overflow"))
		test_popup_choice_height(true);
	else if (!strcmp(argv[1], "list-icons-stored"))
		test_list_icons(false);
	else if (!strcmp(argv[1], "list-icons-virtual"))
		test_list_icons(true);
	else if (!strncmp(argv[1], "key-filter-", 11))
		test_screen_filter(argv[1] + 11);
	else if (!strcmp(argv[1], "filter-cover"))
		test_filter_screen_change(false, true);
	else if (!strcmp(argv[1], "filter-replace"))
		test_filter_screen_change(true, true);
	else if (!strcmp(argv[1], "filter-replace-unclaimed"))
		test_filter_screen_change(true, false);
	else if (!strcmp(argv[1], "list-focus-listener"))
		test_list_focus_listener();
	else if (!strcmp(argv[1], "menu-pending-visibility"))
		test_menu_visibility(false);
	else if (!strcmp(argv[1], "menu-clear-visibility"))
		test_menu_visibility(true);
	else if (!strcmp(argv[1], "canvas-menu-visibility"))
		test_canvas_menu_visibility();
	else if (!strcmp(argv[1], "scrollbars"))
		test_scrollbars();
	else if (!strcmp(argv[1], "conditional-scrollbar"))
		test_conditional_scrollbar();
	else if (!strcmp(argv[1], "content-insets")) {
		assert(argc == 9);
		test_content_insets(atoi(argv[5]), atoi(argv[6]), atoi(argv[7]),
				    atoi(argv[8]));
	} else if (!strcmp(argv[1], "menu-position"))
		test_menu_position();
	else if (!strcmp(argv[1], "menu-resize"))
		test_menu_resize();
	else if (!strncmp(argv[1], "menu-width-", 11)) {
		assert(argc == 4);
		test_menu_width(argv[1] + 11, atoi(argv[3]));
	} else if (!strcmp(argv[1], "menu-insets") ||
		   !strcmp(argv[1], "menu-title-insets")) {
		assert(argc == 11);
		test_menu_geometry(atoi(argv[5]), atoi(argv[6]), atoi(argv[7]),
				   atoi(argv[8]),
				   (enum fplinux_ui_menu_origin)atoi(argv[9]),
				   atoi(argv[10]),
				   !strcmp(argv[1], "menu-title-insets"));
	} else
		abort();
	fplinux_ui_screens_delete(ui.screens);
	lv_display_delete(ui.display);
	lv_deinit();
	printf("%s: ok\n", argv[1]);
	return 0;
}
