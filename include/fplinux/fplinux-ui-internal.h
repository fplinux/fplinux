/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Calls between the runtime library's own sources; not installed. */

#ifndef FPLINUX_UI_INTERNAL_H
#define FPLINUX_UI_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>

#include "fplinux-ui-menu.h"
#include "fplinux-ui-profile.h"
#include "fplinux-ui-text.h"

/* Shared by the library's sources but not exported from it. */
#define FPLINUX_UI_INTERNAL __attribute__((visibility("hidden")))

/*
 * fplinux-ui.c: the token table entry of the profile chosen from the
 * display size in CONFIG; the public metrics are a subset of it.
 */
FPLINUX_UI_INTERNAL const struct fplinux_ui_profile_spec *
fplinux_ui_spec(const struct fplinux_ui *ui);

/*
 * CONFIG explicitly names Launcher mode. Its high-level screens use the
 * runtime's local legend bar; its Canvas keeps the launcher's own bars.
 */
FPLINUX_UI_INTERNAL bool fplinux_ui_is_launcher(const struct fplinux_ui *ui);

/*
 * Static modal dimming, using the semantic theme's scrim roles. Only the
 * background is translucent: popup children keep their normal opacity.
 * A screen modal styles its full-surface root; a top-layer popup attaches
 * a full-surface sibling immediately below it, deleted with the popup.
 */
FPLINUX_UI_INTERNAL void fplinux_ui_scrim_style(struct fplinux_ui *ui,
						lv_obj_t *root);
FPLINUX_UI_INTERNAL bool fplinux_ui_scrim_attach(struct fplinux_ui *ui,
						 lv_obj_t *popup);

/*
 * fplinux-ui.c: the runtime's own modal elements. A fullscreen application
 * has no system bars, so while one of them is up the runtime shows a
 * soft-key bar with the last session legends at the bottom of the surface,
 * on the sys layer; the elements keep above it with
 * fplinux_ui_modal_height().
 */
enum fplinux_ui_modal {
	/* The options menu. */
	FPLINUX_UI_MODAL_MENU = 1 << 0,
	/*
	 * The top screen: an alert, a popup choice, the date editor, or a
	 * non-Canvas high-level screen in Launcher mode.
	 */
	FPLINUX_UI_MODAL_SCREEN = 1 << 1,
	/* A text field's symbol grid. */
	FPLINUX_UI_MODAL_PICKER = 1 << 2,
};

FPLINUX_UI_INTERNAL void fplinux_ui_modal_set(struct fplinux_ui *ui,
					      enum fplinux_ui_modal source,
					      bool up);
/* The surface height above that bar: less softkey_height in fullscreen. */
FPLINUX_UI_INTERNAL int fplinux_ui_modal_height(const struct fplinux_ui *ui);

/*
 * fplinux-ui.c: the marquee, one per display. A one-line
 * label (one line high, LV_LABEL_LONG_MODE_DOTS at rest) whose text does
 * not fit scrolls only when the controller gives it a turn: a title makes
 * its one pass first (fplinux_ui_title_scroll()), then the focused
 * element's labels take turns, each scrolling to its end and back, for as
 * long as the element keeps focus. One label moves at a time and nothing
 * moves while the application is hidden.
 *
 * fplinux_ui_marquee_focus() names the focused element's labels, first and
 * an optional second such as a list row's detail; the previous element's
 * labels rest with dots. Naming the same labels again changes nothing.
 * A deleted label leaves the controller by itself.
 */
FPLINUX_UI_INTERNAL void fplinux_ui_marquee_focus(struct fplinux_ui *ui,
						  lv_obj_t *first,
						  lv_obj_t *second);
/* The element owning label lost focus: its labels rest with dots. */
FPLINUX_UI_INTERNAL void fplinux_ui_marquee_blur(struct fplinux_ui *ui,
						 lv_obj_t *label);
/*
 * The focused element's text changed: when first is one of its labels,
 * first and second become its labels and their turns start over, so that
 * a text that grew scrolls and one that now fits gives up its turn. No
 * effect for an element without focus.
 */
FPLINUX_UI_INTERNAL void fplinux_ui_marquee_changed(struct fplinux_ui *ui,
						    lv_obj_t *first,
						    lv_obj_t *second);

/*
 * fplinux-ui-screen.c: fplinux_ui_image_load(), and with alpha true an
 * alpha-only (A8) image of any source, as system glyphs are drawn.
 */
FPLINUX_UI_INTERNAL lv_draw_buf_t *
fplinux_ui_image_decode(const void *src, int width, int height, bool alpha);

/*
 * fplinux-ui-text.c: a field with FPLINUX_UI_TEXT_UNEDITABLE shows its text
 * without caret, mode, counter, picker or Clear.
 */
FPLINUX_UI_INTERNAL bool
fplinux_ui_text_is_read_only(const struct fplinux_ui_text *field);
/*
 * Whether OK and Enter start a new line: an editable multi-line field that
 * takes any text, not a number or phone type.
 */
