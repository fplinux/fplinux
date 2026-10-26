/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef FPLINUX_UI_SCREEN_H
#define FPLINUX_UI_SCREEN_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#include "fplinux-ui.h"
#include "fplinux-ui-text.h"

/*
 * High-level screens and commands.
 *
 * An application describes screens (List, Form, TextBox, Alert or Canvas)
 * and attaches typed commands to them. The runtime draws the title band and
 * the content, moves focus, scrolls, and assigns commands to the keys so that
 * every application behaves alike:
 *
 *   right soft key  the first BACK, CANCEL, EXIT or STOP command by priority;
 *   OK              the focused element's own action (open, select, mark,
 *                   edit), otherwise the first OK command;
 *   left soft key   the remaining command if there is one, otherwise
 *                   "Options", which opens the options menu with all the
 *                   remaining commands in priority order.
 *
 * Legends follow the assignment and never repeat. While the stack holds a
 * screen, the runtime handles every key for it: the application receives
 * commands and item changes, not key events, except through a screen key
 * filter or a Canvas key handler. VISIBILITY and CLOSE events still reach the fplinux_ui_open()
 * callback. Screens are stacked: the top screen is shown, and the runtime
 * deletes a screen when it is popped or replaced. The launcher's base
 * Canvas keeps its system bars visible behind ordinary screens.
 */
struct fplinux_ui_screen;

enum fplinux_ui_command_type {
	FPLINUX_UI_COMMAND_BACK,
	FPLINUX_UI_COMMAND_CANCEL,
	FPLINUX_UI_COMMAND_EXIT,
	FPLINUX_UI_COMMAND_STOP,
	FPLINUX_UI_COMMAND_OK,
	FPLINUX_UI_COMMAND_ITEM,
	FPLINUX_UI_COMMAND_SCREEN,
	FPLINUX_UI_COMMAND_HELP,
};

/*
 * id is the application's; a lower priority number chooses keys first, ties
 * keep the order of addition. Options keeps that order with Back last.
 * A NULL label uses the type's word in the system language
 * (Back, Cancel, Exit, Stop, OK, Help); ITEM and SCREEN commands need one.
 * Labels are printable text shorter than 128 bytes. HELP always goes to the
 * options menu. While the right soft key is Clear for a text field holding
 * text, its displaced back-type command remains available in Options; it
 * returns to the right key when the text is empty or focus moves.
 */
struct fplinux_ui_command {
	int id;
	enum fplinux_ui_command_type type;
	const char *label;
	int priority;
	/*
	 * Optional image shown in Options, such as a row-sized system glyph.
	 * NULL uses the type's standard glyph for Back, Cancel, Exit, Stop, OK
	 * and Help; ITEM and SCREEN have no default. Supply an image when a
	 * custom label gives the action another meaning, such as Play or Clear.
	 */
	const void *icon;
	/*
	 * Retains a directly assigned soft-key command: the left action stays
	 * visible when long legends need shortening, and the right action is
	 * not displaced by a text field's Clear (then offered on the left or
	 * in Options). Does not override command
	 * priority or move commands out of Options.
	 */
	bool keep_visible;
};

/* An implicit List reports OK on a row with this reserved command id. */
#define FPLINUX_UI_SELECT_COMMAND (-2)

typedef void (*fplinux_ui_command_cb)(struct fplinux_ui *ui,
				      struct fplinux_ui_screen *screen,
				      int command_id, void *context);
/* A form item or list row changed by the user; index identifies it. */
typedef void (*fplinux_ui_change_cb)(struct fplinux_ui *ui,
				     struct fplinux_ui_screen *screen,
				     size_t index, void *context);

/* Labels are copied. Adding a command id twice replaces it. */
bool fplinux_ui_screen_add_command(struct fplinux_ui_screen *screen,
				   const struct fplinux_ui_command *command);
void fplinux_ui_screen_remove_command(struct fplinux_ui_screen *screen, int id);
void fplinux_ui_screen_set_listener(struct fplinux_ui_screen *screen,
				    fplinux_ui_command_cb command,
				    fplinux_ui_change_cb change, void *context);

