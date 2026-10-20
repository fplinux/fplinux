/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define _POSIX_C_SOURCE 200809L
#include "fplinux-ui-screen.h"
#include "fplinux-language.h"
#include "fplinux-ui-internal.h"
#include "fplinux-ui-style-internal.h"
#include "fplinux-ui-listview.h"
#include "fplinux-ui-locale.h"
#include "fplinux-ui-screen-model.h"
#include "fplinux-ui-session.h"
#include "fplinux-ui-text.h"

#include <errno.h>
#include <fcntl.h>
#include <libintl.h>
#include <linux/input-event-codes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define NONE ((size_t)-1)
/* A date item's value as fplinux_ui_format_date() writes it. */
#define DATE_VALUE_BYTES 128
/* A popup choice's private Back command. */
#define POPUP_BACK_COMMAND (-3)
/* A busy alert's spinner: a quarter ring stepping round eight times a turn. */
#define SPINNER_ARC_DEGREES 90
#define SPINNER_STEP_DEGREES 45
#define SPINNER_STEP_MS 125
/* Image sources the runtime decodes: file size and decoded pixels. */
#define IMAGE_FILE_MAX_BYTES (1024 * 1024)
#define IMAGE_MAX_PIXELS (1024U * 1024U)
/* A file with this ending is decoded as an alpha mask. */
#define SYMBOLIC_SUFFIX "-symbolic.png"

enum screen_kind {
	SCREEN_LIST,
	SCREEN_FORM,
	SCREEN_TEXT_BOX,
	SCREEN_ALERT,
	SCREEN_CANVAS,
};

enum item_kind {
	ITEM_STRING,
	ITEM_TEXT,
	ITEM_CHOICE,
	ITEM_GAUGE,
	ITEM_DATE,
	ITEM_IMAGE,
};

/* How focus enters a choice's rows. */
enum entry {
	ENTER_TOP,
	ENTER_BOTTOM,
	ENTER_KEEP,
};

struct command {
	int id;
	enum fplinux_ui_command_type type;
	/* NULL for the type's localized default. */
	char *label;
	int priority;
	/* The decoded options-menu image, or NULL. */
	lv_draw_buf_t *icon;
};

/* A list row's or form item's own command; index follows its row. */
struct item_command {
	size_t index;
	struct command command;
};

/* A command the soft keys may take: the focused item's or the screen's. */
struct candidate {
	const struct command *command;
	bool item;
};

/* An options menu entry, for the command it fires; index follows its row. */
struct menu_entry {
	int id;
	bool item;
	size_t index;
};

struct form_item {
	struct fplinux_ui_screen *form;
	size_t index;
	enum item_kind kind;
	lv_obj_t *box;
	lv_obj_t *label;
	/* A gauge's one-line name. */
	lv_obj_t *gauge_name;
	/* String text, gauge number, date text or image. */
	lv_obj_t *value;
	/*
	 * The border of a gauge or date that shows the focus, or the box of a
	 * link or button around its value.
	 */
	lv_obj_t *frame;
	lv_obj_t *bar;
	struct fplinux_ui_text *field;
	enum fplinux_ui_text_constraint constraint;
	/* A DECIMAL field's text with '.', as the application reads it. */
	char *content;
	/* The text last reported, and whether a report is queued. */
	char *reported;
	bool report_pending;
	/* Set while the application changes the text itself. */
	bool quiet;
	struct fplinux_ui_listview *list;
	enum fplinux_ui_choice_kind choice_kind;
	char **choices;
	size_t count;
	size_t chosen;
	/* A popup choice's label, also the title of its list. */
	char *name;
	int minimum;
	int maximum;
	int number;
	bool interactive;
	enum fplinux_ui_date_mode date_mode;
	time_t date;
	/* Takes no space and never has focus. */
	bool hidden;
	/* A string or image item drawn as a link or button with commands. */
	enum fplinux_ui_appearance appearance;
	/* An image's decoded picture, and a copy that fits inside the box. */
	lv_draw_buf_t *image;
	lv_draw_buf_t *fitted;
};

struct fplinux_ui_screen {
	struct fplinux_ui *ui;
	struct fplinux_ui_screens *layer;
	enum screen_kind kind;
	/* Every screen of the layer, and the stack below this one. */
	struct fplinux_ui_screen *next;
	struct fplinux_ui_screen *below;
	bool stacked;
	char *title;
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
	lv_obj_t *content;

	struct fplinux_ui_listview *list;
	enum fplinux_ui_list_kind list_kind;
	/* Only a form's popup choice retains and dims the screen below it. */
	bool popup_choice;
	char *select_label;
	fplinux_ui_virtual_row_cb virtual_row;
	void *virtual_context;

	/* Form items; a text box keeps its field as item 0. */
	struct form_item **items;
	size_t item_count;
	size_t item_capacity;
	size_t focus;
	bool settled;
	bool editing;
	struct fplinux_ui_date_edit edit;

	enum fplinux_ui_alert_kind alert_kind;
	unsigned int timeout_ms;
	lv_timer_t *timer;
	lv_obj_t *box;
	lv_obj_t *box_title;
	/* The alert's text, hidden while it has none. */
	lv_obj_t *message;
	/* A busy alert: a spinner for maximum 0, otherwise a progress bar. */
	bool busy;
	int maximum;
	lv_obj_t *indicator_box;
	lv_obj_t *indicator;
	lv_timer_t *spin_timer;
	int32_t spin_angle;

	fplinux_ui_canvas_key_cb canvas_key;
	void *canvas_context;
	/* Keyboard Escape and Enter go to canvas_key, not to the commands. */
	bool keyboard_raw;
};

struct fplinux_ui_screens {
	struct fplinux_ui *ui;
	struct fplinux_ui_screen *top;
	struct fplinux_ui_screen *all;
	/* Presses the runtime used for commands; their releases go nowhere. */
	unsigned char used[2][KEY_CNT / 8];
	bool visible;
	bool closing;
	char legends[3][FPLINUX_UI_SESSION_TEXT_BYTES];
	bool legends_sent;
	/* The screen whose options menu is open, and its commands. */
	struct fplinux_ui_screen *menu_screen;
	struct menu_entry *menu_entries;
	size_t menu_entry_count;
	/* The short date pattern of the language named date_language. */
	char date_language[FPLINUX_LANGUAGE_CODE_BYTES];
	char date_pattern[FPLINUX_LANGUAGE_TEXT_BYTES];
};

/*
 * What the three keys do on the top screen; command indices name
 * screen->candidates.
 */
struct soft_keys {
	struct fplinux_ui_command_keys keys;
	/* The centre legend when the focused element acts on OK. */
	const char *action;
	/* The right soft key clears the focused text. */
	bool clear;
	/* Left, centre and right legends. */
	const char *legends[3];
};

static void refresh(struct fplinux_ui_screens *layer);
static void restack(struct fplinux_ui_screens *layer);
static void delete_screen(struct fplinux_ui_screen *screen);
static void options_chosen(struct fplinux_ui *ui, int action, void *context);
static void options_changed(struct fplinux_ui_screen *screen);

/* In the language that fplinux_ui_open() selected for messages. */
static const char *word(const struct fplinux_ui_screen *screen,
			enum fplinux_ui_label label)
{
	(void)screen;
	return dgettext(FPLINUX_UI_TEXT_DOMAIN, fplinux_ui_label(label));
}

/* Legends travel in session messages: printable and shorter than a field. */
static bool valid_label(const char *label)
{
	size_t length = 0;

	for (; label[length]; ++length)
		if ((unsigned char)label[length] < 0x20 ||
		    label[length] == 0x7f)
			return false;
	return length < FPLINUX_UI_SESSION_TEXT_BYTES;
}

static bool is_top(const struct fplinux_ui_screen *screen)
{
	return screen->stacked && screen->layer->top == screen;
}

/* Legends and the options menu follow every change of the top screen. */
static void changed(struct fplinux_ui_screen *screen)
{
	options_changed(screen);
	if (is_top(screen))
		refresh(screen->layer);
}

static void report(struct fplinux_ui_screen *screen, size_t index)
{
	if (screen->on_change)
		screen->on_change(screen->ui, screen, index, screen->context);
}

static void notify_command(struct fplinux_ui_screen *screen, int id)
{
	if (screen->on_command)
		screen->on_command(screen->ui, screen, id, screen->context);
}

/* Layer and frame */

static struct fplinux_ui_screens *layer_of(struct fplinux_ui *ui)
{
	struct fplinux_ui_screens **slot = fplinux_ui_screens_slot(ui);

	if (!*slot) {
		*slot = calloc(1, sizeof(**slot));
		if (*slot) {
			(*slot)->ui = ui;
			(*slot)->visible = fplinux_ui_is_visible(ui);
		}
	}
	return *slot;
}

static const struct fplinux_ui_profile_spec *
spec_of(const struct fplinux_ui_screen *screen)
{
	return fplinux_ui_spec(screen->ui);
}

