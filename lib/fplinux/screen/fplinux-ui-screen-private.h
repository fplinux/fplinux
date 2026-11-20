/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef FPLINUX_UI_SCREEN_PRIVATE_H
#define FPLINUX_UI_SCREEN_PRIVATE_H

#include "fplinux-ui-screen.h"
#include "fplinux-language.h"
#include "fplinux-ui-internal.h"
#include "fplinux-ui-screen-model.h"
#include "fplinux-ui-session.h"

#define NONE ((size_t)-1)

struct candidate;
struct command;
struct form_item;
struct item_command;

enum screen_kind {
	SCREEN_LIST,
	SCREEN_FORM,
	SCREEN_TEXT_BOX,
	SCREEN_ALERT,
	SCREEN_CANVAS,
};

/* An options menu entry, for the command it fires; index follows its row. */
struct menu_entry {
	int id;
	bool item;
	bool clear;
	size_t index;
};

/* A child opened explicitly from its immediate owner's Options. */
struct options_return {
	struct fplinux_ui_screen *owner;
	struct menu_entry entry;
	struct fplinux_ui_menu_position position;
};

struct screen_form {
	/* Form items; a text box keeps its field as item 0. */
	struct form_item **items;
	size_t item_count;
	size_t item_capacity;
	size_t focus;
	bool settled;
	bool editing;
	struct fplinux_ui_date_edit edit;
	fplinux_ui_change_cb on_edit;
	void *edit_context;
};

struct fplinux_ui_screen {
	struct fplinux_ui *ui;
	struct fplinux_ui_screens *layer;
	enum screen_kind kind;
	/* Every screen of the layer, and the stack below this one. */
	struct fplinux_ui_screen *next;
	struct fplinux_ui_screen *below;
	bool stacked;
	struct options_return options_return;
	char *title;
	char *header_full;
	/* NULL uses header_full. */
	char *header_compact;
	struct command *commands;
	size_t command_count;
	size_t command_capacity;
	struct item_command *item_commands;
	size_t item_command_count;
	size_t item_command_capacity;
	/* Scratch for the soft keys and the options menu order. */
	struct candidate *candidates;
	struct fplinux_ui_command_entry *entries;
	size_t *order;
	size_t candidate_capacity;
	fplinux_ui_command_cb on_command;
	fplinux_ui_change_cb on_change;
	void *context;
	fplinux_ui_item_command_cb on_item;
	void *item_context;
	/* The item whose command is being reported, else SIZE_MAX. */
	size_t command_item;
	void *data;
	lv_obj_t *root;
	lv_obj_t *band;
	lv_obj_t *band_label;
	bool title_active;
	lv_obj_t *header_status;
	lv_obj_t *content;

	struct fplinux_ui_listview *list;
	enum fplinux_ui_list_kind list_kind;
	fplinux_ui_change_cb on_focus;
	void *focus_context;
	/* Only a form's popup choice retains and dims the screen below it. */
	bool popup_choice;
	char *select_label;
	fplinux_ui_virtual_row_cb virtual_row;
	void *virtual_context;

	struct screen_form form;

	enum fplinux_ui_alert_kind alert_kind;
	unsigned int timeout_ms;
	lv_timer_t *timer;
	lv_obj_t *box;
	/* The alert's text, hidden while it has none. */
	lv_obj_t *message;
	lv_obj_t *message_view;
	lv_obj_t *progress_text;
	/* A busy alert: a spinner for maximum 0, otherwise a progress bar. */
	bool busy;
	int maximum;
	int progress_value;
	lv_obj_t *indicator_box;
	lv_obj_t *indicator;
	lv_timer_t *spin_timer;
	unsigned int activity_phase;
	lv_obj_t *activity_indicator;

	fplinux_ui_canvas_key_cb canvas_key;
	void *canvas_context;
	fplinux_ui_canvas_active_cb on_canvas_active;
	void *canvas_active_context;
	fplinux_ui_screen_key_filter_cb key_filter;
	void *key_filter_context;
	/* A filter owns these releases, even when it has since been removed. */
	unsigned char filtered[2][KEY_CNT / 8];
	/* Keyboard Escape and Enter go to canvas_key, not to the commands. */
	bool keyboard_raw;
	enum fplinux_ui_canvas_repeat key_repeat;
	bool canvas_active;
};

/* From fplinux_ui_wait_begin() until done runs or the wait is dropped. */
struct screen_wait {
	/* The application's top screen at the start; NULL without a wait. */
	struct fplinux_ui_screen *origin;
	/* Stacked directly above the origin while shown. */
	struct fplinux_ui_screen *alert;
	/* The next step: showing the alert or finishing. */
	lv_timer_t *timer;
	uint32_t begun_ms;
	uint32_t shown_ms;
	/* What the origin showed at the start, whatever its commands become. */
	char legends[3][FPLINUX_UI_SESSION_TEXT_BYTES];
	bool shown;
	bool ending;
	fplinux_ui_wait_cb cancel;
	void *cancel_context;
	fplinux_ui_wait_cb done;
	void *done_context;
};