/*
 * Item commands: a list row's or form item's own
 * commands, offered only while it has focus. The soft keys then choose from
 * the screen's commands and the focused item's, by priority, with an item
 * command first on equal priority; the options menu lists them in that
 * order. An OK command of a row of an IMPLICIT list replaces
 * FPLINUX_UI_SELECT_COMMAND on that row. Labels are copied; adding an id
 * twice to the same row or item replaces it. Commands move with their rows
 * when rows are inserted or deleted; a hidden item offers none. Fails with
 * EINVAL for another kind of screen, an index out of range or a command
 * that fplinux_ui_screen_add_command() would refuse.
 */
bool fplinux_ui_list_add_item_command(struct fplinux_ui_screen *list,
				      size_t index,
				      const struct fplinux_ui_command *command);
bool fplinux_ui_form_add_item_command(struct fplinux_ui_screen *form,
				      size_t item,
				      const struct fplinux_ui_command *command);
void fplinux_ui_screen_remove_item_command(struct fplinux_ui_screen *screen,
					   size_t index, int id);
typedef void (*fplinux_ui_item_command_cb)(struct fplinux_ui *ui,
					   struct fplinux_ui_screen *screen,
					   size_t index, int command_id,
					   void *context);
/*
 * Without an item listener, item commands reach the screen's command
 * listener; fplinux_ui_screen_get_command_item() then names the item.
 */
void fplinux_ui_screen_set_item_listener(struct fplinux_ui_screen *screen,
					 fplinux_ui_item_command_cb listener,
					 void *context);
/* During a command callback: the item whose command fired, else SIZE_MAX. */
size_t
fplinux_ui_screen_get_command_item(const struct fplinux_ui_screen *screen);
bool fplinux_ui_screen_set_title(struct fplinux_ui_screen *screen,
				 const char *title);
/*
 * One right-aligned status in the existing title band. Strings are copied;
 * NULL or empty full clears it, and NULL compact uses full. Full is shown
 * when it fits beside the whole title and the normal spacing; otherwise
 * compact is shown and only the title may end with dots. The caller supplies
 * a compact status that fits the band. Neither text scrolls while a status
 * is set. A status does not create a band without a title. False with EINVAL
 * for an alert or NULL screen; an allocation failure keeps the old status.
 */
bool fplinux_ui_screen_set_header_status(struct fplinux_ui_screen *screen,
					 const char *full, const char *compact);
/* The screen's data pointer for the application. */
void fplinux_ui_screen_set_data(struct fplinux_ui_screen *screen, void *data);
void *fplinux_ui_screen_get_data(const struct fplinux_ui_screen *screen);
/*
 * The size of the screen's content area for the current fplinux_ui_width()
 * and fplinux_ui_height(): below its title band, or all of it without a
 * title, such as a Canvas's drawing area. For an alert, the inside of its
 * box at the largest size the box may take. False with EINVAL for a NULL
 * argument.
 */
bool fplinux_ui_screen_get_content_size(const struct fplinux_ui_screen *screen,
					int *width, int *height);

/* The screen stack. push, navigate and replace take ownership of the screen. */
void fplinux_ui_push(struct fplinux_ui *ui, struct fplinux_ui_screen *screen);
/*
 * Opens a child. During a high-level Options command callback, Back from
 * this child also restores that owner's Options, with the selected command
 * and scroll position. Other calls act like push. The command's type does
 * not establish navigation: SCREEN and HELP still describe its key scope.
 */
void fplinux_ui_navigate(struct fplinux_ui *ui,
			 struct fplinux_ui_screen *screen);
void fplinux_ui_replace(struct fplinux_ui *ui,
			struct fplinux_ui_screen *screen);
/* Completes an action without restoring Options; false for the only screen. */
bool fplinux_ui_pop(struct fplinux_ui *ui);
struct fplinux_ui_screen *fplinux_ui_current(const struct fplinux_ui *ui);
/*
 * Returns to the immediate parent, restoring its current Options after a
 * navigate from that menu. Removed commands are not restored. Leaves the
 * application from its first screen.
 */
void fplinux_ui_go_back(struct fplinux_ui *ui);

/*
 * List. IMPLICIT reports OK on a row as FPLINUX_UI_SELECT_COMMAND; EXCLUSIVE
 * marks exactly one row (radio) and MULTIPLE toggles rows (check), both
 * reporting the change through the change callback. EXCLUSIVE reports OK
 * also on the row already marked, so that OK can mean "choose and return";
 * unmarking a row of an EXCLUSIVE list is ignored.
 */