/* The one scroll indicator: a thumb in focus, no track. */
static void style_scrollbar(lv_obj_t *object, struct fplinux_ui *ui)
{
	const struct fplinux_ui_profile_spec *spec = fplinux_ui_spec(ui);

	lv_obj_set_scroll_dir(object, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(object, LV_SCROLLBAR_MODE_AUTO);
	fplinux_ui_style_scrollbar(ui, object);
	lv_obj_set_style_pad_ver(object, spec->content_padding,
				 LV_PART_SCROLLBAR);
}

static lv_obj_t *plain_object(lv_obj_t *parent)
{
	lv_obj_t *object = lv_obj_create(parent);

	lv_obj_remove_style_all(object);
	lv_obj_set_scrollable(object, false);
	return object;
}

/* Launcher Canvas owns its bars; other high-level screens use the local bar. */
static bool launcher_legend_bar(const struct fplinux_ui_screen *screen)
{
	return fplinux_ui_is_launcher(screen->ui) &&
	       screen->kind != SCREEN_CANVAS;
}

static int32_t screen_height(const struct fplinux_ui_screen *screen)
{
	return launcher_legend_bar(screen) ?
		       fplinux_ui_modal_height(screen->ui) :
		       fplinux_ui_height(screen->ui);
}

/*
 * The band and the content fill their screen, or a popup choice's box; the
 * date editor's form ends above the legend bar of a fullscreen application.
 */
static void size_frame(struct fplinux_ui_screen *screen)
{
	const struct fplinux_ui_profile_spec *spec = spec_of(screen);
	int32_t top = screen->title ? spec->header_height : 0;
	int32_t width = fplinux_ui_width(screen->ui);
	int32_t height = screen->editing ? fplinux_ui_modal_height(screen->ui) :
					   screen_height(screen);

	if (screen->popup_choice) {
		lv_obj_update_layout(screen->box);
		width = lv_obj_get_content_width(screen->box);
		height = lv_obj_get_content_height(screen->box);
	}
	lv_obj_set_size(screen->band, width, spec->header_height);
	lv_obj_set_width(screen->band_label, width - 2 * spec->margin);
	lv_obj_set_pos(screen->content, 0, top);
	lv_obj_set_size(screen->content, width, height - top);
}

/* The title band of fplinux_ui_header(); no band without a title. */
static void layout_frame(struct fplinux_ui_screen *screen)
{
	size_frame(screen);
	lv_obj_set_hidden(screen->band, !screen->title);
	if (screen->title) {
		lv_label_set_text(screen->band_label, screen->title);
		/* A hidden screen's title scrolls once it shows. */
		if (!lv_obj_is_hidden(screen->root))
			fplinux_ui_title_scroll(screen->band_label);
		else
			fplinux_ui_title_hide(screen->band_label);
	} else if (screen->band_label) {
		fplinux_ui_title_hide(screen->band_label);
	}
}

/* A modal screen's root, its scrim, ends above the legend bar. */
static int32_t root_height(const struct fplinux_ui_screen *screen)
{
	return screen->kind == SCREEN_ALERT || screen->popup_choice ?
		       fplinux_ui_modal_height(screen->ui) :
		       screen_height(screen);
}

static void build_frame(struct fplinux_ui_screen *screen)
{
	const struct fplinux_ui_theme *theme = fplinux_ui_theme(screen->ui);
	const struct fplinux_ui_profile_spec *spec = spec_of(screen);
	int32_t width = fplinux_ui_width(screen->ui);

	screen->root = plain_object(fplinux_ui_screen(screen->ui));
	lv_obj_set_size(screen->root, width, root_height(screen));
	lv_obj_set_hidden(screen->root, true);
	lv_obj_set_style_text_color(screen->root, theme->foreground, 0);
	lv_obj_set_style_text_font(screen->root, theme->font, 0);
	/* An alert dims the screen below it, not its opaque popup children. */
	if (screen->kind == SCREEN_ALERT) {
		fplinux_ui_scrim_style(screen->ui, screen->root);
		return;
	}
	lv_obj_set_style_bg_color(screen->root, theme->background, 0);
	lv_obj_set_style_bg_opa(screen->root, LV_OPA_COVER, 0);
	screen->band = plain_object(screen->root);
	lv_obj_set_size(screen->band, width, spec->header_height);
	lv_obj_set_style_bg_color(screen->band, theme->surface, 0);
	lv_obj_set_style_bg_opa(screen->band, LV_OPA_COVER, 0);
	lv_obj_set_style_border_color(screen->band, theme->border, 0);
	lv_obj_set_style_border_width(screen->band, spec->divider_width, 0);
	lv_obj_set_style_border_side(screen->band, LV_BORDER_SIDE_BOTTOM, 0);
	screen->band_label = lv_label_create(screen->band);
	fplinux_ui_title_init(screen->band_label, theme->font_large);
	lv_obj_set_width(screen->band_label, width - 2 * spec->margin);
	lv_label_set_text(screen->band_label, "");
	lv_obj_align(screen->band_label, LV_ALIGN_LEFT_MID, spec->margin, 0);
	screen->content = plain_object(screen->root);
	layout_frame(screen);
}

static struct fplinux_ui_screen *
screen_new(struct fplinux_ui *ui, enum screen_kind kind, const char *title)
{
	struct fplinux_ui_screens *layer;
	struct fplinux_ui_screen *screen;

	if (!ui) {
		errno = EINVAL;
		return NULL;
	}
	layer = layer_of(ui);
	if (!layer)
		return NULL;
	screen = calloc(1, sizeof(*screen));
	if (!screen)
		return NULL;
	screen->ui = ui;
	screen->layer = layer;
	screen->kind = kind;
	screen->focus = NONE;
	screen->command_item = NONE;
	if (title && !(screen->title = strdup(title))) {
		free(screen);
		return NULL;
	}
	screen->next = layer->all;
	layer->all = screen;
	build_frame(screen);
	return screen;
}

/* Commands */

static enum fplinux_ui_command_role role_of(enum fplinux_ui_command_type type)
{
	switch (type) {
	case FPLINUX_UI_COMMAND_BACK:
	case FPLINUX_UI_COMMAND_CANCEL:
	case FPLINUX_UI_COMMAND_EXIT:
	case FPLINUX_UI_COMMAND_STOP:
		return FPLINUX_UI_ROLE_BACK;
	case FPLINUX_UI_COMMAND_OK:
		return FPLINUX_UI_ROLE_OK;
	case FPLINUX_UI_COMMAND_HELP:
		return FPLINUX_UI_ROLE_HELP;
	default:
		return FPLINUX_UI_ROLE_OTHER;
	}
}

static const enum fplinux_ui_label default_labels[] = {
	[FPLINUX_UI_COMMAND_BACK] = FPLINUX_UI_LABEL_BACK,
	[FPLINUX_UI_COMMAND_CANCEL] = FPLINUX_UI_LABEL_CANCEL,
	[FPLINUX_UI_COMMAND_EXIT] = FPLINUX_UI_LABEL_EXIT,
	[FPLINUX_UI_COMMAND_STOP] = FPLINUX_UI_LABEL_STOP,
	[FPLINUX_UI_COMMAND_OK] = FPLINUX_UI_LABEL_OK,
	[FPLINUX_UI_COMMAND_ITEM] = FPLINUX_UI_LABEL_COUNT,
	[FPLINUX_UI_COMMAND_SCREEN] = FPLINUX_UI_LABEL_COUNT,
	[FPLINUX_UI_COMMAND_HELP] = FPLINUX_UI_LABEL_HELP,
};

/* An alert without commands offers the runtime's Close, unless it is busy. */
static bool system_close(const struct fplinux_ui_screen *screen)
{
	return screen->kind == SCREEN_ALERT && !screen->command_count &&
	       !screen->busy;
}

/* The candidate's legend; index names screen->candidates. */
static const char *command_label(const struct fplinux_ui_screen *screen,
				 size_t index)
{
	const struct command *command;

	if (system_close(screen))
		return word(screen, FPLINUX_UI_LABEL_CLOSE);
	command = screen->candidates[index].command;
	return command->label ? command->label :
				word(screen, default_labels[command->type]);
}

static size_t find_command(const struct fplinux_ui_screen *screen, int id)
{
	for (size_t i = 0; i < screen->command_count; ++i)
		if (screen->commands[i].id == id)
			return i;
	return NONE;
}

static size_t find_item_command(const struct fplinux_ui_screen *screen,
				size_t index, int id)
{
	for (size_t i = 0; i < screen->item_command_count; ++i)
		if (screen->item_commands[i].index == index &&
		    screen->item_commands[i].command.id == id)
			return i;
	return NONE;
}

static void free_command(struct command *command)
{
	free(command->label);
	if (command->icon)
		lv_draw_buf_destroy(command->icon);
}

/* The scratch arrays hold every screen command and every item command. */
static bool reserve_candidates(struct fplinux_ui_screen *screen)
{
	size_t capacity =
		screen->command_capacity + screen->item_command_capacity + 1;
	struct candidate *candidates;
	struct fplinux_ui_command_entry *entries;
	size_t *order;

	if (capacity <= screen->candidate_capacity)
		return true;
	candidates =
		realloc(screen->candidates, capacity * sizeof(*candidates));
	if (!candidates)
		return false;
	screen->candidates = candidates;
	entries = realloc(screen->entries, capacity * sizeof(*entries));
	if (!entries)
		return false;
	screen->entries = entries;
	order = realloc(screen->order, capacity * sizeof(*order));
	if (!order)
		return false;
	screen->order = order;
	screen->candidate_capacity = capacity;
	return true;
}

static bool reserve_commands(struct fplinux_ui_screen *screen)
{
	size_t capacity =
		screen->command_capacity ? 2 * screen->command_capacity : 4;
	struct command *commands;

	if (screen->command_count < screen->command_capacity)
		return reserve_candidates(screen);
	commands = realloc(screen->commands, capacity * sizeof(*commands));
	if (!commands)
		return false;
	screen->commands = commands;
	screen->command_capacity = capacity;
	return reserve_candidates(screen);
}

static bool reserve_item_commands(struct fplinux_ui_screen *screen)
{
	size_t capacity = screen->item_command_capacity ?
				  2 * screen->item_command_capacity :
				  4;
	struct item_command *commands;

	if (screen->item_command_count < screen->item_command_capacity)
		return reserve_candidates(screen);
	commands = realloc(screen->item_commands, capacity * sizeof(*commands));
	if (!commands)
		return false;
	screen->item_commands = commands;
	screen->item_command_capacity = capacity;
	return reserve_candidates(screen);
}

/* Copies the label and decodes the icon for the options menu. */
static bool copy_command(struct fplinux_ui_screen *screen,
			 const struct fplinux_ui_command *from,
			 struct command *to)
{
	int size = spec_of(screen)->row_icon_size;

	*to = (struct command){
		.id = from->id,
		.type = from->type,
		.priority = from->priority,
	};
	if (from->label && !(to->label = strdup(from->label)))
		return false;
	/* A command keeps working without an image that cannot be shown. */
	if (from->icon)
		to->icon =
			fplinux_ui_image_decode(from->icon, size, size, false);
	return true;
}

static bool add_command(struct fplinux_ui_screen *screen,
			const struct fplinux_ui_command *command)
{
	size_t index = find_command(screen, command->id);
	struct command copy;

	if (!reserve_commands(screen) || !copy_command(screen, command, &copy))
		return false;
	if (index == NONE)
		index = screen->command_count++;
	else
		free_command(&screen->commands[index]);
	screen->commands[index] = copy;
	changed(screen);
	return true;
}

static bool valid_command(const struct fplinux_ui_command *command)
{
	return command &&
	       (unsigned int)command->type <= FPLINUX_UI_COMMAND_HELP &&
	       command->id != FPLINUX_UI_SELECT_COMMAND &&
	       (command->label ||
		default_labels[command->type] != FPLINUX_UI_LABEL_COUNT) &&
	       (!command->label || valid_label(command->label));
}

/*
 * A NULL label stands for the type's localized word; ITEM and SCREEN
 * commands need their own.
 */
bool fplinux_ui_screen_add_command(struct fplinux_ui_screen *screen,
				   const struct fplinux_ui_command *command)
{
	if (!screen || !valid_command(command)) {
		errno = EINVAL;
		return false;
	}
	return add_command(screen, command);
}

void fplinux_ui_screen_remove_command(struct fplinux_ui_screen *screen, int id)
{
	size_t index;

	if (!screen)
		return;
	index = find_command(screen, id);
	if (index == NONE)
		return;
	free_command(&screen->commands[index]);
	--screen->command_count;
	memmove(&screen->commands[index], &screen->commands[index + 1],
		(screen->command_count - index) * sizeof(*screen->commands));
	changed(screen);
}

void fplinux_ui_screen_set_listener(struct fplinux_ui_screen *screen,
				    fplinux_ui_command_cb command,
				    fplinux_ui_change_cb change, void *context)
{
	if (!screen)
		return;
	screen->on_command = command;
	screen->on_change = change;
	screen->context = context;
}

/* Item commands */

static size_t row_count(const struct fplinux_ui_screen *screen)
{
	if (screen->kind == SCREEN_LIST)
		return fplinux_ui_listview_count(screen->list);
	return screen->kind == SCREEN_FORM ? screen->item_count : 0;
}

static void update_appearance(struct fplinux_ui_screen *screen, size_t index);

static bool add_item_command(struct fplinux_ui_screen *screen, size_t index,
			     const struct fplinux_ui_command *command)
{
	size_t at;
	struct command copy;

	if (!valid_command(command) || index >= row_count(screen)) {
		errno = EINVAL;
		return false;
	}
	if (!reserve_item_commands(screen) ||
	    !copy_command(screen, command, &copy))
		return false;
	at = find_item_command(screen, index, command->id);
	if (at == NONE)
		at = screen->item_command_count++;
	else
		free_command(&screen->item_commands[at].command);
	screen->item_commands[at] = (struct item_command){
		.index = index,
		.command = copy,
	};
	update_appearance(screen, index);
	changed(screen);
	return true;
}

bool fplinux_ui_list_add_item_command(struct fplinux_ui_screen *list,
				      size_t index,
				      const struct fplinux_ui_command *command)
{
	if (!list || list->kind != SCREEN_LIST) {
		errno = EINVAL;
		return false;
	}
	return add_item_command(list, index, command);
}

bool fplinux_ui_form_add_item_command(struct fplinux_ui_screen *form,
				      size_t item,
				      const struct fplinux_ui_command *command)
{
	if (!form || form->kind != SCREEN_FORM) {
		errno = EINVAL;
		return false;
	}
	return add_item_command(form, item, command);
}

static void drop_item_command(struct fplinux_ui_screen *screen, size_t at)
{
	free_command(&screen->item_commands[at].command);
	--screen->item_command_count;
	memmove(&screen->item_commands[at], &screen->item_commands[at + 1],
		(screen->item_command_count - at) *
			sizeof(*screen->item_commands));
}

void fplinux_ui_screen_remove_item_command(struct fplinux_ui_screen *screen,
					   size_t index, int id)
{
	size_t at;

	if (!screen)
		return;
	at = find_item_command(screen, index, id);
	if (at == NONE)
		return;
	drop_item_command(screen, at);
	update_appearance(screen, index);
	changed(screen);
}

void fplinux_ui_screen_set_item_listener(struct fplinux_ui_screen *screen,
					 fplinux_ui_item_command_cb listener,
					 void *context)
{
	if (!screen)
		return;
	screen->on_item = listener;
	screen->item_context = context;
}

size_t
fplinux_ui_screen_get_command_item(const struct fplinux_ui_screen *screen)
{
	return screen ? screen->command_item : NONE;
}

/*
 * Rows moved under the open options menu: its entries follow their row as
 * item commands do, to NONE when the row went.
 */
static void menu_rows_moved(struct fplinux_ui_screen *screen,
			    size_t (*move)(size_t index, size_t a, size_t b),
			    size_t a, size_t b)
{
	struct fplinux_ui_screens *layer = screen->layer;

	if (layer->menu_screen != screen)
		return;
	for (size_t i = 0; i < layer->menu_entry_count; ++i)
		layer->menu_entries[i].index =
			move(layer->menu_entries[i].index, a, b);
}

/* count rows were inserted before at: later rows' commands move down. */
static void item_commands_inserted(struct fplinux_ui_screen *screen, size_t at,
				   size_t count)
{
	for (size_t i = 0; i < screen->item_command_count; ++i)
		screen->item_commands[i].index = fplinux_ui_index_inserted(
			screen->item_commands[i].index, at, count);
	menu_rows_moved(screen, fplinux_ui_index_inserted, at, count);
}

/* count rows were deleted from at: their commands go, later ones move up. */
static void item_commands_deleted(struct fplinux_ui_screen *screen, size_t at,
				  size_t count)
{
	for (size_t i = screen->item_command_count; i-- > 0;) {
		size_t index = fplinux_ui_index_deleted(
			screen->item_commands[i].index, at, count);

		if (index == NONE)
			drop_item_command(screen, i);
		else
			screen->item_commands[i].index = index;
	}
	menu_rows_moved(screen, fplinux_ui_index_deleted, at, count);
}

static void clear_item_commands(struct fplinux_ui_screen *screen)
{
	while (screen->item_command_count)
		drop_item_command(screen, screen->item_command_count - 1);
}

static const enum fplinux_ui_label alert_titles[] = {
	[FPLINUX_UI_ALERT_INFO] = FPLINUX_UI_LABEL_INFORMATION,
	[FPLINUX_UI_ALERT_CONFIRMATION] = FPLINUX_UI_LABEL_CONFIRMATION,
	[FPLINUX_UI_ALERT_WARNING] = FPLINUX_UI_LABEL_WARNING,
	[FPLINUX_UI_ALERT_ERROR] = FPLINUX_UI_LABEL_ERROR,
};

static void show_alert_title(struct fplinux_ui_screen *alert)
{
	lv_label_set_text(alert->box_title,
			  alert->title ?
				  alert->title :
				  word(alert, alert_titles[alert->alert_kind]));
}

/* NULL removes the band; an alert then names its kind. */
bool fplinux_ui_screen_set_title(struct fplinux_ui_screen *screen,
				 const char *title)
{
	char *copy = NULL;

	if (!screen) {
		errno = EINVAL;
		return false;
	}
	if (title && !(copy = strdup(title)))
		return false;
	free(screen->title);
	screen->title = copy;
	if (screen->kind == SCREEN_ALERT)
		show_alert_title(screen);
	else
		layout_frame(screen);
	return true;
}

void fplinux_ui_screen_set_data(struct fplinux_ui_screen *screen, void *data)
{
	if (screen)
		screen->data = data;
}

void *fplinux_ui_screen_get_data(const struct fplinux_ui_screen *screen)
{
	return screen ? screen->data : NULL;
}

/*
 * An alert's box: the surface less the alert margins, at most, above the
 * legend bar of a fullscreen application.
 */
static int32_t alert_box_width(struct fplinux_ui *ui)
{
	return fplinux_ui_width(ui) - 2 * fplinux_ui_spec(ui)->alert_margin;
}

static int32_t alert_box_max_height(struct fplinux_ui *ui)
{
	return fplinux_ui_modal_height(ui) -
	       2 * fplinux_ui_spec(ui)->alert_margin;
}

/*
 * From the sizes the screens are laid out with, not from LVGL's layout,
 * which may still be pending.
 */
bool fplinux_ui_screen_get_content_size(const struct fplinux_ui_screen *screen,
					int *width, int *height)
{
	const struct fplinux_ui_profile_spec *spec;
	int32_t top;
	int32_t w;
	int32_t h;

	if (!screen || !width || !height) {
		errno = EINVAL;
		return false;
	}
	spec = spec_of(screen);
	top = screen->title ? spec->header_height : 0;
	if (screen->kind == SCREEN_ALERT) {
		int32_t inset = 2 * (spec->alert_border + spec->alert_padding);

		w = alert_box_width(screen->ui) - inset;
		h = alert_box_max_height(screen->ui) - inset;
	} else if (screen->popup_choice) {
		/* The runtime's list of a popup choice, inside its frame. */
		w = lv_obj_get_style_width(screen->box, LV_PART_MAIN) -
		    2 * spec->alert_border;
		h = lv_obj_get_style_height(screen->box, LV_PART_MAIN) -
		    2 * spec->alert_border - top;
	} else {
		w = fplinux_ui_width(screen->ui);
		h = screen_height(screen) - top;
	}
	*width = w > 0 ? (int)w : 0;
	*height = h > 0 ? (int)h : 0;
	return true;
}

/* Focus and legends */

static struct form_item *focused_item(const struct fplinux_ui_screen *screen)
{
	if ((screen->kind != SCREEN_FORM && screen->kind != SCREEN_TEXT_BOX) ||
	    screen->focus == NONE)
		return NULL;
	return screen->items[screen->focus];
}

static struct form_item *focused_text(const struct fplinux_ui_screen *screen)
{
	struct form_item *item = focused_item(screen);

	return item && item->kind == ITEM_TEXT ? item : NULL;
}

/* The focused list row or form item, whose item commands apply; else NONE. */
static size_t focused_index(const struct fplinux_ui_screen *screen)
{
	if (screen->kind == SCREEN_LIST)
		return fplinux_ui_listview_selected(screen->list);
	return screen->kind == SCREEN_FORM ? screen->focus : NONE;
}

static bool actionable(const struct form_item *item);

/*
 * The item command that is the focused element's own OK action: on a row
 * of an IMPLICIT list, the row's first OK item command by priority, which
 * replaces the select command; on a form's link or button, its first item
 * command by priority. The index of that item command, else NONE.
 */
static size_t own_ok_command(const struct fplinux_ui_screen *screen)
{
	size_t index = focused_index(screen);
	size_t found = NONE;
	bool any_type;

	if (index == NONE)
		return NONE;
	if (screen->kind == SCREEN_LIST &&
	    screen->list_kind == FPLINUX_UI_LIST_IMPLICIT)
		any_type = false;
	else if (screen->kind == SCREEN_FORM &&
		 actionable(screen->items[index]))
		any_type = true;
	else
		return NONE;
	for (size_t i = 0; i < screen->item_command_count; ++i) {
		const struct item_command *command = &screen->item_commands[i];

		if (command->index == index &&
		    (any_type ||
		     role_of(command->command.type) == FPLINUX_UI_ROLE_OK) &&
		    (found == NONE ||
		     command->command.priority <
			     screen->item_commands[found].command.priority))
			found = i;
	}
	return found;
}

static const char *own_label(const struct fplinux_ui_screen *screen,
			     const struct command *command)
{
	return command->label ? command->label :
				word(screen, default_labels[command->type]);
}

static bool inline_choice(const struct form_item *item)
{
	return item->kind == ITEM_CHOICE &&
	       item->choice_kind != FPLINUX_UI_CHOICE_POPUP;
}

static const char *mark_word(const struct fplinux_ui_screen *screen,
			     const struct fplinux_ui_listview *list)
{
	return word(screen, fplinux_ui_listview_is_marked(
				    list, fplinux_ui_listview_selected(list)) ?
				    FPLINUX_UI_LABEL_UNMARK :
				    FPLINUX_UI_LABEL_MARK);
}

/* The centre legend of the focused element's own OK action, or NULL. */
static const char *element_action(const struct fplinux_ui_screen *screen)
{
	const struct form_item *item;

	switch (screen->kind) {
	case SCREEN_LIST:
		if (!fplinux_ui_listview_count(screen->list))
			return NULL;
		if (screen->list_kind == FPLINUX_UI_LIST_MULTIPLE)
			return mark_word(screen, screen->list);
		if (own_ok_command(screen) != NONE)
			return own_label(
				screen,
				&screen->item_commands[own_ok_command(screen)]
					 .command);
		if (screen->list_kind == FPLINUX_UI_LIST_IMPLICIT &&
		    screen->select_label)
			return screen->select_label;
		return word(screen, FPLINUX_UI_LABEL_SELECT);
	case SCREEN_FORM:
		item = focused_item(screen);
		if (!item)
			return NULL;
		if (own_ok_command(screen) != NONE)
			return own_label(
				screen,
				&screen->item_commands[own_ok_command(screen)]
					 .command);
		if (item->kind == ITEM_DATE ||
		    (item->kind == ITEM_CHOICE &&
		     item->choice_kind == FPLINUX_UI_CHOICE_POPUP))
			return word(screen, FPLINUX_UI_LABEL_EDIT);
		if (item->kind != ITEM_CHOICE)
			return NULL;
		if (item->choice_kind == FPLINUX_UI_CHOICE_MULTIPLE)
			return mark_word(screen, item->list);
		return word(screen, FPLINUX_UI_LABEL_SELECT);
	case SCREEN_TEXT_BOX:
		if (!fplinux_ui_text_takes_newline(screen->items[0]->field))
			return NULL;
		return word(screen, FPLINUX_UI_LABEL_NEW_LINE);
	default:
		return NULL;
	}
}

/*
 * The soft keys choose from the focused item's commands, then the
 * screen's, so that an item command comes first on equal priority.
 */
static size_t build_candidates(struct fplinux_ui_screen *screen)
{
	size_t index = focused_index(screen);
	size_t own_ok = own_ok_command(screen);
	size_t count = 0;

	for (size_t i = 0; index != NONE && i < screen->item_command_count;
	     ++i) {
		const struct command *command =
			&screen->item_commands[i].command;

		/* The element's own OK action is on OK, not in the commands. */
		if (screen->item_commands[i].index != index || i == own_ok)
			continue;
		screen->candidates[count] =
			(struct candidate){ .command = command, .item = true };
		screen->entries[count++] = (struct fplinux_ui_command_entry){
			.role = role_of(command->type),
			.priority = command->priority,
		};
	}
	for (size_t i = 0; i < screen->command_count; ++i) {
		const struct command *command = &screen->commands[i];

		screen->candidates[count] =
			(struct candidate){ .command = command };
		screen->entries[count++] = (struct fplinux_ui_command_entry){
			.role = role_of(command->type),
			.priority = command->priority,
		};
	}
	return count;
}

static int32_t legend_width(const lv_font_t *font, const char *text)
{
	lv_point_t size;

	if (!text[0])
		return 0;
	lv_text_get_size(&size, text, font, 0, 0, LV_COORD_MAX,
			 LV_TEXT_FLAG_NONE);
	return size.x;
}

/*
 * The launcher's soft-key bar spans the display and shows each legend at
 * its natural width while all of them fit between its margins.
 */
static bool legends_fit(const struct fplinux_ui_screen *screen,
			const char *const legends[3])
{
	const struct fplinux_ui_profile_spec *spec = spec_of(screen);
	const lv_font_t *font = fplinux_ui_theme(screen->ui)->font_softkey;
	int32_t width = 0;
	int32_t shown = 0;

	for (int i = 0; i < 3; ++i) {
		int32_t legend = legend_width(font, legends[i]);

		width += legend;
		shown += legend > 0;
	}
	if (shown > 1)
		width += (shown - 1) * spec->legend_gap;
	return width <= fplinux_ui_metrics(screen->ui)->screen_width -
				2 * spec->legend_margin;
}

/*
 * Assigns the keys and their legends. When the left command's legend does
 * not fit beside the others, the left key becomes Options with that
 * command first in the menu.
 */
static void assign_keys(struct fplinux_ui_screen *screen,
			struct soft_keys *soft)
{
	static const struct fplinux_ui_command_entry close = {
		.role = FPLINUX_UI_ROLE_BACK,
	};
	const struct fplinux_ui_command_entry *entries = &close;
	struct form_item *text = focused_text(screen);
	size_t count = 1;

	soft->action = element_action(screen);
	soft->clear = text && !fplinux_ui_text_is_read_only(text->field) &&
		      !fplinux_ui_text_is_empty(text->field);
	if (!system_close(screen)) {
		entries = screen->entries;
		count = build_candidates(screen);
	}
	fplinux_ui_command_keys_assign(entries, count, soft->action != NULL,
				       soft->clear, &soft->keys, screen->order);
	soft->legends[0] = soft->keys.left != NONE ?
				   command_label(screen, soft->keys.left) :
			   soft->keys.options ?
				   word(screen, FPLINUX_UI_LABEL_OPTIONS) :
				   "";
	soft->legends[1] = soft->action ? soft->action :
			   soft->keys.ok != NONE ?
					  command_label(screen, soft->keys.ok) :
					  "";
	soft->legends[2] = soft->clear ?
				   word(screen, FPLINUX_UI_LABEL_CLEAR) :
			   soft->keys.right != NONE ?
				   command_label(screen, soft->keys.right) :
				   "";
	if (soft->keys.left != NONE && !legends_fit(screen, soft->legends)) {
		fplinux_ui_command_keys_collapse(&soft->keys, screen->order);
		soft->legends[0] = word(screen, FPLINUX_UI_LABEL_OPTIONS);
	}
}

static void form_settle(struct fplinux_ui_screen *form);
static void keep_focus_visible(struct fplinux_ui_screen *form);

static void refresh(struct fplinux_ui_screens *layer)
{
	struct fplinux_ui_screen *screen = layer->top;
	const char *legends[3] = { "", "", "" };
	struct form_item *text;
	struct soft_keys soft;

	fplinux_ui_modal_set(layer->ui, FPLINUX_UI_MODAL_SCREEN,
			     screen && !layer->closing &&
				     (screen->kind == SCREEN_ALERT ||
				      screen->popup_choice || screen->editing ||
				      launcher_legend_bar(screen)));
	if (!screen || layer->closing)
		return;
	form_settle(screen);
	text = focused_text(screen);
	if (screen->kind == SCREEN_FORM) {
		/* The date editor's item stays in view above the bar. */
		size_frame(screen);
		if (screen->editing)
			keep_focus_visible(screen);
	}
	if (screen->editing) {
		legends[1] = word(screen, FPLINUX_UI_LABEL_DONE);
		legends[2] = word(screen, FPLINUX_UI_LABEL_CANCEL);
	} else if (text && fplinux_ui_text_is_picker_open(text->field)) {
		legends[1] = word(screen, FPLINUX_UI_LABEL_INSERT);
		legends[2] = word(screen, FPLINUX_UI_LABEL_CANCEL);
	} else {
		assign_keys(screen, &soft);
		for (int i = 0; i < 3; ++i)
			legends[i] = soft.legends[i];
	}
	if (layer->legends_sent && !strcmp(legends[0], layer->legends[0]) &&
	    !strcmp(legends[1], layer->legends[1]) &&
	    !strcmp(legends[2], layer->legends[2]))
		return;
	if (!fplinux_ui_set_softkeys(screen->ui, legends[0], legends[1],
				     legends[2]))
		return;
	for (int i = 0; i < 3; ++i)
		strcpy(layer->legends[i], legends[i]);
	layer->legends_sent = true;
}

/* Alerts */

static void dismiss_alert(struct fplinux_ui_screen *alert, const int *id);

static void alert_expired(lv_timer_t *timer)
{
	struct fplinux_ui_screen *alert = lv_timer_get_user_data(timer);

	lv_timer_delete(timer);
	alert->timer = NULL;
	dismiss_alert(alert, NULL);
}

static void spin_step(lv_timer_t *timer)
{
	struct fplinux_ui_screen *alert = lv_timer_get_user_data(timer);

	alert->spin_angle = (alert->spin_angle + SPINNER_STEP_DEGREES) % 360;
	lv_arc_set_rotation(alert->indicator, alert->spin_angle);
}

static void stop_timer(lv_timer_t **timer)
{
	if (*timer)
		lv_timer_delete(*timer);
	*timer = NULL;
}

/*
 * The timeout and the spinner run only while the alert is on top and
 * visible; a busy alert has no timeout.
 */
static void run_alert_timer(struct fplinux_ui_screen *alert, bool run)
{
	bool expire = run && alert->timeout_ms && !alert->busy;
	bool spin = run && alert->busy && !alert->maximum;

	if (!expire)
		stop_timer(&alert->timer);
	else if (!alert->timer)
		alert->timer = lv_timer_create(alert_expired, alert->timeout_ms,
					       alert);
	if (!spin)
		stop_timer(&alert->spin_timer);
	else if (!alert->spin_timer)
		alert->spin_timer =
			lv_timer_create(spin_step, SPINNER_STEP_MS, alert);
}

/*
 * An alert leaves the stack before its listener runs, which may still read
 * it, and is deleted afterwards unless the listener pushed it again. When it
 * was the only screen and the listener pushed none, the application leaves
 * as fplinux_ui_go_back() does from its first screen.
 */
static void dismiss_alert(struct fplinux_ui_screen *alert, const int *id)
{
	struct fplinux_ui_screens *layer = alert->layer;
	struct fplinux_ui *ui = alert->ui;

	/* Keys and timers reach only the top screen; nothing to remove. */
	if (layer->top != alert) {
		if (id)
			notify_command(alert, *id);
		return;
	}
	layer->top = alert->below;
	alert->below = NULL;
	alert->stacked = false;
	run_alert_timer(alert, false);
	restack(layer);
	if (id)
		notify_command(alert, *id);
	if (!alert->stacked)
		delete_screen(alert);
	if (layer->top) {
		refresh(layer);
		return;
	}
	/* Nothing is left to name the keys while the application leaves. */
	fplinux_ui_modal_set(ui, FPLINUX_UI_MODAL_SCREEN, false);
	fplinux_ui_set_softkeys(ui, "", "", "");
	layer->legends_sent = false;
	fplinux_ui_back(ui);
}

static void fire(struct fplinux_ui_screen *screen, int id)
{
	if (screen->kind == SCREEN_ALERT)
		dismiss_alert(screen, &id);
	else
		notify_command(screen, id);
}

static bool is_alive(const struct fplinux_ui_screens *layer,
		     const struct fplinux_ui_screen *screen)
{
	for (const struct fplinux_ui_screen *other = layer->all; other;
	     other = other->next)
		if (other == screen)
			return true;
	return false;
}

/*
 * An item command goes to the item listener, else to the command listener
 * while fplinux_ui_screen_get_command_item() names the item. The listener
 * may delete the screen.
 */
static void fire_item(struct fplinux_ui_screen *screen, size_t index, int id)
{
	struct fplinux_ui_screens *layer = screen->layer;

	if (screen->on_item) {
		screen->on_item(screen->ui, screen, index, id,
				screen->item_context);
		return;
	}
	screen->command_item = index;
	notify_command(screen, id);
	if (is_alive(layer, screen))
		screen->command_item = NONE;
}

/* Fires a command that assign_keys() put on a key. */
static void fire_candidate(struct fplinux_ui_screen *screen, size_t index)
{
	if (system_close(screen))
		dismiss_alert(screen, NULL);
	else if (screen->candidates[index].item)
		fire_item(screen, focused_index(screen),
			  screen->candidates[index].command->id);
	else
		fire(screen, screen->candidates[index].command->id);
}

/* Form focus */

/* A gauge's name or a date's value scrolls while it has focus. */
static lv_obj_t *marquee_label(const struct form_item *item)
{
	if (item->kind == ITEM_GAUGE)
		return item->gauge_name;
	return item->kind == ITEM_DATE ? item->value : NULL;
}

static bool has_item_commands(const struct form_item *item)
{
	const struct fplinux_ui_screen *form = item->form;

	for (size_t i = 0; i < form->item_command_count; ++i)
		if (form->item_commands[i].index == item->index)
			return true;
	return false;
}

/*
 * A string or image item drawn as a link or button: it takes the focus,
 * and OK runs its first item command, only while it has item commands.
 */
static bool actionable(const struct form_item *item)
{
	return (item->kind == ITEM_STRING || item->kind == ITEM_IMAGE) &&
	       item->appearance != FPLINUX_UI_APPEARANCE_PLAIN &&
	       has_item_commands(item);
}

/*
 * A focused link is filled with the accent colour, as a focused row is; a
 * button is a framed control, whose frame turns to the focus colour.
 */
static void show_appearance_focus(struct form_item *item, bool focused)
{
	const struct fplinux_ui_theme *theme = fplinux_ui_theme(item->form->ui);
	lv_color_t colour = focused ? theme->accent_text : theme->accent;

	if (item->appearance == FPLINUX_UI_APPEARANCE_BUTTON) {
		lv_obj_set_style_border_color(
			item->frame, focused ? theme->focus : theme->border, 0);
		return;
	}
	lv_obj_set_style_bg_opa(item->frame,
				focused ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
	if (item->kind == ITEM_IMAGE)
		lv_obj_set_style_image_recolor(item->value, colour, 0);
	else
		lv_obj_set_style_text_color(item->value, colour, 0);
}

/*
 * Draws the focus of an active form; text fields and choices take the
 * marquee themselves, gauges and dates through marquee_label().
 */
static void form_apply_focus(struct fplinux_ui_screen *form, bool active)
{
	const struct fplinux_ui_theme *theme = fplinux_ui_theme(form->ui);
	lv_obj_t *marquee = NULL;

	for (size_t i = 0; i < form->item_count; ++i) {
		struct form_item *item = form->items[i];
		bool focused = active && i == form->focus;
		lv_obj_t *label = marquee_label(item);

		if (item->kind == ITEM_TEXT)
			fplinux_ui_text_set_focused(item->field, focused);
		else if (item->kind == ITEM_CHOICE)
			fplinux_ui_listview_set_selectable(item->list, focused);
		else if (actionable(item))
			show_appearance_focus(item, focused);
		else if (item->frame &&
			 (item->kind == ITEM_DATE || item->interactive))
			lv_obj_set_style_border_color(
				item->frame,
				focused ? theme->focus : theme->border, 0);
		if (label && focused)
			marquee = label;
		else if (label)
			fplinux_ui_marquee_blur(form->ui, label);
	}
	if (marquee)
		fplinux_ui_marquee_focus(form->ui, marquee, NULL);
}

static void view_area(lv_obj_t *content, lv_area_t *view)
{
	lv_obj_get_coords(content, view);
	view->y1 += lv_obj_get_style_pad_top(content, LV_PART_MAIN);
	view->y2 -= lv_obj_get_style_pad_bottom(content, LV_PART_MAIN);
}

/* Scrolls the least that shows area, or its top when it is taller. */
static void reveal(lv_obj_t *content, const lv_area_t *area)
{
	lv_area_t view;
	int32_t dy;

	view_area(content, &view);
	dy = fplinux_ui_reveal_scroll(view.y1, view.y2, area->y1, area->y2);
	if (dy)
		lv_obj_scroll_by_bounded(content, 0, dy, LV_ANIM_OFF);
}

/* The focused row of an inline choice, or the whole item. */
static void item_area(const struct form_item *item, lv_area_t *area)
{
	if (inline_choice(item) && fplinux_ui_listview_count(item->list)) {
		lv_obj_t *rows = fplinux_ui_listview_object(item->list);

		lv_obj_get_coords(
			lv_obj_get_child(rows,
					 (int32_t)fplinux_ui_listview_selected(
						 item->list)),
			area);
		return;
	}
	lv_obj_get_coords(item->box, area);
}

static bool focusable(const struct form_item *item)
{
	return !item->hidden &&
	       (item->kind == ITEM_TEXT || item->kind == ITEM_CHOICE ||
		item->kind == ITEM_DATE ||
		(item->kind == ITEM_GAUGE && item->interactive) ||
		actionable(item));
}

static void form_focus(struct fplinux_ui_screen *form, size_t index,
		       enum entry entry)
{
	struct form_item *item = form->items[index];
	lv_area_t area;

	form->focus = index;
	if (inline_choice(item) && entry != ENTER_KEEP)
		fplinux_ui_listview_select(
			item->list, entry == ENTER_TOP ? 0 : item->count - 1);
	form_apply_focus(form, is_top(form));
	lv_obj_update_layout(form->content);
	item_area(item, &area);
	reveal(form->content, &area);
}

/*
 * When a form first shows a focusable item, that item takes the focus if it
 * is in sight; otherwise Down reads up to it.
 */
static void form_settle(struct fplinux_ui_screen *form)
{
	lv_area_t view;
	lv_area_t area;

	if (form->kind != SCREEN_FORM || form->settled)
		return;
	for (size_t i = 0; i < form->item_count; ++i) {
		if (!focusable(form->items[i]))
			continue;
		form->settled = true;
		lv_obj_update_layout(form->content);
		view_area(form->content, &view);
		lv_obj_get_coords(form->items[i]->box, &area);
		if (area.y1 <= view.y2 && area.y2 >= view.y1)
			form_focus(form, i, ENTER_TOP);
		return;
	}
}

/*
 * The next focusable item that way, or from the view without a focus: one
 * whose leading edge is in sight or still ahead, never one that a scroll
 * has partly passed (form_move()).
 */
static size_t neighbour(const struct fplinux_ui_screen *form, bool down,
			const lv_area_t *view)
{
	lv_area_t area;

	if (form->focus != NONE) {
		for (size_t i = form->focus;
		     down ? i + 1 < form->item_count : i > 0;) {
			i = down ? i + 1 : i - 1;
			if (focusable(form->items[i]))
				return i;
		}
		return NONE;
	}
	for (size_t n = 0; n < form->item_count; ++n) {
		size_t i = down ? n : form->item_count - 1 - n;

		if (!focusable(form->items[i]))
			continue;
		lv_obj_get_coords(form->items[i]->box, &area);
		if (down ? area.y1 >= view->y1 : area.y2 <= view->y2)
			return i;
	}
	return NONE;
}

/* The first item met going that way: after the focus, or the first in sight. */
static long first_passed(const struct fplinux_ui_screen *form, bool down,
			 const lv_area_t *view)
{
	long count = (long)form->item_count;
	lv_area_t area;

	if (form->focus != NONE)
		return (long)form->focus + (down ? 1 : -1);
	for (long n = 0; n < count; ++n) {
		long i = down ? n : count - 1 - n;

		if (form->items[i]->hidden)
			continue;
		lv_obj_get_coords(form->items[i]->box, &area);
		if (down ? area.y2 > view->y1 : area.y1 < view->y2)
			return i;
	}
	return down ? count : -1;
}

/* True when read-only content before the next item is not yet in sight. */
static bool unread(const struct fplinux_ui_screen *form, bool down, size_t next,
		   const lv_area_t *view)
{
	long end = next != NONE ? (long)next :
		   down		? (long)form->item_count :
				  -1;
	lv_area_t area;

	for (long i = first_passed(form, down, view); i != end;
	     i += down ? 1 : -1) {
		if (focusable(form->items[i]) || form->items[i]->hidden)
			continue;
		lv_obj_get_coords(form->items[i]->box, &area);
		if (down ? area.y2 > view->y2 : area.y1 < view->y1)
			return true;
	}
	return false;
}

/*
 * Up and Down step through a choice's rows, then to the next focusable item;
 * read-only content on the way scrolls by a row until it has been seen.
 */
static void form_move(struct fplinux_ui_screen *form, bool down)
{
	lv_obj_t *content = form->content;
	struct form_item *item = focused_item(form);
	lv_area_t view;
	lv_area_t area;
	int32_t top;
	size_t next;

	lv_obj_update_layout(content);
	view_area(content, &view);
	if (item && inline_choice(item)) {
		size_t row = fplinux_ui_listview_selected(item->list);

		if (down ? row + 1 < item->count : row > 0) {
			fplinux_ui_listview_select(item->list,
						   down ? row + 1 : row - 1);
			item_area(item, &area);
			reveal(content, &area);
			return;
		}
	}
	next = neighbour(form, down, &view);
	if (next != NONE && !unread(form, down, next, &view)) {
		form_focus(form, next, down ? ENTER_TOP : ENTER_BOTTOM);
		return;
	}
	/* Read-only content scrolls by a row. */
	if (item)
		item_area(item, &area);
	lv_obj_scroll_by_bounded(content, 0,
				 down ? -spec_of(form)->row_height :
					spec_of(form)->row_height,
				 LV_ANIM_OFF);
	if (!item)
		return;
	/*
	 * A step that leaves the focused item, or a choice's focused row,
	 * partly hidden takes the focus from it, and the form reads on as one
	 * without focus; going back, the item takes it again once its edge is
	 * in sight (neighbour()). The form's padding still shows it.
	 */
	top = area.y1;
	lv_obj_get_coords(content, &view);
	item_area(item, &area);
	if (area.y1 != top && (area.y1 < view.y1 || area.y2 > view.y2)) {
		form->focus = NONE;
		form_apply_focus(form, true);
	}
}

/* The stack */

static void set_active(struct fplinux_ui_screen *screen, bool active)
{
	active = active && screen->layer->visible &&
		 !fplinux_ui_menu_is_open(screen->ui);
	if (screen->kind == SCREEN_FORM || screen->kind == SCREEN_TEXT_BOX)
		form_apply_focus(screen, active);
	else if (screen->kind == SCREEN_LIST)
		fplinux_ui_listview_set_active(screen->list, active);
	else if (screen->kind == SCREEN_ALERT)
		run_alert_timer(screen, active && screen->layer->visible);
	else if (screen->kind == SCREEN_CANVAS)
		fplinux_ui_text_canvas_active(screen->root, active);
}

/* The top screen shows, and an alert shows the screens below it too. */
static void restack(struct fplinux_ui_screens *layer)
{
	bool shown = true;

	for (struct fplinux_ui_screen *screen = layer->top; screen;
	     screen = screen->below) {
		bool showing = shown && lv_obj_is_hidden(screen->root);

		lv_obj_set_hidden(screen->root, !shown);
		/* A long title scrolls once each time its screen shows. */
		if (showing && screen->title && screen->band_label)
			fplinux_ui_title_scroll(screen->band_label);
		else if (!shown && screen->band_label)
			fplinux_ui_title_hide(screen->band_label);
		set_active(screen, screen == layer->top);
		if (screen->kind != SCREEN_ALERT && !screen->popup_choice)
			shown = false;
	}
}

static void report_text(void *data);
static void popup_changed(struct fplinux_ui *ui,
			  struct fplinux_ui_screen *screen, size_t index,
			  void *context);

/*
 * The list of a popup choice deleted while the list is open stays until
 * the user leaves it, and then chooses nothing.
 */
static void detach_popup(struct form_item *item)
{
	for (struct fplinux_ui_screen *screen = item->form->layer->all; screen;
	     screen = screen->next)
		if (screen->on_change == popup_changed &&
		    screen->context == item)
			screen->context = NULL;
}

static void free_item(struct form_item *item)
{
	detach_popup(item);
	lv_async_call_cancel(report_text, item);
	for (size_t i = 0; i < item->count; ++i)
		free(item->choices[i]);
	free(item->choices);
	free(item->content);
	free(item->reported);
	free(item->name);
	/* The image object, deleted before, owned item->image. */
	if (item->fitted)
		lv_draw_buf_destroy(item->fitted);
	free(item);
}

/* Frees a screen that is no longer in the layer's list. */
static void destroy_screen(struct fplinux_ui_screen *screen)
{
	stop_timer(&screen->timer);
	stop_timer(&screen->spin_timer);
	/* The list views and text fields go with their objects. */
	lv_obj_delete(screen->root);
	for (size_t i = 0; i < screen->item_count; ++i)
		free_item(screen->items[i]);
	free(screen->items);
	for (size_t i = 0; i < screen->command_count; ++i)
		free_command(&screen->commands[i]);
	free(screen->commands);
	clear_item_commands(screen);
	free(screen->item_commands);
	free(screen->candidates);
	free(screen->entries);
	free(screen->order);
	free(screen->select_label);
	free(screen->title);
	free(screen);
}

static void delete_screen(struct fplinux_ui_screen *screen)
{
	struct fplinux_ui_screens *layer = screen->layer;
	struct fplinux_ui_screen **link = &layer->all;

	if (layer->menu_screen == screen) {
		layer->menu_screen = NULL;
		fplinux_ui_menu_close(screen->ui);
	}
	while (*link != screen)
		link = &(*link)->next;
	*link = screen->next;
	destroy_screen(screen);
}

void fplinux_ui_push(struct fplinux_ui *ui, struct fplinux_ui_screen *screen)
{
	struct fplinux_ui_screens *layer;

	if (!ui || !screen || screen->ui != ui || screen->stacked)
		return;
	layer = screen->layer;
	/* A new screen supersedes top-layer Options and its captured commands. */
	if (fplinux_ui_menu_is_open(ui)) {
		layer->menu_screen = NULL;
		fplinux_ui_menu_close(ui);
	}
	screen->below = layer->top;
	screen->stacked = true;
	layer->top = screen;
	lv_obj_move_to_index(screen->root, -1);
	restack(layer);
	refresh(layer);
}

void fplinux_ui_replace(struct fplinux_ui *ui, struct fplinux_ui_screen *screen)
{
	struct fplinux_ui_screen *old;

	if (!ui || !screen || screen->ui != ui || screen->stacked)
		return;
	old = screen->layer->top;
	if (old) {
		screen->layer->top = old->below;
		old->below = NULL;
		old->stacked = false;
	}
	fplinux_ui_push(ui, screen);
	if (old)
		delete_screen(old);
}

bool fplinux_ui_pop(struct fplinux_ui *ui)
{
	struct fplinux_ui_screens *layer = ui ? *fplinux_ui_screens_slot(ui) :
						NULL;
	struct fplinux_ui_screen *top = layer ? layer->top : NULL;

	if (!top || !top->below)
		return false;
	layer->top = top->below;
	top->below = NULL;
	top->stacked = false;
	restack(layer);
	delete_screen(top);
	refresh(layer);
	return true;
}

struct fplinux_ui_screen *fplinux_ui_current(const struct fplinux_ui *ui)
{
	struct fplinux_ui_screens *layer;

	if (!ui)
		return NULL;
	layer = *fplinux_ui_screens_slot((struct fplinux_ui *)ui);
	return layer ? layer->top : NULL;
}

void fplinux_ui_go_back(struct fplinux_ui *ui)
{
	if (ui && !fplinux_ui_pop(ui))
		fplinux_ui_back(ui);
}

/* Images */

static const uint8_t png_signature[8] = { 0x89, 'P',  'N',  'G',
					  '\r', '\n', 0x1a, '\n' };

static uint32_t read_be32(const uint8_t *bytes)
{
	return (uint32_t)bytes[0] << 24 | (uint32_t)bytes[1] << 16 |
	       (uint32_t)bytes[2] << 8 | bytes[3];
}

static bool read_all(int fd, uint8_t *data, size_t size)
{
	size_t done = 0;

	while (done < size) {
		ssize_t count = read(fd, data + done, size - done);

		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0) {
			if (!count)
				errno = EINVAL;
			return false;
		}
		done += (size_t)count;
	}
	return true;
}

/* Reads a PNG file whole into a descriptor of LVGL's PNG decoder. */
static uint8_t *read_png(const char *path, lv_image_dsc_t *image)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	struct stat status;
	uint8_t *data = NULL;
	int error;

	if (fd < 0)
		return NULL;
	if (fstat(fd, &status) < 0)
		goto fail;
	if (!S_ISREG(status.st_mode) || status.st_size > IMAGE_FILE_MAX_BYTES) {
		errno = S_ISREG(status.st_mode) ? EFBIG : EINVAL;
		goto fail;
	}
	data = malloc(status.st_size ? (size_t)status.st_size : 1);
	if (!data || !read_all(fd, data, (size_t)status.st_size))
		goto fail;
	/* The signature, then IHDR: 13-byte length, type, width and height. */
	if (status.st_size < 24 ||
	    memcmp(data, png_signature, sizeof(png_signature)) ||
	    read_be32(data + 8) != 13 || memcmp(data + 12, "IHDR", 4) ||
	    read_be32(data + 16) > UINT16_MAX ||
	    read_be32(data + 20) > UINT16_MAX) {
		errno = EINVAL;
		goto fail;
	}
	close(fd);
	*image = (lv_image_dsc_t){
		.header = {
			.magic = LV_IMAGE_HEADER_MAGIC,
			.cf = LV_COLOR_FORMAT_RAW_ALPHA,
			.w = read_be32(data + 16),
			.h = read_be32(data + 20),
		},
		.data_size = (uint32_t)status.st_size,
		.data = data,
	};
	return data;
fail:
	error = errno;
	free(data);
	close(fd);
	errno = error;
	return NULL;
}

