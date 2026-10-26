/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "fplinux-ui-symbol-picker.h"
#include "fplinux-ui-internal.h"
#include "fplinux-text-input.h"

#include <errno.h>
#include <stdlib.h>

struct fplinux_ui_symbol_picker {
	struct fplinux_ui *ui;
	lv_obj_t *view;
	unsigned int selected;
	unsigned int columns;
	bool modal_active;
};

static void render_cell(struct fplinux_ui_symbol_picker *picker,
			unsigned int index, bool selected)
{
	const struct fplinux_ui_theme *theme = fplinux_ui_theme(picker->ui);
	lv_obj_t *cell = lv_obj_get_child(picker->view, (int32_t)index);

	lv_obj_set_style_bg_opa(cell, selected ? LV_OPA_COVER : LV_OPA_TRANSP,
				0);
	lv_obj_set_style_text_color(
		cell, selected ? theme->accent_text : theme->foreground, 0);
}

static void view_delete_cb(lv_event_t *event)
{
	struct fplinux_ui_symbol_picker *picker = lv_event_get_user_data(event);

	/* The selecting key must not begin repeating into the uncovered input. */
	fplinux_ui_cancel_key_repeat(picker->ui, NULL);
	picker->view = NULL;
	if (picker->modal_active) {
		picker->modal_active = false;
		fplinux_ui_modal_set(picker->ui, FPLINUX_UI_MODAL_PICKER,
				     false);
	}
}

/* Large, bold strokes keep thin punctuation legible in the grid. */
static bool open_view(struct fplinux_ui_symbol_picker *picker)
{
	const struct fplinux_ui_theme *theme = fplinux_ui_theme(picker->ui);
	const struct fplinux_ui_profile_spec *spec =
		fplinux_ui_spec(picker->ui);
	lv_display_t *display =
		lv_obj_get_display(fplinux_ui_screen(picker->ui));
	unsigned int count = (unsigned int)fplinux_text_input_symbol_count();
	unsigned int columns = (unsigned int)spec->picker_columns;
	unsigned int rows = (count + columns - 1) / columns;
	int32_t cell_width = spec->picker_cell_width;
	int32_t cell_height = spec->picker_cell_height;
	int32_t inset = spec->picker_border + spec->picker_padding;
	int32_t width = (int32_t)columns * cell_width + 2 * inset;
	int32_t height = (int32_t)rows * cell_height + 2 * inset;
	int32_t line = lv_font_get_line_height(theme->font_large);
	int32_t y = (fplinux_ui_modal_height(picker->ui) - height) / 2;

	picker->view = lv_obj_create(lv_display_get_layer_top(display));
	if (!picker->view)
		return false;
	lv_obj_add_event_cb(picker->view, view_delete_cb, LV_EVENT_DELETE,
			    picker);
	lv_obj_remove_style_all(picker->view);
	lv_obj_set_size(picker->view, width, height);
	lv_obj_set_pos(picker->view,
		       (lv_display_get_horizontal_resolution(display) - width) /
			       2,
		       y > 0 ? y : 0);
	lv_obj_set_style_bg_color(picker->view, theme->popup, 0);
	lv_obj_set_style_bg_opa(picker->view, LV_OPA_COVER, 0);
	lv_obj_set_style_border_color(picker->view, theme->focus, 0);
	lv_obj_set_style_border_width(picker->view, spec->picker_border, 0);
	lv_obj_set_style_pad_all(picker->view, spec->picker_padding, 0);
	lv_obj_set_style_text_font(picker->view, theme->font_large, 0);
	lv_obj_set_scrollable(picker->view, false);
	for (unsigned int index = 0; index < count; ++index) {
		lv_obj_t *cell = lv_label_create(picker->view);
		char label[5];

		if (!cell)
			goto fail;
		lv_label_set_text(cell, fplinux_text_input_symbol_label(index,
									label));
		lv_obj_set_size(cell, cell_width, cell_height);
		lv_obj_set_pos(cell, (int32_t)(index % columns) * cell_width,
			       (int32_t)(index / columns) * cell_height);
		lv_obj_set_style_pad_top(cell, (cell_height - line) / 2, 0);
		lv_obj_set_style_text_align(cell, LV_TEXT_ALIGN_CENTER, 0);
		lv_obj_set_style_bg_color(cell, theme->accent, 0);
		render_cell(picker, index, false);
	}
	if (!fplinux_ui_scrim_attach(picker->ui, picker->view))
		goto fail;
	picker->columns = columns;
	render_cell(picker, picker->selected, true);
	picker->modal_active = true;
	fplinux_ui_modal_set(picker->ui, FPLINUX_UI_MODAL_PICKER, true);
	return true;

fail:
	lv_obj_delete(picker->view);
	return false;
}

struct fplinux_ui_symbol_picker *
fplinux_ui_symbol_picker_create(struct fplinux_ui *ui, unsigned int selected)
{
	struct fplinux_ui_symbol_picker *picker = calloc(1, sizeof(*picker));

	if (!picker)
		return NULL;
	picker->ui = ui;
	picker->selected =
		selected < fplinux_text_input_symbol_count() ? selected : 0;
	if (!open_view(picker)) {
		free(picker);
		errno = ENOMEM;
		return NULL;
	}
	return picker;
}

void fplinux_ui_symbol_picker_delete(struct fplinux_ui_symbol_picker *picker)
{
	if (!picker)
		return;
	if (picker->view)
		lv_obj_delete(picker->view);
	free(picker);
}

void fplinux_ui_symbol_picker_select(struct fplinux_ui_symbol_picker *picker,
				     unsigned int selected)
{
	if (!picker->view || selected >= fplinux_text_input_symbol_count() ||
	    selected == picker->selected)
		return;
	render_cell(picker, picker->selected, false);
	render_cell(picker, selected, true);
	picker->selected = selected;
}

bool fplinux_ui_symbol_picker_refresh(struct fplinux_ui_symbol_picker *picker)
{
	if (!picker->view)
		return false;
	lv_obj_delete(picker->view);
	if (!open_view(picker)) {
		errno = ENOMEM;
		return false;
	}
	return true;
}

bool fplinux_ui_symbol_picker_is_open(
	const struct fplinux_ui_symbol_picker *picker)
{
	return picker && picker->view;
}

unsigned int
fplinux_ui_symbol_picker_columns(const struct fplinux_ui_symbol_picker *picker)
{
	return picker->columns;
}