enum fplinux_ui_list_kind {
	FPLINUX_UI_LIST_IMPLICIT,
	FPLINUX_UI_LIST_EXCLUSIVE,
	FPLINUX_UI_LIST_MULTIPLE,
};

struct fplinux_ui_screen *fplinux_ui_list_new(struct fplinux_ui *ui,
					      const char *title,
					      enum fplinux_ui_list_kind kind);
/*
 * detail, which may be NULL, is shown on a second line under the text; the
 * first detail gives every row of the list the taller detail pitch.
 */
size_t fplinux_ui_list_append(struct fplinux_ui_screen *list, const char *text,
			      const char *detail);
/*
 * Inserts a row before index (index == size appends). Returns its index, or
 * SIZE_MAX with errno EINVAL for index > size. Rows after it, their marks,
 * icons and item commands move down by one; the focused row stays the
 * same row, at the same place on the screen when it can. A first row takes
 * the focus. Programmatic changes never call the change listener.
 */
size_t fplinux_ui_list_insert(struct fplinux_ui_screen *list, size_t index,
			      const char *text, const char *detail);
/*
 * Deletes the row at index; false with EINVAL when out of range. A deleted
 * focus goes to the row that takes its index, or to the new last row; when
 * an EXCLUSIVE list loses its marked row, the row that receives the focus
 * is marked.
 */
bool fplinux_ui_list_delete(struct fplinux_ui_screen *list, size_t index);
bool fplinux_ui_list_set(struct fplinux_ui_screen *list, size_t index,
			 const char *text, const char *detail);
void fplinux_ui_list_delete_all(struct fplinux_ui_screen *list);
size_t fplinux_ui_list_size(const struct fplinux_ui_screen *list);
/*
 * Up and Down move the focus by a row and wrap at the ends. Left and Right
 * move by a visible page and stop at the ends. An empty list has no focus:
 * SIZE_MAX.
 */
size_t fplinux_ui_list_get_focused(const struct fplinux_ui_screen *list);
/*
 * The first completely visible row; SIZE_MAX for an empty list or another
 * screen. If no whole row fits, returns the first intersecting row.
 */
size_t fplinux_ui_list_get_first_visible(const struct fplinux_ui_screen *list);
void fplinux_ui_list_set_focused(struct fplinux_ui_screen *list, size_t index);
/*
 * Reports a focus move made with the navigation keys, separately from the
 * change listener for marked values. Programmatic focus and row edits stay
 * quiet. NULL removes the listener; the callback may delete the list.
 */
void fplinux_ui_list_set_focus_listener(struct fplinux_ui_screen *list,
					fplinux_ui_change_cb focused,
					void *context);
bool fplinux_ui_list_is_selected(const struct fplinux_ui_screen *list,
				 size_t index);
void fplinux_ui_list_set_selected(struct fplinux_ui_screen *list, size_t index,
				  bool selected);
/* The centre legend for IMPLICIT/EXCLUSIVE lists; NULL restores "Select". */
bool fplinux_ui_list_set_select_label(struct fplinux_ui_screen *list,
				      const char *label);

/*
 * Images take an LVGL image source: the path of a PNG file, or a pointer to
 * an lv_image_dsc_t in any format LVGL draws, including PNG data. The runtime
 * decodes the image once when it is set, scaled down with its proportions
 * kept and never scaled up, and keeps its own copy: the file and the
 * descriptor are not used afterwards. A file may hold up to 1 MiB and an
 * image up to one million pixels. Failures set errno: the file's own error,
 * EINVAL for an image that cannot be decoded, EFBIG above the limits.
 */

enum fplinux_ui_list_icon_kind {
	FPLINUX_UI_LIST_ICON_IMAGE,
	FPLINUX_UI_LIST_ICON_APP_TILE,
};

/*
 * Shows src at the left of the row in a square box of the profile's
 * row_icon_size; NULL removes the icon. An alpha-only image (a system glyph
 * or a "-symbolic.png" file) is drawn in the row's text colour. APP_TILE
 * adds the theme's tile ring around a colour image while its row is focused.
 * Once a row has an icon, the rows without one keep an empty box.
 * An unknown kind fails with EINVAL.
 */
