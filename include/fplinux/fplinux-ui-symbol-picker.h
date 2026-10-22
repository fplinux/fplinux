/* SPDX-License-Identifier: LGPL-2.1-or-later */

#ifndef FPLINUX_UI_SYMBOL_PICKER_H
#define FPLINUX_UI_SYMBOL_PICKER_H

#include <stdbool.h>
#include <lvgl/lvgl.h>

#include "fplinux-ui.h"

struct fplinux_ui_symbol_picker;

enum fplinux_ui_symbol_picker_placement {
	FPLINUX_UI_SYMBOL_PICKER_ANCHORED,
	FPLINUX_UI_SYMBOL_PICKER_CENTERED,
};

/*
 * Opens the shared symbol grid inside the UI's modal height: over anchor
 * for a text field, or centered in the available area for a terminal.
 * The caller owns selection, insertion and cancellation of input. Delete the
 * picker before anchor or ui. Returns NULL with errno set on failure.
 */
struct fplinux_ui_symbol_picker *fplinux_ui_symbol_picker_create(
	struct fplinux_ui *ui, lv_obj_t *anchor,
	enum fplinux_ui_symbol_picker_placement placement,
	unsigned int selected);
void fplinux_ui_symbol_picker_delete(struct fplinux_ui_symbol_picker *picker);
void fplinux_ui_symbol_picker_select(struct fplinux_ui_symbol_picker *picker,
				     unsigned int selected);
/*
 * Recreates an open grid with the current theme, profile and anchor position.
 * False means its view is closed; the caller cancels the input picker state.
 */
bool fplinux_ui_symbol_picker_refresh(struct fplinux_ui_symbol_picker *picker);
/* The handle remains owned by the caller if LVGL deletes its view. */
bool fplinux_ui_symbol_picker_is_open(
	const struct fplinux_ui_symbol_picker *picker);
unsigned int
fplinux_ui_symbol_picker_columns(const struct fplinux_ui_symbol_picker *picker);

#endif
