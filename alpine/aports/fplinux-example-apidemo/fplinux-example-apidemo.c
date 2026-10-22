/* SPDX-License-Identifier: 0BSD */
/*
 * API Demos: the start page, the catalogue and the helpers every demo uses.
 *
 * The start page is the raw LVGL screen of fplinux-ui.h: the title band of
 * fplinux_ui_header(), the system text field to search the demos, a text
 * that Up and Down scroll, legends from fplinux_ui_set_softkeys() and an
 * options menu from fplinux_ui_menu_open(). The runtime gives key events to
 * the fplinux_ui_open() callback only while the screen stack is empty, and
 * fplinux_ui_pop() never removes the last screen, so the raw screen shows
 * until OK pushes the catalogue; Back from the catalogue then leaves the
 * application, as fplinux_ui_go_back() does from a first screen.
 */
#include "apidemo.h"
#include "fplinux-cli.h"

#include <fplinux/fplinux-ui-session.h>

#include <errno.h>
#include <linux/input-event-codes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PROGRAM "fplinux-example-apidemo"
/* The longest search the field takes, in characters. */
#define SEARCH_CHARS 24
/* Room for a search folded to lower case: UTF-8 takes up to 4 bytes. */
#define SEARCH_BYTES (4 * SEARCH_CHARS + 1)
#define MESSAGE_BYTES 1024

/* Actions of the start page's options menu. */
enum start_action {
	START_OPEN = 1,
	START_CLEAR,
	START_KEYS,
	START_ABOUT,
	START_EXIT,
};

/* What the start page's text shows. */
enum start_text {
	TEXT_ABOUT,
	TEXT_KEYS,
	TEXT_NOTHING_FOUND,
};

/* The categories in catalogue order. */
static const struct apidemo_category *const categories[] = {
	&apidemo_lists,	    &apidemo_forms,	&apidemo_alerts,
	&apidemo_text,	    &apidemo_commands,	&apidemo_canvas,
	&apidemo_frame,	    &apidemo_softkeys,	&apidemo_look,
	&apidemo_locale,    &apidemo_lifecycle, &apidemo_lvgl,
	&apidemo_vibration, &apidemo_sound,
};

/* Every entry in all categories, enough for a search that matches all. */
#define ENTRY_LIMIT 512

struct start_page {
	/* The title band of fplinux_ui_header(). */
	lv_obj_t *band;
	struct fplinux_ui_text *field;
	/* The scrolling container of the text below the field, its top and
	 * the text. */
	lv_obj_t *notes;
	int32_t notes_top;
	lv_obj_t *note;
	/* The legends last sent, so that they are sent only when they change. */
	const char *legends[3];
	/* The catalogue is on the stack: the raw screen no longer gets keys. */
	bool left;
};

struct apidemo_main {
	/* First, so that a struct apidemo pointer is also this one. */
	struct apidemo demo;
	struct start_page start;
	/* The entries of the search results list, by row. */
	const struct apidemo_entry *found[ENTRY_LIMIT];
	size_t found_count;
};

/* Helpers for the demos (apidemo.h) */

const struct fplinux_ui_command apidemo_back_command = {
	.id = APIDEMO_COMMAND_BACK,
	.type = FPLINUX_UI_COMMAND_BACK,
	/* NULL: the runtime's word in the system language, Back. */
	.label = NULL,
	.priority = 0,
};

bool apidemo_add_back(struct fplinux_ui_screen *screen)
{
	return fplinux_ui_screen_add_command(screen, &apidemo_back_command);
}

void apidemo_back_listener(struct fplinux_ui *ui,
			   struct fplinux_ui_screen *screen, int command_id,
			   void *context)
{
	(void)screen;
	(void)context;
	if (command_id == APIDEMO_COMMAND_BACK)
		fplinux_ui_go_back(ui);
}

bool apidemo_push(struct apidemo *app, struct fplinux_ui_screen *screen,
		  const char *call)
{
	if (!screen) {
		apidemo_error(app, call);
		return false;
	}
	/* The stack owns the screen from here on and deletes it when popped. */
	fplinux_ui_push(app->ui, screen);
	return true;
}