struct fplinux_ui_screens {
	struct fplinux_ui *ui;
	struct fplinux_ui_screen *top;
	struct fplinux_ui_screen *all;
	/* Runtime-owned choice above the application's stack. */
	struct fplinux_ui_screen *transient;
	void (*cancel_transient)(struct fplinux_ui *ui, void *context);
	void *transient_context;
	struct screen_wait wait;
	unsigned int row_provider_depth;
	bool navigating;
	/* Presses the runtime used for commands; their releases go nowhere. */
	unsigned char used[2][KEY_CNT / 8];
	/* Filter-owned keys are released to their original surviving screen. */
	unsigned char filtered[2][KEY_CNT / 8];
	bool visible;
	bool closing;
	char legends[3][FPLINUX_UI_SESSION_TEXT_BYTES];
	bool legends_sent;
	/* The screen whose options menu is open, and its commands. */
	struct fplinux_ui_screen *menu_screen;
	struct menu_entry *menu_entries;
	size_t menu_entry_count;
	struct fplinux_ui_menu_position menu_position;
	/* Valid only while a high-level Options callback runs. */
	struct options_return options_source;
	/* The short date pattern of the language named date_language. */
	char date_language[FPLINUX_LANGUAGE_CODE_BYTES];
	char date_pattern[FPLINUX_LANGUAGE_TEXT_BYTES];
};

FPLINUX_UI_INTERNAL struct fplinux_ui_screen *
fplinux_ui_screen_new(struct fplinux_ui *ui, enum screen_kind kind,
		      const char *title);
FPLINUX_UI_INTERNAL void
fplinux_ui_screen_delete(struct fplinux_ui_screen *screen);
FPLINUX_UI_INTERNAL bool
fplinux_ui_screen_is_alive(const struct fplinux_ui_screens *layer,
			   const struct fplinux_ui_screen *screen);
FPLINUX_UI_INTERNAL bool
fplinux_ui_screen_is_top(const struct fplinux_ui_screen *screen);
FPLINUX_UI_INTERNAL const struct fplinux_ui_profile_spec *
fplinux_ui_screen_spec(const struct fplinux_ui_screen *screen);
FPLINUX_UI_INTERNAL lv_obj_t *fplinux_ui_screen_plain_object(lv_obj_t *parent);
FPLINUX_UI_INTERNAL void
fplinux_ui_screen_style_scrollbar(lv_obj_t *object, struct fplinux_ui *ui);
FPLINUX_UI_INTERNAL lv_obj_t *fplinux_ui_screen_make_bar(lv_obj_t *parent,
							 struct fplinux_ui *ui);

FPLINUX_UI_INTERNAL void
fplinux_ui_screen_refresh(struct fplinux_ui_screens *layer);
FPLINUX_UI_INTERNAL void
fplinux_ui_screen_changed(struct fplinux_ui_screen *screen);
FPLINUX_UI_INTERNAL void
fplinux_ui_screen_report(struct fplinux_ui_screen *screen, size_t index);
FPLINUX_UI_INTERNAL const char *
fplinux_ui_screen_word(const struct fplinux_ui_screen *screen,
		       enum fplinux_ui_label label);
FPLINUX_UI_INTERNAL const char *
fplinux_ui_screen_mark_word(const struct fplinux_ui_screen *screen,
			    const struct fplinux_ui_listview *list);
FPLINUX_UI_INTERNAL bool
fplinux_ui_screen_is_ok_key(const struct fplinux_ui_key *key);
FPLINUX_UI_INTERNAL bool
fplinux_ui_screen_is_right_key(const struct fplinux_ui_key *key);

FPLINUX_UI_INTERNAL bool
fplinux_ui_screen_has_item_commands(const struct fplinux_ui_screen *screen,
				    size_t index);
FPLINUX_UI_INTERNAL void fplinux_ui_screen_item_commands_moved(
	struct fplinux_ui_screen *screen,
	size_t (*move)(size_t index, size_t a, size_t b), size_t a, size_t b);
FPLINUX_UI_INTERNAL void
fplinux_ui_screen_item_commands_deleted(struct fplinux_ui_screen *screen,
					size_t at, size_t count);
FPLINUX_UI_INTERNAL void
fplinux_ui_screen_clear_item_commands(struct fplinux_ui_screen *screen);

FPLINUX_UI_INTERNAL void
fplinux_ui_form_free_items(struct fplinux_ui_screen *screen);
FPLINUX_UI_INTERNAL void fplinux_ui_form_settle(struct fplinux_ui_screen *form);
FPLINUX_UI_INTERNAL void
fplinux_ui_form_apply_focus(struct fplinux_ui_screen *form, bool active);
FPLINUX_UI_INTERNAL void
fplinux_ui_form_keep_focus_visible(struct fplinux_ui_screen *form);
FPLINUX_UI_INTERNAL struct fplinux_ui_text *
fplinux_ui_form_focused_text(const struct fplinux_ui_screen *screen);
FPLINUX_UI_INTERNAL bool
fplinux_ui_form_focus_is_actionable(const struct fplinux_ui_screen *screen);
FPLINUX_UI_INTERNAL const char *
fplinux_ui_form_action(const struct fplinux_ui_screen *screen);
FPLINUX_UI_INTERNAL void
fplinux_ui_form_update_appearance(struct fplinux_ui_screen *screen,
				  size_t index);
FPLINUX_UI_INTERNAL void fplinux_ui_form_ok(struct fplinux_ui_screen *form);
FPLINUX_UI_INTERNAL bool fplinux_ui_form_key(struct fplinux_ui_screen *form,
					     const struct fplinux_ui_key *key);
FPLINUX_UI_INTERNAL void
fplinux_ui_form_date_key(struct fplinux_ui_screen *form,
			 const struct fplinux_ui_key *key);
FPLINUX_UI_INTERNAL void
fplinux_ui_screen_frame_popup_choice(struct fplinux_ui_screen *list,
				     size_t count);

#endif
