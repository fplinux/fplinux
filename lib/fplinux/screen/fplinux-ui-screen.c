/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define _POSIX_C_SOURCE 200809L
#include "fplinux-ui-screen-private.h"
#include "fplinux-activity.h"
#include "fplinux-ui-activity.h"
#include "fplinux-ui-listview.h"

#include <errno.h>
#include <libintl.h>
#include <limits.h>
#include <linux/input-event-codes.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* A wait alert's private Cancel command. */
#define WAIT_CANCEL_COMMAND (-4)
/*
 * A wait shows its alert only for work longer than this, which most folder
 * reads and file changes stay under, yet early enough that the ignored keys
 * do not look like a stalled phone. A shown alert stays at least the same
 * time, so that it can be read rather than flash, which bounds the delay it
 * adds to work that ends just after it appears.
 */
#define WAIT_ALERT_DELAY_MS 500U
#define WAIT_ALERT_MINIMUM_MS 500U

struct command {
	int id;
	enum fplinux_ui_command_type type;
	/* NULL for the type's localized default. */
	char *label;
	int priority;
	/* The decoded options-menu image, or NULL. */
	lv_draw_buf_t *icon;
	bool direct;
	bool keep_right_on_clear;
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
	bool clear;
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
	/* A caller retained its right command instead of the text's Clear. */
	bool keep_right;
	/* Left, centre and right legends. */
	const char *legends[3];
};

static void restack(struct fplinux_ui_screens *layer);
static void resize_screen(struct fplinux_ui_screen *screen);
static void options_chosen(struct fplinux_ui *ui, int action, void *context);
static void options_changed(struct fplinux_ui_screen *screen);
static bool entry_current(const struct fplinux_ui_screen *screen,
			  const struct menu_entry *entry);
static void restore_options(struct fplinux_ui_screens *layer,
			    const struct options_return *origin);
static void cancel_wait(struct fplinux_ui_screens *layer);
static void drop_wait(struct fplinux_ui_screens *layer,
		      const struct fplinux_ui_screen *deleted);
static void wait_stack_changed(struct fplinux_ui_screens *layer);

