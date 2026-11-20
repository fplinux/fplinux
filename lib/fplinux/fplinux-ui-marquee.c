/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "fplinux-ui-marquee.h"

#include <string.h>

/* A row or form marquee: 400 ms before the first pass, 30 px/s, 1 s pauses. */
#define MARQUEE_DELAY_MS 400
#define MARQUEE_PAUSE_MS 1000
#define MARQUEE_SPEED_PX_S 30
#define MARQUEE_MIN_MS 300
#define MARQUEE_MAX_MS 10000

static void title_done(lv_anim_t *animation);
static void turn_done(lv_anim_t *animation);

/*
 * Every marquee copies its timing from one of these: a title's one pass,
 * the focused element's first pass and every later one.
 */
static lv_anim_t title_pass;
static lv_anim_t first_pass;
static lv_anim_t next_pass;

static void init_passes(void)
{
	if (title_pass.completed_cb)
		return;
	lv_anim_init(&title_pass);
	lv_anim_set_delay(&title_pass, MARQUEE_DELAY_MS);
	lv_anim_set_reverse_delay(&title_pass, MARQUEE_PAUSE_MS);
	lv_anim_set_repeat_count(&title_pass, 1);
	lv_anim_set_completed_cb(&title_pass, title_done);
	first_pass = title_pass;
	lv_anim_set_completed_cb(&first_pass, turn_done);
	next_pass = first_pass;
	lv_anim_set_delay(&next_pass, MARQUEE_PAUSE_MS);
}

static struct fplinux_ui *ui_of(const lv_obj_t *label)
{
	return lv_display_get_user_data(lv_obj_get_display(label));
}

static void rest(lv_obj_t *label)
{
	if (lv_label_get_long_mode(label) != LV_LABEL_LONG_MODE_DOTS)
		lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
}

/* The label scrolls once to its end and back, then calls back. */
static void arm(lv_obj_t *label, const lv_anim_t *pass)
{
	uint32_t speed = lv_anim_speed_clamped(MARQUEE_SPEED_PX_S,
					       MARQUEE_MIN_MS, MARQUEE_MAX_MS);

	if (lv_obj_get_style_anim(label, LV_PART_MAIN) != pass)
		lv_obj_set_style_anim(label, pass, 0);
	if (lv_obj_get_style_anim_duration(label, LV_PART_MAIN) != speed)
		lv_obj_set_style_anim_duration(label, speed, 0);
	lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_SCROLL);
}

