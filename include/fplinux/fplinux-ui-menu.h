/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef FPLINUX_UI_MENU_H
#define FPLINUX_UI_MENU_H

#include <stdbool.h>
#include <stddef.h>

#include "fplinux-ui.h"

/*
 * Scrolls any LVGL object that holds more content than it shows, such as a
 * long text: Up and Down by one line of the system font, Left and Right by
 * a page. Returns true when the key was used, including at either end, so
 * that the application does not move focus while the user reads.
 */
bool fplinux_ui_scroll_key(lv_obj_t *object, const struct fplinux_ui_key *key);

/*
 * The options menu: a system pop-up list of actions over the application,
 * usually opened by the left soft key labelled "Options". While it is open
 * the runtime gives it every key press and shows its own legends; the
 * application receives no key presses, and a release goes where its press
 * went. The callback receives the chosen item's action, or
 * FPLINUX_UI_MENU_CANCELLED when the user leaves it with Back. Item texts are
 * copied.
 */
#define FPLINUX_UI_MENU_CANCELLED (-1)

struct fplinux_ui_menu_item {
	int action;
	const char *text;
	/* Optional image at the left of the item; NULL for none. */
	const void *icon;
	/* Optional group boundary; the first item never has one. */
	bool separator_before;
};

enum fplinux_ui_menu_origin {
	FPLINUX_UI_MENU_ORIGIN_LEFT,
	FPLINUX_UI_MENU_ORIGIN_CENTRE,
	FPLINUX_UI_MENU_ORIGIN_RIGHT,
};

typedef void (*fplinux_ui_menu_cb)(struct fplinux_ui *ui, int action,
				   void *context);

/*
 * Aligns a content-sized window above the soft-key bar at origin's side.
 * NULL or an empty title omits the title band. Origin does not change the
 * menu's Select and Back keys. The size stays fixed while navigating.
 */
bool fplinux_ui_menu_open(struct fplinux_ui *ui,
			  enum fplinux_ui_menu_origin origin, const char *title,
			  const struct fplinux_ui_menu_item *items,
			  size_t count, fplinux_ui_menu_cb callback,
			  void *context);
bool fplinux_ui_menu_is_open(const struct fplinux_ui *ui);
/* Closes the menu without calling its callback. */
void fplinux_ui_menu_close(struct fplinux_ui *ui);

#endif