/* In the language that fplinux_ui_open() selected for messages. */
const char *fplinux_ui_screen_word(const struct fplinux_ui_screen *screen,
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

bool fplinux_ui_screen_is_top(const struct fplinux_ui_screen *screen)
{
	return fplinux_ui_screens_active(screen->layer) == screen;
}

struct fplinux_ui_screen *
fplinux_ui_screens_active(const struct fplinux_ui_screens *layer)
{
	return layer ? (layer->transient ? layer->transient : layer->top) :
		       NULL;
}

/* Legends and the options menu follow every change of the top screen. */
void fplinux_ui_screen_changed(struct fplinux_ui_screen *screen)
{
	options_changed(screen);
	if (fplinux_ui_screen_is_top(screen))
		fplinux_ui_screen_refresh(screen->layer);
}

void fplinux_ui_screen_report(struct fplinux_ui_screen *screen, size_t index)
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

const struct fplinux_ui_profile_spec *
fplinux_ui_screen_spec(const struct fplinux_ui_screen *screen)
{
	return fplinux_ui_spec(screen->ui);
}

/* The one scroll indicator: a thumb in focus over its channel. */
void fplinux_ui_screen_style_scrollbar(lv_obj_t *object, struct fplinux_ui *ui)
{
	lv_obj_set_scroll_dir(object, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(object, LV_SCROLLBAR_MODE_AUTO);
	fplinux_ui_style_scrollbar(ui, object);
}

lv_obj_t *fplinux_ui_screen_plain_object(lv_obj_t *parent)
{
	lv_obj_t *object = lv_obj_create(parent);

	lv_obj_remove_style_all(object);
	lv_obj_set_scrollable(object, false);
	return object;
}

static bool local_legend_bar(const struct fplinux_ui_screen *screen)
{
	if (screen->kind == SCREEN_CANVAS)
		return screen->canvas_fullscreen_bar &&
		       fplinux_ui_is_fullscreen(screen->ui);
	return fplinux_ui_is_launcher(screen->ui) ||
	       fplinux_ui_is_fullscreen(screen->ui);
}

static int32_t screen_top(const struct fplinux_ui_screen *screen)
{
	return fplinux_ui_is_launcher(screen->ui) && local_legend_bar(screen) ?
		       fplinux_ui_screen_spec(screen)->status_height :
		       0;
}

static int32_t screen_height(const struct fplinux_ui_screen *screen)
{
	return local_legend_bar(screen) ? fplinux_ui_modal_height(screen->ui) -
						  screen_top(screen) :
					  fplinux_ui_height(screen->ui);
}

static int32_t label_text_width(lv_obj_t *label, const char *text)
{
	lv_point_t size;

	lv_text_get_size(
		&size, text, lv_obj_get_style_text_font(label, LV_PART_MAIN),
		lv_obj_get_style_text_letter_space(label, LV_PART_MAIN), 0,
		LV_COORD_MAX, LV_TEXT_FLAG_NONE);
	return size.x;
}

/*
 * The band and the content fill their screen, or a popup choice's box; the
 * date editor's form ends above the legend bar of a fullscreen application.
 */
static void size_frame(struct fplinux_ui_screen *screen)
{
	const struct fplinux_ui_profile_spec *spec =
		fplinux_ui_screen_spec(screen);
	int32_t top = screen->title ? spec->header_height : 0;
	int32_t width = fplinux_ui_width(screen->ui);
	int32_t height = screen->form.editing ?
				 fplinux_ui_modal_height(screen->ui) -
					 screen_top(screen) :
				 screen_height(screen);
	int32_t title_width;

	if (screen->popup_choice) {
		lv_obj_update_layout(screen->box);
		width = lv_obj_get_content_width(screen->box);
		height = lv_obj_get_content_height(screen->box);
	}
	lv_obj_set_size(screen->band, width, spec->header_height);
	title_width = width - 2 * spec->margin;
	if (screen->header_status) {
		const char *text = screen->header_full;
		int32_t status_width =
			label_text_width(screen->header_status, text);
		int32_t full_title_width =
			screen->title ? label_text_width(screen->band_label,
							 screen->title) :
					0;
		int32_t gap = full_title_width && status_width ? spec->spacing :
								 0;

		if (full_title_width + gap + status_width > title_width &&
		    screen->header_compact) {
			text = screen->header_compact;
			status_width =
				label_text_width(screen->header_status, text);
			gap = full_title_width && status_width ? spec->spacing :
								 0;
		}
		lv_label_set_text_static(screen->header_status, text);
		lv_obj_set_width(screen->header_status, status_width);
		lv_obj_align(screen->header_status, LV_ALIGN_TOP_RIGHT,
			     -spec->margin,
			     fplinux_ui_band_line_y(
				     spec, lv_obj_get_style_text_font(
						   screen->header_status,
						   LV_PART_MAIN)));
		title_width -= status_width + gap;
	}
	lv_obj_set_width(screen->band_label, title_width > 0 ? title_width : 0);
	lv_obj_set_pos(screen->content, 0, top);
	lv_obj_set_size(screen->content, width, height - top);
}

/* Only the active screen owns the shared title marquee. */
static void title_active(struct fplinux_ui_screen *screen, bool active)
{
	active = active && screen->layer->visible && screen->band_label &&
		 (screen->title || screen->kind == SCREEN_ALERT) &&
		 !screen->header_status;
	if (active == screen->title_active)
		return;
	if (active)
		fplinux_ui_title_scroll(screen->band_label);
	else
		fplinux_ui_title_hide(screen->band_label);
	screen->title_active = active;
}

/* The title band of fplinux_ui_header(); no band without a title. */
static void layout_frame(struct fplinux_ui_screen *screen)
{
	bool was_active = screen->title_active;

	size_frame(screen);
	lv_obj_set_hidden(screen->band, !screen->title);
	if (screen->title)
		lv_label_set_text(screen->band_label, screen->title);
	title_active(screen, fplinux_ui_screen_is_top(screen) &&
				     !lv_obj_is_hidden(screen->root));
	if (was_active && screen->title_active)
		fplinux_ui_title_scroll(screen->band_label);
	else if (!screen->title_active && screen->title)
		lv_label_set_long_mode(screen->band_label,
				       LV_LABEL_LONG_MODE_DOTS);
}

/* A modal screen's root, its scrim, ends above the legend bar. */
static int32_t root_height(const struct fplinux_ui_screen *screen)
{
	return screen->kind == SCREEN_ALERT || screen->popup_choice ?
		       fplinux_ui_modal_height(screen->ui) -
			       screen_top(screen) :
		       screen_height(screen);
}

static void build_band(struct fplinux_ui_screen *screen, lv_obj_t *parent)
{
	const struct fplinux_ui_theme *theme = fplinux_ui_theme(screen->ui);
	const struct fplinux_ui_profile_spec *spec =
		fplinux_ui_screen_spec(screen);
	int32_t width = fplinux_ui_width(screen->ui);

	screen->band = fplinux_ui_screen_plain_object(parent);
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
	lv_obj_set_pos(screen->band_label, spec->margin,
		       fplinux_ui_band_line_y(spec, theme->font_large));
}

static void build_frame(struct fplinux_ui_screen *screen)
{
	const struct fplinux_ui_theme *theme = fplinux_ui_theme(screen->ui);
	int32_t width = fplinux_ui_width(screen->ui);

	screen->root =
		fplinux_ui_screen_plain_object(fplinux_ui_screen(screen->ui));
	lv_obj_set_y(screen->root, screen_top(screen));
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
	build_band(screen, screen->root);
	screen->content = fplinux_ui_screen_plain_object(screen->root);
	layout_frame(screen);
}

struct fplinux_ui_screen *fplinux_ui_screen_new(struct fplinux_ui *ui,
						enum screen_kind kind,
						const char *title)
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
	screen->form.focus = NONE;
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
		return fplinux_ui_screen_word(screen, FPLINUX_UI_LABEL_CLOSE);
	if (screen->candidates[index].clear)
		return fplinux_ui_screen_word(screen, FPLINUX_UI_LABEL_CLEAR);
	command = screen->candidates[index].command;
	return command->label ?
		       command->label :
		       fplinux_ui_screen_word(screen,
					      default_labels[command->type]);
}

/* Application images override the standard meaning of a command type. */
static const void *command_icon(struct fplinux_ui_screen *screen,
				const struct candidate *candidate)
{
	static const char *const glyphs[] = {
		[FPLINUX_UI_COMMAND_BACK] = "back",
		[FPLINUX_UI_COMMAND_CANCEL] = "cancel",
		[FPLINUX_UI_COMMAND_EXIT] = "exit",
		[FPLINUX_UI_COMMAND_STOP] = "stop",
		[FPLINUX_UI_COMMAND_OK] = "check",
		[FPLINUX_UI_COMMAND_ITEM] = NULL,
		[FPLINUX_UI_COMMAND_SCREEN] = NULL,
		[FPLINUX_UI_COMMAND_HELP] = "help",
	};
	const struct command *command = candidate->command;
	const char *glyph;

	if (command->icon)
		return command->icon;
	/* Synthetic Clear has no command type of its own. */
	glyph = candidate->clear ? "backspace" : glyphs[command->type];
	return glyph ? fplinux_ui_glyph(screen->ui, glyph,
					FPLINUX_UI_GLYPH_ROW) :
		       NULL;
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

/* Screen and item commands, plus the text field's optional Clear action. */
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
	int size = fplinux_ui_screen_spec(screen)->row_icon_size;

	*to = (struct command){
		.id = from->id,
		.type = from->type,
		.priority = from->priority,
		.direct = from->direct,
		.keep_right_on_clear = from->keep_right_on_clear,
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
	fplinux_ui_screen_changed(screen);
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
	fplinux_ui_screen_changed(screen);
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
	return screen->kind == SCREEN_FORM ? screen->form.item_count : 0;
}

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
	fplinux_ui_form_update_appearance(screen, index);
	fplinux_ui_screen_changed(screen);
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
	fplinux_ui_form_update_appearance(screen, index);
	fplinux_ui_screen_changed(screen);
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

	if (layer->menu_screen == screen)
		for (size_t i = 0; i < layer->menu_entry_count; ++i)
			layer->menu_entries[i].index =
				move(layer->menu_entries[i].index, a, b);
	if (layer->options_source.owner == screen)
		layer->options_source.entry.index =
			move(layer->options_source.entry.index, a, b);
	for (struct fplinux_ui_screen *child = layer->all; child;
	     child = child->next)
		if (child->options_return.owner == screen)
			child->options_return.entry.index =
				move(child->options_return.entry.index, a, b);
}

bool fplinux_ui_screen_has_item_commands(const struct fplinux_ui_screen *screen,
					 size_t index)
{
	for (size_t i = 0; i < screen->item_command_count; ++i)
		if (screen->item_commands[i].index == index)
			return true;
	return false;
}

void fplinux_ui_screen_item_commands_moved(struct fplinux_ui_screen *screen,
					   size_t (*move)(size_t index,
							  size_t a, size_t b),
					   size_t a, size_t b)
{
	for (size_t i = 0; i < screen->item_command_count; ++i)
		screen->item_commands[i].index =
			move(screen->item_commands[i].index, a, b);
	menu_rows_moved(screen, move, a, b);
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
void fplinux_ui_screen_item_commands_deleted(struct fplinux_ui_screen *screen,
					     size_t at, size_t count)
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

void fplinux_ui_screen_clear_item_commands(struct fplinux_ui_screen *screen)
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
	lv_label_set_text(
		alert->band_label,
		alert->title ? alert->title :
			       fplinux_ui_screen_word(
				       alert, alert_titles[alert->alert_kind]));
	if (alert->title_active)
		fplinux_ui_title_scroll(alert->band_label);
	else
		fplinux_ui_title_hide(alert->band_label);
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
	if ((!title && !screen->title) ||
	    (title && screen->title && !strcmp(title, screen->title)))
		return true;
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

bool fplinux_ui_screen_set_header_status(struct fplinux_ui_screen *screen,
					 const char *full, const char *compact)
{
	char *full_copy = NULL;
	char *compact_copy = NULL;
	char *old_full;
	char *old_compact;
	lv_obj_t *label;

	if (!screen || screen->kind == SCREEN_ALERT) {
		errno = EINVAL;
		return false;
	}
	if (!full || !full[0]) {
		if (!screen->header_full)
			return true;
	} else {
		if (!compact)
			compact = full;
		if (screen->header_full && !strcmp(screen->header_full, full) &&
		    !strcmp(screen->header_compact ? screen->header_compact :
						     screen->header_full,
			    compact))
			return true;
		full_copy = strdup(full);
		if (!full_copy)
			return false;
		if (strcmp(full, compact)) {
			compact_copy = strdup(compact);
			if (!compact_copy) {
				free(full_copy);
				return false;
			}
		}
	}
	label = screen->header_status;
	if (full_copy && !label) {
		const lv_font_t *font = fplinux_ui_theme(screen->ui)->font;

		label = lv_label_create(screen->band);
		if (!label) {
			free(compact_copy);
			free(full_copy);
			errno = ENOMEM;
			return false;
		}
		lv_obj_set_style_text_font(label, font, 0);
		lv_obj_set_height(label, lv_font_get_line_height(font));
		lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_CLIP);
	} else if (!full_copy) {
		lv_obj_delete(label);
		label = NULL;
	}
	old_full = screen->header_full;
	old_compact = screen->header_compact;
	screen->header_full = full_copy;
	screen->header_compact = compact_copy;
	screen->header_status = label;
	layout_frame(screen);
	free(old_full);
	free(old_compact);
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
	       (fplinux_ui_is_launcher(ui) ?
			fplinux_ui_spec(ui)->status_height :
			0) -
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
	spec = fplinux_ui_screen_spec(screen);
	top = screen->title ? spec->header_height : 0;
	if (screen->kind == SCREEN_ALERT) {
		int32_t inset = 2 * (spec->alert_border + spec->alert_padding);

		fplinux_ui_scrollbar_layout(screen->message_view);
		w = alert_box_width(screen->ui) - inset -
		    (lv_obj_get_style_pad_right(screen->message_view, 0) -
		     spec->alert_padding);
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

/* The focused list row or form item, whose item commands apply; else NONE. */
static size_t focused_index(const struct fplinux_ui_screen *screen)
{
	if (screen->kind == SCREEN_LIST)
		return fplinux_ui_listview_selected(screen->list);
	return screen->kind == SCREEN_FORM ? screen->form.focus : NONE;
}

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
		 fplinux_ui_form_focus_is_actionable(screen))
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
	return command->label ?
		       command->label :
		       fplinux_ui_screen_word(screen,
					      default_labels[command->type]);
}

const char *fplinux_ui_screen_mark_word(const struct fplinux_ui_screen *screen,
					const struct fplinux_ui_listview *list)
{
	return fplinux_ui_screen_word(
		screen, fplinux_ui_listview_is_marked(
				list, fplinux_ui_listview_selected(list)) ?
				FPLINUX_UI_LABEL_UNMARK :
				FPLINUX_UI_LABEL_MARK);
}

/* The centre legend of the focused element's own OK action, or NULL. */
static const char *element_action(const struct fplinux_ui_screen *screen)
{
	switch (screen->kind) {
	case SCREEN_LIST:
		if (!fplinux_ui_listview_count(screen->list))
			return NULL;
		if (screen->list_kind == FPLINUX_UI_LIST_MULTIPLE)
			return fplinux_ui_screen_mark_word(screen,
							   screen->list);
		if (own_ok_command(screen) != NONE)
			return own_label(
				screen,
				&screen->item_commands[own_ok_command(screen)]
					 .command);
		if (screen->select_label)
			return screen->select_label;
		return fplinux_ui_screen_word(screen, FPLINUX_UI_LABEL_SELECT);
	case SCREEN_FORM:
		if (own_ok_command(screen) != NONE)
			return own_label(
				screen,
				&screen->item_commands[own_ok_command(screen)]
					 .command);
		return fplinux_ui_form_action(screen);
	case SCREEN_TEXT_BOX:
		return fplinux_ui_form_action(screen);
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
			.direct = command->direct,
		};
	}
	for (size_t i = 0; i < screen->command_count; ++i) {
		const struct command *command = &screen->commands[i];

		screen->candidates[count] =
			(struct candidate){ .command = command };
		screen->entries[count++] = (struct fplinux_ui_command_entry){
			.role = role_of(command->type),
			.priority = command->priority,
			.direct = command->direct,
		};
	}
	return count;
}

/* Use the same assignment for key dispatch, legends and the Options menu. */
static void assign_keys(struct fplinux_ui_screen *screen,
			struct soft_keys *soft)
{
	static const struct fplinux_ui_command_entry close = {
		.role = FPLINUX_UI_ROLE_BACK,
	};
	static const struct command clear = { .priority = INT_MAX };
	const struct fplinux_ui_command_entry *entries = &close;
	struct fplinux_ui_text *text = fplinux_ui_form_focused_text(screen);
	size_t count = 1;

