/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* LVGL-free rules of the high-level screens; not installed. */

#ifndef FPLINUX_UI_SCREEN_MODEL_H
#define FPLINUX_UI_SCREEN_MODEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

/* No command for a key. */
#define FPLINUX_UI_COMMAND_NONE ((size_t)-1)

/* Where a command type may go; BACK, CANCEL, EXIT and STOP share BACK. */
enum fplinux_ui_command_role {
	FPLINUX_UI_ROLE_BACK,
	FPLINUX_UI_ROLE_OK,
	FPLINUX_UI_ROLE_HELP,
	FPLINUX_UI_ROLE_OTHER,
};

struct fplinux_ui_command_entry {
	enum fplinux_ui_command_role role;
	int priority;
};

/* Command indices for the three keys and the options menu. */
struct fplinux_ui_command_keys {
	size_t right;
	size_t ok;
	size_t left;
	/* The left soft key opens the options menu with menu_count commands. */
	bool options;
	size_t menu_count;
};

/*
 * Assigns commands by priority, then by their order: the right soft key gets
 * the first BACK-role command; OK the first OK-role command unless the
 * focused element acts on OK; the left soft key the single remaining command,
 * or "Options" for several or for any HELP command. When Clear uses the right
 * soft key, the displaced BACK-role command stays in the remaining commands
 * and forces Options even when it is the only one, so leaving never erases text.
 * menu receives the options menu's command indices and must hold count.
 * Item commands come first in commands, before the screen's, so that they
 * win ties in priority.
 */
void fplinux_ui_command_keys_assign(
	const struct fplinux_ui_command_entry *commands, size_t count,
	bool element_acts, bool right_taken,
	struct fplinux_ui_command_keys *keys, size_t *menu);

/*
 * The collapse rule, for a left legend that does not fit beside the centre
 * and right legends: the command of keys->left becomes the first entry of
 * the options menu and the left soft key shows "Options". menu holds the
 * order from fplinux_ui_command_keys_assign() and room for one more. Nothing
 * changes when keys->left is FPLINUX_UI_COMMAND_NONE.
 */
void fplinux_ui_command_keys_collapse(struct fplinux_ui_command_keys *keys,
				      size_t *menu);

/*
 * Rows of lists and virtual lists, items of forms and marks after an
 * insertion or deletion; SIZE_MAX means none. A list that was empty has no
 * focus to move: the caller focuses its first row, as an append does. An
 * EXCLUSIVE list keeps its one mark: when fplinux_ui_index_deleted() drops
 * the mark, the caller marks the row fplinux_ui_focus_deleted() focuses.
 */
/* The index of a row after count rows were inserted before at. */
size_t fplinux_ui_index_inserted(size_t index, size_t at, size_t count);
/*
 * The focus after count rows were deleted from at in a list of size rows
 * (size before the deletion): the same row; a deleted focus goes to the row
 * that takes its index, else to the new last row; SIZE_MAX when the list
 * becomes empty or focus was SIZE_MAX.
 */
size_t fplinux_ui_focus_deleted(size_t focus, size_t at, size_t count,
				size_t size);
/* A mark, or any index but the focus, after a deletion; SIZE_MAX if deleted. */
size_t fplinux_ui_index_deleted(size_t index, size_t at, size_t count);
/*
 * The form focus when the focused item can no longer have it (hidden,
 * deleted): the first item at or after from for which focusable() is true,
 * else the nearest one before from; SIZE_MAX when none.
 */
size_t fplinux_ui_focus_next(size_t from, size_t count,
			     bool (*focusable)(size_t index, void *context),
			     void *context);
/*
 * The focus moved by rows, without wrapping: Up and Down move it by one,
 * Left and Right by a page. No focus gives the first row; an empty list
 * gives SIZE_MAX.
 */
size_t fplinux_ui_focus_step(size_t focus, size_t count, long rows);

/*
 * The least scroll that brings the span top .. bottom (inclusive, content
 * coordinates on the screen) into the view view_top .. view_bottom, or its
 * top when it is taller: the distance to move the content down (negative:
 * up), 0 when it is in view.
 */
int32_t fplinux_ui_reveal_scroll(int32_t view_top, int32_t view_bottom,
				 int32_t top, int32_t bottom);