bool fplinux_ui_list_set_icon(struct fplinux_ui_screen *list, size_t index,
			      const void *src,
			      enum fplinux_ui_list_icon_kind kind);

/*
 * Virtual list, for large data sets: a List whose rows are not stored. The
 * runtime asks the row callback for a row each time it becomes visible or
 * is invalidated, keeping objects only for the visible rows, so memory does
 * not grow with count. It looks like any List, details, icons and markers
 * included. OK on a row of an EXCLUSIVE or MULTIPLE virtual list calls the
 * change listener: the application updates its data and invalidates the
 * row. fplinux_ui_list_append(), _insert(), _delete(), _set(),
 * _is_selected(), _set_selected() and _set_icon() fail with EINVAL on a
 * virtual list; focus and item commands work by index.
 *
 * struct fplinux_ui_virtual_row is allocated and zeroed by the runtime
 * before each call. size is the number of bytes the runtime allocated:
 * write a field only when FPLINUX_UI_VIRTUAL_ROW_HAS(row, field) is true,
 * so that an application built against a newer header runs on an older
 * runtime. Later versions append fields. The runtime copies the strings
 * and decodes the icon after the callback returns, so they must stay valid
 * until the runtime calls the row callback again or the application's code
 * runs again: point them at the application's data or at a static buffer,
 * never at a local array of the callback.
 */
struct fplinux_ui_virtual_row {
	size_t size;
	/* Required. */
	const char *text;
	/* NULL for none; a second line as in every list. */
	const char *detail;
	/* An image source as for fplinux_ui_list_set_icon(), or NULL. */
	const void *icon;
	/* EXCLUSIVE and MULTIPLE lists. */
	bool marked;
	/* IMAGE by default; APP_TILE adds the focused tile ring. */
	enum fplinux_ui_list_icon_kind icon_kind;
};

#define FPLINUX_UI_VIRTUAL_ROW_HAS(row, field)            \
	(offsetof(struct fplinux_ui_virtual_row, field) + \
		 sizeof((row)->field) <=                  \
	 (row)->size)

/* Fills row for index; returns false to show an empty row. */
typedef bool (*fplinux_ui_virtual_row_cb)(struct fplinux_ui *ui,
					  struct fplinux_ui_screen *list,
					  size_t index,
					  struct fplinux_ui_virtual_row *row,
					  void *context);
struct fplinux_ui_screen *
fplinux_ui_list_new_virtual(struct fplinux_ui *ui, const char *title,
			    enum fplinux_ui_list_kind kind, size_t count,
			    fplinux_ui_virtual_row_cb row, void *context);
/* The data of rows first .. first + count - 1 changed: visible ones are asked again. */
void fplinux_ui_list_invalidate(struct fplinux_ui_screen *list, size_t first,
				size_t count);
/* count rows were inserted before index / deleted from index in the data. */
void fplinux_ui_list_rows_inserted(struct fplinux_ui_screen *list, size_t index,
				   size_t count);
void fplinux_ui_list_rows_deleted(struct fplinux_ui_screen *list, size_t index,
				  size_t count);

/*
 * Form: a vertical sequence of items under the title band. Up and Down move
 * focus between interactive items and scroll through read-only ones. Item
 * indices are assigned in order from 0 by the append functions; to insert,
 * append and then move the item.
 */
enum fplinux_ui_choice_kind {
	FPLINUX_UI_CHOICE_EXCLUSIVE,
	FPLINUX_UI_CHOICE_MULTIPLE,
	/* One line showing the current choice; OK opens the list of choices. */
	FPLINUX_UI_CHOICE_POPUP,
};

enum fplinux_ui_date_mode {
	FPLINUX_UI_DATE,
	FPLINUX_UI_TIME,
	FPLINUX_UI_DATE_TIME,
};

struct fplinux_ui_screen *fplinux_ui_form_new(struct fplinux_ui *ui,
					      const char *title);
/* A read-only label and text; either may be NULL. */
size_t fplinux_ui_form_append_string(struct fplinux_ui_screen *form,
				     const char *label, const char *text);
/*
 * Form text fields are single-line; a TextBox is the multi-line editor.
 * constraint is a type of fplinux-ui-text.h, optionally with
 * | FPLINUX_UI_TEXT_PASSWORD. A DECIMAL field shows the system language's
 * separator, but fplinux_ui_form_get_text() returns its number with '.', as
 * strtod() reads it, and fplinux_ui_form_set_text() takes '.' or ','.
 */