void apidemo_show(struct apidemo *app, const char *title, const char *text)
{
	struct fplinux_ui_screen *alert = fplinux_ui_alert_new(
		app->ui, FPLINUX_UI_ALERT_INFO, title, text, 0);

	if (!alert) {
		perror(PROGRAM ": fplinux_ui_alert_new");
		return;
	}
	fplinux_ui_push(app->ui, alert);
}

void apidemo_showf(struct apidemo *app, const char *title, const char *format,
		   ...)
{
	char text[MESSAGE_BYTES];
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(text, sizeof(text), format, arguments);
	va_end(arguments);
	apidemo_show(app, title, text);
}

const char *apidemo_errno_name(int error)
{
	static const struct {
		int value;
		const char *name;
	} names[] = {
		{ EINVAL, "EINVAL" },
		{ ENOENT, "ENOENT" },
		{ ENOMEM, "ENOMEM" },
		{ EBUSY, "EBUSY" },
		{ ERANGE, "ERANGE" },
		{ E2BIG, "E2BIG" },
		{ EFBIG, "EFBIG" },
		{ EIO, "EIO" },
		{ EACCES, "EACCES" },
		{ EPERM, "EPERM" },
		{ EEXIST, "EEXIST" },
		{ ENOTDIR, "ENOTDIR" },
		{ EISDIR, "EISDIR" },
		{ ENAMETOOLONG, "ENAMETOOLONG" },
		{ EPROTO, "EPROTO" },
		{ EAGAIN, "EAGAIN" },
		{ EBADF, "EBADF" },
		{ ENOSPC, "ENOSPC" },
		/* Linux gives EOPNOTSUPP the same value. */
		{ ENOTSUP, "ENOTSUP" },
		{ ECONNREFUSED, "ECONNREFUSED" },
		{ EMFILE, "EMFILE" },
		{ EPIPE, "EPIPE" },
		{ ETIMEDOUT, "ETIMEDOUT" },
		{ ECONNRESET, "ECONNRESET" },
	};
	static char other[32];

	for (size_t i = 0; i < APIDEMO_COUNT(names); i++)
		if (names[i].value == error)
			return names[i].name;
	snprintf(other, sizeof(other), "errno %d", error);
	return other;
}

void apidemo_error(struct apidemo *app, const char *call)
{
	int error = errno;
	struct fplinux_ui_screen *alert;
	char text[MESSAGE_BYTES];

	fprintf(stderr, PROGRAM ": %s: %s\n", call, strerror(error));
	/* TRANSLATORS: %1$s is a C function, %2$s an errno name and %3$s
	 * its English description. */
	snprintf(text, sizeof(text), _("%1$s failed: %2$s (%3$s)."), call,
		 apidemo_errno_name(error), strerror(error));
	/* NULL title: the runtime's word for the kind, Error. */
	alert = fplinux_ui_alert_new(app->ui, FPLINUX_UI_ALERT_ERROR, NULL,
				     text, 0);
	if (alert)
		fplinux_ui_push(app->ui, alert);
}

const void *apidemo_glyph(struct apidemo *app, const char *name)
{
	return fplinux_ui_glyph(app->ui, name, FPLINUX_UI_GLYPH_ROW);
}