static int32_t text_width(lv_obj_t *label)
{
	lv_point_t size;

	lv_text_get_size(&size, lv_label_get_text(label),
			 lv_obj_get_style_text_font(label, LV_PART_MAIN),
			 lv_obj_get_style_text_letter_space(label,
							    LV_PART_MAIN),
			 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
	return size.x;
}

/* The text ends with LV_LABEL_DOT_NUM dots. */
static bool ends_with_dots(const char *text)
{
	size_t length = strlen(text);

	if (length < LV_LABEL_DOT_NUM)
		return false;
	for (size_t i = length - LV_LABEL_DOT_NUM; i < length; ++i)
		if (text[i] != '.')
			return false;
	return true;
}

/*
 * The label's whole text fits width on one line. A label that rested with
 * dots keeps them in its text, over the letters they hide, until LVGL
 * refreshes it, later than layout_done() or a title shown again measure
 * it. A text that ends with dots and fits is set again: that puts the
 * letters back, and the whole text is measured.
 */
static bool text_fits(lv_obj_t *label, int32_t width)
{
	if (text_width(label) > width)
		return false;
	if (!ends_with_dots(lv_label_get_text(label)))
		return true;
	lv_label_set_text(label, NULL);
	return text_width(label) <= width;
}

static void label_deleted(lv_event_t *event);

static void watch(struct fplinux_ui *ui, lv_obj_t *label)
{
	lv_obj_add_event_cb(label, label_deleted, LV_EVENT_DELETE, ui);
}

static void unwatch(struct fplinux_ui *ui, lv_obj_t *label)
{
	lv_obj_remove_event_cb_with_user_data(label, label_deleted, ui);
}

/*
 * The next label from start, in turn order, not known to fit; count when
 * every label fits.
 */
static unsigned int next_turn(const struct fplinux_ui_marquee *marquee,
			      unsigned int start, unsigned int finished)
{
	for (unsigned int n = 0; n < marquee->count; ++n) {
		unsigned int turn = (start + n) % marquee->count;

		if (!(finished & 1U << turn))
			return turn;
	}
	return marquee->count;
}

static void start_turn(struct fplinux_ui *ui, unsigned int start)
{
	struct fplinux_ui_marquee *marquee = fplinux_ui_marquee_state(ui);
	unsigned int turn = next_turn(marquee, start, marquee->finished);

	if (marquee->armed && turn != marquee->turn)
		rest(marquee->labels[marquee->turn]);
	marquee->armed = false;
	if (turn == marquee->count)
		return;
	marquee->turn = turn;
	marquee->checked = false;
	marquee->armed = true;
	arm(marquee->labels[turn], marquee->passed ? &next_pass : &first_pass);
}

/* The focused element's marquee starts unless a title or hiding holds it. */
static void begin(struct fplinux_ui *ui)
{
	struct fplinux_ui_marquee *marquee = fplinux_ui_marquee_state(ui);

	if (!fplinux_ui_is_visible(ui) || marquee->title || !marquee->count)
		return;
	marquee->passed = false;
	start_turn(ui, 0);
}

static void pause_focus(struct fplinux_ui_marquee *marquee)
{
	if (marquee->armed)
		rest(marquee->labels[marquee->turn]);
	marquee->armed = false;
}

static void title_done(lv_anim_t *animation)
{
	lv_obj_t *label = animation->var;
	struct fplinux_ui *ui = ui_of(label);
	struct fplinux_ui_marquee *marquee = fplinux_ui_marquee_state(ui);

	rest(label);
	if (marquee->title != label)
		return;
	unwatch(ui, label);
	marquee->title = NULL;
	begin(ui);
}

static void turn_done(lv_anim_t *animation)
{
	lv_obj_t *label = animation->var;
	struct fplinux_ui *ui = ui_of(label);
	struct fplinux_ui_marquee *marquee = fplinux_ui_marquee_state(ui);

	if (!marquee->armed || marquee->labels[marquee->turn] != label) {
		rest(label);
		return;
	}
	marquee->passed = true;
	marquee->finished |= 1U << marquee->turn;
	start_turn(ui, marquee->turn + 1);
}

/*
 * A focused label is measured once laid out: one that fits gives its turn
 * to the next, so that a short text never holds back a long detail.
 */
static void layout_done(lv_event_t *event)
{
	struct fplinux_ui *ui = lv_event_get_user_data(event);
	struct fplinux_ui_marquee *marquee = fplinux_ui_marquee_state(ui);
	lv_obj_t *label;
	int32_t width;

	if (!marquee->armed || marquee->checked)
		return;
	label = marquee->labels[marquee->turn];
	width = lv_obj_get_content_width(label);
	if (width <= 0)
		return;
	marquee->checked = true;
	if (text_fits(label, width)) {
		marquee->finished |= 1U << marquee->turn;
		start_turn(ui, marquee->turn + 1);
	}
}

void fplinux_ui_marquee_watch_display(struct fplinux_ui *ui,
				      lv_display_t *display)
{
	lv_display_add_event_cb(display, layout_done,
				LV_EVENT_UPDATE_LAYOUT_COMPLETED, ui);
}

static void forget_focus(struct fplinux_ui *ui, lv_obj_t *deleted)
{
	struct fplinux_ui_marquee *marquee = fplinux_ui_marquee_state(ui);

	for (unsigned int i = 0; i < marquee->count; ++i) {
		if (marquee->labels[i] == deleted)
			continue;
		unwatch(ui, marquee->labels[i]);
		rest(marquee->labels[i]);
	}
	marquee->labels[0] = NULL;
	marquee->labels[1] = NULL;
	marquee->count = 0;
	marquee->armed = false;
}

/* label is one of the focused element's labels. */
static bool is_focus_label(const struct fplinux_ui_marquee *marquee,
			   const lv_obj_t *label)
{
	for (unsigned int i = 0; i < marquee->count; ++i)
		if (label && marquee->labels[i] == label)
			return true;
	return false;
}

static void label_deleted(lv_event_t *event)
{
	struct fplinux_ui *ui = lv_event_get_user_data(event);
	struct fplinux_ui_marquee *marquee = fplinux_ui_marquee_state(ui);
	lv_obj_t *label = lv_event_get_target(event);

	if (is_focus_label(marquee, label))
		forget_focus(ui, label);
	if (marquee->title == label) {
		marquee->title = NULL;
		begin(ui);
	}
}

void fplinux_ui_marquee_focus(struct fplinux_ui *ui, lv_obj_t *first,
			      lv_obj_t *second)
{
	struct fplinux_ui_marquee *marquee = fplinux_ui_marquee_state(ui);
	lv_obj_t *labels[2] = { first ? first : second, first ? second : NULL };
	unsigned int count = labels[1] ? 2 : labels[0] ? 1 : 0;

	init_passes();
	if (count == marquee->count &&
	    (!count || labels[0] == marquee->labels[0]) &&
	    (count < 2 || labels[1] == marquee->labels[1])) {
		return;
	}
	for (unsigned int i = 0; i < marquee->count; ++i) {
		if (marquee->labels[i] == labels[0] ||
		    marquee->labels[i] == labels[1])
			continue;
		unwatch(ui, marquee->labels[i]);
		rest(marquee->labels[i]);
	}
	for (unsigned int i = 0; i < count; ++i)
		if (!is_focus_label(marquee, labels[i]))
			watch(ui, labels[i]);
	marquee->labels[0] = labels[0];
	marquee->labels[1] = labels[1];
	marquee->count = count;
	marquee->finished = 0;
	marquee->armed = false;
	if (count > 1)
		rest(labels[1]);
	if (count && (marquee->title || !fplinux_ui_is_visible(ui)))
		rest(labels[0]);
	begin(ui);
}

void fplinux_ui_marquee_changed(struct fplinux_ui *ui, lv_obj_t *first,
				lv_obj_t *second)
{
	struct fplinux_ui_marquee *marquee = fplinux_ui_marquee_state(ui);

	if (!is_focus_label(marquee, first))
		return;
	fplinux_ui_marquee_focus(ui, first, second);
	pause_focus(marquee);
	marquee->finished = 0;
	begin(ui);
}

void fplinux_ui_marquee_blur(struct fplinux_ui *ui, lv_obj_t *label)
{
	struct fplinux_ui_marquee *marquee = fplinux_ui_marquee_state(ui);

	if (!is_focus_label(marquee, label))
		return;
	if (marquee->hiding)
		pause_focus(marquee);
	else
		forget_focus(ui, NULL);
}

/* Hidden: nothing moves. Shown again: unfinished labels resume their turns. */
void fplinux_ui_marquee_set_visible(struct fplinux_ui *ui, bool visible)
{
	struct fplinux_ui_marquee *marquee = fplinux_ui_marquee_state(ui);

	if (!visible) {
		if (marquee->title)
			rest(marquee->title);
		pause_focus(marquee);
	} else if (marquee->title) {
		arm(marquee->title, &title_pass);
	} else {
		begin(ui);
	}
}

/* New widths: the focused element's labels are measured again. */
void fplinux_ui_marquee_restart(struct fplinux_ui *ui)
{
	struct fplinux_ui_marquee *marquee = fplinux_ui_marquee_state(ui);

	pause_focus(marquee);
	marquee->finished = 0;
	begin(ui);
}

void fplinux_ui_title_init(lv_obj_t *label, const lv_font_t *font)
{
	init_passes();
	lv_obj_set_style_text_font(label, font, 0);
	lv_obj_set_height(label, lv_font_get_line_height(font));
	lv_obj_set_style_anim(label, &title_pass, 0);
	lv_obj_set_style_anim_duration(label,
				       lv_anim_speed_clamped(MARQUEE_SPEED_PX_S,
							     MARQUEE_MIN_MS,
							     MARQUEE_MAX_MS),
				       0);
	lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
}

/* The title stops its pass and lets the focused element's marquee run. */
static void release_title(struct fplinux_ui *ui)
{
	struct fplinux_ui_marquee *marquee = fplinux_ui_marquee_state(ui);

	if (!marquee->title)
		return;
	unwatch(ui, marquee->title);
	rest(marquee->title);
	marquee->title = NULL;
	begin(ui);
}

void fplinux_ui_title_hide(lv_obj_t *label)
{
	struct fplinux_ui *ui = ui_of(label);

	if (fplinux_ui_marquee_state(ui)->title == label)
		release_title(ui);
}

/*
 * The title now shown: one that fits rests at once, a longer one makes its
 * pass now and holds the focused element's marquee until it is back with
 * dots. A title shown before gives up its pass either way.
 */
void fplinux_ui_title_scroll(lv_obj_t *label)
{
	struct fplinux_ui *ui = ui_of(label);
	struct fplinux_ui_marquee *marquee = fplinux_ui_marquee_state(ui);
	int32_t width = lv_obj_get_style_width(label, LV_PART_MAIN);

	init_passes();
	if (LV_COORD_IS_SPEC(width))
		width = lv_obj_get_content_width(label);
	if (text_fits(label, width)) {
		rest(label);
		release_title(ui);
		return;
	}
	if (marquee->title && marquee->title != label) {
		unwatch(ui, marquee->title);
		rest(marquee->title);
		marquee->title = NULL;
	}
	if (marquee->title != label) {
		marquee->title = label;
		watch(ui, label);
	}
	pause_focus(marquee);
	if (fplinux_ui_is_visible(ui))
		arm(label, &title_pass);
}