size_t fplinux_ui_form_append_text_field(
	struct fplinux_ui_screen *form, const char *label, const char *text,
	size_t max_chars, enum fplinux_ui_text_constraint constraint);
size_t fplinux_ui_form_append_choice(struct fplinux_ui_screen *form,
				     const char *label,
				     enum fplinux_ui_choice_kind kind,
				     const char *const *choices, size_t count);
/* An interactive gauge changes with Left and Right; otherwise a progress bar. */
size_t fplinux_ui_form_append_gauge(struct fplinux_ui_screen *form,
				    const char *label, int minimum, int maximum,
				    int value, bool interactive);
/*
 * Shows the value in the system language: DATE as its short date, TIME as
 * the 24-hour clock, DATE_TIME as both after a space. OK edits it as numbers
 * in the order shown, such as month, day, year in English: Left/Right move
 * between parts, Up adds 1 to the focused part and Down subtracts 1, digits
 * type.
 */
size_t fplinux_ui_form_append_date(struct fplinux_ui_screen *form,
				   const char *label,
				   enum fplinux_ui_date_mode mode,
				   time_t value);
/*
 * One HH:MM:SS value, from 00:00:00 through 99:59:59, edited like Date/Time.
 * Values are elapsed seconds, independent of the calendar and timezone.
 * Append returns SIZE_MAX and set returns false with ERANGE above 359999.
 */
size_t fplinux_ui_form_append_duration(struct fplinux_ui_screen *form,
				       const char *label, unsigned int seconds);
/*
 * A read-only image, centred and no wider than the form; label may be NULL.
 * The source is an image source as for fplinux_ui_list_set_icon().
 */
size_t fplinux_ui_form_append_image(struct fplinux_ui_screen *form,
				    const char *label, const void *src);

bool fplinux_ui_form_set_string(struct fplinux_ui_screen *form, size_t item,
				const char *text);
const char *fplinux_ui_form_get_text(const struct fplinux_ui_screen *form,
				     size_t item);
bool fplinux_ui_form_set_text(struct fplinux_ui_screen *form, size_t item,
			      const char *text);
/* Exclusive and popup choices: the chosen index. */
size_t fplinux_ui_form_get_choice(const struct fplinux_ui_screen *form,
				  size_t item);
void fplinux_ui_form_set_choice(struct fplinux_ui_screen *form, size_t item,
				size_t index);
/* Multiple choices: one flag per choice. */
bool fplinux_ui_form_is_chosen(const struct fplinux_ui_screen *form,
			       size_t item, size_t index);
void fplinux_ui_form_set_chosen(struct fplinux_ui_screen *form, size_t item,
				size_t index, bool chosen);
int fplinux_ui_form_get_value(const struct fplinux_ui_screen *form,
			      size_t item);
void fplinux_ui_form_set_value(struct fplinux_ui_screen *form, size_t item,
			       int value);
time_t fplinux_ui_form_get_date(const struct fplinux_ui_screen *form,
				size_t item);
void fplinux_ui_form_set_date(struct fplinux_ui_screen *form, size_t item,
			      time_t value);
/*
 * Called when the user accepts a Date, Time or Duration editor with Done,
 * including an unchanged value. Cancel does not call it. The callback runs
 * before the ordinary change listener, which still reports only changed
 * values. NULL removes the listener.
 */
void fplinux_ui_form_set_edit_listener(struct fplinux_ui_screen *form,
				       fplinux_ui_change_cb accepted,
				       void *context);
/* A Date, Time or Duration editor is active; false for NULL or another screen. */
bool fplinux_ui_form_is_editing(const struct fplinux_ui_screen *form);
/* UINT_MAX with EINVAL for another item or screen. */
unsigned int fplinux_ui_form_get_duration(const struct fplinux_ui_screen *form,
					  size_t item);
bool fplinux_ui_form_set_duration(struct fplinux_ui_screen *form, size_t item,
				  unsigned int seconds);
void fplinux_ui_form_set_focus(struct fplinux_ui_screen *form, size_t item);
/* The focused item; SIZE_MAX when no item has focus or for another screen. */
size_t fplinux_ui_form_get_focus(const struct fplinux_ui_screen *form);