void apidemo_style_scroll(struct apidemo *app, lv_obj_t *object)
{
	lv_obj_set_scroll_dir(object, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(object, LV_SCROLLBAR_MODE_AUTO);
	fplinux_ui_style_scrollbar(app->ui, object);
}

/* The catalogue */

static void open_entry(struct apidemo *app, const struct apidemo_entry *entry)
{
	if (entry && entry->open)
		entry->open(app, entry);
}

/*
 * One category: an IMPLICIT list of its demos. The screen keeps the
 * category with fplinux_ui_screen_set_data(); the listener's context is the
 * application.
 */
static void category_command(struct fplinux_ui *ui,
			     struct fplinux_ui_screen *list, int command_id,
			     void *context)
{
	const struct apidemo_category *category =
		fplinux_ui_screen_get_data(list);
	size_t row = fplinux_ui_list_get_focused(list);

	if (command_id == APIDEMO_COMMAND_BACK)
		fplinux_ui_go_back(ui);
	else if (command_id == FPLINUX_UI_SELECT_COMMAND && category &&
		 row < category->count)
		open_entry(context, &category->entries[row]);
}

static void open_category(struct apidemo *app,
			  const struct apidemo_category *category)
{
	struct fplinux_ui_screen *list = fplinux_ui_list_new(
		app->ui, _(category->title), FPLINUX_UI_LIST_IMPLICIT);

	if (!list) {
		apidemo_error(app, "fplinux_ui_list_new");
		return;
	}
	for (size_t i = 0; i < category->count; i++)
		fplinux_ui_list_append(list, _(category->entries[i].title),
				       category->entries[i].detail);
	fplinux_ui_list_set_select_label(list, _("Open"));
	fplinux_ui_screen_set_data(list, (void *)category);
	apidemo_add_back(list);
	fplinux_ui_screen_set_listener(list, category_command, NULL, app);
	fplinux_ui_push(app->ui, list);
}

/* The first screen of the stack: Back from it leaves the application. */
static void catalogue_command(struct fplinux_ui *ui,
			      struct fplinux_ui_screen *list, int command_id,
			      void *context)
{
	size_t row = fplinux_ui_list_get_focused(list);

	if (command_id == APIDEMO_COMMAND_BACK)
		fplinux_ui_go_back(ui);
	else if (command_id == FPLINUX_UI_SELECT_COMMAND &&
		 row < APIDEMO_COUNT(categories))
		open_category(context, categories[row]);
}

static struct fplinux_ui_screen *new_catalogue(struct apidemo *app)
{
	struct fplinux_ui_screen *list = fplinux_ui_list_new(
		app->ui, _("API Demos"), FPLINUX_UI_LIST_IMPLICIT);
	char detail[64];

	if (!list)
		return NULL;
	for (size_t i = 0; i < APIDEMO_COUNT(categories); i++) {
		const struct apidemo_category *category = categories[i];
		unsigned int count = (unsigned int)category->count;
		size_t row;

		snprintf(detail, sizeof(detail),
			 ngettext("%u demo", "%u demos", count), count);
		row = fplinux_ui_list_append(list, _(category->title), detail);
		/* A "-symbolic.png" icon is drawn in the row's text colour. */
		if (row != SIZE_MAX && category->icon &&
		    !fplinux_ui_list_set_icon(list, row, category->icon))
			perror(category->icon);
	}
	fplinux_ui_list_set_select_label(list, _("Open"));
	apidemo_add_back(list);
	fplinux_ui_screen_set_listener(list, catalogue_command, NULL, app);
	return list;
}

/* Search */

/*
 * Copies text in lower case for a search that ignores case: ASCII and the
 * Russian alphabet, the letters the installed input languages type.
 */
static void fold(const char *text, char *out, size_t size)
{
	const unsigned char *in = (const unsigned char *)text;
	size_t used = 0;

	while (*in && used + 2 < size) {
		unsigned char first = in[0];
		unsigned char second = in[1];

		if (first >= 'A' && first <= 'Z') {
			out[used++] = (char)(first - 'A' + 'a');
			in++;
		} else if (first == 0xd0 && second >= 0x90 && second <= 0x9f) {
			/* А to П: U+0410 to U+041F become U+0430 to U+043F. */
			out[used++] = (char)0xd0;
			out[used++] = (char)(second + 0x20);
			in += 2;
		} else if (first == 0xd0 && second >= 0xa0 && second <= 0xaf) {
			/* Р to Я: U+0420 to U+042F become U+0440 to U+044F. */
			out[used++] = (char)0xd1;
			out[used++] = (char)(second - 0x20);
			in += 2;
		} else if (first == 0xd0 && second == 0x81) {
			/* Ё, U+0401, becomes ё, U+0451. */
			out[used++] = (char)0xd1;
			out[used++] = (char)0x91;
			in += 2;
		} else {
			out[used++] = (char)first;
			in++;
		}
	}
	out[used] = '\0';
}

static bool matches(const char *text, const char *query)
{
	char folded[MESSAGE_BYTES];

	if (!text)
		return false;
	fold(text, folded, sizeof(folded));
	return strstr(folded, query) != NULL;
}

static void search_command(struct fplinux_ui *ui,
			   struct fplinux_ui_screen *list, int command_id,
			   void *context)
{
	struct apidemo_main *main_app = context;
	size_t row = fplinux_ui_list_get_focused(list);

	if (command_id == APIDEMO_COMMAND_BACK)
		fplinux_ui_go_back(ui);
	else if (command_id == FPLINUX_UI_SELECT_COMMAND &&
		 row < main_app->found_count)
		open_entry(&main_app->demo, main_app->found[row]);
}

/*
 * The demos whose title, in the system language or in English, or whose C
 * names contain the search. NULL with found_count 0 when none does.
 */
static struct fplinux_ui_screen *new_results(struct apidemo_main *main_app,
					     const char *search)
{
	struct apidemo *app = &main_app->demo;
	struct fplinux_ui_screen *list;
	char query[SEARCH_BYTES];
	char title[MESSAGE_BYTES];

	fold(search, query, sizeof(query));
	main_app->found_count = 0;
	for (size_t i = 0; i < APIDEMO_COUNT(categories); i++) {
		for (size_t j = 0; j < categories[i]->count; j++) {
			const struct apidemo_entry *entry =
				&categories[i]->entries[j];

			if (main_app->found_count < ENTRY_LIMIT &&
			    (matches(_(entry->title), query) ||
			     matches(entry->title, query) ||
			     matches(entry->detail, query)))
				main_app->found[main_app->found_count++] =
					entry;
		}
	}
	if (!main_app->found_count)
		return NULL;
	/* TRANSLATORS: the title of the search results; %s is the search. */
	snprintf(title, sizeof(title), _("Search: %s"), search);
	list = fplinux_ui_list_new(app->ui, title, FPLINUX_UI_LIST_IMPLICIT);
	if (!list)
		return NULL;
	for (size_t i = 0; i < main_app->found_count; i++)
		fplinux_ui_list_append(list, _(main_app->found[i]->title),
				       main_app->found[i]->detail);
	fplinux_ui_list_set_select_label(list, _("Open"));
	apidemo_add_back(list);
	fplinux_ui_screen_set_listener(list, search_command, NULL, main_app);
	return list;
}

/* The start page: the raw screen */

static void show_note(struct apidemo_main *main_app, enum start_text which)
{
	struct start_page *start = &main_app->start;
	const char *text;

	switch (which) {
	case TEXT_KEYS:
		text = _("OK opens the catalogue, or the demos whose names "
			 "contain the search.\n"
			 "2 to 9 type the search, # changes the input mode "
			 "and * opens the symbols.\n"
			 "Up and Down scroll this text.\n"
			 "In a demo, the soft-key legends name what each key "
			 "does.");
		break;
	case TEXT_NOTHING_FOUND:
		text = _("No demo matches the search. Change it, or clear "
			 "it to open the whole catalogue.");
		break;
	case TEXT_ABOUT:
	default:
		text = _("Explore FPLinux UI functions and LVGL widgets, "
			 "one demo at a time. Type a word to "
			 "search, or press OK for the catalogue.\n"
			 "This page is the raw LVGL screen: "
			 "fplinux_ui_header(), fplinux_ui_text_create(), "
			 "fplinux_ui_set_softkeys(), fplinux_ui_scroll_key() "
			 "and fplinux_ui_menu_open().");
		break;
	}
	lv_label_set_text(start->note, text);
	lv_obj_scroll_to_y(start->notes, 0, LV_ANIM_OFF);
}

/*
 * The legends follow the field: Insert and Cancel while its symbol grid is
 * open, Clear while it holds text and Back when it is empty. Send them
 * only when they change.
 */
static void relabel(struct fplinux_ui_text *field, void *context)
{
	struct apidemo_main *main_app = context;
	struct start_page *start = &main_app->start;
	bool picker = fplinux_ui_text_is_picker_open(field);
	bool empty = fplinux_ui_text_is_empty(field);
	const char *legends[3] = {
		picker ? "" : _("Options"),
		picker ? _("Insert") :
		empty  ? _("Open") :
			 _("Search"),
		picker ? _("Cancel") :
		empty  ? _("Back") :
			 _("Clear"),
	};

	if (start->left)
		return;
	if (start->legends[0] && !strcmp(legends[0], start->legends[0]) &&
	    !strcmp(legends[1], start->legends[1]) &&
	    !strcmp(legends[2], start->legends[2]))
		return;
	if (!fplinux_ui_set_softkeys(main_app->demo.ui, legends[0], legends[1],
				     legends[2])) {
		perror(PROGRAM ": fplinux_ui_set_softkeys");
		return;
	}
	memcpy(start->legends, legends, sizeof(legends));
}

/* OK: the catalogue, or the demos that match the search. */
static void open_catalogue(struct apidemo_main *main_app)
{
	struct apidemo *app = &main_app->demo;
	struct start_page *start = &main_app->start;
	struct fplinux_ui_screen *first;
	char search[SEARCH_BYTES];

	/*
	 * An unfocused field commits a pending letter and closes its symbol
	 * grid; do this before the field is hidden by another screen.
	 */
	fplinux_ui_text_set_focused(start->field, false);
	snprintf(search, sizeof(search), "%s",
		 fplinux_ui_text_get(start->field));
	if (search[0]) {
		first = new_results(main_app, search);
		if (!first && !main_app->found_count) {
			/* An alert pushed now would be the only screen and
			 * leave the application when closed: say it here. */
			show_note(main_app, TEXT_NOTHING_FOUND);
			fplinux_ui_text_set_focused(start->field, true);
			return;
		}
	} else {
		first = new_catalogue(app);
	}
	if (!first) {
		perror(PROGRAM ": fplinux_ui_list_new");
		fplinux_ui_text_set_focused(start->field, true);
		return;
	}
	/* From here on the screen stack draws the legends. */
	start->left = true;
	fplinux_ui_push(app->ui, first);
	/*
	 * The stack's screens are drawn above the raw screen, which stays
	 * below them for good: hide it, so that it costs no drawing.
	 */
	lv_obj_set_hidden(start->band, true);
	lv_obj_set_hidden(fplinux_ui_text_object(start->field), true);
	lv_obj_set_hidden(start->notes, true);
}

static void start_menu_chosen(struct fplinux_ui *ui, int action, void *context)
{
	struct apidemo_main *main_app = context;

	switch (action) {
	case START_OPEN:
		open_catalogue(main_app);
		break;
	case START_CLEAR:
		fplinux_ui_text_set(main_app->start.field, "");
		show_note(main_app, TEXT_ABOUT);
		break;
	case START_KEYS:
		show_note(main_app, TEXT_KEYS);
		break;
	case START_ABOUT:
		show_note(main_app, TEXT_ABOUT);
		break;
	case START_EXIT:
		fplinux_ui_back(ui);
		break;
	case FPLINUX_UI_MENU_CANCELLED:
	default:
		break;
	}
	if (!main_app->start.left && action != START_EXIT) {
		fplinux_ui_text_set_focused(main_app->start.field, true);
		relabel(main_app->start.field, main_app);
	}
}

/*
 * The options menu of the raw screen, with system glyphs as icons. The
 * runtime copies the texts and decodes the icons, so a local array is fine.
 */
static void open_start_menu(struct apidemo_main *main_app)
{
	struct apidemo *app = &main_app->demo;
	bool empty = fplinux_ui_text_is_empty(main_app->start.field);
	struct fplinux_ui_menu_item items[5];
	size_t count = 0;

	if (fplinux_ui_menu_is_open(app->ui))
		return;
	items[count++] = (struct fplinux_ui_menu_item){
		.action = START_OPEN,
		.text = empty ? _("Catalogue") : _("Search"),
		.icon = apidemo_glyph(app, empty ? "list" : "search"),
	};
	if (!empty)
		items[count++] = (struct fplinux_ui_menu_item){
			.action = START_CLEAR,
			.text = _("Clear the search"),
			.icon = apidemo_glyph(app, "backspace"),
		};
	items[count++] = (struct fplinux_ui_menu_item){
		.action = START_KEYS,
		.text = _("Keys"),
		.icon = apidemo_glyph(app, "help"),
	};
	items[count++] = (struct fplinux_ui_menu_item){
		.action = START_ABOUT,
		.text = _("About"),
		.icon = apidemo_glyph(app, "about"),
	};
	items[count++] = (struct fplinux_ui_menu_item){
		.action = START_EXIT,
		.text = _("Exit"),
		.icon = apidemo_glyph(app, "exit"),
	};
	/* A raw field is application-owned: stop held Clear behind the menu. */
	fplinux_ui_text_set_focused(main_app->start.field, false);
	if (!fplinux_ui_menu_open(app->ui, _("Options"), items, count,
				  start_menu_chosen, main_app)) {
		perror(PROGRAM ": fplinux_ui_menu_open");
		fplinux_ui_text_set_focused(main_app->start.field, true);
	}
}

static bool is_ok(const struct fplinux_ui_key *key)
{
	return key->code == KEY_OK ||
	       (key->source == FPLINUX_UI_KEY_KEYBOARD &&
		(key->code == KEY_ENTER || key->code == KEY_KPENTER));
}

/* Keys reach here only while the stack is empty and no menu is open. */
static void start_key(struct apidemo_main *main_app,
		      const struct fplinux_ui_key *key)
{
	struct start_page *start = &main_app->start;
	bool keyboard = key->source == FPLINUX_UI_KEY_KEYBOARD;

	if (start->left)
		return;
	/* The field first: typing, the caret, Clear and the symbol grid. */
	if (fplinux_ui_text_key(start->field, key) || !key->pressed)
		return;
	if (fplinux_ui_is_softkey(key, true) ||
	    (keyboard && key->code == KEY_MENU))
		open_start_menu(main_app);
	else if (fplinux_ui_is_softkey(key, false) ||
		 (keyboard && key->code == KEY_ESC))
		fplinux_ui_back(main_app->demo.ui);
	else if (is_ok(key))
		open_catalogue(main_app);
	else
		fplinux_ui_scroll_key(start->notes, key);
}

static bool build_start(struct apidemo_main *main_app)
{
	struct apidemo *app = &main_app->demo;
	struct start_page *start = &main_app->start;
	const struct fplinux_ui_metrics *metrics = app->metrics;
	lv_obj_t *screen = fplinux_ui_screen(app->ui);
	int32_t width = fplinux_ui_width(app->ui);
	int32_t top = metrics->header_height + metrics->spacing;
	lv_obj_t *frame;

	start->band = fplinux_ui_header(app->ui, _("API Demos"));
	if (!start->band)
		return false;
	start->field =
		fplinux_ui_text_create(app->ui, screen, false, SEARCH_CHARS);
	if (!start->field)
		return false;
	frame = fplinux_ui_text_object(start->field);
	lv_obj_set_pos(frame, metrics->margin, top);
	lv_obj_set_width(frame, width - 2 * metrics->margin);
	fplinux_ui_text_set_change_cb(start->field, relabel, main_app);
	/* The field's height follows the profile's font; measure it. */
	lv_obj_update_layout(frame);
	top += lv_obj_get_height(frame) + metrics->spacing;

	start->notes = lv_obj_create(screen);
	if (!start->notes)
		return false;
	lv_obj_remove_style_all(start->notes);
	start->notes_top = top;
	lv_obj_set_pos(start->notes, 0, top);
	lv_obj_set_size(start->notes, width, fplinux_ui_height(app->ui) - top);
	lv_obj_set_style_pad_left(start->notes, metrics->margin, 0);
	lv_obj_set_style_pad_right(start->notes, metrics->gutter, 0);
	lv_obj_set_style_pad_bottom(start->notes, metrics->spacing, 0);
	apidemo_style_scroll(app, start->notes);
	start->note = lv_label_create(start->notes);
	if (!start->note)
		return false;
	lv_obj_set_width(start->note, lv_pct(100));
	lv_label_set_long_mode(start->note, LV_LABEL_LONG_MODE_WRAP);
	lv_obj_set_style_text_font(start->note, app->theme->font, 0);
	lv_obj_set_style_text_color(start->note, app->theme->muted, 0);
	show_note(main_app, TEXT_ABOUT);
	relabel(start->field, main_app);
	return true;
}

/*
 * A new surface size (FPLINUX_UI_EVENT_RESIZE): the start page, below the
 * stack's screens, fills it again as build_start() laid it out.
 */
static void layout_start(struct apidemo_main *main_app)
{
	struct apidemo *app = &main_app->demo;
	struct start_page *start = &main_app->start;
	int32_t width = fplinux_ui_width(app->ui);
	int32_t inner = width - 2 * app->metrics->margin;

	lv_obj_set_width(start->band, width);
	/* The band's one child is the title of fplinux_ui_header(). */
	lv_obj_set_width(lv_obj_get_child(start->band, 0), inner);
	lv_obj_set_width(fplinux_ui_text_object(start->field), inner);
	lv_obj_set_size(start->notes, width,
			fplinux_ui_height(app->ui) - start->notes_top);
}

/* Lifecycle */

static void handle_event(struct fplinux_ui *ui,
			 const struct fplinux_ui_event *event, void *context)
{
	struct apidemo_main *main_app = context;
	struct apidemo *app = &main_app->demo;

	(void)ui;
	switch (event->type) {
	case FPLINUX_UI_EVENT_KEY:
		start_key(main_app, &event->key);
		break;
	case FPLINUX_UI_EVENT_VISIBILITY:
		app->visible = event->visible;
		for (size_t i = 0; i < APIDEMO_COUNT(categories); i++)
			if (categories[i]->visibility)
				categories[i]->visibility(app, event->visible);
		/* A hidden application may never be shown again: save. */
		if (!event->visible && app->prefs)
			apidemo_save_prefs(app, false);
		break;
	case FPLINUX_UI_EVENT_CLOSE:
		for (size_t i = 0; i < APIDEMO_COUNT(categories); i++)
			if (categories[i]->close)
				categories[i]->close(app);
		if (app->prefs)
			apidemo_save_prefs(app, false);
		break;
	case FPLINUX_UI_EVENT_RESIZE:
		layout_start(main_app);
		for (size_t i = 0; i < APIDEMO_COUNT(categories); i++)
			if (categories[i]->resize)
				categories[i]->resize(app);
		break;
	case FPLINUX_UI_EVENT_SESSION:
		if (event->session &&
		    event->session->type == FPLINUX_UI_SESSION_ERROR &&
		    !strcmp(event->session->name, "vibration"))
			apidemo_vibration_error(app, event->session->reason);
		break;
	default:
		break;
	}
}

int main(int argc, char **argv)
{
	struct fplinux_cli cli = {
		.program = PROGRAM,
		.description = "Explore FPLinux UI functions and LVGL widgets.",
	};
	static struct apidemo_main main_app;
	struct apidemo *app = &main_app.demo;
	int result;

	result = fplinux_cli_parse(&cli, argc, argv);
	if (result != FPLINUX_CLI_READY)
		return result;

	/* Preferences may be opened before the UI; NULL if unreadable. */
	app->prefs = fplinux_ui_prefs_open(APIDEMO_APP_ID);
	if (!app->prefs)
		perror(PROGRAM ": fplinux_ui_prefs_open");
	/* fplinux_ui_open() also selects the language for gettext(). */
	app->ui = fplinux_ui_open(APIDEMO_APP_ID, handle_event, &main_app);
	if (!app->ui) {
		perror(PROGRAM ": fplinux_ui_open");
		fplinux_ui_prefs_close(app->prefs);
		return 1;
	}
	app->metrics = fplinux_ui_metrics(app->ui);
	app->theme = fplinux_ui_theme(app->ui);
	app->visible = fplinux_ui_is_visible(app->ui);
	if (!build_start(&main_app)) {
		perror(PROGRAM ": start page");
		if (main_app.start.field)
			fplinux_ui_text_delete(main_app.start.field);
		fplinux_ui_close(app->ui);
		fplinux_ui_prefs_close(app->prefs);
		return 1;
	}
	result = fplinux_ui_run(app->ui);
	/* Timers, watches and input devices go before the display. */
	for (size_t i = 0; i < APIDEMO_COUNT(categories); i++)
		if (categories[i]->cleanup)
			categories[i]->cleanup(app);
	fplinux_ui_text_delete(main_app.start.field);
	if (app->prefs)
		apidemo_save_prefs(app, false);
	fplinux_ui_close(app->ui);
	fplinux_ui_prefs_close(app->prefs);
	return result < 0;
}