/* A failed draw leaves the cleared buffer transparent. */
static bool has_visible_pixel(const lv_draw_buf_t *buffer)
{
	for (uint32_t y = 0; y < buffer->header.h; ++y) {
		const lv_color32_t *row =
			(const lv_color32_t *)(buffer->data +
					       y * buffer->header.stride);

		for (uint32_t x = 0; x < buffer->header.w; ++x)
			if (row[x].alpha)
				return true;
	}
	return false;
}

/*
 * Draws a decodable source once, at its own size, into a new ARGB8888
 * buffer. Without an image cache LVGL would decode a PNG source again on
 * every frame; the buffer is drawn as it is. An alpha-only source draws its
 * mask in black.
 */
static lv_draw_buf_t *draw_image(const void *src,
				 const lv_image_header_t *header)
{
	const lv_area_t area = { 0, 0, (int32_t)header->w - 1,
				 (int32_t)header->h - 1 };
	lv_draw_buf_t *buffer = lv_draw_buf_create(
		header->w, header->h, LV_COLOR_FORMAT_ARGB8888, LV_STRIDE_AUTO);
	lv_draw_image_dsc_t image;
	lv_layer_t layer;
	lv_obj_t *canvas;

	if (!buffer)
		return NULL;
	canvas = lv_canvas_create(NULL);
	if (!canvas) {
		lv_draw_buf_destroy(buffer);
		return NULL;
	}
	lv_draw_buf_clear(buffer, NULL);
	lv_canvas_set_draw_buf(canvas, buffer);
	lv_canvas_init_layer(canvas, &layer);
	lv_draw_image_dsc_init(&image);
	image.src = src;
	image.scale_x = LV_SCALE_NONE;
	image.scale_y = LV_SCALE_NONE;
	image.pivot.x = 0;
	image.pivot.y = 0;
	lv_draw_image(&layer, &image, &area);
	lv_canvas_finish_layer(canvas, &layer);
	lv_obj_delete(canvas);
	return buffer;
}

