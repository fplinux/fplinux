/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Shared text field with phone multi-tap and external keyboard entry. */

#ifndef FPLINUX_UI_TEXT_H
#define FPLINUX_UI_TEXT_H

#include <stdbool.h>
#include <stddef.h>
#include <lvgl/lvgl.h>

#include "fplinux-ui.h"

struct fplinux_ui_text;

/*
 * What a field accepts. The types other than ANY type
 * digits at once and show the mode "123"; only PHONE takes #:
 *
 *   NUMERIC  an integer: * adds or removes a leading minus;
 *   DECIMAL  a number with one decimal separator in the system language's
 *            style: * types the separator, * pressed twice within the
 *            multi-tap second toggles the minus instead, and once there is a
 *            separator * toggles the minus;
 *   PHONE    digits, # and, cycled by repeated *, the characters * + p w
 *            (p pauses dialling, w waits); holding 0 types +.
 *
 * An external keyboard types the same characters; - toggles the minus.
 * PASSWORD may be added to any type with |: the field shows a dot for every
 * committed character and keeps the pending multi-tap character readable
 * until it is committed; it does not capitalize letters on its own.
 * UNEDITABLE may be added too: the field shows its text, still focusable,
 * without caret, mode, counter or symbol picker, and takes no typing, Clear
 * or OK, so the right soft key stays the application's (Back). A single line
 * that does not fit scrolls while focused; a multi-line field scrolls by a
 * line with Up and Down and by a page with Left and Right. The text can still
 * be set.
 */
enum fplinux_ui_text_constraint {
	FPLINUX_UI_TEXT_ANY,
	FPLINUX_UI_TEXT_NUMERIC,
	FPLINUX_UI_TEXT_DECIMAL,
	FPLINUX_UI_TEXT_PHONE,
	FPLINUX_UI_TEXT_PASSWORD = 0x10000,
	FPLINUX_UI_TEXT_UNEDITABLE = 0x20000,
};

/* Runs after the text, caret, mode, emptiness or symbol picker changed. */
typedef void (*fplinux_ui_text_change_cb)(struct fplinux_ui_text *field,
					  void *context);

/*
 * Creates a focused, framed field on parent. The text is limited to
 * max_chars Unicode characters, shown as a counter; 0 means no limit and no
 * counter. The modes are the sentence, lower and upper case of each installed
 * input language, the preferred input language first, then digits; the field
 * starts in the first. Deleting the field's object or its parent deletes it as
 * well.
 * Returns NULL with errno set.
 */
struct fplinux_ui_text *fplinux_ui_text_create(struct fplinux_ui *ui,
					       lv_obj_t *parent, bool multiline,
					       size_t max_chars);
void fplinux_ui_text_delete(struct fplinux_ui_text *field);
/* The frame object, for size and position; its children are private. */
lv_obj_t *fplinux_ui_text_object(struct fplinux_ui_text *field);
/* The callback must not delete the field. NULL removes it. */
void fplinux_ui_text_set_change_cb(struct fplinux_ui_text *field,
				   fplinux_ui_text_change_cb callback,
				   void *context);

/*
 * Feeds a press or release; returns true when the field used it. The right
 * soft key is Clear while the field is not empty, consumed even when nothing
 * precedes the caret, and is left to the application (Back) when it is empty.
 */
bool fplinux_ui_text_key(struct fplinux_ui_text *field,
			 const struct fplinux_ui_key *key);

/*
 * The text with a pending multi-tap character; a password's real text. A
 * DECIMAL field returns the separator it shows, '.' or ','.
 */
const char *fplinux_ui_text_get(const struct fplinux_ui_text *field);
/*
 * Replaces the text and puts the caret at its end; E2BIG above the limit,
 * EINVAL for a text that the constraint does not take. A DECIMAL field
 * takes either separator.
 */
bool fplinux_ui_text_set(struct fplinux_ui_text *field, const char *text);
/*
 * A new field takes ANY. Commits pending input; ANY returns to the first
 * mode. Fails with EINVAL for an unknown constraint or a text that does not
 * suit it, keeping the constraint it had.
 */
bool fplinux_ui_text_set_constraint(struct fplinux_ui_text *field,
				    enum fplinux_ui_text_constraint constraint);
bool fplinux_ui_text_is_empty(const struct fplinux_ui_text *field);
/* The mode's indicator from its language, such as "Abc" or "абв", or "123". */
const char *fplinux_ui_text_indicator(const struct fplinux_ui_text *field);
/* While the symbol picker is open, OK inserts and the right soft key cancels. */
bool fplinux_ui_text_is_picker_open(const struct fplinux_ui_text *field);
/* Cancels only symbol selection, keeping the text, caret and input mode. */
void fplinux_ui_text_cancel_picker(struct fplinux_ui_text *field);
/* An unfocused field commits pending input, hides its caret and takes no keys. */
void fplinux_ui_text_set_focused(struct fplinux_ui_text *field, bool focused);

#endif
