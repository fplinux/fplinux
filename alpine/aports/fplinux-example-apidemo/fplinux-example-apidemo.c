/* SPDX-License-Identifier: 0BSD */
/* The catalogue, returning search and helpers shared by the examples. */
#include "apidemo.h"
#include "fplinux-cli.h"

#include <fplinux/fplinux-ui-session.h>
#include <fplinux/fplinux-utf8.h>

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PROGRAM "fplinux-example-apidemo"
/* The longest search the field takes, in characters. */
#define SEARCH_CHARS 24
/* UTF-8 storage for the retained query, including its terminator. */
#define SEARCH_BYTES (4 * SEARCH_CHARS + 1)
#define MESSAGE_BYTES 1024

enum {
	COMMAND_SEARCH = 1,
	COMMAND_HELP,
};

/* Modules retain their resource owners when several share a section. */
static const struct apidemo_category *const categories[] = {
	&apidemo_lists,	   &apidemo_forms,     &apidemo_text,
	&apidemo_commands, &apidemo_alerts,    &apidemo_softkeys,
	&apidemo_canvas,   &apidemo_frame,     &apidemo_look,
	&apidemo_locale,   &apidemo_lifecycle, &apidemo_vibration,
	&apidemo_sound,
};

struct catalogue_section {
	const char *title;
	const char *description;
	const char *icon;
	const struct apidemo_category *groups[3];
};

static const struct catalogue_section sections[] = {
	{ N_("Lists"),
	  N_("Choose, mark and change rows"),
	  "lists",
	  { &apidemo_lists } },
	{ N_("Forms and text"),
	  N_("Edit fields and complete a form"),
	  "forms",
	  { &apidemo_forms, &apidemo_text } },
	{ N_("Commands and dialogs"),
	  N_("Actions, menus and progress"),
	  "commands",
	  { &apidemo_commands, &apidemo_alerts, &apidemo_softkeys } },
	{ N_("Graphics and input"),
	  N_("Draw and handle keys"),
	  "canvas",
	  { &apidemo_canvas, &apidemo_frame } },
	{ N_("Appearance"),
	  N_("Colours, fonts and images"),
	  "look",
	  { &apidemo_look } },
	{ N_("Data and language"),
	  N_("Save values and format local text"),
	  "locale",
	  { &apidemo_locale } },
	{ N_("Lifecycle"),
	  N_("Screens, visibility and resources"),
	  "lifecycle",
	  { &apidemo_lifecycle } },
	{ N_("Sound and vibration"),
	  N_("Start, stop and receive a result"),
	  "sound",
	  { &apidemo_sound, &apidemo_vibration } },
};

#define ENTRY_LIMIT 128

struct apidemo_main {
	struct apidemo demo;
	const struct apidemo_entry *found[ENTRY_LIMIT];
	size_t found_count;
	char search[SEARCH_BYTES];
	size_t search_item;
	struct fplinux_ui_screen *search_form;
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

struct fplinux_ui_screen *apidemo_error(struct apidemo *app, const char *call)
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
	return alert;
}

static void error_closed(struct fplinux_ui *ui,
			 struct fplinux_ui_screen *screen, int command_id,
			 void *context)
{
	struct apidemo *app = context;

	(void)ui;
	(void)screen;
	(void)command_id;
	app->decision = NULL;
}