/*
 * other * limit / side rounded to the nearest pixel, at least 1 and at
 * most other: the proportional size of the side that does not limit.
 */
static uint32_t proportional(uint32_t other, uint32_t limit, uint32_t side)
{
	uint64_t result = ((uint64_t)other * limit + side / 2) / side;

	return result < 1 ? 1 : result > other ? other : (uint32_t)result;
}

/*
 * The size that fits width x height with the proportions kept, never
 * larger than the image: the limiting side takes the box's size exactly,
 * so that a 48 px icon becomes exactly 32 x 32.
 */
static void fit_size(const lv_image_header_t *header, uint32_t width,
		     uint32_t height, uint32_t *fit_width, uint32_t *fit_height)
{
	*fit_width = header->w;
	*fit_height = header->h;
	if (header->w <= width && header->h <= height)
		return;
	if ((uint64_t)header->w * height >= (uint64_t)header->h * width) {
		*fit_width = width;
		*fit_height = proportional(header->h, width, header->w);
	} else {
		*fit_height = height;
		*fit_width = proportional(header->w, height, header->h);
	}
}

/* The length both spans share, in units of 1 / (scaled size) pixel. */
static uint64_t overlap(uint64_t start, uint64_t end, uint64_t from,
			uint64_t to)
{
	uint64_t low = start > from ? start : from;
	uint64_t high = end < to ? end : to;

	return high > low ? high - low : 0;
}

/*
 * sum / divisor rounded to the nearest channel value; 0 without weight.
 * Both are below 2^53, so the double quotient is exact enough.
 */
static uint8_t average(uint64_t sum, uint64_t divisor)
{
	return divisor ? (uint8_t)((double)sum / (double)divisor + 0.5) : 0;
}

/*
 * Scales an ARGB8888 image down with an area filter: every pixel of the
 * result averages the source area it covers, weighted by coverage, with
 * premultiplied alpha so that transparent pixels add no colour.
 */
static lv_draw_buf_t *area_scale(const lv_draw_buf_t *source, uint32_t width,
				 uint32_t height)
{
	uint32_t source_width = source->header.w;
	uint32_t source_height = source->header.h;
	uint64_t total = (uint64_t)source_width * source_height;
	lv_draw_buf_t *result;

	if (!total || !width || !height)
		return NULL;
	result = lv_draw_buf_create(width, height, LV_COLOR_FORMAT_ARGB8888,
				    LV_STRIDE_AUTO);
	if (!result)
		return NULL;
	for (uint32_t y = 0; y < height; ++y) {
		lv_color32_t *out = (lv_color32_t *)(result->data +
						     y * result->header.stride);
		uint64_t top = (uint64_t)y * source_height;
		uint64_t bottom = top + source_height;

		for (uint32_t x = 0; x < width; ++x) {
			uint64_t left = (uint64_t)x * source_width;
			uint64_t right = left + source_width;
			uint64_t red = 0;
			uint64_t green = 0;
			uint64_t blue = 0;
			uint64_t alpha = 0;

			for (uint32_t sy = (uint32_t)(top / height);
			     sy < source_height &&
			     (uint64_t)sy * height < bottom;
			     ++sy) {
				const lv_color32_t *row =
					(const lv_color32_t
						 *)(source->data +
						    sy * source->header.stride);
				uint64_t weight_y = overlap(
					top, bottom, (uint64_t)sy * height,
					(uint64_t)(sy + 1) * height);

				for (uint32_t sx = (uint32_t)(left / width);
				     sx < source_width &&
				     (uint64_t)sx * width < right;
				     ++sx) {
					uint64_t weight =
						weight_y *
						overlap(left, right,
							(uint64_t)sx * width,
							(uint64_t)(sx + 1) *
								width);
					lv_color32_t pixel = row[sx];
					uint64_t covered = weight * pixel.alpha;

					red += covered * pixel.red;
					green += covered * pixel.green;
					blue += covered * pixel.blue;
					alpha += covered;
				}
			}
			out[x].alpha = average(alpha, total);
			out[x].red = average(red, alpha);
			out[x].green = average(green, alpha);
			out[x].blue = average(blue, alpha);
		}
	}
	return result;
}

/* The alpha channel alone, which LVGL draws in the image_recolor colour. */
static lv_draw_buf_t *alpha_only(const lv_draw_buf_t *source)
{
	lv_draw_buf_t *mask =
		lv_draw_buf_create(source->header.w, source->header.h,
				   LV_COLOR_FORMAT_A8, LV_STRIDE_AUTO);

	if (!mask)
		return NULL;
	for (uint32_t y = 0; y < source->header.h; ++y) {
		const lv_color32_t *row =
			(const lv_color32_t *)(source->data +
					       y * source->header.stride);
		uint8_t *out = mask->data + y * mask->header.stride;

		for (uint32_t x = 0; x < source->header.w; ++x)
			out[x] = row[x].alpha;
	}
	return mask;
}

static bool is_symbolic(const char *path)
{
	size_t length = strlen(path);
	size_t suffix = strlen(SYMBOLIC_SUFFIX);

	return length >= suffix &&
	       !strcmp(path + length - suffix, SYMBOLIC_SUFFIX);
}

/*
 * Decodes an image source into a buffer that fits width x height, scaled
 * down with its proportions kept and never up; NULL with errno. A system
 * glyph, an alpha-only source or a "-symbolic.png" file gives an A8 mask.
 */
lv_draw_buf_t *fplinux_ui_image_decode(const void *src, int width, int height,
				       bool alpha)
{
	lv_image_dsc_t file_image;
	lv_image_header_t header;
	lv_draw_buf_t *buffer = NULL;
	uint8_t *data = NULL;
	uint32_t fit_width;
	uint32_t fit_height;

	if (width <= 0 || height <= 0) {
		errno = EINVAL;
		return NULL;
	}
	switch (src ? lv_image_src_get_type(src) : LV_IMAGE_SRC_UNKNOWN) {
	case LV_IMAGE_SRC_FILE:
		alpha = alpha || is_symbolic(src);
		data = read_png(src, &file_image);
		if (!data)
			return NULL;
		src = &file_image;
		break;
	case LV_IMAGE_SRC_VARIABLE:
		alpha = alpha || ((const lv_image_dsc_t *)src)->header.cf ==
					 LV_COLOR_FORMAT_A8;
		break;
	default:
		errno = EINVAL;
		return NULL;
	}
	if (lv_image_decoder_get_info(src, &header) != LV_RESULT_OK ||
	    !header.w || !header.h) {
		errno = EINVAL;
	} else if ((uint64_t)header.w * header.h > IMAGE_MAX_PIXELS) {
		errno = EFBIG;
	} else {
		fit_size(&header, (uint32_t)width, (uint32_t)height, &fit_width,
			 &fit_height);
		buffer = draw_image(src, &header);
		if (buffer &&
		    (fit_width != header.w || fit_height != header.h)) {
			lv_draw_buf_t *full = buffer;

			buffer = area_scale(full, fit_width, fit_height);
			lv_draw_buf_destroy(full);
		}
		if (!buffer)
			errno = ENOMEM;
	}
	free(data);
	/*
	 * Nothing visible means that decoding failed; pixels that begin like
	 * a PNG file would be decoded as one again on every draw.
	 */
	if (buffer &&
	    (!has_visible_pixel(buffer) ||
	     !memcmp(buffer->data, png_signature, sizeof(png_signature)))) {
		lv_draw_buf_destroy(buffer);
		buffer = NULL;
		errno = EINVAL;
	}
	if (buffer && alpha) {
		lv_draw_buf_t *mask = alpha_only(buffer);

		lv_draw_buf_destroy(buffer);
		buffer = mask;
		if (!buffer)
			errno = ENOMEM;
	}
	return buffer;
}

lv_draw_buf_t *fplinux_ui_image_load(const void *src, int width, int height)
{
	return fplinux_ui_image_decode(src, width, height, false);
}

/* An image object owns the buffer it shows. */
static void image_delete_cb(lv_event_t *event)
{
	lv_draw_buf_destroy(lv_event_get_user_data(event));
}

/* Lists */

static bool is_list(const struct fplinux_ui_screen *screen)
{
	return screen && screen->kind == SCREEN_LIST;
}