FPLINUX_UI_INTERNAL bool
fplinux_ui_text_takes_newline(const struct fplinux_ui_text *field);
/* Raw fields within a Canvas follow that screen's focus under stacked modals. */
FPLINUX_UI_INTERNAL void fplinux_ui_text_canvas_active(lv_obj_t *canvas,
						       bool active);
/* Stop held input in all focused FPLinux fields, including Canvas/raw fields. */
FPLINUX_UI_INTERNAL void fplinux_ui_text_menu_suspend(struct fplinux_ui *ui);
/* Restore surviving visible fields that the menu took focus from. */
FPLINUX_UI_INTERNAL void fplinux_ui_text_menu_reclaim(struct fplinux_ui *ui);
/* The surface changed size: an open symbol grid takes its place again. */
FPLINUX_UI_INTERNAL void fplinux_ui_text_resize(struct fplinux_ui *ui);

/*
 * The options menu pop-up drawn by fplinux-ui-menu.c. fplinux-ui.c keeps the
 * open menu, routes keys to it, shows its legends and calls the callback.
 */
struct fplinux_ui_menu_view;

/* Draws the pop-up on the display's top layer; NULL with errno. */
FPLINUX_UI_INTERNAL struct fplinux_ui_menu_view *
fplinux_ui_menu_view_create(struct fplinux_ui *ui, const char *title,
			    const struct fplinux_ui_menu_item *items,
			    size_t count);
/* Takes a press or release; true when the menu ends with *action. */
FPLINUX_UI_INTERNAL bool
fplinux_ui_menu_view_key(struct fplinux_ui_menu_view *view,
			 const struct fplinux_ui_key *key, int *action);
FPLINUX_UI_INTERNAL void
fplinux_ui_menu_view_delete(struct fplinux_ui_menu_view *view);
/* The surface changed size: the pop-up fits it again; NULL does nothing. */
FPLINUX_UI_INTERNAL void
fplinux_ui_menu_view_resize(struct fplinux_ui_menu_view *view);

/*
 * fplinux-ui.c: a title label in font, one line high so that a longer title
 * ends with dots, as in fplinux_ui_header().
 */
FPLINUX_UI_INTERNAL void fplinux_ui_title_init(lv_obj_t *label,
					       const lv_font_t *font);
/*
 * A title too long for its label holds, scrolls once to its end, pauses,
 * returns and rests with dots; one that fits stays still.
 */
FPLINUX_UI_INTERNAL void fplinux_ui_title_scroll(lv_obj_t *label);
/* The title's screen is hidden: a pass it makes ends with dots. */
FPLINUX_UI_INTERNAL void fplinux_ui_title_hide(lv_obj_t *label);

/*
 * The screen stack of fplinux-ui-screen.c, kept in the runtime: an open
 * menu takes keys first, then the stack's top screen, then the application.
 */
struct fplinux_ui_screens;
struct fplinux_ui_screen;

enum fplinux_ui_key_route {
	FPLINUX_UI_KEY_UNHANDLED,
	FPLINUX_UI_KEY_HANDLED,
	FPLINUX_UI_KEY_TO_REPEAT_CANVAS,
};

/* NULL cancels every seat; a screen cancels keys owned by that Canvas. */
FPLINUX_UI_INTERNAL void
fplinux_ui_cancel_key_repeat(struct fplinux_ui *ui,
			     const struct fplinux_ui_screen *canvas);

/* fplinux-ui.c: where the stack lives; NULL until the first screen. */
FPLINUX_UI_INTERNAL struct fplinux_ui_screens **
fplinux_ui_screens_slot(struct fplinux_ui *ui);
/* Takes every key while a screen is stacked; false when none is. */
FPLINUX_UI_INTERNAL enum fplinux_ui_key_route
fplinux_ui_screens_key(struct fplinux_ui_screens *screens,
		       const struct fplinux_ui_key *key);
/* The captured Canvas must still be the active, opted-in screen. */
FPLINUX_UI_INTERNAL bool
fplinux_ui_screens_repeat_key(struct fplinux_ui_screens *screens,
			      struct fplinux_ui_screen *canvas,
			      const struct fplinux_ui_key *key);
/* Hidden screens run no timers. */
FPLINUX_UI_INTERNAL void
fplinux_ui_screens_set_visible(struct fplinux_ui_screens *screens,
			       bool visible);
/* A menu takes focus; pending input commits and held editing stops below it. */
FPLINUX_UI_INTERNAL void
fplinux_ui_screens_suspend(struct fplinux_ui_screens *screens);
/* The top screen's focused element takes the marquee again (menu closed). */
FPLINUX_UI_INTERNAL void
fplinux_ui_screens_reclaim(struct fplinux_ui_screens *screens);
/*
 * The surface changed size: every screen takes the new size, alerts and
 * popup choices fit their boxes to it, and lists and forms keep their
 * focus in view. NULL does nothing.
 */
FPLINUX_UI_INTERNAL void
fplinux_ui_screens_resize(struct fplinux_ui_screens *screens);
/* Deletes every screen at close, without callbacks or legends. */
FPLINUX_UI_INTERNAL void
fplinux_ui_screens_delete(struct fplinux_ui_screens *screens);

#endif