/* The most pooled rows of a virtual list. */
#define FPLINUX_UI_VIRTUAL_SLOTS_MAX 16U

/*
 * The rows a virtual list shows. Data row r lies at content y r * pitch (the
 * caller adds the list padding) and is shown by pooled row (slot) r % slots,
 * which holds the rows first .. first + slots - 1. slots is
 * min(count, FPLINUX_UI_VIRTUAL_SLOTS_MAX, (view + pitch - 2) / pitch + 1),
 * the most rows a view can show in part, so the pool covers every visible
 * row. The focused row is always wholly visible. count * pitch must fit
 * int32_t, as LVGL coordinates do.
 *
 * The functions taking rebind (room for FPLINUX_UI_VIRTUAL_SLOTS_MAX) write
 * into it, in ascending order, the slots whose row changed or whose row's
 * data changed, and return how many: the caller fills each from the row
 * callback and places it at bound * pitch, or hides it when its bound is
 * SIZE_MAX. One row up or down rebinds at most one slot.
 */
struct fplinux_ui_virtual_window {
	size_t count;
	size_t slots;
	int32_t pitch;
	/* The height for rows, list padding excluded. */
	int32_t view;
	/* SIZE_MAX when count is 0. */
	size_t focus;
	/* The content y at the top of the view, up to count * pitch - view. */
	int32_t top;
	size_t first;
	/* The data row each slot shows, SIZE_MAX for none. */
	size_t bound[FPLINUX_UI_VIRTUAL_SLOTS_MAX];
};

/* Focuses the first row and shows the top of the list. */
size_t fplinux_ui_virtual_init(struct fplinux_ui_virtual_window *window,
			       size_t count, int32_t pitch, int32_t view,
			       size_t *rebind);
/*
 * Focus changes scroll the least that shows the focused row wholly, as
 * lv_obj_scroll_to_view() does. A focus out of range changes nothing.
 */
size_t fplinux_ui_virtual_set_focus(struct fplinux_ui_virtual_window *window,
				    size_t focus, size_t *rebind);
/*
 * count rows were inserted before at, or deleted from at, in the data. The
 * focus follows the index rules above and keeps its place in the view when
 * the list can scroll there, else the list scrolls the least needed. An
 * insertion into an empty list focuses the first row.
 */
size_t fplinux_ui_virtual_inserted(struct fplinux_ui_virtual_window *window,
				   size_t at, size_t count, size_t *rebind);
size_t fplinux_ui_virtual_deleted(struct fplinux_ui_virtual_window *window,
				  size_t at, size_t count, size_t *rebind);
/* The data of rows first .. first + count - 1 changed. */
size_t fplinux_ui_virtual_invalidate(struct fplinux_ui_virtual_window *window,
				     size_t first, size_t count,
				     size_t *rebind);
/*
 * The row pitch (first detail) or the view (title set) changed; the focus
 * keeps its place in the view as after an insertion.
 */
size_t fplinux_ui_virtual_resize(struct fplinux_ui_virtual_window *window,
				 int32_t pitch, int32_t view, size_t *rebind);

/* System words shown on soft keys, in menus and in alert titles. */
enum fplinux_ui_label {
	FPLINUX_UI_LABEL_OPTIONS,
	FPLINUX_UI_LABEL_SELECT,
	FPLINUX_UI_LABEL_MARK,
	FPLINUX_UI_LABEL_UNMARK,
	FPLINUX_UI_LABEL_EDIT,
	FPLINUX_UI_LABEL_DONE,
	FPLINUX_UI_LABEL_CLOSE,
	FPLINUX_UI_LABEL_CLEAR,
	FPLINUX_UI_LABEL_BACK,
	FPLINUX_UI_LABEL_CANCEL,
	FPLINUX_UI_LABEL_EXIT,
	FPLINUX_UI_LABEL_STOP,
	FPLINUX_UI_LABEL_OK,
	FPLINUX_UI_LABEL_HELP,
	FPLINUX_UI_LABEL_NEW_LINE,
	FPLINUX_UI_LABEL_INSERT,
	FPLINUX_UI_LABEL_INFORMATION,
	FPLINUX_UI_LABEL_CONFIRMATION,
	FPLINUX_UI_LABEL_WARNING,
	FPLINUX_UI_LABEL_ERROR,
	FPLINUX_UI_LABEL_COUNT,
};