/* A regular list: its rows are stored, unlike a virtual list's. */
static bool is_stored_list(const struct fplinux_ui_screen *screen)
{
	return is_list(screen) && !fplinux_ui_listview_is_virtual(screen->list);
}

static void list_selected(struct fplinux_ui_listview *list, size_t index,
			  void *context)
{
	(void)list;
	(void)index;
	changed(context);
}

static const enum fplinux_ui_listview_marker list_markers[] = {
	[FPLINUX_UI_LIST_IMPLICIT] = FPLINUX_UI_LISTVIEW_PLAIN,
	[FPLINUX_UI_LIST_EXCLUSIVE] = FPLINUX_UI_LISTVIEW_RADIO,
	[FPLINUX_UI_LIST_MULTIPLE] = FPLINUX_UI_LISTVIEW_CHECK,
};

/* The list shows its focus only once its screen is on top. */
static struct fplinux_ui_screen *finish_list(struct fplinux_ui_screen *screen)
{
	if (!screen->list) {
		int error = errno;

		delete_screen(screen);
		errno = error;
		return NULL;
	}
	fplinux_ui_listview_set_active(screen->list, false);
	fplinux_ui_listview_set_callbacks(screen->list, list_selected, NULL,
					  screen);
	return screen;
}

struct fplinux_ui_screen *fplinux_ui_list_new(struct fplinux_ui *ui,
					      const char *title,
					      enum fplinux_ui_list_kind kind)
{
	struct fplinux_ui_screen *screen;

	if ((unsigned int)kind > FPLINUX_UI_LIST_MULTIPLE) {
		errno = EINVAL;
		return NULL;
	}
	screen = screen_new(ui, SCREEN_LIST, title);
	if (!screen)
		return NULL;
	screen->list_kind = kind;
	screen->list = fplinux_ui_listview_create(ui, screen->content,
						  list_markers[kind]);
	return finish_list(screen);
}

static bool virtual_row(struct fplinux_ui_listview *list, size_t index,
			struct fplinux_ui_virtual_row *row, void *context)
{
	struct fplinux_ui_screen *screen = context;

	(void)list;
	return screen->virtual_row(screen->ui, screen, index, row,
				   screen->virtual_context);
}

/* LVGL coordinates are 32-bit: the content of every row must fit. */
static bool rows_fit(struct fplinux_ui *ui, size_t count)
{
	return count <=
	       (size_t)(INT32_MAX / fplinux_ui_spec(ui)->row_height_detail);
}

struct fplinux_ui_screen *
fplinux_ui_list_new_virtual(struct fplinux_ui *ui, const char *title,
			    enum fplinux_ui_list_kind kind, size_t count,
			    fplinux_ui_virtual_row_cb row, void *context)
{
	struct fplinux_ui_screen *screen;

	if (!ui || (unsigned int)kind > FPLINUX_UI_LIST_MULTIPLE || !row ||
	    !rows_fit(ui, count)) {
		errno = EINVAL;
		return NULL;
	}
	screen = screen_new(ui, SCREEN_LIST, title);
	if (!screen)
		return NULL;
	screen->list_kind = kind;
	screen->virtual_row = row;
	screen->virtual_context = context;
	screen->list = fplinux_ui_listview_create_virtual(ui, screen->content,
							  list_markers[kind],
							  count, virtual_row,
							  screen);
	return finish_list(screen);
}

void fplinux_ui_list_invalidate(struct fplinux_ui_screen *list, size_t first,
				size_t count)
{
	if (is_list(list))
		fplinux_ui_listview_invalidate(list->list, first, count);
}

void fplinux_ui_list_rows_inserted(struct fplinux_ui_screen *list, size_t index,
				   size_t count)
{
	if (!is_list(list) || !fplinux_ui_listview_is_virtual(list->list) ||
	    index > fplinux_ui_listview_count(list->list) ||
	    !rows_fit(list->ui,
		      fplinux_ui_listview_count(list->list) + count) ||
	    !count)
		return;
	fplinux_ui_listview_rows_inserted(list->list, index, count);
	item_commands_inserted(list, index, count);
	changed(list);
}

void fplinux_ui_list_rows_deleted(struct fplinux_ui_screen *list, size_t index,
				  size_t count)
{
	size_t size;

	if (!is_list(list) || !fplinux_ui_listview_is_virtual(list->list))
		return;
	size = fplinux_ui_listview_count(list->list);
	if (index >= size || !count)
		return;
	if (count > size - index)
		count = size - index;
	fplinux_ui_listview_rows_deleted(list->list, index, count);
	item_commands_deleted(list, index, count);
	changed(list);
}

size_t fplinux_ui_list_append(struct fplinux_ui_screen *list, const char *text,
			      const char *detail)
{
	size_t index;

	if (!is_stored_list(list)) {
		errno = EINVAL;
		return NONE;
	}
	index = fplinux_ui_listview_add(list->list, text, detail);
	changed(list);
	return index;
}

size_t fplinux_ui_list_insert(struct fplinux_ui_screen *list, size_t index,
			      const char *text, const char *detail)
{
	size_t row;

	if (!is_stored_list(list)) {
		errno = EINVAL;
		return NONE;
	}
	row = fplinux_ui_listview_insert(list->list, index, text, detail);
	if (row == NONE)
		return NONE;
	item_commands_inserted(list, row, 1);
	changed(list);
	return row;
}

bool fplinux_ui_list_delete(struct fplinux_ui_screen *list, size_t index)
{
	if (!is_stored_list(list) ||
	    !fplinux_ui_listview_remove(list->list, index)) {
		errno = EINVAL;
		return false;
	}
	item_commands_deleted(list, index, 1);
	changed(list);
	return true;
}

bool fplinux_ui_list_set(struct fplinux_ui_screen *list, size_t index,
			 const char *text, const char *detail)
{
	if (!is_stored_list(list)) {
		errno = EINVAL;
		return false;
	}
	return fplinux_ui_listview_set_text(list->list, index, text, detail);
}

void fplinux_ui_list_delete_all(struct fplinux_ui_screen *list)
{
	if (!is_stored_list(list))
		return;
	fplinux_ui_listview_clear(list->list);
	clear_item_commands(list);
	changed(list);
}

size_t fplinux_ui_list_size(const struct fplinux_ui_screen *list)
{
	return is_list(list) ? fplinux_ui_listview_count(list->list) : 0;
}

/* SIZE_MAX for an empty list, which has no focus. */
size_t fplinux_ui_list_get_focused(const struct fplinux_ui_screen *list)
{
	return is_list(list) ? fplinux_ui_listview_selected(list->list) : NONE;
}

void fplinux_ui_list_set_focused(struct fplinux_ui_screen *list, size_t index)
{
	if (is_list(list))
		fplinux_ui_listview_select(list->list, index);
}

/* An implicit list's selection is its focused row. */
bool fplinux_ui_list_is_selected(const struct fplinux_ui_screen *list,
				 size_t index)
{
	if (!is_stored_list(list)) {
		errno = EINVAL;
		return false;
	}
	if (index >= fplinux_ui_listview_count(list->list))
		return false;
	if (list->list_kind == FPLINUX_UI_LIST_IMPLICIT)
		return index == fplinux_ui_listview_selected(list->list);
	return fplinux_ui_listview_is_marked(list->list, index);
}

void fplinux_ui_list_set_selected(struct fplinux_ui_screen *list, size_t index,
				  bool selected)
{
	if (!is_stored_list(list)) {
		errno = EINVAL;
		return;
	}
	if (list->list_kind == FPLINUX_UI_LIST_IMPLICIT) {
		if (selected)
			fplinux_ui_listview_select(list->list, index);
		return;
	}
	fplinux_ui_listview_set_marked(list->list, index, selected);
	changed(list);
}

bool fplinux_ui_list_set_select_label(struct fplinux_ui_screen *list,
				      const char *label)
{
	char *copy = NULL;

	if (!is_list(list) || (label && !valid_label(label))) {
		errno = EINVAL;
		return false;
	}
	if (label && !(copy = strdup(label)))
		return false;
	free(list->select_label);
	list->select_label = copy;
	changed(list);
	return true;
}

bool fplinux_ui_list_set_icon(struct fplinux_ui_screen *list, size_t index,
			      const void *src)
{
	int size = 0;
	lv_draw_buf_t *icon = NULL;

	if (!is_stored_list(list) ||
	    index >= fplinux_ui_listview_count(list->list)) {
		errno = EINVAL;
		return false;
	}
	size = spec_of(list)->row_icon_size;
	if (src && !(icon = fplinux_ui_image_decode(src, size, size, false)))
		return false;
	if (!fplinux_ui_listview_set_icon(list->list, index, icon)) {
		if (icon)
			lv_draw_buf_destroy(icon);
		return false;
	}
	return true;
}

/*
 * OK on a row: an implicit list reports the select command, an exclusive
 * list marks the row and reports it even when it was marked already, so
 * that a choice can be confirmed, and a multiple list toggles it. A
 * virtual list reports the row and leaves its marks to the application.
 */
static void list_ok(struct fplinux_ui_screen *screen)
{
	struct fplinux_ui_listview *list = screen->list;
	size_t index = fplinux_ui_listview_selected(list);

	if (screen->list_kind == FPLINUX_UI_LIST_IMPLICIT) {
		notify_command(screen, FPLINUX_UI_SELECT_COMMAND);
		return;
	}
	if (!fplinux_ui_listview_is_virtual(list))
		fplinux_ui_listview_set_marked(
			list, index,
			screen->list_kind == FPLINUX_UI_LIST_EXCLUSIVE ||
				!fplinux_ui_listview_is_marked(list, index));
	report(screen, index);
}

/* Forms */

static bool is_form(const struct fplinux_ui_screen *screen)
{
	return screen && screen->kind == SCREEN_FORM;
}

static struct form_item *form_item(const struct fplinux_ui_screen *form,
				   size_t index, enum item_kind kind)
{
	if (!is_form(form) || index >= form->item_count ||
	    form->items[index]->kind != kind)
		return NULL;
	return form->items[index];
}

struct fplinux_ui_screen *fplinux_ui_form_new(struct fplinux_ui *ui,
					      const char *title)
{
	struct fplinux_ui_screen *form = screen_new(ui, SCREEN_FORM, title);
	lv_obj_t *content;

	if (!form)
		return NULL;
	content = form->content;
	lv_obj_set_scrollable(content, true);
	lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_ver(content, spec_of(form)->content_padding, 0);
	lv_obj_set_style_pad_row(content, spec_of(form)->spacing, 0);
	style_scrollbar(content, ui);
	return form;
}

static lv_obj_t *muted_label(struct fplinux_ui_screen *form, lv_obj_t *parent,
			     const char *text)
{
	lv_obj_t *label = lv_label_create(parent);

	lv_obj_set_width(label, lv_pct(100));
	lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
	lv_obj_set_style_text_color(label, fplinux_ui_theme(form->ui)->muted,
				    0);
	lv_label_set_text(label, text);
	return label;
}

static struct form_item *add_item(struct fplinux_ui_screen *form,
				  enum item_kind kind, const char *label)
{
	struct form_item *item;

