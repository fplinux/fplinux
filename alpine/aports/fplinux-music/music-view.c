/* SPDX-License-Identifier: GPL-2.0-only */
#include "music-view.h"
#include "fplinux-ui-system.h"

#include <libintl.h>
#include <stdio.h>
#include <string.h>

#define _(text) gettext(text)

static lv_obj_t *new_label(lv_obj_t *parent, const lv_font_t *font,
			   lv_color_t color)
{
	lv_obj_t *label = lv_label_create(parent);
	lv_obj_set_style_text_font(label, font, 0);
	lv_obj_set_style_text_color(label, color, 0);
	lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
	return label;
}

static void set_text(lv_obj_t *label, const char *text)
{
	if (strcmp(lv_label_get_text(label), text))
		lv_label_set_text(label, text);
}

bool music_view_create(struct music_view *view, struct fplinux_ui *ui)
{
	const struct fplinux_ui_theme *theme = fplinux_ui_theme(ui);
	view->screen = fplinux_ui_canvas_new(ui, _("Music"));
	if (!view->screen)
		return false;
	lv_obj_t *canvas = fplinux_ui_canvas_object(view->screen);
	view->title = new_label(canvas, theme->font_large, theme->foreground);
	view->artist = new_label(canvas, theme->font, theme->muted);
	view->state = new_label(canvas, theme->font, theme->foreground);
	view->time = new_label(canvas, theme->font, theme->muted);
	view->hint = new_label(canvas, theme->font, theme->muted);
	view->progress = lv_bar_create(canvas);
	lv_obj_remove_style_all(view->progress);
	lv_obj_set_style_bg_color(view->progress, theme->border, LV_PART_MAIN);
	lv_obj_set_style_bg_opa(view->progress, LV_OPA_COVER, LV_PART_MAIN);
	lv_obj_set_style_bg_color(view->progress, theme->focus,
				  LV_PART_INDICATOR);
	lv_obj_set_style_bg_opa(view->progress, LV_OPA_COVER,
				LV_PART_INDICATOR);
	lv_bar_set_range(view->progress, 0, 1000);
	music_view_layout(view, ui);
	music_view_update_volume(view, ui, false);
	return true;
}

void music_view_layout(struct music_view *view, struct fplinux_ui *ui)
{
	const struct fplinux_ui_metrics *metrics = fplinux_ui_metrics(ui);
	const struct fplinux_ui_theme *theme = fplinux_ui_theme(ui);
	int width, height;
	fplinux_ui_screen_get_content_size(view->screen, &width, &height);
	int left = metrics->margin;
	int content = width - 2 * left;
	int line = lv_font_get_line_height(theme->font);
	int title_line = lv_font_get_line_height(theme->font_large);
	int gap = metrics->gap_small;
	int top = metrics->spacing;
	int bar_height = metrics->bar_height;
	/* Reserve state and transport before expanding the title and metadata. */
	int available = height - (3 * metrics->spacing + title_line + 2 * line +
				  2 * gap + bar_height);
	int title_height = title_line;
	if (available >= title_line) {
		title_height += title_line;
		available -= title_line;
	}
	bool artist = available >= line + gap;
	if (artist)
		available -= line + gap;
	const char *hint_text =
		_("Left/Right: track · hold: seek\nUp/Down: volume");
	lv_point_t hint_size;

	lv_text_get_size(&hint_size, hint_text, theme->font, 0, 0, content,
			 LV_TEXT_FLAG_NONE);
	int hint_height = available >= hint_size.y + metrics->spacing ?
				  hint_size.y :
				  line;
	bool hint = available >= hint_height + metrics->spacing;
	lv_label_set_text(view->hint,
			  hint_height > line ?
				  hint_text :
				  _("Left/Right: track · hold: seek"));
	lv_obj_set_pos(view->title, left, top);
	lv_obj_set_size(view->title, content, title_height);
	lv_label_set_long_mode(view->title, LV_LABEL_LONG_MODE_DOTS);
	top += title_height + gap;
	lv_obj_set_pos(view->artist, left, top);
	lv_obj_set_size(view->artist, content, line);
	lv_obj_set_hidden(view->artist, !artist);
	if (artist)
		top += line + gap;
	lv_obj_set_pos(view->state, left, top);
	lv_obj_set_size(view->state, content, line);
	int bottom = height - metrics->spacing;
	lv_obj_set_hidden(view->hint, !hint);
	if (hint) {
		bottom -= hint_height;
		lv_obj_set_pos(view->hint, left, bottom);
		lv_obj_set_size(view->hint, content, hint_height);
		bottom -= metrics->spacing;
	}
	bottom -= bar_height;
	lv_obj_set_pos(view->progress, left, bottom);
	lv_obj_set_size(view->progress, content, bar_height);
	bottom -= line + gap;
	lv_obj_set_pos(view->time, left, bottom);
	lv_obj_set_size(view->time, content, line);
}

void music_view_update_volume(struct music_view *view, struct fplinux_ui *ui,
			      bool warning)
{
	struct fplinux_system_level_state state;
	char full[96], compact[64];
	const char *suffix = warning ? " !" : "";
	bool received = fplinux_ui_system_level_get(
		ui, FPLINUX_SYSTEM_LEVEL_MEDIA, &state);

	if (received && state.availability == FPLINUX_SYSTEM_LEVEL_READY) {
		snprintf(full, sizeof(full), _("Volume %u/%u"), state.value,
			 state.maximum);
		snprintf(compact, sizeof(compact), _("Vol %u/%u"), state.value,
			 state.maximum);
	} else if (received &&
		   state.availability == FPLINUX_SYSTEM_LEVEL_UNAVAILABLE) {
		snprintf(full, sizeof(full), "%s", _("Volume unavailable"));
		snprintf(compact, sizeof(compact), "%s", _("Vol n/a"));
	} else {
		snprintf(full, sizeof(full), "%s", _("Volume --"));
		snprintf(compact, sizeof(compact), "%s", _("Vol --"));
	}
	strncat(full, suffix, sizeof(full) - strlen(full) - 1);
	strncat(compact, suffix, sizeof(compact) - strlen(compact) - 1);
	fplinux_ui_screen_set_header_status(view->screen, full, compact);
}

static void format_time(char *out, size_t capacity, uint64_t milliseconds)
{
	uint64_t seconds = milliseconds / 1000U;
	snprintf(out, capacity, "%llu:%02u",
		 (unsigned long long)(seconds / 60U),
		 (unsigned int)(seconds % 60U));
}

void music_view_update(struct music_view *view, const char *title,
		       const char *artist, const char *state,
		       uint64_t position_ms, uint64_t duration_ms,
		       bool duration_known, bool empty)
{
	set_text(view->title, empty ? _("Choose your music") : title);
	set_text(view->artist, artist);
	set_text(view->state,
		 empty ? _("Open a file or folder in Options") : state);
	char position[32], duration[32], text[80];
	format_time(position, sizeof(position), position_ms);
	if (duration_known)
		format_time(duration, sizeof(duration), duration_ms);
	else
		snprintf(duration, sizeof(duration), "--:--");
	snprintf(text, sizeof(text), "%s / %s", position, duration);
	set_text(view->time, empty ? "" : text);
	int progress = duration_known && duration_ms ?
			       (int)((position_ms < duration_ms ? position_ms :
								  duration_ms) *
				     1000U / duration_ms) :
			       0;
	if (lv_bar_get_value(view->progress) != progress)
		lv_bar_set_value(view->progress, progress, LV_ANIM_OFF);
	lv_obj_set_hidden(view->progress, empty);
}