/*
 * The appearance of a string or image item.
 * PLAIN, the default, is read-only content. A HYPERLINK or BUTTON item is
 * drawn as a link or a button and takes the focus like an interactive item
 * while it has item commands (fplinux_ui_form_add_item_command()): OK runs
 * the first of them by priority and the soft keys offer the others. Fails
 * with EINVAL for another kind of item or screen or an unknown appearance.
 */
enum fplinux_ui_appearance {
	FPLINUX_UI_APPEARANCE_PLAIN,
	FPLINUX_UI_APPEARANCE_HYPERLINK,
	FPLINUX_UI_APPEARANCE_BUTTON,
};

bool fplinux_ui_form_set_appearance(struct fplinux_ui_screen *form, size_t item,
				    enum fplinux_ui_appearance appearance);

/*
 * The item at from moves to index to and the items between shift by one;
 * their indices, focus and item commands follow. False with EINVAL out of
 * range.
 */
bool fplinux_ui_form_move_item(struct fplinux_ui_screen *form, size_t from,
			       size_t to);
/*
 * Deletes an item; the items after it shift down by one. A deleted focused
 * item passes the focus to the next item that can take it, else to the
 * previous one. False with EINVAL out of range.
 */
bool fplinux_ui_form_delete(struct fplinux_ui_screen *form, size_t item);
/* Leaves an empty form with no focus. */
void fplinux_ui_form_delete_all(struct fplinux_ui_screen *form);
/*
 * A hidden item keeps its index, takes no space and never has focus; a
 * focused item that is hidden passes the focus on as a deleted one does.
 */
bool fplinux_ui_form_set_hidden(struct fplinux_ui_screen *form, size_t item,
				bool hidden);
bool fplinux_ui_form_is_hidden(const struct fplinux_ui_screen *form,
			       size_t item);
/*
 * Changes a text field's constraint, as fplinux_ui_text_set_constraint()
 * does; FPLINUX_UI_TEXT_UNEDITABLE makes it read-only. False with EINVAL
 * for an item that is not a text field or a constraint it refuses.
 */
bool fplinux_ui_form_set_constraint(struct fplinux_ui_screen *form, size_t item,
				    enum fplinux_ui_text_constraint constraint);

/*
 * TextBox: a full-screen multi-line text field. While it holds text the
 * right soft key is Clear; when it is empty, the screen's BACK command.
 */
struct fplinux_ui_screen *fplinux_ui_text_box_new(struct fplinux_ui *ui,
						  const char *title,
						  const char *text,
						  size_t max_chars);
const char *fplinux_ui_text_box_get(const struct fplinux_ui_screen *box);
bool fplinux_ui_text_box_set(struct fplinux_ui_screen *box, const char *text);
/*
 * Cancels symbol selection without replacing the field.
 * False with EINVAL for a screen that is not a TextBox.
 */
bool fplinux_ui_text_box_cancel_picker(struct fplinux_ui_screen *box);
/*
 * As fplinux_ui_form_set_constraint() for the box's field. A read-only box
 * scrolls its text by a line with Up and Down and by a page with Left and
 * Right, and its right soft key keeps the screen's back command.
 */
bool fplinux_ui_text_box_set_constraint(
	struct fplinux_ui_screen *box,
	enum fplinux_ui_text_constraint constraint);

/*
 * Alert: a message box over the current screen. A timed alert closes itself
 * after timeout_ms without calling the listener; with timeout 0 it stays
 * until a command. Without commands it offers "Close" unless it is busy (see
 * fplinux_ui_alert_set_progress()). A confirmation has the application's
 * commands and starts with the cancelling one on the right soft key. A
 * command removes the alert before its listener runs, so the listener must
 * not pop it; the alert stays readable during the call and is deleted after
 * it. When the alert closed by its timeout, Close or a command was the only
 * screen, the application then leaves as fplinux_ui_go_back() does from its
 * first screen, unless the listener pushed or replaced a screen. A commandless
 * informational alert opened by navigate restores its owner's Options after
 * Close or timeout, provided that owner is still current.
 */
enum fplinux_ui_alert_kind {
	FPLINUX_UI_ALERT_INFO,
	FPLINUX_UI_ALERT_CONFIRMATION,
	FPLINUX_UI_ALERT_WARNING,
	FPLINUX_UI_ALERT_ERROR,
};