	soft->action = element_action(screen);
	soft->clear = text && !fplinux_ui_text_is_read_only(text) &&
		      !fplinux_ui_text_is_empty(text);
	soft->keep_right = false;
	if (!system_close(screen)) {
		entries = screen->entries;
		count = build_candidates(screen);
	}
	fplinux_ui_command_keys_assign(entries, count, soft->action != NULL,
				       false, &soft->keys, screen->order);
	if (!system_close(screen) && soft->keys.right != NONE)
		soft->keep_right = screen->candidates[soft->keys.right]
					   .command->keep_right_on_clear;
	if (soft->clear && soft->keep_right) {
		/* Phones without a separate Clear key still offer deletion. */
		screen->candidates[count] =
			(struct candidate){ .command = &clear, .clear = true };
		screen->entries[count++] = (struct fplinux_ui_command_entry){
			.role = FPLINUX_UI_ROLE_OTHER,
			.priority = clear.priority,
			.direct = true,
		};
		soft->clear = false;
		fplinux_ui_command_keys_assign(entries, count,
					       soft->action != NULL, false,
					       &soft->keys, screen->order);
	} else if (soft->clear) {
		fplinux_ui_command_keys_assign(entries, count,
					       soft->action != NULL, true,
					       &soft->keys, screen->order);
	}
	soft->legends[0] = soft->keys.left != NONE ?
				   command_label(screen, soft->keys.left) :
			   soft->keys.options ?
				   fplinux_ui_screen_word(
					   screen, FPLINUX_UI_LABEL_OPTIONS) :
				   "";
	soft->legends[1] = soft->action ? soft->action :
			   soft->keys.ok != NONE ?
					  command_label(screen, soft->keys.ok) :
					  "";
	soft->legends[2] =
		soft->clear ?
			fplinux_ui_screen_word(screen, FPLINUX_UI_LABEL_CLEAR) :
		soft->keys.right != NONE ?
			command_label(screen, soft->keys.right) :
			"";
}

static void update_legend_owner(struct fplinux_ui_screens *layer)
{
	struct fplinux_ui_screen *screen = fplinux_ui_screens_active(layer);

	fplinux_ui_modal_set(
		layer->ui, FPLINUX_UI_MODAL_SCREEN,
		screen && !layer->closing &&
			(screen->kind == SCREEN_ALERT || screen->popup_choice ||
			 screen->form.editing || local_legend_bar(screen)));
}