	if (form->item_count == form->item_capacity) {
		size_t capacity =
			form->item_capacity ? 2 * form->item_capacity : 8;
		struct form_item **items =
			realloc(form->items, capacity * sizeof(*items));

		if (!items)
			return NULL;
		form->items = items;
		form->item_capacity = capacity;
	}
	item = calloc(1, sizeof(*item));
	if (!item)
		return NULL;
	item->form = form;
	item->index = form->item_count;
	item->kind = kind;
	item->box = plain_object(form->content);
	lv_obj_set_size(item->box, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(item->box, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(item->box, spec_of(form)->gap_small, 0);
	lv_obj_set_style_pad_left(item->box, spec_of(form)->margin, 0);
	lv_obj_set_style_pad_right(item->box, spec_of(form)->gutter, 0);
	if (label)
		item->label = muted_label(form, item->box, label);
	form->items[form->item_count++] = item;
	return item;
}

/* Undoes a failed append of the last item. */
static size_t drop_item(struct fplinux_ui_screen *form, int error)
{
	struct form_item *item = form->items[--form->item_count];

	lv_obj_delete(item->box);
	free_item(item);
	errno = error;
	return NONE;
}

static size_t appended(struct fplinux_ui_screen *form, struct form_item *item)
{
	changed(form);
	return item->index;
}

size_t fplinux_ui_form_append_string(struct fplinux_ui_screen *form,
				     const char *label, const char *text)
{
	struct form_item *item;

	if (!is_form(form)) {
		errno = EINVAL;
		return NONE;
	}
	item = add_item(form, ITEM_STRING, label);
	if (!item)
		return NONE;
	item->value = lv_label_create(item->box);
	lv_obj_set_width(item->value, lv_pct(100));
	lv_label_set_long_mode(item->value, LV_LABEL_LONG_MODE_WRAP);
	lv_label_set_text(item->value, text ? text : "");
	lv_obj_set_hidden(item->value, !text || !text[0]);
	return appended(form, item);
}

static void keep_focus_visible(struct fplinux_ui_screen *form);

/* An empty text takes no space, except in the box of a link or button. */
static void show_string(struct form_item *string)
{
	lv_obj_set_hidden(string->value, !lv_label_get_text(string->value)[0] &&
						 !actionable(string));
}

/* A new text above the focus moves it: the form keeps it in view. */
bool fplinux_ui_form_set_string(struct fplinux_ui_screen *form, size_t item,
				const char *text)
{
	struct form_item *string = form_item(form, item, ITEM_STRING);

	if (!string) {
		errno = EINVAL;
		return false;
	}
	lv_label_set_text(string->value, text ? text : "");
	show_string(string);
	/* A focused link or button keeps its new text in view too. */
	if (item <= form->focus)
		keep_focus_visible(form);
	return true;
}

/* Text items */

/* Reports a changed text once the text field's own callback is over. */
static void report_text(void *data)
{
	struct form_item *item = data;
	const char *text = fplinux_ui_text_get(item->field);
	char *copy;

	item->report_pending = false;
	if (!strcmp(text, item->reported) || !(copy = strdup(text)))
		return;
	free(item->reported);
	item->reported = copy;
	report(item->form, item->index);
}

static void text_changed(struct fplinux_ui_text *field, void *context)
{
	struct form_item *item = context;

	changed(item->form);
	if (item->quiet || item->report_pending ||
	    !strcmp(fplinux_ui_text_get(field), item->reported))
		return;
	if (lv_async_call(report_text, item) == LV_RESULT_OK)
		item->report_pending = true;
}

/*
 * Sets the text without reporting it as the user's change. The field may
 * store a DECIMAL separator of its own, which is one byte like the given one.
 */
static bool set_field_text(struct form_item *item, const char *text)
{
	size_t size = strlen(text) + 1;
	char *copy = malloc(size);
	bool done;

	if (!copy)
		return false;
	item->quiet = true;
	done = fplinux_ui_text_set(item->field, text);
	item->quiet = false;
	if (!done) {
		free(copy);
		return false;
	}
	snprintf(copy, size, "%s", fplinux_ui_text_get(item->field));
	free(item->reported);
	item->reported = copy;
	return true;
}

static bool make_field(struct form_item *item, lv_obj_t *parent, bool multiline,
		       size_t max_chars, const char *text)
{
	item->field = fplinux_ui_text_create(item->form->ui, parent, multiline,
					     max_chars);
	if (!item->field)
		return false;
	lv_obj_set_width(fplinux_ui_text_object(item->field), lv_pct(100));
	/* Unfocused first, so that a read-only field takes no marquee. */
	fplinux_ui_text_set_focused(item->field, false);
	if (!fplinux_ui_text_set_constraint(item->field, item->constraint))
		return false;
	if (!set_field_text(item, text ? text : ""))
		return false;
	fplinux_ui_text_set_change_cb(item->field, text_changed, item);
	return true;
}

size_t fplinux_ui_form_append_text_field(
	struct fplinux_ui_screen *form, const char *label, const char *text,
	size_t max_chars, enum fplinux_ui_text_constraint constraint)
{
	struct form_item *item;

	if (!is_form(form)) {
		errno = EINVAL;
		return NONE;
	}
	item = add_item(form, ITEM_TEXT, label);
	if (!item)
		return NONE;
	item->constraint = constraint;
	if (!make_field(item, item->box, false, max_chars, text))
		return drop_item(form, errno);
	return appended(form, item);
}

/* A DECIMAL field, whatever its PASSWORD and UNEDITABLE modifiers. */
static bool is_decimal(const struct form_item *item)
{
	return ((unsigned int)item->constraint &
		~(unsigned int)(FPLINUX_UI_TEXT_PASSWORD |
				FPLINUX_UI_TEXT_UNEDITABLE)) ==
	       FPLINUX_UI_TEXT_DECIMAL;
}

/* A DECIMAL field's number with '.', or as shown without memory. */
static const char *decimal_text(struct form_item *item)
{
	const char *text = fplinux_ui_text_get(item->field);
	char *copy = strdup(text);
	char *separator;

	if (!copy)
		return text;
	separator = strchr(copy, ',');
	if (separator)
		*separator = '.';
	free(item->content);
	item->content = copy;
	return copy;
}

/* A string item's text or a text field's text. */
const char *fplinux_ui_form_get_text(const struct fplinux_ui_screen *form,
				     size_t item)
{
	struct form_item *field = form_item(form, item, ITEM_TEXT);
	struct form_item *string = form_item(form, item, ITEM_STRING);

	if (field)
		return is_decimal(field) ? decimal_text(field) :
					   fplinux_ui_text_get(field->field);
	return string ? lv_label_get_text(string->value) : NULL;
}

bool fplinux_ui_form_set_text(struct fplinux_ui_screen *form, size_t item,
			      const char *text)
{
	struct form_item *field = form_item(form, item, ITEM_TEXT);

	if (!field || !text) {
		errno = EINVAL;
		return false;
	}
	if (!set_field_text(field, text))
		return false;
	changed(form);
	return true;
}

/* Choices */

static void choice_selected(struct fplinux_ui_listview *list, size_t index,
			    void *context)
{
	struct form_item *item = context;

	(void)list;
	(void)index;
	changed(item->form);
}

static bool copy_choices(struct form_item *item, const char *const *choices,
			 size_t count)
{
	item->choices = calloc(count, sizeof(*item->choices));
	if (!item->choices)
		return false;
	for (; item->count < count; ++item->count)
		if (!(item->choices[item->count] =
			      strdup(choices[item->count])))
			return false;
	return true;
}

/*
 * Rows of the list component without list padding; a popup choice is one
 * row with its value at the right ("label ... value").
 */
static bool make_choice_list(struct form_item *item, size_t rows,
			     enum fplinux_ui_listview_marker marker, bool popup)
{
	lv_obj_t *holder = plain_object(item->box);

	lv_obj_set_size(holder, lv_pct(100),
			(int32_t)rows * spec_of(item->form)->row_height);
	item->list = fplinux_ui_listview_create(item->form->ui, holder, marker);
	if (!item->list)
		return false;
	fplinux_ui_listview_set_inline_detail(item->list, popup);
	lv_obj_set_style_pad_ver(fplinux_ui_listview_object(item->list), 0, 0);
	fplinux_ui_listview_set_selectable(item->list, false);
	fplinux_ui_listview_set_callbacks(item->list, choice_selected, NULL,
					  item);
	return true;
}

size_t fplinux_ui_form_append_choice(struct fplinux_ui_screen *form,
				     const char *label,
				     enum fplinux_ui_choice_kind kind,
				     const char *const *choices, size_t count)
{
	bool popup = kind == FPLINUX_UI_CHOICE_POPUP;
	struct form_item *item;

	if (!is_form(form) || (unsigned int)kind > FPLINUX_UI_CHOICE_POPUP ||
	    !choices || !count) {
		errno = EINVAL;
		return NONE;
	}
	for (size_t i = 0; i < count; ++i)
		if (!choices[i]) {
			errno = EINVAL;
			return NONE;
		}
	item = add_item(form, ITEM_CHOICE, popup ? NULL : label);
	if (!item)
		return NONE;
	item->choice_kind = kind;
	/* The rows bring their own insets; the label keeps the form's. */
	lv_obj_set_style_pad_hor(item->box, 0, 0);
	if (item->label) {
		lv_obj_set_style_pad_left(item->label, spec_of(form)->margin,
					  0);
		lv_obj_set_style_pad_right(item->label, spec_of(form)->gutter,
					   0);
	}
	if (!copy_choices(item, choices, count) ||
	    (popup && label && !(item->name = strdup(label))) ||
	    !make_choice_list(item, popup ? 1 : count,
			      kind == FPLINUX_UI_CHOICE_MULTIPLE ?
				      FPLINUX_UI_LISTVIEW_CHECK :
			      popup ? FPLINUX_UI_LISTVIEW_PLAIN :
				      FPLINUX_UI_LISTVIEW_RADIO,
			      popup))
		return drop_item(form, errno ? errno : ENOMEM);
	if (popup) {
		fplinux_ui_listview_add(item->list, label ? label : "",
					choices[0]);
	} else {
		for (size_t i = 0; i < count; ++i)
			fplinux_ui_listview_add(item->list, choices[i], NULL);
		if (kind == FPLINUX_UI_CHOICE_EXCLUSIVE)
			fplinux_ui_listview_set_marked(item->list, 0, true);
	}
	return appended(form, item);
}

static void set_chosen(struct form_item *item, size_t index)
{
	item->chosen = index;
	if (item->choice_kind == FPLINUX_UI_CHOICE_POPUP)
		fplinux_ui_listview_set_text(item->list, 0,
					     item->name ? item->name : "",
					     item->choices[index]);
	else
		fplinux_ui_listview_set_marked(item->list, index, true);
}

size_t fplinux_ui_form_get_choice(const struct fplinux_ui_screen *form,
				  size_t item)
{
	struct form_item *choice = form_item(form, item, ITEM_CHOICE);

	if (!choice || choice->choice_kind == FPLINUX_UI_CHOICE_MULTIPLE)
		return NONE;
	return choice->chosen;
}

void fplinux_ui_form_set_choice(struct fplinux_ui_screen *form, size_t item,
				size_t index)
{
	struct form_item *choice = form_item(form, item, ITEM_CHOICE);

	if (!choice || choice->choice_kind == FPLINUX_UI_CHOICE_MULTIPLE ||
	    index >= choice->count)
		return;
	set_chosen(choice, index);
	changed(form);
}

bool fplinux_ui_form_is_chosen(const struct fplinux_ui_screen *form,
			       size_t item, size_t index)
{
	struct form_item *choice = form_item(form, item, ITEM_CHOICE);

	if (!choice || index >= choice->count)
		return false;
	if (choice->choice_kind == FPLINUX_UI_CHOICE_MULTIPLE)
		return fplinux_ui_listview_is_marked(choice->list, index);
	return choice->chosen == index;
}

void fplinux_ui_form_set_chosen(struct fplinux_ui_screen *form, size_t item,
				size_t index, bool chosen)
{
	struct form_item *choice = form_item(form, item, ITEM_CHOICE);

	if (!choice || index >= choice->count)
		return;
	if (choice->choice_kind == FPLINUX_UI_CHOICE_MULTIPLE)
		fplinux_ui_listview_set_marked(choice->list, index, chosen);
	else if (chosen)
		set_chosen(choice, index);
	changed(form);
}

/* The popup's list: Back leaves it, OK on a row chooses and returns. */
static void popup_command(struct fplinux_ui *ui,
			  struct fplinux_ui_screen *screen, int id,
			  void *context)
{
	(void)screen;
	(void)context;
	if (id == POPUP_BACK_COMMAND)
		fplinux_ui_pop(ui);
}

static void popup_changed(struct fplinux_ui *ui,
			  struct fplinux_ui_screen *screen, size_t index,
			  void *context)
{
	struct form_item *item = context;

	(void)screen;
	fplinux_ui_pop(ui);
	/* The application deleted the item (detach_popup()). */
	if (!item)
		return;
	set_chosen(item, index);
	changed(item->form);
	report(item->form, item->index);
}

/* A popup choice is a framed list over its retained, dimmed form. */
/* The box holds count rows, at most the surface less the alert margins. */
static void size_popup_choice(struct fplinux_ui_screen *list, size_t count)
{
	const struct fplinux_ui_profile_spec *spec = spec_of(list);
	int32_t height =
		fplinux_ui_modal_height(list->ui) - 2 * spec->alert_margin;
	int32_t title = list->title ? spec->header_height : 0;
	int32_t inset = 2 * (spec->alert_border + spec->list_padding);

	if (count < (size_t)((height - title - inset) / spec->row_height))
		height = title + inset + (int32_t)count * spec->row_height;
	lv_obj_set_size(list->box,
			fplinux_ui_width(list->ui) - 2 * spec->alert_margin,
			height);
}

static void frame_popup_choice(struct fplinux_ui_screen *list, size_t count)
{
	const struct fplinux_ui_profile_spec *spec = spec_of(list);
	const struct fplinux_ui_theme *theme = fplinux_ui_theme(list->ui);

	list->popup_choice = true;
	lv_obj_set_height(list->root, root_height(list));
	fplinux_ui_scrim_style(list->ui, list->root);
	list->box = plain_object(list->root);
	size_popup_choice(list, count);
	lv_obj_set_style_bg_color(list->box, theme->popup, 0);
	lv_obj_set_style_bg_opa(list->box, LV_OPA_COVER, 0);
	lv_obj_set_style_border_color(list->box, theme->focus, 0);
	lv_obj_set_style_border_width(list->box, spec->alert_border, 0);
	lv_obj_center(list->box);
	lv_obj_set_parent(list->band, list->box);
	lv_obj_set_parent(list->content, list->box);
	lv_obj_set_style_bg_color(list->band, theme->popup, 0);
	layout_frame(list);
}

static void open_popup(struct fplinux_ui_screen *form, struct form_item *item)
{
	static const struct fplinux_ui_command back = {
		.id = POPUP_BACK_COMMAND,
		.type = FPLINUX_UI_COMMAND_BACK,
	};
	struct fplinux_ui_screen *list = fplinux_ui_list_new(
		form->ui, item->name ? item->name : form->title,
		FPLINUX_UI_LIST_EXCLUSIVE);

	if (!list)
		return;
	frame_popup_choice(list, item->count);
	for (size_t i = 0; i < item->count; ++i)
		fplinux_ui_listview_add(list->list, item->choices[i], NULL);
	fplinux_ui_listview_set_marked(list->list, item->chosen, true);
	fplinux_ui_listview_select(list->list, item->chosen);
	if (!add_command(list, &back)) {
		delete_screen(list);
		return;
	}
	fplinux_ui_screen_set_listener(list, popup_command, popup_changed,
				       item);
	fplinux_ui_push(form->ui, list);
}

/* Gauges */

static void show_number(struct form_item *gauge)
{
	lv_label_set_text_fmt(gauge->value, "%d", gauge->number);
	lv_bar_set_value(gauge->bar, gauge->number, LV_ANIM_OFF);
}

/* The bar of a gauge or a busy alert: a border track and a focus fill. */
static lv_obj_t *make_bar(lv_obj_t *parent, struct fplinux_ui *ui)
{
	const struct fplinux_ui_theme *theme = fplinux_ui_theme(ui);
	lv_obj_t *bar = lv_bar_create(parent);

	lv_obj_remove_style_all(bar);
	lv_obj_set_size(bar, lv_pct(100), fplinux_ui_spec(ui)->bar_height);
	lv_obj_set_style_bg_color(bar, theme->border, LV_PART_MAIN);
	lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
	lv_obj_set_style_bg_color(bar, theme->focus, LV_PART_INDICATOR);
	lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
	return bar;
}

static lv_obj_t *make_frame(struct form_item *item, bool border)
{
	const struct fplinux_ui_theme *theme = fplinux_ui_theme(item->form->ui);
	const struct fplinux_ui_profile_spec *spec = spec_of(item->form);
	lv_obj_t *frame = plain_object(item->box);

	lv_obj_set_size(frame, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(frame, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_all(frame, spec->frame_padding, 0);
	lv_obj_set_style_pad_row(frame, spec->gap_small, 0);
	lv_obj_set_style_bg_color(frame, theme->surface, 0);
	lv_obj_set_style_bg_opa(frame, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(frame, spec->focus_width, 0);
	lv_obj_set_style_border_color(frame, theme->border, 0);
	lv_obj_set_style_border_opa(frame,
				    border ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
	return frame;
}

size_t fplinux_ui_form_append_gauge(struct fplinux_ui_screen *form,
				    const char *label, int minimum, int maximum,
				    int value, bool interactive)
{
	const struct fplinux_ui_theme *theme;
	struct form_item *item;
	lv_obj_t *heading;

	if (!is_form(form) || minimum >= maximum) {
		errno = EINVAL;
		return NONE;
	}
	item = add_item(form, ITEM_GAUGE, NULL);
	if (!item)
		return NONE;
	theme = fplinux_ui_theme(form->ui);
	item->minimum = minimum;
	item->maximum = maximum;
	item->number = value < minimum ? minimum :
		       value > maximum ? maximum :
					 value;
	item->interactive = interactive;
	item->frame = make_frame(item, interactive);
	heading = plain_object(item->frame);
	lv_obj_set_size(heading, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(heading, LV_FLEX_FLOW_ROW);
	lv_obj_set_style_pad_column(heading, spec_of(form)->spacing, 0);
	item->gauge_name = lv_label_create(heading);
	lv_obj_set_flex_grow(item->gauge_name, 1);
	/* One line high, so that a longer name ends with dots. */
	lv_obj_set_height(item->gauge_name,
			  lv_font_get_line_height(theme->font));
	lv_label_set_long_mode(item->gauge_name, LV_LABEL_LONG_MODE_DOTS);
	lv_label_set_text(item->gauge_name, label ? label : "");
	item->value = lv_label_create(heading);
	lv_obj_set_style_text_color(item->value, theme->muted, 0);
	item->bar = make_bar(item->frame, form->ui);
	lv_bar_set_range(item->bar, minimum, maximum);
	show_number(item);
	return appended(form, item);
}

int fplinux_ui_form_get_value(const struct fplinux_ui_screen *form, size_t item)
{
	struct form_item *gauge = form_item(form, item, ITEM_GAUGE);

	return gauge ? gauge->number : 0;
}

void fplinux_ui_form_set_value(struct fplinux_ui_screen *form, size_t item,
			       int value)
{
	struct form_item *gauge = form_item(form, item, ITEM_GAUGE);

	if (!gauge)
		return;
	gauge->number = value < gauge->minimum ? gauge->minimum :
			value > gauge->maximum ? gauge->maximum :
						 value;
	show_number(gauge);
}

/* One press moves a short range by 1 and a longer one by a twentieth. */
static void gauge_step(struct fplinux_ui_screen *form, struct form_item *gauge,
		       int direction)
{
	long range = (long)gauge->maximum - gauge->minimum;
	long step = range <= 20 ? 1 : range / 20;
	long number = gauge->number + direction * step;

	if (number < gauge->minimum)
		number = gauge->minimum;
	if (number > gauge->maximum)
		number = gauge->maximum;
	if (number == gauge->number)
		return;
	gauge->number = (int)number;
	show_number(gauge);
	report(form, gauge->index);
}

/* Dates */

/*
 * The short date pattern that fplinux_ui_format_date() follows, with its
 * fallback to English, so that the editor shows the parts in the same order.
 * It is read again when the system language changes.
 */
static const char *date_pattern(struct fplinux_ui_screens *layer)
{
	const char *code = fplinux_ui_locale(layer->ui);
	struct fplinux_language language;

	if (layer->date_language[0] && !strcmp(layer->date_language, code))
		return layer->date_pattern;
	if (!fplinux_language_load(FPLINUX_LANGUAGE_DIRECTORY, code,
				   &language) ||
	    !language.system)
		fplinux_language_english(&language);
	snprintf(layer->date_pattern, sizeof(layer->date_pattern), "%s",
		 language.date_short);
	snprintf(layer->date_language, sizeof(layer->date_language), "%s",
		 code);
	return layer->date_pattern;
}

static void begin_date_edit(struct form_item *item,
			    struct fplinux_ui_date_edit *edit)
{
	fplinux_ui_date_edit_begin(edit, item->date,
				   item->date_mode != FPLINUX_UI_TIME,
				   item->date_mode != FPLINUX_UI_DATE,
				   date_pattern(item->form->layer));
}

/* The value in the system language: short date, clock or both. */
static bool format_date_value(const struct form_item *item, char *text,
			      size_t size)
{
	struct fplinux_ui *ui = item->form->ui;
	size_t length;

	if (item->date_mode == FPLINUX_UI_TIME)
		return fplinux_ui_format_date(
			ui, item->date, FPLINUX_UI_DATE_CLOCK, text, size);
	if (!fplinux_ui_format_date(ui, item->date, FPLINUX_UI_DATE_SHORT, text,
				    size))
		return false;
	if (item->date_mode == FPLINUX_UI_DATE)
		return true;
	length = strlen(text);
	if (length + 1 >= size)
		return false;
	text[length++] = ' ';
	return fplinux_ui_format_date(ui, item->date, FPLINUX_UI_DATE_CLOCK,
				      text + length, size - length);
}

/*
 * The localized value, or while it is edited the editor's numbers in the
 * same order with the focused part selected.
 */
static void show_date(struct form_item *item,
		      const struct fplinux_ui_date_edit *edit)
{
	struct fplinux_ui_date_edit view;
	char text[DATE_VALUE_BYTES];
	size_t start = LV_DRAW_LABEL_NO_TXT_SEL;
	size_t end = LV_DRAW_LABEL_NO_TXT_SEL;

	if (edit) {
		fplinux_ui_date_edit_format(edit, text, sizeof(text), &start,
					    &end);
	} else if (!format_date_value(item, text, sizeof(text))) {
		size_t unused;

		begin_date_edit(item, &view);
		fplinux_ui_date_edit_format(&view, text, sizeof(text), &unused,
					    &unused);
	}
	lv_label_set_text(item->value, text);
	lv_label_set_text_selection_start(item->value, (uint32_t)start);
	lv_label_set_text_selection_end(item->value, (uint32_t)end);
	fplinux_ui_marquee_changed(item->form->ui, item->value, NULL);
}

size_t fplinux_ui_form_append_date(struct fplinux_ui_screen *form,
				   const char *label,
				   enum fplinux_ui_date_mode mode, time_t value)
{
	const struct fplinux_ui_theme *theme;
	struct form_item *item;

	if (!is_form(form) || (unsigned int)mode > FPLINUX_UI_DATE_TIME) {
		errno = EINVAL;
		return NONE;
	}
	item = add_item(form, ITEM_DATE, label);
	if (!item)
		return NONE;
	theme = fplinux_ui_theme(form->ui);
	item->date_mode = mode;
	item->date = value;
	item->frame = make_frame(item, true);
	item->value = lv_label_create(item->frame);
	/* One line, so that a longer value ends with dots or scrolls. */
	lv_obj_set_size(item->value, lv_pct(100),
			lv_font_get_line_height(theme->font));
	lv_label_set_long_mode(item->value, LV_LABEL_LONG_MODE_DOTS);
	lv_obj_set_style_bg_color(item->value, theme->accent, LV_PART_SELECTED);
	lv_obj_set_style_text_color(item->value, theme->accent_text,
				    LV_PART_SELECTED);
	show_date(item, NULL);
	return appended(form, item);
}

time_t fplinux_ui_form_get_date(const struct fplinux_ui_screen *form,
				size_t item)
{
	struct form_item *date = form_item(form, item, ITEM_DATE);

	return date ? date->date : (time_t)-1;
}

void fplinux_ui_form_set_date(struct fplinux_ui_screen *form, size_t item,
			      time_t value)
{
	struct form_item *date = form_item(form, item, ITEM_DATE);

	if (!date)
		return;
	date->date = value;
	if (!(form->editing && form->focus == item))
		show_date(date, NULL);
}

/* Images */

/* No wider than the form's items and no taller than its view. */
static void image_limits(const struct fplinux_ui_screen *form, bool label,
			 int32_t *width, int32_t *height)
{
	const struct fplinux_ui_profile_spec *spec = spec_of(form);

	*width = fplinux_ui_width(form->ui) - spec->margin - spec->gutter;
	*height = screen_height(form) - 2 * spec->content_padding -
		  (form->title ? spec->header_height : 0) -
		  (label ? lv_font_get_line_height(
				   fplinux_ui_theme(form->ui)->font) +
				   spec->gap_small :
			   0);
}

size_t fplinux_ui_form_append_image(struct fplinux_ui_screen *form,
				    const char *label, const void *src)
{
	const struct fplinux_ui_theme *theme;
	struct form_item *item;
	lv_draw_buf_t *buffer;
	int32_t width;
	int32_t height;

	if (!is_form(form) || !src) {
		errno = EINVAL;
		return NONE;
	}
	theme = fplinux_ui_theme(form->ui);
	image_limits(form, label != NULL, &width, &height);
	buffer = fplinux_ui_image_decode(src, width, height, false);
	if (!buffer)
		return NONE;
	item = add_item(form, ITEM_IMAGE, label);
	if (!item) {
		lv_draw_buf_destroy(buffer);
		return NONE;
	}
	lv_obj_set_flex_align(item->box, LV_FLEX_ALIGN_START,
			      LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	item->value = lv_image_create(item->box);
	/*
	 * An alpha-only image (a glyph, a "-symbolic.png" file) is drawn in
	 * the form's text colour, as row icons are; without a recolour
	 * opacity a colour image is unchanged.
	 */
	lv_obj_set_style_image_recolor(item->value, theme->foreground, 0);
	lv_image_set_src(item->value, buffer);
	lv_obj_add_event_cb(item->value, image_delete_cb, LV_EVENT_DELETE,
			    buffer);
	item->image = buffer;
	return appended(form, item);
}

static int key_digit(const struct fplinux_ui_key *key)
{
	if (key->source == FPLINUX_UI_KEY_PHONE)
		return key->code >= KEY_NUMERIC_0 &&
				       key->code <= KEY_NUMERIC_9 ?
			       (int)(key->code - KEY_NUMERIC_0) :
			       -1;
	return key->text[0] >= '0' && key->text[0] <= '9' && !key->text[1] ?
		       key->text[0] - '0' :
		       -1;
}

static bool is_ok_key(const struct fplinux_ui_key *key)
{
	return key->code == KEY_OK || key->code == KEY_ENTER ||
	       key->code == KEY_KPENTER;
}

static bool is_right_key(const struct fplinux_ui_key *key)
{
	return fplinux_ui_is_softkey(key, false) || key->code == KEY_ESC;
}

static bool is_left_key(const struct fplinux_ui_key *key)
{
	return fplinux_ui_is_softkey(key, true) ||
	       (key->source == FPLINUX_UI_KEY_KEYBOARD &&
		key->code == KEY_COMPOSE);
}

/*
 * While a date is edited: Left and Right move between parts, Up adds 1 to
 * the part and Down subtracts 1, digits type, OK keeps the value and Cancel
 * restores it.
 */
static void date_key(struct fplinux_ui_screen *form,
		     const struct fplinux_ui_key *key)
{
	struct form_item *item = form->items[form->focus];
	int digit = key_digit(key);
	time_t before = item->date;

	if (key->code == KEY_LEFT || key->code == KEY_RIGHT)
		fplinux_ui_date_edit_move(&form->edit,
					  key->code == KEY_LEFT ? -1 : 1);
	else if (key->code == KEY_UP || key->code == KEY_DOWN)
		fplinux_ui_date_edit_step(&form->edit,
					  key->code == KEY_UP ? 1 : -1);
	else if (digit >= 0)
		fplinux_ui_date_edit_digit(&form->edit, (unsigned int)digit);
	else if (!is_ok_key(key) && !is_right_key(key))
		return;
	if (!is_ok_key(key) && !is_right_key(key)) {
		show_date(item, &form->edit);
		return;
	}
	form->editing = false;
	if (is_ok_key(key))
		item->date = fplinux_ui_date_edit_value(&form->edit);
	show_date(item, NULL);
	refresh(form->layer);
	if (item->date != before)
		report(form, item->index);
}

void fplinux_ui_form_set_focus(struct fplinux_ui_screen *form, size_t item)
{
	if (!is_form(form) || item >= form->item_count ||
	    !focusable(form->items[item]) || form->editing)
		return;
	form->settled = true;
	form_focus(form, item, ENTER_KEEP);
	changed(form);
}

/*
 * NONE until the form first shows an item that takes the focus or the
 * application sets it, and after the focus scrolled out of sight.
 */
size_t fplinux_ui_form_get_focus(const struct fplinux_ui_screen *form)
{
	if (!is_form(form)) {
		errno = EINVAL;
		return NONE;
	}
	return form->focus;
}

/* Item order: insert, delete, hide */

static bool focusable_at(size_t index, void *context)
{
	const struct fplinux_ui_screen *form = context;

	return focusable(form->items[index]);
}

/*
 * The focused item at from can no longer have the focus: the next item
 * that can takes it, else the previous one.
 */
static void pass_focus(struct fplinux_ui_screen *form, size_t from)
{
	size_t next = fplinux_ui_focus_next(from, form->item_count,
					    focusable_at, form);

	form->editing = false;
	form->focus = NONE;
	if (next != NONE)
		form_focus(form, next, ENTER_TOP);
	else
		form_apply_focus(form, is_top(form));
}

/* Items above the focus changed: the form scrolls to keep it in view. */
static void keep_focus_visible(struct fplinux_ui_screen *form)
{
	lv_area_t area;

	if (form->focus == NONE)
		return;
	lv_obj_update_layout(form->content);
	item_area(form->items[form->focus], &area);
	reveal(form->content, &area);
}

/* Items after a change take the indices of their places. */
static void renumber(struct fplinux_ui_screen *form, size_t from)
{
	for (size_t i = from; i < form->item_count; ++i)
		form->items[i]->index = i;
}

/* Where an index goes when the item at from moves to to. */
static size_t moved_index(size_t index, size_t from, size_t to)
{
	if (index == from)
		return to;
	if (from < to && index > from && index <= to)
		return index - 1;
	if (to < from && index >= to && index < from)
		return index + 1;
	return index;
}

bool fplinux_ui_form_move_item(struct fplinux_ui_screen *form, size_t from,
			       size_t to)
{
	struct form_item *item;

	if (!is_form(form) || from >= form->item_count ||
	    to >= form->item_count) {
		errno = EINVAL;
		return false;
	}
	if (from == to)
		return true;
	item = form->items[from];
	if (from < to)
		memmove(&form->items[from], &form->items[from + 1],
			(to - from) * sizeof(*form->items));
	else
		memmove(&form->items[to + 1], &form->items[to],
			(from - to) * sizeof(*form->items));
	form->items[to] = item;
	lv_obj_move_to_index(item->box, (int32_t)to);
	renumber(form, from < to ? from : to);
	if (form->focus != NONE)
		form->focus = moved_index(form->focus, from, to);
	for (size_t i = 0; i < form->item_command_count; ++i)
		form->item_commands[i].index =
			moved_index(form->item_commands[i].index, from, to);
	menu_rows_moved(form, moved_index, from, to);
	keep_focus_visible(form);
	changed(form);
	return true;
}

bool fplinux_ui_form_delete(struct fplinux_ui_screen *form, size_t item)
{
	struct form_item *deleted;
	bool focused;

	if (!is_form(form) || item >= form->item_count) {
		errno = EINVAL;
		return false;
	}
	deleted = form->items[item];
	focused = form->focus == item;
	--form->item_count;
	memmove(&form->items[item], &form->items[item + 1],
		(form->item_count - item) * sizeof(*form->items));
	lv_obj_delete(deleted->box);
	free_item(deleted);
	renumber(form, item);
	item_commands_deleted(form, item, 1);
	if (focused) {
		pass_focus(form, item);
	} else {
		if (form->focus != NONE && form->focus > item)
			--form->focus;
		keep_focus_visible(form);
	}
	changed(form);
	return true;
}

void fplinux_ui_form_delete_all(struct fplinux_ui_screen *form)
{
	if (!is_form(form))
		return;
	while (form->item_count) {
		struct form_item *item = form->items[--form->item_count];

		lv_obj_delete(item->box);
		free_item(item);
	}
	clear_item_commands(form);
	form->focus = NONE;
	form->editing = false;
	form->settled = false;
	lv_obj_scroll_to_y(form->content, 0, LV_ANIM_OFF);
	changed(form);
}

bool fplinux_ui_form_set_hidden(struct fplinux_ui_screen *form, size_t item,
				bool hidden)
{
	struct form_item *target;

	if (!is_form(form) || item >= form->item_count) {
		errno = EINVAL;
		return false;
	}
	target = form->items[item];
	if (target->hidden == hidden)
		return true;
	if (hidden && form->editing && form->focus == item)
		show_date(target, NULL);
	target->hidden = hidden;
	lv_obj_set_hidden(target->box, hidden);
	if (hidden && form->focus == item)
		pass_focus(form, item);
	else
		keep_focus_visible(form);
	changed(form);
	return true;
}

bool fplinux_ui_form_is_hidden(const struct fplinux_ui_screen *form,
			       size_t item)
{
	return is_form(form) && item < form->item_count &&
	       form->items[item]->hidden;
}

bool fplinux_ui_form_set_constraint(struct fplinux_ui_screen *form, size_t item,
				    enum fplinux_ui_text_constraint constraint)
{
	struct form_item *field = form_item(form, item, ITEM_TEXT);

	if (!field) {
		errno = EINVAL;
		return false;
	}
	if (!fplinux_ui_text_set_constraint(field->field, constraint))
		return false;
	field->constraint = constraint;
	changed(form);
	return true;
}

/* String and image items shown as links or buttons */

/*
 * The picture that fits inside the box of a link or button: the decoded
 * one, or once a copy scaled down to the box's inside. Without memory for
 * the copy, the box clips the picture.
 */
static const lv_draw_buf_t *boxed_image(struct form_item *item)
{
	const struct fplinux_ui_profile_spec *spec = spec_of(item->form);
	int32_t inset = 2 * (spec->focus_width + spec->frame_padding);
	int32_t width;
	int32_t height;

	if (item->fitted)
		return item->fitted;
	image_limits(item->form, item->label != NULL, &width, &height);
	width -= inset;
	height -= inset;
	if (width <= 0 || height <= 0 ||
	    ((int32_t)item->image->header.w <= width &&
	     (int32_t)item->image->header.h <= height))
		return item->image;
	item->fitted =
		fplinux_ui_image_decode(item->image, width, height, false);
	return item->fitted ? item->fitted : item->image;
}

/*
 * A link or button with item commands shows its value in a box with a
 * frame of focus_width and frame_padding inside. A
 * link has no visible frame: its text is underlined in the accent colour,
 * and an image has an accent line below it. A button is a surface box in a
 * border frame with its text centred. Without commands, or PLAIN, the item
 * is plain content again. Returns whether the item's size changed.
 */
static bool apply_appearance(struct form_item *item)
{
	const struct fplinux_ui_profile_spec *spec = spec_of(item->form);
	const struct fplinux_ui_theme *theme = fplinux_ui_theme(item->form->ui);
	bool link = item->appearance == FPLINUX_UI_APPEARANCE_HYPERLINK;
	bool image = item->kind == ITEM_IMAGE;
	bool created = false;
	lv_obj_t *frame;

	if (!actionable(item)) {
		if ((item->kind != ITEM_STRING && !image) || !item->frame)
			return false;
		lv_obj_set_parent(item->value, item->box);
		lv_obj_delete(item->frame);
		item->frame = NULL;
		link = false;
	} else {
		if (!item->frame) {
			item->frame = plain_object(item->box);
			lv_obj_set_parent(item->value, item->frame);
			created = true;
		}
		frame = item->frame;
		lv_obj_set_size(frame, image ? LV_SIZE_CONTENT : lv_pct(100),
				LV_SIZE_CONTENT);
		lv_obj_set_style_bg_color(
			frame, link ? theme->accent : theme->surface, 0);
		lv_obj_set_style_bg_opa(frame, LV_OPA_COVER, 0);
		lv_obj_set_style_border_width(frame, spec->focus_width, 0);
		lv_obj_set_style_border_color(
			frame, link ? theme->accent : theme->border, 0);
		lv_obj_set_style_border_opa(
			frame, link && !image ? LV_OPA_TRANSP : LV_OPA_COVER,
			0);
		/* The image's line takes a frame's place: the same size. */
		lv_obj_set_style_border_side(frame,
					     link && image ?
						     LV_BORDER_SIDE_BOTTOM :
						     LV_BORDER_SIDE_FULL,
					     0);
		lv_obj_set_style_pad_all(
			frame,
			spec->frame_padding +
				(link && image ? spec->focus_width : 0),
			0);
		lv_obj_set_style_pad_bottom(frame, spec->frame_padding, 0);
	}
	if (image) {
		lv_image_set_src(item->value,
				 item->frame ? boxed_image(item) : item->image);
		lv_obj_set_style_image_recolor(
			item->value, link ? theme->accent : theme->foreground,
			0);
	} else {
		lv_obj_set_style_text_color(
			item->value, link ? theme->accent : theme->foreground,
			0);
		lv_obj_set_style_text_decor(
			item->value,
			link ? LV_TEXT_DECOR_UNDERLINE : LV_TEXT_DECOR_NONE, 0);
		lv_obj_set_style_text_align(item->value,
					    item->frame && !link ?
						    LV_TEXT_ALIGN_CENTER :
						    LV_TEXT_ALIGN_AUTO,
					    0);
		show_string(item);
	}
	if (item->frame)
		show_appearance_focus(item,
				      is_top(item->form) &&
					      item->form->focus == item->index);
	return created || !item->frame;
}

/*
 * A form item's commands or appearance changed: a link or button looks
 * like one while it has item commands, and when it can no longer take the
 * focus, passes it on as a deleted item does.
 */
static void update_appearance(struct fplinux_ui_screen *screen, size_t index)
{
	struct form_item *item;

	if (!is_form(screen))
		return;
	item = screen->items[index];
	if (!apply_appearance(item))
		return;
	if (screen->focus == index && !focusable(item))
		pass_focus(screen, index);
	else
		keep_focus_visible(screen);
}

bool fplinux_ui_form_set_appearance(struct fplinux_ui_screen *form, size_t item,
				    enum fplinux_ui_appearance appearance)
{
	struct form_item *target;

	if (!is_form(form) || item >= form->item_count ||
	    (form->items[item]->kind != ITEM_STRING &&
	     form->items[item]->kind != ITEM_IMAGE) ||
	    (unsigned int)appearance > FPLINUX_UI_APPEARANCE_BUTTON) {
		errno = EINVAL;
		return false;
	}
	target = form->items[item];
	if (target->appearance == appearance)
		return true;
	target->appearance = appearance;
	update_appearance(form, item);
	changed(form);
	return true;
}

/* OK on the focused item. */
static void form_ok(struct fplinux_ui_screen *form)
{
	struct form_item *item = focused_item(form);
	size_t row;

	if (item->kind == ITEM_DATE) {
		begin_date_edit(item, &form->edit);
		form->editing = true;
		show_date(item, &form->edit);
		return;
	}
	if (item->choice_kind == FPLINUX_UI_CHOICE_POPUP) {
		open_popup(form, item);
		return;
	}
	row = fplinux_ui_listview_selected(item->list);
	if (item->choice_kind == FPLINUX_UI_CHOICE_EXCLUSIVE)
		set_chosen(item, row);
	else
		fplinux_ui_listview_set_marked(
			item->list, row,
			!fplinux_ui_listview_is_marked(item->list, row));
	report(form, item->index);
}

static void form_key(struct fplinux_ui_screen *form,
		     const struct fplinux_ui_key *key)
{
	struct form_item *item = focused_item(form);

	if (key->code == KEY_UP || key->code == KEY_DOWN)
		form_move(form, key->code == KEY_DOWN);
	else if ((key->code == KEY_LEFT || key->code == KEY_RIGHT) && item &&
		 item->kind == ITEM_GAUGE)
		gauge_step(form, item, key->code == KEY_LEFT ? -1 : 1);
}

/* Text boxes */

struct fplinux_ui_screen *fplinux_ui_text_box_new(struct fplinux_ui *ui,
						  const char *title,
						  const char *text,
						  size_t max_chars)
{
	struct fplinux_ui_screen *box = screen_new(ui, SCREEN_TEXT_BOX, title);
	struct form_item *item;
	int error;

	if (!box)
		return NULL;
	box->items = calloc(1, sizeof(*box->items));
	item = calloc(1, sizeof(*item));
	if (!box->items || !item) {
		free(item);
		error = ENOMEM;
		goto fail;
	}
	box->items[0] = item;
	box->item_count = box->item_capacity = 1;
	box->focus = 0;
	item->form = box;
	item->kind = ITEM_TEXT;
	item->box = box->content;
	lv_obj_set_style_pad_all(box->content, spec_of(box)->text_box_padding,
				 0);
	if (!make_field(item, box->content, true, max_chars, text)) {
		error = errno;
		goto fail;
	}
	lv_obj_set_height(fplinux_ui_text_object(item->field), lv_pct(100));
	return box;
fail:
	delete_screen(box);
	errno = error;
	return NULL;
}

const char *fplinux_ui_text_box_get(const struct fplinux_ui_screen *box)
{
	if (!box || box->kind != SCREEN_TEXT_BOX)
		return NULL;
	return fplinux_ui_text_get(box->items[0]->field);
}

bool fplinux_ui_text_box_set(struct fplinux_ui_screen *box, const char *text)
{
	if (!box || box->kind != SCREEN_TEXT_BOX || !text) {
		errno = EINVAL;
		return false;
	}
	if (!set_field_text(box->items[0], text))
		return false;
	changed(box);
	return true;
}

bool fplinux_ui_text_box_set_constraint(
	struct fplinux_ui_screen *box,
	enum fplinux_ui_text_constraint constraint)
{
	if (!box || box->kind != SCREEN_TEXT_BOX) {
		errno = EINVAL;
		return false;
	}
	if (!fplinux_ui_text_set_constraint(box->items[0]->field, constraint))
		return false;
	box->items[0]->constraint = constraint;
	changed(box);
	return true;
}

/* Alerts and canvases */

/*
 * An alert without text has no line for it: the box, as high as its
 * content, holds the title and any indicator below it.
 */
static void show_alert_text(struct fplinux_ui_screen *alert, const char *text)
{
	lv_label_set_text(alert->message, text ? text : "");
	lv_obj_set_hidden(alert->message, !text || !text[0]);
}

struct fplinux_ui_screen *fplinux_ui_alert_new(struct fplinux_ui *ui,
					       enum fplinux_ui_alert_kind kind,
					       const char *title,
					       const char *text,
					       unsigned int timeout_ms)
{
	const struct fplinux_ui_profile_spec *spec;
	const struct fplinux_ui_theme *theme;
	struct fplinux_ui_screen *alert;
	lv_color_t colour;
	int32_t inset;

	if ((unsigned int)kind > FPLINUX_UI_ALERT_ERROR) {
		errno = EINVAL;
		return NULL;
	}
	alert = screen_new(ui, SCREEN_ALERT, title);
	if (!alert)
		return NULL;
	theme = fplinux_ui_theme(ui);
	spec = fplinux_ui_spec(ui);
	alert->alert_kind = kind;
	alert->timeout_ms = timeout_ms;
	/* The kind shows in the frame and, for a warning or error, the title. */
	colour = kind == FPLINUX_UI_ALERT_WARNING ? theme->warning :
		 kind == FPLINUX_UI_ALERT_ERROR	  ? theme->danger :
						    theme->focus;
	alert->box = plain_object(alert->root);
	lv_obj_set_size(alert->box, alert_box_width(ui), LV_SIZE_CONTENT);
	lv_obj_set_style_max_height(alert->box, alert_box_max_height(ui), 0);
	lv_obj_set_flex_flow(alert->box, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_all(alert->box, spec->alert_padding, 0);
	lv_obj_set_style_pad_row(alert->box, spec->alert_gap, 0);
	lv_obj_set_style_bg_color(alert->box, theme->popup, 0);
	lv_obj_set_style_bg_opa(alert->box, LV_OPA_COVER, 0);
	lv_obj_set_style_border_color(alert->box, colour, 0);
	lv_obj_set_style_border_width(alert->box, spec->alert_border, 0);
	lv_obj_set_scrollable(alert->box, true);
	style_scrollbar(alert->box, ui);
	/* The scroll indicator stays inside the frame, clear of the text. */
	inset = spec->alert_padding + spec->alert_border -
		spec->scrollbar_width;
	if (inset > spec->scrollbar_offset)
		inset = spec->scrollbar_offset;
	lv_obj_set_style_pad_right(alert->box, inset > 0 ? inset : 0,
				   LV_PART_SCROLLBAR);
	lv_obj_center(alert->box);
	alert->box_title = lv_label_create(alert->box);
	lv_obj_set_width(alert->box_title, lv_pct(100));
	lv_label_set_long_mode(alert->box_title, LV_LABEL_LONG_MODE_WRAP);
	lv_obj_set_style_text_font(alert->box_title, theme->font_large, 0);
	if (kind == FPLINUX_UI_ALERT_WARNING || kind == FPLINUX_UI_ALERT_ERROR)
		lv_obj_set_style_text_color(alert->box_title, colour, 0);
	show_alert_title(alert);
	alert->message = lv_label_create(alert->box);
	lv_obj_set_width(alert->message, lv_pct(100));
	lv_label_set_long_mode(alert->message, LV_LABEL_LONG_MODE_WRAP);
	show_alert_text(alert, text);
	return alert;
}

static lv_obj_t *make_spinner(lv_obj_t *parent, struct fplinux_ui *ui,
			      int32_t rotation)
{
	const struct fplinux_ui_theme *theme = fplinux_ui_theme(ui);
	const struct fplinux_ui_profile_spec *spec = fplinux_ui_spec(ui);
	lv_obj_t *arc = lv_arc_create(parent);

	lv_obj_remove_style_all(arc);
	lv_obj_set_clickable(arc, false);
	lv_obj_set_size(arc, spec->spinner_size, spec->spinner_size);
	lv_obj_set_style_arc_width(arc, spec->spinner_width, LV_PART_MAIN);
	lv_obj_set_style_arc_color(arc, theme->border, LV_PART_MAIN);
	lv_obj_set_style_arc_width(arc, spec->spinner_width, LV_PART_INDICATOR);
	lv_obj_set_style_arc_color(arc, theme->focus, LV_PART_INDICATOR);
	lv_arc_set_bg_angles(arc, 0, 360);
	lv_arc_set_angles(arc, 0, SPINNER_ARC_DEGREES);
	lv_arc_set_rotation(arc, rotation);
	lv_obj_center(arc);
	return arc;
}

bool fplinux_ui_alert_set_progress(struct fplinux_ui_screen *alert, int value,
				   int maximum)
{
	const struct fplinux_ui_profile_spec *spec;

	if (!alert || alert->kind != SCREEN_ALERT || maximum < 0) {
		errno = EINVAL;
		return false;
	}
	spec = spec_of(alert);
	if (!alert->indicator_box) {
		alert->indicator_box = plain_object(alert->box);
		lv_obj_set_width(alert->indicator_box, lv_pct(100));
	}
	if (!alert->indicator || !alert->maximum != !maximum) {
		lv_obj_clean(alert->indicator_box);
		lv_obj_set_height(alert->indicator_box,
				  maximum ? spec->bar_height :
					    spec->spinner_size);
		alert->indicator =
			maximum ? make_bar(alert->indicator_box, alert->ui) :
				  make_spinner(alert->indicator_box, alert->ui,
					       alert->spin_angle);
	}
	if (maximum) {
		lv_bar_set_range(alert->indicator, 0, maximum);
		lv_bar_set_value(alert->indicator,
				 value < 0	 ? 0 :
				 value > maximum ? maximum :
						   value,
				 LV_ANIM_OFF);
	}
	alert->busy = true;
	alert->maximum = maximum;
	run_alert_timer(alert, is_top(alert) && alert->layer->visible);
	changed(alert);
	return true;
}

/*
 * The box keeps fitting its content, as at creation, with the indicator
 * below the text. A new text reads from its start; the same text again
 * keeps the scroll position.
 */
bool fplinux_ui_alert_set_text(struct fplinux_ui_screen *alert,
			       const char *text)
{
	if (!alert || alert->kind != SCREEN_ALERT) {
		errno = EINVAL;
		return false;
	}
	if (!strcmp(lv_label_get_text(alert->message), text ? text : ""))
		return true;
	show_alert_text(alert, text);
	lv_obj_scroll_to_y(alert->box, 0, LV_ANIM_OFF);
	return true;
}

struct fplinux_ui_screen *fplinux_ui_canvas_new(struct fplinux_ui *ui,
						const char *title)
{
	return screen_new(ui, SCREEN_CANVAS, title);
}

lv_obj_t *fplinux_ui_canvas_object(struct fplinux_ui_screen *canvas)
{
	return canvas && canvas->kind == SCREEN_CANVAS ? canvas->content : NULL;
}

void fplinux_ui_canvas_set_key_handler(struct fplinux_ui_screen *canvas,
				       fplinux_ui_canvas_key_cb handler,
				       void *context)
{
	if (!canvas || canvas->kind != SCREEN_CANVAS)
		return;
	canvas->canvas_key = handler;
	canvas->canvas_context = context;
}

void fplinux_ui_canvas_set_keyboard_keys(struct fplinux_ui_screen *canvas,
					 bool raw)
{
	if (canvas && canvas->kind == SCREEN_CANVAS)
		canvas->keyboard_raw = raw;
}

/* Keys */

static void open_options(struct fplinux_ui_screen *screen,
			 const struct soft_keys *soft)
{
	struct fplinux_ui_screens *layer = screen->layer;
	size_t count = soft->keys.menu_count;
	struct fplinux_ui_menu_item *items = calloc(count, sizeof(*items));
	struct menu_entry *entries =
		realloc(layer->menu_entries, count * sizeof(*entries));

	if (entries)
		layer->menu_entries = entries;
	if (!items || !entries) {
		free(items);
		return;
	}
	for (size_t i = 0; i < count; ++i) {
		const struct candidate *candidate =
			&screen->candidates[screen->order[i]];

		entries[i] = (struct menu_entry){
			.id = candidate->command->id,
			.item = candidate->item,
			.index = focused_index(screen),
		};
		items[i] = (struct fplinux_ui_menu_item){
			.action = (int)i,
			.text = command_label(screen, screen->order[i]),
			.icon = candidate->command->icon,
		};
	}
	layer->menu_entry_count = count;
	layer->menu_screen = screen;
	if (!fplinux_ui_menu_open(screen->ui,
				  word(screen, FPLINUX_UI_LABEL_OPTIONS), items,
				  count, options_chosen, screen))
		layer->menu_screen = NULL;
	free(items);
}

/* Passes a key to a canvas; false when it took it, as an element. */
static bool canvas_key(struct fplinux_ui_screen *screen,
		       const struct fplinux_ui_key *key)
{
	if (screen->kind != SCREEN_CANVAS)
		return true;
	if (screen->canvas_key)
		screen->canvas_key(screen->ui, screen, key,
				   screen->canvas_context);
	return false;
}

static void element_ok(struct fplinux_ui_screen *screen)
{
	size_t own_ok = own_ok_command(screen);

	if (own_ok != NONE)
		fire_item(screen, focused_index(screen),
			  screen->item_commands[own_ok].command.id);
	else if (screen->kind == SCREEN_LIST)
		list_ok(screen);
	else if (screen->kind == SCREEN_FORM)
		form_ok(screen);
}

/*
 * Returns true when the press became a command or a system action, whose
 * release nothing else receives, and false when an element took it.
 */
static bool screen_press(struct fplinux_ui_screen *screen,
			 const struct fplinux_ui_key *key)
{
	struct form_item *text = focused_text(screen);
	struct soft_keys soft;

	if (screen->editing) {
		date_key(screen, key);
		return true;
	}
	if (text && fplinux_ui_text_key(text->field, key))
		return false;
	if (screen->keyboard_raw && key->source == FPLINUX_UI_KEY_KEYBOARD &&
	    (key->code == KEY_ESC || is_ok_key(key)))
		return canvas_key(screen, key);
	assign_keys(screen, &soft);
	if (is_right_key(key)) {
		if (soft.keys.right == NONE)
			return canvas_key(screen, key);
		fire_candidate(screen, soft.keys.right);
		return true;
	}
	if (is_left_key(key)) {
		if (soft.keys.left != NONE)
			fire_candidate(screen, soft.keys.left);
		else if (soft.keys.options)
			open_options(screen, &soft);
		else
			return canvas_key(screen, key);
		return true;
	}
	if (is_ok_key(key)) {
		if (soft.action)
			element_ok(screen);
		else if (soft.keys.ok != NONE)
			fire_candidate(screen, soft.keys.ok);
		else
			return canvas_key(screen, key);
		return true;
	}
	if (screen->kind == SCREEN_LIST)
		fplinux_ui_listview_key(screen->list, key);
	else if (screen->kind == SCREEN_FORM)
		form_key(screen, key);
	else if (screen->kind == SCREEN_ALERT)
		fplinux_ui_scroll_key(screen->box, key);
	else
		return canvas_key(screen, key);
	return false;
}

static void screen_release(struct fplinux_ui_screen *screen,
			   const struct fplinux_ui_key *key)
{
	struct form_item *text = focused_text(screen);

	if (text)
		fplinux_ui_text_key(text->field, key);
	else if (screen->kind == SCREEN_LIST)
		fplinux_ui_listview_key(screen->list, key);
	else
		canvas_key(screen, key);
}

bool fplinux_ui_screens_key(struct fplinux_ui_screens *layer,
			    const struct fplinux_ui_key *key)
{
	bool keyboard = key->source == FPLINUX_UI_KEY_KEYBOARD;
	unsigned char bit = (unsigned char)(1U << (key->code % 8));
	unsigned char *used;

	if (!layer || !layer->top)
		return false;
	if (key->code >= KEY_CNT)
		return true;
	used = &layer->used[keyboard][key->code / 8];
	if (!key->pressed) {
		if (*used & bit)
			*used &= (unsigned char)~bit;
		else
			screen_release(layer->top, key);
	} else if (screen_press(layer->top, key)) {
		*used |= bit;
	}
	refresh(layer);
	return true;
}

/* Whether an options menu entry still names a command the screen offers. */
static bool entry_current(const struct fplinux_ui_screen *screen,
			  const struct menu_entry *entry)
{
	if (!entry->item)
		return find_command(screen, entry->id) != NONE;
	return find_item_command(screen, entry->index, entry->id) != NONE &&
	       !(screen->kind == SCREEN_FORM &&
		 screen->items[entry->index]->hidden);
}

/*
 * The screen of the open options menu changed: the menu closes without a
 * choice when an entry's command was removed or its row deleted or hidden.
 */
static void options_changed(struct fplinux_ui_screen *screen)
{
	struct fplinux_ui_screens *layer = screen->layer;

	if (layer->menu_screen != screen)
		return;
	for (size_t i = 0; i < layer->menu_entry_count; ++i)
		if (!entry_current(screen, &layer->menu_entries[i])) {
			layer->menu_screen = NULL;
			fplinux_ui_menu_close(screen->ui);
			return;
		}
}

/* The options menu reports here through the runtime. */
static void options_chosen(struct fplinux_ui *ui, int action, void *context)
{
	/* Deleting a screen closes its menu without a choice: screen lives. */
	struct fplinux_ui_screen *screen = context;
	struct fplinux_ui_screens *layer = screen->layer;

	(void)ui;
	if (action >= 0 && (size_t)action < layer->menu_entry_count) {
		const struct menu_entry *entry = &layer->menu_entries[action];

		if (entry->item)
			fire_item(screen, entry->index, entry->id);
		else
			fire(screen, entry->id);
	}
	refresh(layer);
}

void fplinux_ui_screens_set_visible(struct fplinux_ui_screens *layer,
				    bool visible)
{
	if (!layer)
		return;
	layer->visible = visible;
	restack(layer);
}

void fplinux_ui_screens_suspend(struct fplinux_ui_screens *layer)
{
	/* Canvas fields were suspended by the menu, not by a screen change. */
	if (layer && layer->top && !layer->closing &&
	    layer->top->kind != SCREEN_CANVAS)
		set_active(layer->top, false);
}

void fplinux_ui_screens_reclaim(struct fplinux_ui_screens *layer)
{
	if (!layer)
		return;
	/* Any menu closed, also by fplinux_ui_menu_close(): Options is not open. */
	layer->menu_screen = NULL;
	if (layer->top && !layer->closing)
		set_active(layer->top, true);
}

/* The focused row of a stored list, which scrolls itself, back in view. */
static void reveal_row(struct fplinux_ui_screen *screen)
{
	lv_obj_t *row;

	if (fplinux_ui_listview_is_virtual(screen->list))
		return;
	row = fplinux_ui_listview_selected_row(screen->list);
	if (row)
		lv_obj_scroll_to_view(row, LV_ANIM_OFF);
}

/*
 * A new surface size, as build_frame(), fplinux_ui_alert_new() and
 * frame_popup_choice() size each screen; a virtual list sees its new view
 * through LVGL's size change. Titles and the canvas's drawing are left as
 * they are: the application lays out a canvas on FPLINUX_UI_EVENT_RESIZE.
 */
static void resize_screen(struct fplinux_ui_screen *screen)
{
	struct fplinux_ui *ui = screen->ui;

	lv_obj_set_size(screen->root, fplinux_ui_width(ui),
			root_height(screen));
	if (screen->kind == SCREEN_ALERT) {
		lv_obj_set_width(screen->box, alert_box_width(ui));
		lv_obj_set_style_max_height(screen->box,
					    alert_box_max_height(ui), 0);
		return;
	}
	if (screen->popup_choice)
		size_popup_choice(screen,
				  fplinux_ui_listview_count(screen->list));
	size_frame(screen);
	lv_obj_update_layout(screen->root);
	if (screen->kind == SCREEN_FORM)
		keep_focus_visible(screen);
	else if (screen->kind == SCREEN_LIST)
		reveal_row(screen);
}

void fplinux_ui_screens_resize(struct fplinux_ui_screens *layer)
{
	if (!layer || layer->closing)
		return;
	for (struct fplinux_ui_screen *screen = layer->all; screen;
	     screen = screen->next)
		resize_screen(screen);
}

void fplinux_ui_screens_delete(struct fplinux_ui_screens *layer)
{
	struct fplinux_ui_screen *screen = layer ? layer->all : NULL;

	if (!layer)
		return;
	layer->closing = true;
	layer->menu_screen = NULL;
	layer->all = NULL;
	while (screen) {
		struct fplinux_ui_screen *next = screen->next;

		destroy_screen(screen);
		screen = next;
	}
	free(layer->menu_entries);
	free(layer);
}
