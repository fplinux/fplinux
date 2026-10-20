/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef FPLINUX_UI_LISTVIEW_H
#define FPLINUX_UI_LISTVIEW_H

#include <stdbool.h>
#include <stddef.h>

#include "fplinux-ui.h"
#include "fplinux-ui-screen.h"

/*
 * Internal to the runtime library, not installed: the list view behind the
 * options menu and the high-level List, Form choice and popup screens. A
 * vertical list of rows: one selected row drawn in the theme
 * accent, dividers, a scrollbar when it does not fit, and an optional check
 * or radio marker per row. Its owner gives it keys. Sizes come from the
 * profile in use.
 */
struct fplinux_ui_listview;

enum fplinux_ui_listview_marker {
	FPLINUX_UI_LISTVIEW_PLAIN,
	FPLINUX_UI_LISTVIEW_CHECK,
	FPLINUX_UI_LISTVIEW_RADIO,
};

typedef void (*fplinux_ui_listview_cb)(struct fplinux_ui_listview *list,
				       size_t index, void *context);

/* The list fills its parent; deleting the parent deletes the list. */
struct fplinux_ui_listview *
fplinux_ui_listview_create(struct fplinux_ui *ui, lv_obj_t *parent,
			   enum fplinux_ui_listview_marker marker);
void fplinux_ui_listview_delete(struct fplinux_ui_listview *list);
lv_obj_t *fplinux_ui_listview_object(struct fplinux_ui_listview *list);

/*
 * A detail, which may be NULL, is shown on a second line under the row's
 * text; the first detail moves every row to the detail pitch. A list with
 * inline details instead shows it at the right of the text, at most half
 * the row wide, as a popup choice does ("label ... value"). Set it before
 * adding rows.
 */
void fplinux_ui_listview_set_inline_detail(struct fplinux_ui_listview *list,
					   bool inline_detail);
size_t fplinux_ui_listview_add(struct fplinux_ui_listview *list,
			       const char *text, const char *detail);
/*
 * Inserts a row before index (index == count appends) and returns its
 * index, or SIZE_MAX with errno EINVAL. The focused row stays the same
 * row and, when it can, at the same place on the screen; marks and icons
 * move with their rows. A first row takes the focus.
 */
size_t fplinux_ui_listview_insert(struct fplinux_ui_listview *list,
				  size_t index, const char *text,
				  const char *detail);
/*
 * Deletes a row; false with errno EINVAL out of range. A deleted focus goes
 * to the row that takes its index, else to the new last row; a radio list
 * then marks that row when the marked row went. No callback runs.
 */
bool fplinux_ui_listview_remove(struct fplinux_ui_listview *list, size_t index);
bool fplinux_ui_listview_set_text(struct fplinux_ui_listview *list,
				  size_t index, const char *text,
				  const char *detail);
void fplinux_ui_listview_clear(struct fplinux_ui_listview *list);

/*
 * Shows icon, an image no larger than the row's icon box, centred in the
 * box, and destroys it with the row; NULL removes the icon. An alpha-only
 * (A8) image is drawn in the row's text colour. On failure (EINVAL for an
 * index out of range or a virtual list) the caller keeps icon. Once a row
 * has had an icon, every row has a box until the list is cleared, so that
 * the texts line up.
 */
bool fplinux_ui_listview_set_icon(struct fplinux_ui_listview *list,
				  size_t index, lv_draw_buf_t *icon);
size_t fplinux_ui_listview_count(const struct fplinux_ui_listview *list);

/*
 * Check lists mark any rows; a radio list marks at most one: marking a row
 * moves the mark there and unmarking a radio row is ignored. OK does not
 * change marks; the owner marks rows, usually from the activate callback.
 * A virtual list keeps no marks of its own: its visible rows show the
 * marks the row callback gave.
 */
void fplinux_ui_listview_set_marked(struct fplinux_ui_listview *list,
				    size_t index, bool marked);
bool fplinux_ui_listview_is_marked(const struct fplinux_ui_listview *list,
				   size_t index);

/* The focused row; SIZE_MAX for an empty list. */
size_t fplinux_ui_listview_selected(const struct fplinux_ui_listview *list);
void fplinux_ui_listview_select(struct fplinux_ui_listview *list, size_t index);
/* The focused row's object, or NULL. */
lv_obj_t *fplinux_ui_listview_selected_row(struct fplinux_ui_listview *list);

/*
 * A list is selectable by default. A read-only list, such as name and value
 * facts, draws no selection: Up and Down scroll it by one row and Left and
 * Right by a page while it does not fit, and OK is left to the application.
 */
void fplinux_ui_listview_set_selectable(struct fplinux_ui_listview *list,
					bool selectable);
/*
 * An active list's focused row takes the marquee; a list on a
 * screen below the top one is inactive. Lists start active.
 */
void fplinux_ui_listview_set_active(struct fplinux_ui_listview *list,
				    bool active);

/* select runs after the selection moves; activate runs for OK on a row. */
void fplinux_ui_listview_set_callbacks(struct fplinux_ui_listview *list,
				       fplinux_ui_listview_cb select,
				       fplinux_ui_listview_cb activate,
				       void *context);

/*
 * Up and Down move by a row and Left and Right by a visible page, without
 * wrapping, and are used at either end of a non-empty list; OK runs the
 * activate callback and is used only when one is set. A release is used
 * when its press was. Callbacks may delete the list.
 */
bool fplinux_ui_listview_key(struct fplinux_ui_listview *list,
			     const struct fplinux_ui_key *key);

/*
 * A virtual list: count rows whose text, detail, icon and
 * mark come from row each time a row becomes visible or is invalidated.
 * Only the visible rows plus one exist as objects. row fills a zeroed
 * struct fplinux_ui_virtual_row and returns false for an empty row. The
 * list starts inactive; row first runs when the list is first made active.
 */
typedef bool (*fplinux_ui_listview_row_cb)(struct fplinux_ui_listview *list,
					   size_t index,
					   struct fplinux_ui_virtual_row *row,
					   void *context);
struct fplinux_ui_listview *
fplinux_ui_listview_create_virtual(struct fplinux_ui *ui, lv_obj_t *parent,
				   enum fplinux_ui_listview_marker marker,
				   size_t count, fplinux_ui_listview_row_cb row,
				   void *context);
bool fplinux_ui_listview_is_virtual(const struct fplinux_ui_listview *list);
/* The data of rows first .. first + count - 1 changed. */
void fplinux_ui_listview_invalidate(struct fplinux_ui_listview *list,
				    size_t first, size_t count);
/* count data rows were inserted before index, or deleted from index. */
void fplinux_ui_listview_rows_inserted(struct fplinux_ui_listview *list,
				       size_t index, size_t count);
void fplinux_ui_listview_rows_deleted(struct fplinux_ui_listview *list,
				      size_t index, size_t count);

#endif