void fplinux_ui_screen_refresh(struct fplinux_ui_screens *layer)
{
	struct fplinux_ui_screen *screen = fplinux_ui_screens_active(layer);
	const char *legends[3] = { "", "", "" };
	struct fplinux_ui_text *text;
	struct soft_keys soft;

	update_legend_owner(layer);
	if (!screen || layer->closing)
		return;
	fplinux_ui_form_settle(screen);
	text = fplinux_ui_form_focused_text(screen);
	if (screen->kind == SCREEN_FORM) {
		/* The date editor's item stays in view above the bar. */
		size_frame(screen);
		if (screen->form.editing)
			fplinux_ui_form_keep_focus_visible(screen);
	}
	if (screen == layer->wait.origin) {
		for (int i = 0; i < 3; ++i)
			legends[i] = layer->wait.legends[i];
	} else if (screen->form.editing) {
		legends[1] =
			fplinux_ui_screen_word(screen, FPLINUX_UI_LABEL_DONE);
		legends[2] =
			fplinux_ui_screen_word(screen, FPLINUX_UI_LABEL_CANCEL);
	} else if (text && fplinux_ui_text_is_picker_open(text)) {
		legends[1] =
			fplinux_ui_screen_word(screen, FPLINUX_UI_LABEL_INSERT);
		legends[2] =
			fplinux_ui_screen_word(screen, FPLINUX_UI_LABEL_CANCEL);
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

static const struct fplinux_ui_activity_entry *
activity_source(const struct fplinux_ui_screen *alert)
{
	const struct fplinux_ui_profile_spec *spec =
		fplinux_ui_screen_spec(alert);
	size_t i;

	for (i = 0;
	     i < sizeof(ui_activity_images) / sizeof(ui_activity_images[0]);
	     i++) {
		const struct fplinux_ui_activity_entry *entry =
			&ui_activity_images[i];

		if (entry->size == (unsigned int)spec->spinner_size &&
		    entry->width == (unsigned int)spec->spinner_width &&
		    entry->phase == alert->activity_phase)
			return entry;
	}
	return NULL;
}

static void activity_frame(struct fplinux_ui_screen *alert)
{
	const struct fplinux_ui_activity_entry *entry = activity_source(alert);

	if (entry)
		lv_image_set_src(alert->activity_indicator, entry->image);
}

static void spin_step(lv_timer_t *timer)
{
	struct fplinux_ui_screen *alert = lv_timer_get_user_data(timer);

	alert->activity_phase =
		(alert->activity_phase + 1) % FPLINUX_STATUS_ACTIVITY_PHASES;
	activity_frame(alert);
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
		alert->spin_timer = lv_timer_create(
			spin_step, FPLINUX_STATUS_ACTIVITY_STEP_MS, alert);
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
	struct options_return origin = { 0 };

	/* Keys and timers reach only the top screen; nothing to remove. */
	if (layer->top != alert) {
		if (id)
			notify_command(alert, *id);
		return;
	}
	if (!id && alert->alert_kind == FPLINUX_UI_ALERT_INFO &&
	    !alert->command_count)
		origin = alert->options_return;
	fplinux_ui_cancel_key_repeat(ui, NULL);
	layer->top = alert->below;
	alert->below = NULL;
	alert->stacked = false;
	run_alert_timer(alert, false);
	restack(layer);
	if (id)
		notify_command(alert, *id);
	if (!alert->stacked)
		fplinux_ui_screen_delete(alert);
	if (layer->top) {
		fplinux_ui_screen_refresh(layer);
		restore_options(layer, &origin);
		wait_stack_changed(layer);
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
	/* A wait's alert stays until its work ends; Cancel asks it to stop. */
	if (screen == screen->layer->wait.alert)
		cancel_wait(screen->layer);
	else if (screen->kind == SCREEN_ALERT)
		dismiss_alert(screen, &id);
	else
		notify_command(screen, id);
}

bool fplinux_ui_screen_is_alive(const struct fplinux_ui_screens *layer,
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
	if (fplinux_ui_screen_is_alive(layer, screen))
		screen->command_item = NONE;
}

/* Fires a command that assign_keys() put on a key. */
static void clear_focused_text(struct fplinux_ui_screen *screen)
{
	struct fplinux_ui_text *text = fplinux_ui_form_focused_text(screen);
	struct fplinux_ui_key key = {
		.source = FPLINUX_UI_KEY_KEYBOARD,
		.code = KEY_BACKSPACE,
		.pressed = true,
	};

	if (!text)
		return;
	fplinux_ui_text_key(text, &key);
	key.pressed = false;
	fplinux_ui_text_key(text, &key);
}

static void fire_candidate(struct fplinux_ui_screen *screen, size_t index)
{
	if (system_close(screen))
		dismiss_alert(screen, NULL);
	else if (screen->candidates[index].clear)
		clear_focused_text(screen);
	else if (screen->candidates[index].item)
		fire_item(screen, focused_index(screen),
			  screen->candidates[index].command->id);
	else
		fire(screen, screen->candidates[index].command->id);
}

/* The stack */

static void canvas_activation(struct fplinux_ui_screen *screen, bool active)
{
	if (screen->canvas_active == active)
		return;
	if (!active)
		fplinux_ui_cancel_key_repeat(screen->ui, screen);
	screen->canvas_active = active;
	if (screen->on_canvas_active && !screen->layer->closing)
		screen->on_canvas_active(screen->ui, screen, active,
					 screen->canvas_active_context);
}

static void set_active(struct fplinux_ui_screen *screen, bool active)
{
	active = active && screen->layer->visible &&
		 !fplinux_ui_menu_is_open(screen->ui);
	if (screen->kind == SCREEN_FORM || screen->kind == SCREEN_TEXT_BOX)
		fplinux_ui_form_apply_focus(screen, active);
	else if (screen->kind == SCREEN_LIST && screen->list)
		fplinux_ui_listview_set_active(screen->list, active);
	else if (screen->kind == SCREEN_ALERT)
		run_alert_timer(screen, active && screen->layer->visible);
	else if (screen->kind == SCREEN_CANVAS) {
		fplinux_ui_text_canvas_active(screen->root, active);
		canvas_activation(screen, active);
	}
}

/* The top screen shows, and an alert shows the screens below it too. */
static void restack(struct fplinux_ui_screens *layer)
{
	bool shown = true;
	struct fplinux_ui_screen *active = fplinux_ui_screens_active(layer);

	if (layer->transient) {
		lv_obj_set_hidden(layer->transient->root, false);
		title_active(layer->transient, true);
		set_active(layer->transient, true);
		shown = layer->transient->popup_choice;
	}

	for (struct fplinux_ui_screen *screen = layer->top; screen;
	     screen = screen->below) {
		/* The launcher's base Canvas owns the persistent system bars. */
		bool backdrop = !screen->below &&
				screen->kind == SCREEN_CANVAS &&
				fplinux_ui_is_launcher(screen->ui);
		bool visible = shown || backdrop;
		lv_obj_set_hidden(screen->root, !visible);
		title_active(screen, visible && screen == active);
		set_active(screen, screen == active);
		if (screen->kind != SCREEN_ALERT && !screen->popup_choice)
			shown = false;
	}
}

/* Frees a screen that is no longer in the layer's list. */
static void destroy_screen(struct fplinux_ui_screen *screen)
{
	if (!screen->layer->closing)
		set_active(screen, false);
	fplinux_ui_cancel_key_repeat(screen->ui, screen);
	if (screen->band_label)
		fplinux_ui_title_hide(screen->band_label);
	stop_timer(&screen->timer);
	stop_timer(&screen->spin_timer);
	/* The list views and text fields go with their objects. */
	lv_obj_delete(screen->root);
	fplinux_ui_form_free_items(screen);
	for (size_t i = 0; i < screen->command_count; ++i)
		free_command(&screen->commands[i]);
	free(screen->commands);
	fplinux_ui_screen_clear_item_commands(screen);
	free(screen->item_commands);
	free(screen->candidates);
	free(screen->entries);
	free(screen->order);
	free(screen->select_label);
	free(screen->title);
	free(screen->header_full);
	free(screen->header_compact);
	free(screen);
}

void fplinux_ui_screen_delete(struct fplinux_ui_screen *screen)
{
	struct fplinux_ui_screens *layer = screen->layer;

	if (layer->options_source.owner == screen)
		layer->options_source.owner = NULL;
	for (struct fplinux_ui_screen **link = &layer->all; *link;) {
		struct fplinux_ui_screen *child = *link;

		if (child->options_return.owner == screen)
			child->options_return.owner = NULL;
		if (child == screen)
			*link = child->next;
		else
			link = &child->next;
	}
	if (layer->menu_screen == screen) {
		layer->menu_screen = NULL;
		fplinux_ui_menu_close(screen->ui);
	}
	if (screen == layer->wait.origin || screen == layer->wait.alert)
		drop_wait(layer, screen);
	destroy_screen(screen);
}

static struct fplinux_ui_screens *navigation_layer(struct fplinux_ui *ui)
{
	struct fplinux_ui_screens *layer;

	if (!ui) {
		errno = EINVAL;
		return NULL;
	}
	layer = *fplinux_ui_screens_slot(ui);
	if (!layer) {
		errno = ENOENT;
		return NULL;
	}
	if (layer->closing || layer->navigating || layer->row_provider_depth) {
		errno = EBUSY;
		return NULL;
	}
	return layer;
}

static bool available_screen(struct fplinux_ui_screens *layer,
			     const struct fplinux_ui_screen *screen)
{
	if (!screen) {
		errno = EINVAL;
		return false;
	}
	if (!fplinux_ui_screen_is_alive(layer, screen)) {
		errno = ENOENT;
		return false;
	}
	if (screen->stacked || layer->transient == screen) {
		errno = EBUSY;
		return false;
	}
	return true;
}

static void close_transient(struct fplinux_ui_screens *layer)
{
	struct fplinux_ui_screen *screen = layer->transient;
	void (*cancel)(struct fplinux_ui *ui, void *context) =
		layer->cancel_transient;
	void *context = layer->transient_context;

	if (!screen)
		return;
	layer->transient = NULL;
	layer->cancel_transient = NULL;
	layer->transient_context = NULL;
	fplinux_ui_screen_delete(screen);
	if (cancel)
		cancel(layer->ui, context);
}

static void begin_navigation(struct fplinux_ui_screens *layer)
{
	layer->navigating = true;
	fplinux_ui_cancel_key_repeat(layer->ui, NULL);
	if (fplinux_ui_menu_is_open(layer->ui)) {
		layer->menu_screen = NULL;
		fplinux_ui_menu_close(layer->ui);
	}
	close_transient(layer);
}

static void finish_navigation(struct fplinux_ui_screens *layer)
{
	restack(layer);
	fplinux_ui_screen_refresh(layer);
	layer->navigating = false;
	wait_stack_changed(layer);
}

static void push_screen(struct fplinux_ui_screens *layer,
			struct fplinux_ui_screen *screen)
{
	screen->options_return.owner = NULL;
	screen->below = layer->top;
	screen->stacked = true;
	layer->top = screen;
	lv_obj_move_to_index(screen->root, -1);
}

bool fplinux_ui_screens_show_transient(
	struct fplinux_ui *ui, struct fplinux_ui_screen *screen,
	void (*cancel)(struct fplinux_ui *ui, void *context), void *context)
{
	struct fplinux_ui_screens *layer = navigation_layer(ui);

	if (!layer || !available_screen(layer, screen))
		return false;
	begin_navigation(layer);
	layer->transient = screen;
	layer->cancel_transient = cancel;
	layer->transient_context = context;
	lv_obj_move_to_index(screen->root, -1);
	finish_navigation(layer);
	return true;
}

void fplinux_ui_screens_close_transient(struct fplinux_ui *ui,
					struct fplinux_ui_screen *screen)
{
	struct fplinux_ui_screens *layer = *fplinux_ui_screens_slot(ui);

	if (!layer || layer->transient != screen)
		return;
	fplinux_ui_cancel_key_repeat(ui, NULL);
	close_transient(layer);
	restack(layer);
	fplinux_ui_screen_refresh(layer);
	wait_stack_changed(layer);
}

bool fplinux_ui_push(struct fplinux_ui *ui, struct fplinux_ui_screen *screen)
{
	struct fplinux_ui_screens *layer = navigation_layer(ui);

	if (!layer || !available_screen(layer, screen))
		return false;
	begin_navigation(layer);
	push_screen(layer, screen);
	finish_navigation(layer);
	return true;
}

bool fplinux_ui_navigate(struct fplinux_ui *ui,
			 struct fplinux_ui_screen *screen)
{
	struct options_return origin;
	struct fplinux_ui_screens *layer = navigation_layer(ui);

	if (!layer || !available_screen(layer, screen))
		return false;
	origin = layer->options_source;
	begin_navigation(layer);
	push_screen(layer, screen);
	if (origin.owner && screen->below == origin.owner &&
	    entry_current(origin.owner, &origin.entry))
		screen->options_return = origin;
	finish_navigation(layer);
	return true;
}

bool fplinux_ui_replace(struct fplinux_ui *ui, struct fplinux_ui_screen *screen)
{
	struct fplinux_ui_screen *old;
	struct fplinux_ui_screens *layer = navigation_layer(ui);

	if (!layer || !available_screen(layer, screen))
		return false;
	begin_navigation(layer);
	old = layer->top;
	if (old) {
		layer->top = old->below;
		old->below = NULL;
		old->stacked = false;
	}
	push_screen(layer, screen);
	if (old)
		fplinux_ui_screen_delete(old);
	finish_navigation(layer);
	return true;
}

bool fplinux_ui_pop(struct fplinux_ui *ui)
{
	struct fplinux_ui_screens *layer = navigation_layer(ui);
	struct fplinux_ui_screen *top = layer ? layer->top : NULL;

	if (!layer)
		return false;
	if (!top || !top->below) {
		errno = ENOENT;
		return false;
	}
	begin_navigation(layer);
	layer->top = top->below;
	top->below = NULL;
	top->stacked = false;
	fplinux_ui_screen_delete(top);
	finish_navigation(layer);
	return true;
}

bool fplinux_ui_pop_to(struct fplinux_ui *ui, struct fplinux_ui_screen *screen)
{
	struct fplinux_ui_screens *layer = navigation_layer(ui);
	struct fplinux_ui_screen *target;

	if (!layer)
		return false;
	for (target = layer->top; target && target != screen;
	     target = target->below)
		;
	if (!target) {
		errno = ENOENT;
		return false;
	}
	begin_navigation(layer);
	while (layer->top != target) {
		struct fplinux_ui_screen *top = layer->top;

		layer->top = top->below;
		top->below = NULL;
		top->stacked = false;
		fplinux_ui_screen_delete(top);
	}
	finish_navigation(layer);
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
	struct fplinux_ui_screens *layer = ui ? *fplinux_ui_screens_slot(ui) :
						NULL;
	struct options_return origin = { 0 };

	if (!ui)
		return;
	if (!layer) {
		fplinux_ui_back(ui);
		return;
	}
	if (!navigation_layer(ui))
		return;
	if (layer->transient) {
		fplinux_ui_screens_close_transient(ui, layer->transient);
		return;
	}
	if (layer && layer->top)
		origin = layer->top->options_return;
	if (!layer->top || !layer->top->below)
		fplinux_ui_back(ui);
	else if (fplinux_ui_pop(ui))
		restore_options(layer, &origin);
}

/* Waiting */

static void wait_step(lv_timer_t *timer);

/* Steps run from the loop's timers, never inside an application call. */
static void schedule_wait(struct fplinux_ui_screens *layer, uint32_t delay_ms)
{
	stop_timer(&layer->wait.timer);
	layer->wait.timer = lv_timer_create(wait_step, delay_ms, layer);
}

/* The origin, or its shown alert, is current: no other screen is above. */
static bool wait_on_top(const struct fplinux_ui_screens *layer)
{
	const struct screen_wait *wait = &layer->wait;

	return !layer->transient &&
	       (layer->top == wait->origin ||
		(wait->shown && layer->top == wait->alert));
}

static void show_wait_alert(struct fplinux_ui_screens *layer)
{
	struct screen_wait *wait = &layer->wait;

	begin_navigation(layer);
	push_screen(layer, wait->alert);
	wait->shown = true;
	wait->shown_ms = lv_tick_get();
	finish_navigation(layer);
}

/*
 * The alert leaves without its own frame: done's navigation, or the origin
 * when done changes nothing, is what the next frame shows.
 */
static void finish_wait(struct fplinux_ui_screens *layer)
{
	struct screen_wait wait = layer->wait;

	layer->wait = (struct screen_wait){ 0 };
	stop_timer(&wait.timer);
	if (layer->top == wait.alert) {
		fplinux_ui_cancel_key_repeat(layer->ui, NULL);
		layer->top = wait.alert->below;
		wait.alert->below = NULL;
		wait.alert->stacked = false;
	}
	fplinux_ui_screen_delete(wait.alert);
	if (wait.done)
		wait.done(layer->ui, wait.done_context);
	restack(layer);
	fplinux_ui_screen_refresh(layer);
}

static void wait_step(lv_timer_t *timer)
{
	struct fplinux_ui_screens *layer = lv_timer_get_user_data(timer);
	struct screen_wait *wait = &layer->wait;
	uint32_t elapsed;

	stop_timer(&wait->timer);
	if (layer->closing)
		return;
	if (wait->ending && wait->shown) {
		elapsed = lv_tick_elaps(wait->shown_ms);
		if (elapsed < WAIT_ALERT_MINIMUM_MS) {
			schedule_wait(layer, WAIT_ALERT_MINIMUM_MS - elapsed);
			return;
		}
	} else if (!wait->ending && !wait->shown) {
		elapsed = lv_tick_elaps(wait->begun_ms);
		if (elapsed < WAIT_ALERT_DELAY_MS) {
			schedule_wait(layer, WAIT_ALERT_DELAY_MS - elapsed);
			return;
		}
	}
	/* Otherwise wait_stack_changed() steps again once nothing is above. */
	if (!wait_on_top(layer))
		return;
	if (wait->ending)
		finish_wait(layer);
	else if (!wait->shown)
		show_wait_alert(layer);
}

/* A step that waited for the origin to be current may run now. */
static void wait_stack_changed(struct fplinux_ui_screens *layer)
{
	struct screen_wait *wait = &layer->wait;

	if (wait->origin && !wait->timer && (wait->ending || !wait->shown) &&
	    wait_on_top(layer))
		schedule_wait(layer, 0);
}

static void cancel_wait(struct fplinux_ui_screens *layer)
{
	struct screen_wait *wait = &layer->wait;
	fplinux_ui_wait_cb cancel = wait->cancel;

	if (!cancel)
		return;
	wait->cancel = NULL;
	fplinux_ui_screen_remove_command(wait->alert, WAIT_CANCEL_COMMAND);
	cancel(layer->ui, wait->cancel_context);
}

/* Navigation deleted the origin or the alert: the wait ends unfinished. */
static void drop_wait(struct fplinux_ui_screens *layer,
		      const struct fplinux_ui_screen *deleted)
{
	struct fplinux_ui_screen *alert = layer->wait.alert;

	stop_timer(&layer->wait.timer);
	layer->wait = (struct screen_wait){ 0 };
	if (alert != deleted && !alert->stacked)
		fplinux_ui_screen_delete(alert);
}

struct fplinux_ui_screen *fplinux_ui_wait_begin(struct fplinux_ui *ui,
						const char *title,
						const char *text,
						fplinux_ui_wait_cb cancel,
						void *context)
{
	const struct fplinux_ui_command stop = {
		.id = WAIT_CANCEL_COMMAND,
		.type = FPLINUX_UI_COMMAND_CANCEL,
	};
	struct fplinux_ui_screens *layer = navigation_layer(ui);
	struct fplinux_ui_screen *alert;

	if (!layer)
		return NULL;
	if (layer->wait.origin) {
		errno = EBUSY;
		return NULL;
	}
	/* First, as in navigation: a closed choice's callback may navigate. */
	fplinux_ui_cancel_key_repeat(ui, NULL);
	if (fplinux_ui_menu_is_open(ui)) {
		layer->menu_screen = NULL;
		fplinux_ui_menu_close(ui);
	}
	if (layer->transient)
		fplinux_ui_screens_close_transient(ui, layer->transient);
	if (!layer->top || layer->wait.origin) {
		errno = layer->top ? EBUSY : ENOENT;
		return NULL;
	}
	alert = fplinux_ui_alert_new(ui, FPLINUX_UI_ALERT_INFO, title, text, 0);
	if (!alert)
		return NULL;
	if (!fplinux_ui_alert_set_progress(alert, 0, 0) ||
	    (cancel && !fplinux_ui_screen_add_command(alert, &stop))) {
		int error = errno;

		fplinux_ui_screen_delete(alert);
		errno = error;
		return NULL;
	}
	/*
	 * The origin's own legends, also in an alert's listener, which runs
	 * before the screen below the alert has them back.
	 */
	fplinux_ui_screen_refresh(layer);
	layer->wait = (struct screen_wait){
		.origin = layer->top,
		.alert = alert,
		.begun_ms = lv_tick_get(),
		.cancel = cancel,
		.cancel_context = context,
	};
	memcpy(layer->wait.legends, layer->legends, sizeof(layer->legends));
	schedule_wait(layer, WAIT_ALERT_DELAY_MS);
	return alert;
}

bool fplinux_ui_wait_end(struct fplinux_ui *ui, fplinux_ui_wait_cb done,
			 void *context)
{
	struct fplinux_ui_screens *layer;

	if (!ui) {
		errno = EINVAL;
		return false;
	}
	layer = *fplinux_ui_screens_slot(ui);
	if (!layer || !layer->wait.origin || layer->wait.ending) {
		errno = ENOENT;
		return false;
	}
	layer->wait.ending = true;
	/* The work is over: a late Cancel has nothing left to stop. */
	layer->wait.cancel = NULL;
	layer->wait.done = done;
	layer->wait.done_context = context;
	schedule_wait(layer, 0);
	return true;
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
	fplinux_ui_screen_changed(context);
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

		fplinux_ui_screen_delete(screen);
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
	screen = fplinux_ui_screen_new(ui, SCREEN_LIST, title);
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
	struct fplinux_ui_screens *layer = screen->layer;
	bool result;

	(void)list;
	++layer->row_provider_depth;
	result = screen->virtual_row(screen->ui, screen, index, row,
				     screen->virtual_context);
	--layer->row_provider_depth;
	return result;
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
	screen = fplinux_ui_screen_new(ui, SCREEN_LIST, title);
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
	fplinux_ui_screen_changed(list);
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
	fplinux_ui_screen_item_commands_deleted(list, index, count);
	fplinux_ui_screen_changed(list);
}

bool fplinux_ui_list_set_value_details(struct fplinux_ui_screen *list)
{
	const struct fplinux_ui_profile_spec *spec;

	if (!is_stored_list(list)) {
		errno = EINVAL;
		return false;
	}
	if (fplinux_ui_listview_count(list->list)) {
		errno = EBUSY;
		return false;
	}
	spec = fplinux_ui_screen_spec(list);
	fplinux_ui_listview_set_inline_detail(list->list,
					      spec->width > spec->height);
	return true;
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
	fplinux_ui_screen_changed(list);
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
	fplinux_ui_screen_changed(list);
	return row;
}

bool fplinux_ui_list_delete(struct fplinux_ui_screen *list, size_t index)
{
	if (!is_stored_list(list) ||
	    !fplinux_ui_listview_remove(list->list, index)) {
		errno = EINVAL;
		return false;
	}
	fplinux_ui_screen_item_commands_deleted(list, index, 1);
	fplinux_ui_screen_changed(list);
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
	fplinux_ui_screen_clear_item_commands(list);
	fplinux_ui_screen_changed(list);
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

size_t fplinux_ui_list_get_first_visible(const struct fplinux_ui_screen *list)
{
	return is_list(list) ? fplinux_ui_listview_first_visible(list->list) :
			       NONE;
}

void fplinux_ui_list_set_first_visible(struct fplinux_ui_screen *list,
				       size_t index)
{
	if (is_list(list))
		fplinux_ui_listview_set_first_visible(list->list, index);
}

void fplinux_ui_list_set_focused(struct fplinux_ui_screen *list, size_t index)
{
	if (is_list(list))
		fplinux_ui_listview_select(list->list, index);
}

void fplinux_ui_list_set_focus_listener(struct fplinux_ui_screen *list,
					fplinux_ui_change_cb focused,
					void *context)
{
	if (!is_list(list))
		return;
	list->on_focus = focused;
	list->focus_context = context;
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
	fplinux_ui_screen_changed(list);
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
	fplinux_ui_screen_changed(list);
	return true;
}

bool fplinux_ui_list_set_icon(struct fplinux_ui_screen *list, size_t index,
			      const void *src,
			      enum fplinux_ui_list_icon_kind kind)
{
	int size = 0;
	lv_draw_buf_t *icon = NULL;

	if (!is_stored_list(list) ||
	    index >= fplinux_ui_listview_count(list->list) ||
	    (kind != FPLINUX_UI_LIST_ICON_IMAGE &&
	     kind != FPLINUX_UI_LIST_ICON_APP_TILE)) {
		errno = EINVAL;
		return false;
	}
	size = fplinux_ui_screen_spec(list)->row_icon_size;
	if (src && !(icon = fplinux_ui_image_decode(src, size, size, false)))
		return false;
	if (!fplinux_ui_listview_set_icon(list->list, index, icon, kind)) {
		if (icon)
			lv_draw_buf_destroy(icon);
		return false;
	}
	return true;
}

bool fplinux_ui_list_set_emphasis(struct fplinux_ui_screen *list, size_t index,
				  bool emphasis)
{
	if (!is_stored_list(list) ||
	    index >= fplinux_ui_listview_count(list->list)) {
		errno = EINVAL;
		return false;
	}
	return fplinux_ui_listview_set_emphasis(list->list, index, emphasis);
}

bool fplinux_ui_list_set_trailing_icon(struct fplinux_ui_screen *list,
				       size_t index, const void *src)
{
	if (!is_stored_list(list) ||
	    index >= fplinux_ui_listview_count(list->list)) {
		errno = EINVAL;
		return false;
	}
	return fplinux_ui_listview_set_trailing_icon(list->list, index, src);
}

bool fplinux_ui_list_set_trailing_value(struct fplinux_ui_screen *list,
					size_t index, const char *value)
{
	if (!is_stored_list(list) ||
	    index >= fplinux_ui_listview_count(list->list)) {
		errno = EINVAL;
		return false;
	}
	return fplinux_ui_listview_set_trailing_value(list->list, index, value);
}

bool fplinux_ui_list_set_level(struct fplinux_ui_screen *list, size_t index,
			       int32_t minimum, int32_t maximum, int32_t value)
{
	if (!is_stored_list(list) ||
	    index >= fplinux_ui_listview_count(list->list)) {
		errno = EINVAL;
		return false;
	}
	return fplinux_ui_listview_set_level(list->list, index, minimum,
					     maximum, value);
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
	fplinux_ui_screen_report(screen, index);
}

/* The box holds count rows, at most the surface less the alert margins. */
static void size_popup_choice(struct fplinux_ui_screen *list, size_t count)
{
	const struct fplinux_ui_profile_spec *spec =
		fplinux_ui_screen_spec(list);
	int32_t height = root_height(list) - 2 * spec->alert_margin;
	int32_t title = list->title ? spec->header_height : 0;
	int32_t inset = 2 * spec->alert_border;

	if (height >= title + inset) {
		size_t rows =
			(size_t)(height - title - inset + spec->divider_width) /
			spec->row_height;

		if (rows > count)
			rows = count;
		if (rows)
			height = title + inset +
				 (int32_t)rows * spec->row_height -
				 spec->divider_width;
	}
	lv_obj_set_size(list->box,
			fplinux_ui_width(list->ui) - 2 * spec->alert_margin,
			height);
}

/* A popup choice is a framed list over its retained, dimmed form. */
void fplinux_ui_screen_frame_popup_choice(struct fplinux_ui_screen *list,
					  size_t count)
{
	const struct fplinux_ui_profile_spec *spec =
		fplinux_ui_screen_spec(list);
	const struct fplinux_ui_theme *theme = fplinux_ui_theme(list->ui);

	list->popup_choice = true;
	lv_obj_set_height(list->root, root_height(list));
	fplinux_ui_scrim_style(list->ui, list->root);
	list->box = fplinux_ui_screen_plain_object(list->root);
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

/* The bar of a gauge or a busy alert: a border track and a focus fill. */
lv_obj_t *fplinux_ui_screen_make_bar(lv_obj_t *parent, struct fplinux_ui *ui)
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

bool fplinux_ui_screen_is_ok_key(const struct fplinux_ui_key *key)
{
	return key->code == KEY_OK || key->code == KEY_ENTER ||
	       key->code == KEY_KPENTER;
}

bool fplinux_ui_screen_is_right_key(const struct fplinux_ui_key *key)
{
	return fplinux_ui_is_softkey(key, false) || key->code == KEY_ESC;
}

static bool is_left_key(const struct fplinux_ui_key *key)
{
	return fplinux_ui_is_softkey(key, true) ||
	       (key->source == FPLINUX_UI_KEY_KEYBOARD &&
		key->code == KEY_COMPOSE);
}

/* Alerts and canvases */

/*
 * An alert without text has no line for it: the box, as high as its
 * content, holds the title and any indicator below it.
 */
static void show_progress(struct fplinux_ui_screen *alert)
{
	int32_t width;
	int minimum, value;

	if (!alert->maximum || !alert->indicator)
		return;
	width = lv_obj_get_content_width(alert->indicator);
	value = alert->progress_value;
	if (value > 0 && width > 0) {
		/* LVGL omits indicator areas shorter than two pixels. */
		minimum = (int)((2LL * alert->maximum + width - 1) / width);
		if (value < minimum)
			value = minimum < alert->maximum ? minimum :
							   alert->maximum;
	}
	lv_bar_set_value(alert->indicator, value, LV_ANIM_OFF);
}

static void progress_size(lv_event_t *event)
{
	show_progress(lv_event_get_user_data(event));
}

static void layout_alert(struct fplinux_ui_screen *alert)
{
	const struct fplinux_ui_profile_spec *spec =
		fplinux_ui_screen_spec(alert);
	int32_t available =
		alert_box_max_height(alert->ui) - 2 * spec->alert_border;
	int32_t line, space;
	lv_obj_t *parts[] = { alert->band, alert->indicator_box,
			      alert->progress_text };

	lv_obj_set_width(alert->band, lv_pct(100));
	lv_obj_set_x(alert->band_label, spec->alert_padding);
	lv_obj_set_width(alert->band_label, alert_box_width(alert->ui) -
						    2 * spec->alert_border -
						    2 * spec->alert_padding);
	lv_obj_update_layout(alert->box);
	for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); ++i) {
		if (!parts[i] || lv_obj_is_hidden(parts[i]))
			continue;
		available -= lv_obj_get_height(parts[i]);
	}
	/*
	 * A message that scrolls shows whole lines, at either end and between,
	 * as fplinux_ui_scroll_key() steps by lines.
	 */
	space = lv_obj_get_style_text_line_space(alert->message, 0);
	line = lv_font_get_line_height(
		       lv_obj_get_style_text_font(alert->message, 0)) +
	       space;
	available -= 2 * spec->alert_padding;
	if (available > line)
		available = (available + space) / line * line - space;
	lv_obj_set_style_max_height(
		alert->message_view,
		(available > 0 ? available : 1) + 2 * spec->alert_padding, 0);
	fplinux_ui_scrollbar_caps(
		alert->message_view, false,
		(alert->indicator_box &&
		 !lv_obj_is_hidden(alert->indicator_box)) ||
			(alert->progress_text &&
			 !lv_obj_is_hidden(alert->progress_text)));
	fplinux_ui_scrollbar_layout(alert->message_view);
}

static void show_alert_text(struct fplinux_ui_screen *alert, const char *text)
{
	lv_label_set_text(alert->message, text ? text : "");
	lv_obj_set_hidden(alert->message, !text || !text[0]);
	lv_obj_set_hidden(alert->message_view, !text || !text[0]);
	layout_alert(alert);
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

	if ((unsigned int)kind > FPLINUX_UI_ALERT_ERROR) {
		errno = EINVAL;
		return NULL;
	}
	alert = fplinux_ui_screen_new(ui, SCREEN_ALERT, title);
	if (!alert)
		return NULL;
	theme = fplinux_ui_theme(ui);
	spec = fplinux_ui_spec(ui);
	alert->alert_kind = kind;
	alert->timeout_ms = timeout_ms;
	/* The kind shows in the frame and, for a warning or error, the title. */
	colour = fplinux_ui_alert_color(ui, kind);
	alert->box = fplinux_ui_screen_plain_object(alert->root);
	lv_obj_set_size(alert->box, alert_box_width(ui), LV_SIZE_CONTENT);
	lv_obj_set_style_max_height(alert->box, alert_box_max_height(ui), 0);
	lv_obj_set_flex_flow(alert->box, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_bg_color(alert->box, theme->popup, 0);
	lv_obj_set_style_bg_opa(alert->box, LV_OPA_COVER, 0);
	lv_obj_set_style_border_color(alert->box, colour, 0);
	lv_obj_set_style_border_width(alert->box, spec->alert_border, 0);
	lv_obj_set_scrollable(alert->box, false);
	lv_obj_center(alert->box);
	build_band(alert, alert->box);
	lv_obj_set_style_bg_color(alert->band, theme->popup, 0);
	if (kind == FPLINUX_UI_ALERT_WARNING || kind == FPLINUX_UI_ALERT_ERROR)
		lv_obj_set_style_text_color(alert->band_label, colour, 0);
	show_alert_title(alert);
	alert->message_view = fplinux_ui_screen_plain_object(alert->box);
	lv_obj_set_size(alert->message_view, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_pad_all(alert->message_view, spec->alert_padding, 0);
	lv_obj_set_flex_flow(alert->message_view, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_scrollable(alert->message_view, true);
	fplinux_ui_screen_style_scrollbar(alert->message_view, ui);
	alert->message = lv_label_create(alert->message_view);
	lv_obj_set_width(alert->message, lv_pct(100));
	lv_label_set_long_mode(alert->message, LV_LABEL_LONG_MODE_WRAP);
	show_alert_text(alert, text);
	return alert;
}

static lv_obj_t *make_spinner(lv_obj_t *parent, struct fplinux_ui_screen *alert)
{
	struct fplinux_ui *ui = alert->ui;
	const struct fplinux_ui_theme *theme = fplinux_ui_theme(ui);
	const struct fplinux_ui_profile_spec *spec = fplinux_ui_spec(ui);
	const struct fplinux_ui_activity_entry *entry = activity_source(alert);
	lv_obj_t *box, *track;

	if (!entry) {
		errno = EINVAL;
		return NULL;
	}
	box = fplinux_ui_screen_plain_object(parent);
	lv_obj_set_size(box, spec->spinner_size, spec->spinner_size);
	lv_obj_center(box);
	track = lv_image_create(box);
	lv_image_set_src(track, entry->track);
	lv_obj_set_style_image_recolor(track, theme->border, 0);
	lv_obj_set_style_image_recolor_opa(track, LV_OPA_COVER, 0);
	alert->activity_indicator = lv_image_create(box);
	lv_image_set_src(alert->activity_indicator, entry->image);
	lv_obj_set_style_image_recolor(alert->activity_indicator, theme->focus,
				       0);
	lv_obj_set_style_image_recolor_opa(alert->activity_indicator,
					   LV_OPA_COVER, 0);
	return box;
}

bool fplinux_ui_alert_set_progress(struct fplinux_ui_screen *alert, int value,
				   int maximum)
{
	const struct fplinux_ui_profile_spec *spec;

	if (!alert || alert->kind != SCREEN_ALERT || maximum < 0) {
		errno = EINVAL;
		return false;
	}
	spec = fplinux_ui_screen_spec(alert);
	if (!alert->indicator_box) {
		alert->indicator_box =
			fplinux_ui_screen_plain_object(alert->box);
		lv_obj_set_width(alert->indicator_box, lv_pct(100));
		lv_obj_set_style_pad_all(alert->indicator_box,
					 spec->alert_padding, 0);
		if (alert->progress_text)
			lv_obj_move_to_index(
				alert->indicator_box,
				lv_obj_get_index(alert->progress_text));
	}
	if (!alert->indicator || !alert->maximum != !maximum) {
		lv_obj_clean(alert->indicator_box);
		alert->activity_indicator = NULL;
		lv_obj_set_height(alert->indicator_box,
				  (maximum ? spec->bar_height :
					     spec->spinner_size) +
					  2 * spec->alert_padding);
		alert->indicator =
			maximum ?
				fplinux_ui_screen_make_bar(alert->indicator_box,
							   alert->ui) :
				make_spinner(alert->indicator_box, alert);
		if (!alert->indicator)
			return false;
		if (maximum)
			lv_obj_add_event_cb(alert->indicator, progress_size,
					    LV_EVENT_SIZE_CHANGED, alert);
	}
	if (maximum) {
		lv_bar_set_range(alert->indicator, 0, maximum);
		alert->progress_value = value < 0	? 0 :
					value > maximum ? maximum :
							  value;
	}
	alert->busy = true;
	alert->maximum = maximum;
	show_progress(alert);
	run_alert_timer(alert, fplinux_ui_screen_is_top(alert) &&
				       alert->layer->visible);
	layout_alert(alert);
	fplinux_ui_screen_changed(alert);
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
	lv_obj_scroll_to_y(alert->message_view, 0, LV_ANIM_OFF);
	return true;
}

bool fplinux_ui_alert_set_footer(struct fplinux_ui_screen *alert,
				 const char *text)
{
	if (!alert || alert->kind != SCREEN_ALERT) {
		errno = EINVAL;
		return false;
	}
	if (!alert->progress_text) {
		if (!text || !text[0])
			return true;
		alert->progress_text = lv_label_create(alert->box);
		if (!alert->progress_text) {
			errno = ENOMEM;
			return false;
		}
		lv_obj_set_width(alert->progress_text, lv_pct(100));
		lv_obj_set_style_pad_all(
			alert->progress_text,
			fplinux_ui_screen_spec(alert)->alert_padding, 0);
		lv_label_set_long_mode(alert->progress_text,
				       LV_LABEL_LONG_MODE_WRAP);
	}
	lv_label_set_text(alert->progress_text, text ? text : "");
	lv_obj_set_hidden(alert->progress_text, !text || !text[0]);
	layout_alert(alert);
	return true;
}

struct fplinux_ui_screen *fplinux_ui_canvas_new(struct fplinux_ui *ui,
						const char *title)
{
	return fplinux_ui_screen_new(ui, SCREEN_CANVAS, title);
}

lv_obj_t *fplinux_ui_canvas_object(struct fplinux_ui_screen *canvas)
{
	return canvas && canvas->kind == SCREEN_CANVAS ? canvas->content : NULL;
}

void fplinux_ui_canvas_set_fullscreen_bar(struct fplinux_ui_screen *canvas,
					  bool enabled)
{
	if (!canvas || canvas->kind != SCREEN_CANVAS ||
	    canvas->canvas_fullscreen_bar == enabled)
		return;
	canvas->canvas_fullscreen_bar = enabled;
	resize_screen(canvas);
	fplinux_ui_screen_changed(canvas);
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

void fplinux_ui_canvas_set_active_listener(struct fplinux_ui_screen *canvas,
					   fplinux_ui_canvas_active_cb callback,
					   void *context)
{
	if (!canvas || canvas->kind != SCREEN_CANVAS)
		return;
	canvas->on_canvas_active = callback;
	canvas->canvas_active_context = context;
}

void fplinux_ui_screen_set_key_filter(struct fplinux_ui_screen *screen,
				      fplinux_ui_screen_key_filter_cb filter,
				      void *context)
{
	if (!screen)
		return;
	screen->key_filter = filter;
	screen->key_filter_context = context;
}

void fplinux_ui_canvas_set_keyboard_keys(struct fplinux_ui_screen *canvas,
					 bool raw)
{
	if (canvas && canvas->kind == SCREEN_CANVAS)
		canvas->keyboard_raw = raw;
}

void fplinux_ui_canvas_set_key_repeat(struct fplinux_ui_screen *canvas,
				      enum fplinux_ui_canvas_repeat policy)
{
	if (!canvas || canvas->kind != SCREEN_CANVAS ||
	    (unsigned int)policy > FPLINUX_UI_CANVAS_REPEAT_KEYMAP)
		return;
	if (canvas->key_repeat != policy)
		fplinux_ui_cancel_key_repeat(canvas->ui, canvas);
	canvas->key_repeat = policy;
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
		size_t index = screen->order[i];
		const struct candidate *candidate = &screen->candidates[index];

		entries[i] = (struct menu_entry){
			.id = candidate->command->id,
			.item = candidate->item,
			.clear = candidate->clear,
			.index = focused_index(screen),
		};
		items[i] = (struct fplinux_ui_menu_item){
			.action = (int)i,
			.text = command_label(screen, index),
			.icon = command_icon(screen, candidate),
		};
	}
	layer->menu_entry_count = count;
	layer->menu_screen = screen;
	if (!fplinux_ui_menu_open(screen->ui, FPLINUX_UI_MENU_ORIGIN_LEFT, NULL,
				  items, count, options_chosen, screen))
		layer->menu_screen = NULL;
	free(items);
}

enum press_result {
	PRESS_COMMAND,
	PRESS_ELEMENT,
	PRESS_REPEATABLE,
	/* The modal editor owns arrow releases as well as their repeats. */
	PRESS_EDITOR_NAVIGATION,
};

static enum press_result canvas_result(const struct fplinux_ui_screen *screen,
				       const struct fplinux_ui_key *key)
{
	if (!screen->canvas_active ||
	    (!screen->canvas_key && !screen->key_filter))
		return PRESS_ELEMENT;
	if (screen->key_repeat == FPLINUX_UI_CANVAS_REPEAT_KEYMAP ||
	    (screen->key_repeat == FPLINUX_UI_CANVAS_REPEAT_NAVIGATION &&
	     fplinux_ui_is_direction_key(key)))
		return PRESS_REPEATABLE;
	return PRESS_ELEMENT;
}

/* Capture the repeat policy before a callback that may delete the screen. */
static enum press_result canvas_key(struct fplinux_ui_screen *screen,
				    const struct fplinux_ui_key *key)
{
	enum press_result result;

	if (screen->kind != SCREEN_CANVAS)
		return PRESS_COMMAND;
	result = canvas_result(screen, key);
	if (screen->canvas_key)
		screen->canvas_key(screen->ui, screen, key,
				   screen->canvas_context);
	return result;
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
		fplinux_ui_form_ok(screen);
}

/* Commands and the modal editor own releases; elements retain their route. */
static enum press_result screen_press(struct fplinux_ui_screen *screen,
				      const struct fplinux_ui_key *key)
{
	struct fplinux_ui_text *text = fplinux_ui_form_focused_text(screen);
	struct soft_keys soft;
	bool navigation = fplinux_ui_is_direction_key(key);
	bool taken;

	if (screen->form.editing) {
		fplinux_ui_form_date_key(screen, key);
		return navigation ? PRESS_EDITOR_NAVIGATION : PRESS_COMMAND;
	}
	assign_keys(screen, &soft);
	if (text &&
	    !(soft.keep_right && fplinux_ui_screen_is_right_key(key) &&
	      !fplinux_ui_text_is_picker_open(text)) &&
	    fplinux_ui_text_key(text, key))
		return PRESS_REPEATABLE;
	if (screen->keyboard_raw && key->source == FPLINUX_UI_KEY_KEYBOARD &&
	    (key->code == KEY_ESC || fplinux_ui_screen_is_ok_key(key)))
		return canvas_key(screen, key);
	if (fplinux_ui_screen_is_right_key(key)) {
		if (soft.keys.right == NONE)
			return canvas_key(screen, key);
		fire_candidate(screen, soft.keys.right);
		return PRESS_COMMAND;
	}
	if (is_left_key(key)) {
		if (soft.keys.left != NONE)
			fire_candidate(screen, soft.keys.left);
		else if (soft.keys.options)
			open_options(screen, &soft);
		else
			return canvas_key(screen, key);
		return PRESS_COMMAND;
	}
	if (fplinux_ui_screen_is_ok_key(key)) {
		if (soft.action)
			element_ok(screen);
		else if (soft.keys.ok != NONE)
			fire_candidate(screen, soft.keys.ok);
		else
			return canvas_key(screen, key);
		return PRESS_COMMAND;
	}
	if (screen->kind == SCREEN_LIST) {
		size_t before = fplinux_ui_listview_selected(screen->list);
		size_t focused;
		fplinux_ui_change_cb callback = screen->on_focus;
		void *context = screen->focus_context;
		struct fplinux_ui *ui = screen->ui;

		taken = fplinux_ui_listview_key(screen->list, key);
		focused = fplinux_ui_listview_selected(screen->list);
		if (focused != before && callback)
			callback(ui, screen, focused, context);
	} else if (screen->kind == SCREEN_FORM)
		taken = fplinux_ui_form_key(screen, key);
	else if (screen->kind == SCREEN_ALERT)
		taken = fplinux_ui_scroll_key(screen->message_view, key);
	else
		return canvas_key(screen, key);
	return taken && navigation ? PRESS_REPEATABLE : PRESS_ELEMENT;
}

static void screen_release(struct fplinux_ui_screen *screen,
			   const struct fplinux_ui_key *key)
{
	struct fplinux_ui_text *text = fplinux_ui_form_focused_text(screen);

	if (text)
		fplinux_ui_text_key(text, key);
	else if (screen->kind == SCREEN_LIST)
		fplinux_ui_listview_key(screen->list, key);
	else
		canvas_key(screen, key);
}

static struct fplinux_ui_screen *filter_owner(struct fplinux_ui_screens *layer,
					      bool keyboard, unsigned int code,
					      unsigned char bit)
{
	for (struct fplinux_ui_screen *screen = layer->all; screen;
	     screen = screen->next)
		if (screen->filtered[keyboard][code / 8] & bit)
			return screen;
	return NULL;
}

/* Record ownership before the callback, which may replace its own screen. */
static bool filter_press(struct fplinux_ui_screen *screen,
			 const struct fplinux_ui_key *key, bool keyboard,
			 unsigned char bit)
{
	struct fplinux_ui_screens *layer = screen->layer;

	if (!screen->key_filter)
		return false;
	layer->filtered[keyboard][key->code / 8] |= bit;
	screen->filtered[keyboard][key->code / 8] |= bit;
	if (screen->key_filter(screen->ui, screen, key,
			       screen->key_filter_context))
		return true;
	layer->filtered[keyboard][key->code / 8] &= (unsigned char)~bit;
	if (fplinux_ui_screen_is_alive(layer, screen))
		screen->filtered[keyboard][key->code / 8] &=
			(unsigned char)~bit;
	return false;
}

enum fplinux_ui_key_route
fplinux_ui_screens_key(struct fplinux_ui_screens *layer,
		       const struct fplinux_ui_key *key)
{
	bool keyboard = key->source == FPLINUX_UI_KEY_KEYBOARD;
	unsigned char bit = (unsigned char)(1U << (key->code % 8));
	unsigned char *used;
	unsigned char *filtered;
	struct fplinux_ui_screen *screen;
	enum press_result result = PRESS_ELEMENT;

	if (!fplinux_ui_screens_active(layer))
		return FPLINUX_UI_KEY_UNHANDLED;
	if (key->code >= KEY_CNT)
		return FPLINUX_UI_KEY_HANDLED;
	used = &layer->used[keyboard][key->code / 8];
	filtered = &layer->filtered[keyboard][key->code / 8];
	if (*filtered & bit) {
		screen = filter_owner(layer, keyboard, key->code, bit);
		if (!key->pressed) {
			*filtered &= (unsigned char)~bit;
			if (screen)
				screen->filtered[keyboard][key->code / 8] &=
					(unsigned char)~bit;
		}
		if (screen &&
		    (!key->pressed ||
		     (screen == fplinux_ui_screens_active(layer) &&
		      layer->visible && !fplinux_ui_menu_is_open(layer->ui)))) {
			if (key->pressed)
				result = canvas_result(screen, key);
			if (screen->key_filter)
				screen->key_filter(screen->ui, screen, key,
						   screen->key_filter_context);
		}
	} else if (!key->pressed) {
		if (*used & bit)
			*used &= (unsigned char)~bit;
		else {
			screen = fplinux_ui_screens_active(layer);
			if ((!screen->key_filter ||
			     !screen->key_filter(layer->ui, screen, key,
						 screen->key_filter_context)) &&
			    fplinux_ui_screens_active(layer) == screen)
				screen_release(screen, key);
		}
	} else if (fplinux_ui_screens_active(layer) == layer->wait.origin) {
		/* A waiting origin takes only Back, which cancels the work. */
		if (fplinux_ui_screen_is_right_key(key))
			cancel_wait(layer);
		*used |= bit;
	} else {
		screen = fplinux_ui_screens_active(layer);
		result = canvas_result(screen, key);
		if (!filter_press(screen, key, keyboard, bit))
			result = fplinux_ui_screens_active(layer) == screen ?
					 screen_press(screen, key) :
					 PRESS_COMMAND;
		else if (fplinux_ui_screens_active(layer) != screen ||
			 !layer->visible || fplinux_ui_menu_is_open(layer->ui))
			result = PRESS_ELEMENT;
		if (result == PRESS_COMMAND ||
		    result == PRESS_EDITOR_NAVIGATION)
			*used |= bit;
	}
	fplinux_ui_screen_refresh(layer);
	return result == PRESS_REPEATABLE || result == PRESS_EDITOR_NAVIGATION ?
		       FPLINUX_UI_KEY_REPEATABLE :
		       FPLINUX_UI_KEY_HANDLED;
}

/* Whether an options menu entry still names a command the screen offers. */
static bool entry_current(const struct fplinux_ui_screen *screen,
			  const struct menu_entry *entry)
{
	if (entry->clear) {
		struct fplinux_ui_text *text =
			fplinux_ui_form_focused_text(screen);

		return focused_index(screen) == entry->index && text &&
		       !fplinux_ui_text_is_read_only(text) &&
		       !fplinux_ui_text_is_empty(text);
	}
	if (!entry->item)
		return find_command(screen, entry->id) != NONE;
	return find_item_command(screen, entry->index, entry->id) != NONE &&
	       !(screen->kind == SCREEN_FORM &&
		 fplinux_ui_form_is_hidden(screen, entry->index));
}

/*
 * The screen of the open options menu changed: the menu closes without a
 * choice when an entry's command was removed or its row deleted or hidden.
 */
static void options_changed(struct fplinux_ui_screen *screen)
{
	struct fplinux_ui_screens *layer = screen->layer;

	if (layer->options_source.owner == screen &&
	    !entry_current(screen, &layer->options_source.entry))
		layer->options_source.owner = NULL;
	for (struct fplinux_ui_screen *child = layer->all; child;
	     child = child->next)
		if (child->options_return.owner == screen &&
		    !entry_current(screen, &child->options_return.entry))
			child->options_return.owner = NULL;
	if (layer->menu_screen != screen)
		return;
	for (size_t i = 0; i < layer->menu_entry_count; ++i)
		if (!entry_current(screen, &layer->menu_entries[i])) {
			layer->menu_screen = NULL;
			fplinux_ui_menu_close(screen->ui);
			return;
		}
}

/* Captured before the raw popup closes and releases its list view. */
void fplinux_ui_screens_menu_position(
	struct fplinux_ui_screens *layer,
	const struct fplinux_ui_menu_position *position)
{
	if (layer && layer->menu_screen)
		layer->menu_position = *position;
}

static bool same_entry(const struct menu_entry *first,
		       const struct menu_entry *second)
{
	return first->id == second->id && first->item == second->item &&
	       first->clear == second->clear &&
	       (!(first->item || first->clear) ||
		first->index == second->index);
}

static void restore_options(struct fplinux_ui_screens *layer,
			    const struct options_return *origin)
{
	struct fplinux_ui_screen *owner = origin->owner;
	struct fplinux_ui_menu_position position = origin->position;
	struct soft_keys soft;

	if (!owner || fplinux_ui_screens_active(layer) != owner ||
	    !fplinux_ui_screen_is_alive(layer, owner) ||
	    !entry_current(owner, &origin->entry) ||
	    fplinux_ui_menu_is_open(layer->ui))
		return;
	assign_keys(owner, &soft);
	if (!soft.keys.options)
		return;
	open_options(owner, &soft);
	if (layer->menu_screen != owner)
		return;
	for (size_t index = 0; index < layer->menu_entry_count; ++index)
		if (same_entry(&layer->menu_entries[index], &origin->entry)) {
			position.selected = index;
			fplinux_ui_menu_set_position(layer->ui, &position);
			return;
		}
	/* The command still exists but no longer belongs in Options. */
	fplinux_ui_menu_close(layer->ui);
}

/* The options menu reports here through the runtime. */
static void options_chosen(struct fplinux_ui *ui, int action, void *context)
{
	/* Deleting a screen closes its menu without a choice: screen lives. */
	struct fplinux_ui_screen *screen = context;
	struct fplinux_ui_screens *layer = screen->layer;

	(void)ui;
	if (action >= 0 && (size_t)action < layer->menu_entry_count) {
		struct menu_entry entry = layer->menu_entries[action];

		layer->options_source = (struct options_return){
			.owner = screen,
			.entry = entry,
			.position = layer->menu_position,
		};
		layer->options_source.position.selected = (size_t)action;
		if (entry.clear)
			clear_focused_text(screen);
		else if (entry.item)
			fire_item(screen, entry.index, entry.id);
		else
			fire(screen, entry.id);
		layer->options_source.owner = NULL;
	}
	fplinux_ui_screen_refresh(layer);
}

void fplinux_ui_screens_set_visible(struct fplinux_ui_screens *layer,
				    bool visible)
{
	if (!layer)
		return;
	if (!visible)
		fplinux_ui_cancel_key_repeat(layer->ui, NULL);
	layer->visible = visible;
	restack(layer);
}

void fplinux_ui_screens_suspend(struct fplinux_ui_screens *layer)
{
	struct fplinux_ui_screen *screen = fplinux_ui_screens_active(layer);

	if (!screen || layer->closing)
		return;
	/* The menu separately retains which raw Canvas fields held focus. */
	if (screen->kind == SCREEN_CANVAS)
		canvas_activation(screen, false);
	else
		set_active(screen, false);
}

void fplinux_ui_screens_reclaim(struct fplinux_ui_screens *layer)
{
	if (!layer)
		return;
	/* Any menu closed, also by fplinux_ui_menu_close(): Options is not open. */
	layer->menu_screen = NULL;
	if (fplinux_ui_screens_active(layer) && !layer->closing)
		set_active(fplinux_ui_screens_active(layer), true);
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
 * fplinux_ui_screen_frame_popup_choice() size each screen; a virtual list sees its new view
 * through LVGL's size change. Titles and the canvas's drawing are left as
 * they are: the application lays out a canvas on FPLINUX_UI_EVENT_RESIZE.
 */
static void resize_screen(struct fplinux_ui_screen *screen)
{
	struct fplinux_ui *ui = screen->ui;

	lv_obj_set_y(screen->root, screen_top(screen));
	lv_obj_set_size(screen->root, fplinux_ui_width(ui),
			root_height(screen));
	if (screen->kind == SCREEN_ALERT) {
		lv_obj_set_width(screen->box, alert_box_width(ui));
		lv_obj_set_style_max_height(screen->box,
					    alert_box_max_height(ui), 0);
		layout_alert(screen);
		if (screen->title_active)
			fplinux_ui_title_scroll(screen->band_label);
		return;
	}
	if (screen->popup_choice)
		size_popup_choice(screen,
				  fplinux_ui_listview_count(screen->list));
	size_frame(screen);
	lv_obj_update_layout(screen->root);
	if (screen->kind == SCREEN_FORM)
		fplinux_ui_form_keep_focus_visible(screen);
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
	update_legend_owner(layer);
}

void fplinux_ui_screens_delete(struct fplinux_ui_screens *layer)
{
	struct fplinux_ui_screen *screen = layer ? layer->all : NULL;

	if (!layer)
		return;
	layer->closing = true;
	layer->menu_screen = NULL;
	layer->transient = NULL;
	stop_timer(&layer->wait.timer);
	layer->all = NULL;
	while (screen) {
		struct fplinux_ui_screen *next = screen->next;

		destroy_screen(screen);
		screen = next;
	}
	free(layer->menu_entries);
	free(layer);
}