void apidemo_protect_error(struct apidemo *app, struct fplinux_ui_screen *alert)
{
	const struct fplinux_ui_command close = {
		.id = APIDEMO_COMMAND_BACK,
		.type = FPLINUX_UI_COMMAND_BACK,
		.label = _("Close"),
	};

	if (!alert || !fplinux_ui_screen_add_command(alert, &close))
		return;
	app->decision = alert;
	fplinux_ui_screen_set_listener(alert, error_closed, NULL, app);
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

static void show_help(struct apidemo *app, const struct apidemo_entry *entry)
{
	if (entry)
		apidemo_showf(app, _(entry->title), "%s\n\n%s",
			      _(entry->description), entry->detail);
}

static void add_help(struct fplinux_ui_screen *screen)
{
	const struct fplinux_ui_command help = {
		.id = COMMAND_HELP,
		.type = FPLINUX_UI_COMMAND_HELP,
	};

	fplinux_ui_screen_add_command(screen, &help);
}

static const struct apidemo_entry *
section_entry(const struct catalogue_section *section, size_t row)
{
	for (size_t i = 0; i < APIDEMO_COUNT(section->groups); i++) {
		const struct apidemo_category *group = section->groups[i];

		if (!group)
			break;
		if (row < group->count)
			return &group->entries[row];
		row -= group->count;
	}
	return NULL;
}

static void category_command(struct fplinux_ui *ui,
			     struct fplinux_ui_screen *list, int command_id,
			     void *context)
{
	const struct catalogue_section *section =
		fplinux_ui_screen_get_data(list);
	const struct apidemo_entry *entry =
		section_entry(section, fplinux_ui_list_get_focused(list));

	if (command_id == APIDEMO_COMMAND_BACK)
		fplinux_ui_go_back(ui);
	else if (command_id == FPLINUX_UI_SELECT_COMMAND)
		open_entry(context, entry);
	else if (command_id == COMMAND_HELP)
		show_help(context, entry);
}

static void open_category(struct apidemo *app,
			  const struct catalogue_section *section)
{
	struct fplinux_ui_screen *list = fplinux_ui_list_new(
		app->ui, _(section->title), FPLINUX_UI_LIST_IMPLICIT);

	if (!list) {
		apidemo_error(app, "fplinux_ui_list_new");
		return;
	}
	for (size_t row = 0;; row++) {
		const struct apidemo_entry *entry = section_entry(section, row);

		if (!entry)
			break;
		fplinux_ui_list_append(list, _(entry->title),
				       _(entry->description));
	}
	fplinux_ui_list_set_select_label(list, _("Open"));
	fplinux_ui_screen_set_data(list, (void *)section);
	apidemo_add_back(list);
	add_help(list);
	fplinux_ui_screen_set_listener(list, category_command, NULL, app);
	fplinux_ui_push(app->ui, list);
}

static void open_search(struct apidemo_main *main_app);

static void catalogue_command(struct fplinux_ui *ui,
			      struct fplinux_ui_screen *list, int command_id,
			      void *context)
{
	struct apidemo_main *main_app = context;
	size_t row = fplinux_ui_list_get_focused(list);

	if (command_id == APIDEMO_COMMAND_BACK)
		fplinux_ui_go_back(ui);
	else if (command_id == COMMAND_SEARCH)
		open_search(main_app);
	else if (command_id == FPLINUX_UI_SELECT_COMMAND &&
		 row < APIDEMO_COUNT(sections))
		open_category(&main_app->demo, &sections[row]);
}

static struct fplinux_ui_screen *new_catalogue(struct apidemo_main *main_app)
{
	struct apidemo *app = &main_app->demo;
	struct fplinux_ui_screen *list = fplinux_ui_list_new(
		app->ui, _("API Demos"), FPLINUX_UI_LIST_IMPLICIT);
	const struct fplinux_ui_command search = {
		.id = COMMAND_SEARCH,
		.type = FPLINUX_UI_COMMAND_SCREEN,
		.label = _("Search"),
		.icon = apidemo_glyph(app, "search"),
	};
	char icon[256];

	if (!list)
		return NULL;
	for (size_t i = 0; i < APIDEMO_COUNT(sections); i++) {
		size_t row = fplinux_ui_list_append(list, _(sections[i].title),
						    _(sections[i].description));

		snprintf(icon, sizeof(icon),
			 APIDEMO_IMAGE_DIR "%s-symbolic.png", sections[i].icon);
		if (row != SIZE_MAX &&
		    !fplinux_ui_list_set_icon(list, row, icon,
					      FPLINUX_UI_LIST_ICON_IMAGE))
			perror(icon);
	}
	fplinux_ui_list_set_select_label(list, _("Open"));
	fplinux_ui_screen_add_command(list, &search);
	apidemo_add_back(list);
	fplinux_ui_screen_set_listener(list, catalogue_command, NULL, main_app);
	return list;
}

/* Search */

static void search_command(struct fplinux_ui *ui,
			   struct fplinux_ui_screen *list, int command_id,
			   void *context)
{
	struct apidemo_main *main_app = context;
	size_t row = fplinux_ui_list_get_focused(list);

	if (command_id == APIDEMO_COMMAND_BACK)
		fplinux_ui_go_back(ui);
	else if (row < main_app->found_count) {
		if (command_id == FPLINUX_UI_SELECT_COMMAND)
			open_entry(&main_app->demo, main_app->found[row]);
		else if (command_id == COMMAND_HELP)
			show_help(&main_app->demo, main_app->found[row]);
	}
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
	char title[MESSAGE_BYTES];

	main_app->found_count = 0;
	for (size_t i = 0; i < APIDEMO_COUNT(categories); i++) {
		for (size_t j = 0; j < categories[i]->count; j++) {
			const struct apidemo_entry *entry =
				&categories[i]->entries[j];

			if (main_app->found_count < ENTRY_LIMIT &&
			    (fplinux_utf8_contains_casefold(_(entry->title),
							    search) ||
			     fplinux_utf8_contains_casefold(entry->title,
							    search) ||
			     fplinux_utf8_contains_casefold(entry->detail,
							    search)))
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
				       _(main_app->found[i]->description));
	fplinux_ui_list_set_select_label(list, _("Open"));
	apidemo_add_back(list);
	add_help(list);
	fplinux_ui_screen_set_listener(list, search_command, NULL, main_app);
	return list;
}

static void search_form_command(struct fplinux_ui *ui,
				struct fplinux_ui_screen *form, int command_id,
				void *context)
{
	struct apidemo_main *main_app = context;
	struct fplinux_ui_screen *results;
	const char *query =
		fplinux_ui_form_get_text(form, main_app->search_item);

	snprintf(main_app->search, sizeof(main_app->search), "%s",
		 query ? query : "");
	if (command_id == APIDEMO_COMMAND_BACK) {
		main_app->search_form = NULL;
		fplinux_ui_go_back(ui);
	} else if (command_id == COMMAND_SEARCH) {
		results = new_results(main_app, main_app->search);
		if (results)
			fplinux_ui_push(ui, results);
		else if (!main_app->found_count)
			apidemo_show(
				&main_app->demo, _("Search"),
				_("No demo matches the search. Change the query and try again."));
		else
			apidemo_error(&main_app->demo, "fplinux_ui_list_new");
	}
}

static void open_search(struct apidemo_main *main_app)
{
	struct apidemo *app = &main_app->demo;
	struct fplinux_ui_screen *form =
		fplinux_ui_form_new(app->ui, _("Search"));
	const struct fplinux_ui_command search = {
		.id = COMMAND_SEARCH,
		.type = FPLINUX_UI_COMMAND_OK,
		.label = _("Search"),
		.icon = apidemo_glyph(app, "search"),
	};

	if (!form) {
		apidemo_error(app, "fplinux_ui_form_new");
		return;
	}
	main_app->search_item = fplinux_ui_form_append_text_field(
		form, _("Name or API"), main_app->search, SEARCH_CHARS,
		FPLINUX_UI_TEXT_ANY);
	fplinux_ui_screen_add_command(form, &search);
	apidemo_add_back(form);
	fplinux_ui_screen_set_listener(form, search_form_command, NULL,
				       main_app);
	main_app->search_form = form;
	fplinux_ui_push(app->ui, form);
}

/* Lifecycle */

static void open_catalogue(struct apidemo_main *main_app)
{
	struct apidemo *app = &main_app->demo;

	fplinux_ui_menu_close(app->ui);
	if (app->decision || !app->catalogue)
		return;
	if (main_app->search_form) {
		const char *query = fplinux_ui_form_get_text(
			main_app->search_form, main_app->search_item);

		snprintf(main_app->search, sizeof(main_app->search), "%s",
			 query ? query : "");
		main_app->search_form = NULL;
	}
	/* Each section releases its resources before its screens are deleted. */
	for (size_t i = 0; i < APIDEMO_COUNT(categories); i++)
		if (categories[i]->cleanup)
			categories[i]->cleanup(app);
	if (fplinux_ui_is_fullscreen(app->ui) &&
	    !fplinux_ui_set_fullscreen(app->ui, false))
		perror("fplinux_ui_set_fullscreen");
	while (fplinux_ui_current(app->ui) != app->catalogue)
		if (!fplinux_ui_pop(app->ui))
			break;
}

static void handle_event(struct fplinux_ui *ui,
			 const struct fplinux_ui_event *event, void *context)
{
	struct apidemo_main *main_app = context;
	struct apidemo *app = &main_app->demo;

	(void)ui;
	switch (event->type) {
	case FPLINUX_UI_EVENT_ACTIVATION:
		if (event->activation == FPLINUX_UI_ACTIVATION_OPEN)
			open_catalogue(main_app);
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
		.description = "Explore the FPLinux application API.",
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
	app->catalogue = new_catalogue(&main_app);
	if (!apidemo_push(app, app->catalogue, "fplinux_ui_list_new")) {
		fplinux_ui_close(app->ui);
		fplinux_ui_prefs_close(app->prefs);
		return 1;
	}
	result = fplinux_ui_run(app->ui);
	/* Timers, watches and input devices go before the display. */
	for (size_t i = 0; i < APIDEMO_COUNT(categories); i++)
		if (categories[i]->cleanup)
			categories[i]->cleanup(app);
	if (app->prefs)
		apidemo_save_prefs(app, false);
	fplinux_ui_close(app->ui);
	fplinux_ui_prefs_close(app->prefs);
	return result < 0;
}