/* The semantic frame colour for native and custom alert boxes. */
lv_color_t fplinux_ui_alert_color(const struct fplinux_ui *ui,
				  enum fplinux_ui_alert_kind kind);

struct fplinux_ui_screen *fplinux_ui_alert_new(struct fplinux_ui *ui,
					       enum fplinux_ui_alert_kind kind,
					       const char *title,
					       const char *text,
					       unsigned int timeout_ms);
/*
 * Makes the alert a busy indicator below its text: a spinner when maximum is
 * 0, for work of unknown length, or a progress bar at value out of maximum,
 * clamped, which later calls update. The spinner turns only while the alert
 * is on top and the application is visible. A busy alert ignores its timeout
 * and, without commands, offers no Close: the application removes it with
 * fplinux_ui_pop() while it is on top of another screen, or replaces it when
 * it is the only one. A CANCEL command on the right soft key lets the user
 * stop the work; like every alert command it removes the alert before the
 * listener runs, which then must neither pop nor update it and must forget
 * it: the runtime deletes it after the call.
 * Fails with EINVAL for another screen or a negative maximum.
 */
bool fplinux_ui_alert_set_progress(struct fplinux_ui_screen *alert, int value,
				   int maximum);
/*
 * Replaces the alert's message, also while it is shown; NULL or "" leaves
 * it without one. The box takes the size of the new text as it does when
 * the alert is created. Fails with EINVAL for another screen.
 */
bool fplinux_ui_alert_set_text(struct fplinux_ui_screen *alert,
			       const char *text);

/*
 * Canvas: a screen the application draws itself with LVGL below the title
 * band (no band when title is NULL). Commands still go to the soft keys;
 * every other key goes to the key handler.
 * The drawing area is fplinux_ui_width() x (fplinux_ui_height() -
 * fplinux_ui_metrics(ui)->header_height) with a title, the whole surface
 * without one.
 */
typedef void (*fplinux_ui_canvas_key_cb)(struct fplinux_ui *ui,
					 struct fplinux_ui_screen *canvas,
					 const struct fplinux_ui_key *key,
					 void *context);

struct fplinux_ui_screen *fplinux_ui_canvas_new(struct fplinux_ui *ui,
						const char *title);
lv_obj_t *fplinux_ui_canvas_object(struct fplinux_ui_screen *canvas);
void fplinux_ui_canvas_set_key_handler(struct fplinux_ui_screen *canvas,
				       fplinux_ui_canvas_key_cb handler,
				       void *context);
/*
 * The filter runs before screen commands and ordinary element key routing.
 * Returning true owns the key, including its release; false leaves it to
 * ordinary routing. This lets an input component handle a held soft key.
 */
typedef bool (*fplinux_ui_screen_key_filter_cb)(
	struct fplinux_ui *ui, struct fplinux_ui_screen *screen,
	const struct fplinux_ui_key *key, void *context);
void fplinux_ui_screen_set_key_filter(struct fplinux_ui_screen *screen,
				      fplinux_ui_screen_key_filter_cb filter,
				      void *context);
/*
 * With raw true, Escape and Enter of an external keyboard go to the key
 * handler instead of the right soft key and OK, for a terminal or an editor;
 * the phone keys and the keyboard's Menu key keep the commands. Off by
 * default.
 */
void fplinux_ui_canvas_set_keyboard_keys(struct fplinux_ui_screen *canvas,
					 bool raw);

enum fplinux_ui_canvas_repeat {
	FPLINUX_UI_CANVAS_REPEAT_NONE,
	FPLINUX_UI_CANVAS_REPEAT_NAVIGATION,
	FPLINUX_UI_CANVAS_REPEAT_KEYMAP,
};

/*
 * NONE is the default. NAVIGATION repeats arrows; KEYMAP also repeats other
 * external keys marked repeatable by XKB, for a terminal or text editor.
 * Phone digits and commands remain one-shot. Covering or leaving the Canvas,
 * or changing its repeat policy, cancels the hold.
 */
void fplinux_ui_canvas_set_key_repeat(struct fplinux_ui_screen *canvas,
				      enum fplinux_ui_canvas_repeat policy);

#endif