/*
 * The gettext domain of the runtime's own words. A label is shown as
 * dgettext(FPLINUX_UI_TEXT_DOMAIN, fplinux_ui_label(label)).
 */
#define FPLINUX_UI_TEXT_DOMAIN "fplinux-ui"

/* The English source text of a label; "" for an unknown one. */
const char *fplinux_ui_label(enum fplinux_ui_label label);

/* Date and time parts; the editor shows them in the language's order. */
enum fplinux_ui_date_part {
	FPLINUX_UI_DATE_DAY,
	FPLINUX_UI_DATE_MONTH,
	FPLINUX_UI_DATE_YEAR,
	FPLINUX_UI_DATE_HOUR,
	FPLINUX_UI_DATE_MINUTE,
	FPLINUX_UI_DATE_PART_COUNT,
};

#define FPLINUX_UI_DATE_YEAR_MIN 1900
#define FPLINUX_UI_DATE_YEAR_MAX 2099
/* The text before a part, such as "/" or ". ", and its terminator. */
#define FPLINUX_UI_DATE_SEPARATOR_BYTES 8U
/* The longest edited text: twelve digits, four separators, terminator. */
#define FPLINUX_UI_DATE_TEXT_BYTES \
	(12U + 4U * (FPLINUX_UI_DATE_SEPARATOR_BYTES - 1U) + 1U)
/* The layout without a usable pattern: "DD.MM.YYYY". */
#define FPLINUX_UI_DATE_DEFAULT_PATTERN "{dd}.{mm}.{yyyy}"

/*
 * Edits the parts of a local time in order[0..count), each preceded by its
 * separator. typed counts the digits entered into the focused part; after
 * two, or four for the year, the part is complete and the focus moves on.
 * Seconds and the parts not edited keep their values. The day shown is the
 * day last typed or stepped, shortened to the month shown: entering 31,
 * then September, then October gives 31 October.
 */
struct fplinux_ui_date_edit {
	struct tm time;
	int values[FPLINUX_UI_DATE_PART_COUNT];
	int day;
	enum fplinux_ui_date_part order[FPLINUX_UI_DATE_PART_COUNT];
	char separator[FPLINUX_UI_DATE_PART_COUNT]
		      [FPLINUX_UI_DATE_SEPARATOR_BYTES];
	unsigned int count;
	unsigned int position;
	enum fplinux_ui_date_part part;
	unsigned int typed;
};

/*
 * date and time select the parts: both, the date only or the time only. The
 * date parts follow pattern, the language's short date pattern such as
 * "{mm}/{dd}/{yyyy}": the order of its day ({dd} or {day}), month ({mm},
 * {month} or {month.genitive}, edited as a number) and year ({yyyy}), and the
 * text between them; text before the first and after the last is left out.
 * A NULL pattern, or one without each of the three exactly once, with other
 * placeholders or with longer separators, gives the default layout. The
 * time follows the date after a space as "HH:MM".
 */
void fplinux_ui_date_edit_begin(struct fplinux_ui_date_edit *edit, time_t value,
				bool date, bool time, const char *pattern);
/* One step down (-1) or up (+1), wrapping inside the part's range. */
void fplinux_ui_date_edit_step(struct fplinux_ui_date_edit *edit, int delta);
/* The previous (-1) or next (+1) part, wrapping. */
void fplinux_ui_date_edit_move(struct fplinux_ui_date_edit *edit, int delta);
void fplinux_ui_date_edit_digit(struct fplinux_ui_date_edit *edit,
				unsigned int digit);
/* Completes a partly typed part and fits the day to its month. */
void fplinux_ui_date_edit_finish(struct fplinux_ui_date_edit *edit);
time_t fplinux_ui_date_edit_value(const struct fplinux_ui_date_edit *edit);
/*
 * Writes the edited parts, such as "10/01/2026 14:05", into at least
 * FPLINUX_UI_DATE_TEXT_BYTES and returns the focused part's range in
 * characters, as LVGL selects label text.
 */
void fplinux_ui_date_edit_format(const struct fplinux_ui_date_edit *edit,
				 char *text, size_t size, size_t *start,
				 size_t *end);

#endif
